#include "controller.hpp"

#include "session_runner.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/settings.hpp"

#include <QMetaObject>
#include <QTimer>

#include <exception>

namespace od::daemon {
namespace {

QString statusName(const Controller::Status status) {
    switch (status) {
    case Controller::Status::Idle: return QStringLiteral("idle");
    case Controller::Status::Connecting: return QStringLiteral("connecting");
    case Controller::Status::Streaming: return QStringLiteral("streaming");
    case Controller::Status::Reconnecting: return QStringLiteral("reconnecting");
    }
    return QStringLiteral("idle");
}

}  // namespace

Controller::Controller(QObject* parent)
    : QObject(parent), runner_(new SessionRunner), retryTimer_(new QTimer(this)),
      statePath_(stateFilePath()), persisted_(loadPersistedState(statePath_)) {
    qRegisterMetaType<od::Options>("od::Options");
    runner_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, runner_, &QObject::deleteLater);
    connect(runner_, &SessionRunner::connecting, this, &Controller::onConnecting);
    connect(runner_, &SessionRunner::streaming, this, &Controller::onStreaming);
    connect(runner_, &SessionRunner::modeChanged, this, &Controller::onModeChanged);
    connect(runner_, &SessionRunner::ended, this, &Controller::onEnded);
    connect(runner_, &SessionRunner::restoreTokenIssued, this, &Controller::onRestoreToken);
    connect(runner_, &SessionRunner::statsUpdated, this, &Controller::onStats);
    workerThread_.setObjectName(QStringLiteral("opendisplay-session"));
    workerThread_.start();

    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &Controller::attempt);

    if (persisted_.autoconnect || persisted_.resume) {
        log(persisted_.resume ? "Resuming the session that was running before the daemon stopped"
                              : "Autoconnect is on; connecting to the last receiver");
        connectRequest(std::nullopt, {});
    }
}

Controller::~Controller() { shutdown(); }

void Controller::shutdown() {
    if (!workerThread_.isRunning()) return;
    // A clean stop (logout, systemctl stop) must not resume on next start.
    persisted_.resume = false;
    save();
    desiredActive_ = false;
    retryTimer_->stop();
    QMetaObject::invokeMethod(runner_, &SessionRunner::stop, Qt::QueuedConnection);
    workerThread_.quit();
    // A share dialog may hold the worker in a nested portal loop; don't hang
    // shutdown on it. Leftover outputs are cleaned up on the next start.
    if (!workerThread_.wait(5'000)) {
        log("Session worker did not stop within 5 s; exiting anyway");
    }
}

QJsonObject Controller::snapshot() const {
    QJsonObject state{
        {QStringLiteral("status"), statusName(status_)},
        {QStringLiteral("active"), desiredActive_},
        {QStringLiteral("mode"), captureModeName(persisted_.mode)},
        {QStringLiteral("detail"), detail_},
        {QStringLiteral("autoconnect"), persisted_.autoconnect},
        {QStringLiteral("settings"), QJsonObject::fromVariantMap(persisted_.settings)},
    };
    if (status_ == Status::Streaming || status_ == Status::Connecting) {
        for (auto it = info_.begin(); it != info_.end(); ++it) {
            if (it.key() != QStringLiteral("mode")) state.insert(it.key(), it.value());
        }
    }
    if (status_ == Status::Reconnecting && retryAt_.isValid()) {
        state.insert(QStringLiteral("retryInMs"),
                     qMax<qint64>(0, QDateTime::currentDateTime().msecsTo(retryAt_)));
    }
    return state;
}

QString Controller::mergeSettings(const QJsonObject& settings) {
    if (settings.isEmpty()) return {};
    auto merged = persisted_.settings;
    const auto incoming = settings.toVariantMap();
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it) merged.insert(it.key(), it.value());
    merged.remove(QStringLiteral("mode"));  // tracked separately in persisted_.mode
    try {
        optionsFromSettings(merged);
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    }
    persisted_.settings = merged;
    return {};
}

QString Controller::connectRequest(std::optional<CaptureMode> mode,
                                   const QJsonObject& settings) {
    if (auto error = mergeSettings(settings); !error.isEmpty()) return error;
    if (!mode) mode = parseCaptureMode(settings.value(QStringLiteral("mode")).toString());
    if (mode) persisted_.mode = *mode;
    persisted_.resume = true;
    save();
    desiredActive_ = true;
    if (status_ == Status::Streaming) {
        if (mode && info_.value(QStringLiteral("mode")).toString() != captureModeName(*mode)) {
            QMetaObject::invokeMethod(runner_, "switchMode", Qt::QueuedConnection,
                                      Q_ARG(int, static_cast<int>(*mode)));
        }
        publish();
        return {};
    }
    if (status_ == Status::Connecting) {
        publish();
        return {};
    }
    backoff_.reset();
    attempt();
    return {};
}

QString Controller::disconnectRequest() {
    desiredActive_ = false;
    persisted_.resume = false;
    save();
    retryTimer_->stop();
    retryAt_ = {};
    if (status_ == Status::Idle || status_ == Status::Reconnecting) {
        setStatus(Status::Idle, QStringLiteral("Disconnected"));
        return {};
    }
    setStatus(status_, QStringLiteral("Disconnecting…"));
    QMetaObject::invokeMethod(runner_, &SessionRunner::stop, Qt::QueuedConnection);
    return {};
}

QString Controller::setMode(const CaptureMode mode) {
    persisted_.mode = mode;
    save();
    if (status_ == Status::Streaming) {
        setStatus(status_, QStringLiteral("Switching to %1…").arg(captureModeName(mode)));
        QMetaObject::invokeMethod(runner_, "switchMode", Qt::QueuedConnection,
                                  Q_ARG(int, static_cast<int>(mode)));
    } else {
        publish();
    }
    return {};
}

QString Controller::toggleMode() {
    return setMode(persisted_.mode == CaptureMode::Extend ? CaptureMode::Mirror
                                                          : CaptureMode::Extend);
}

QString Controller::updateSettings(const QJsonObject& settings,
                                   const std::optional<bool> autoconnect) {
    const auto previousWorkspace = persisted_.settings.value(QStringLiteral("workspace")).toString();
    if (auto error = mergeSettings(settings); !error.isEmpty()) return error;
    const auto workspace = persisted_.settings.value(QStringLiteral("workspace")).toString();
    if (workspace != previousWorkspace && status_ == Status::Streaming) {
        QMetaObject::invokeMethod(runner_, "setWorkspace", Qt::QueuedConnection,
                                  Q_ARG(QString, workspace == QStringLiteral("auto") ? QString() : workspace));
    }
    if (autoconnect) persisted_.autoconnect = *autoconnect;
    if (const auto mode = parseCaptureMode(settings.value(QStringLiteral("mode")).toString())) {
        persisted_.mode = *mode;
    }
    save();
    publish();
    return {};
}

void Controller::attempt() {
    if (!desiredActive_) return;
    retryAt_ = {};
    od::Options options;
    try {
        options = optionsFromSettings(persisted_.settings);
    } catch (const std::exception& error) {
        desiredActive_ = false;
        persisted_.resume = false;
        save();
        setStatus(Status::Idle, QString::fromUtf8(error.what()));
        return;
    }
    options.mode = persisted_.mode;
    info_ = {};
    stats_ = {};
    setStatus(Status::Connecting, QStringLiteral("Starting…"));
    QMetaObject::invokeMethod(runner_, "start", Qt::QueuedConnection,
                              Q_ARG(od::Options, options),
                              Q_ARG(QVariantMap, persisted_.restoreTokens));
}

void Controller::onConnecting(const QString& detail) {
    if (status_ == Status::Connecting) setStatus(Status::Connecting, detail);
}

void Controller::onStreaming(const QJsonObject& info) {
    info_ = info;
    backoff_.reset();
    if (const auto mode = parseCaptureMode(info.value(QStringLiteral("mode")).toString())) {
        if (*mode != persisted_.mode) {
            // The mode was changed while the share dialog was open; apply it.
            QMetaObject::invokeMethod(runner_, "switchMode", Qt::QueuedConnection,
                                      Q_ARG(int, static_cast<int>(persisted_.mode)));
        }
    }
    setStatus(Status::Streaming, QStringLiteral("Streaming to %1")
                                     .arg(info.value(QStringLiteral("device")).toString()));
    if (!desiredActive_) {
        QMetaObject::invokeMethod(runner_, &SessionRunner::stop, Qt::QueuedConnection);
    }
}

void Controller::onModeChanged(const int mode) {
    log(std::string("Now in ") + captureModeName(static_cast<CaptureMode>(mode)).toStdString()
        + " mode");
}

void Controller::onEnded(const QString& reason, const bool failure, const bool cancelled) {
    info_ = {};
    stats_ = {};
    if (cancelled) {
        desiredActive_ = false;
        persisted_.resume = false;
        save();
        setStatus(Status::Idle, QStringLiteral("The share dialog was cancelled"));
        return;
    }
    if (!desiredActive_) {
        setStatus(Status::Idle, failure ? reason : QStringLiteral("Disconnected"));
        return;
    }
    const auto delay = backoff_.next();
    retryAt_ = QDateTime::currentDateTime().addMSecs(delay.count());
    retryTimer_->start(delay);
    log("Session ended (" + reason.toStdString() + "); retrying in "
        + std::to_string(delay.count()) + " ms");
    setStatus(Status::Reconnecting, reason);
}

void Controller::onRestoreToken(const QString& key, const QString& token) {
    persisted_.restoreTokens.insert(key, token);
    save();
    debug("Saved portal restore token for " + key.toStdString());
}

void Controller::onStats(const QJsonObject& stats) {
    stats_ = stats;
    emit statsChanged(stats_);
}

void Controller::save() {
    if (!savePersistedState(statePath_, persisted_)) {
        log("Cannot write " + statePath_.toStdString());
    }
}

void Controller::setStatus(const Status status, const QString& detail) {
    status_ = status;
    detail_ = detail;
    publish();
}

void Controller::publish() { emit stateChanged(snapshot()); }

}  // namespace od::daemon
