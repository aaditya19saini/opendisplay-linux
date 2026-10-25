#include "session_runner.hpp"

#include "opendisplay/daemon_support.hpp"
#include "opendisplay/desktop_backend_factory.hpp"
#include "opendisplay/discovery.hpp"
#include "opendisplay/log.hpp"
#include "opendisplay/settings.hpp"

#include <QTimer>

#include <chrono>
#include <exception>

namespace od::daemon {
namespace {

bool isCancellation(const std::string& message) {
    return message.find("portal request cancelled") != std::string::npos;
}

QJsonObject summaryJson(const od::LatencySummary& summary) {
    return {{QStringLiteral("p50"), static_cast<qint64>(summary.p50)},
            {QStringLiteral("p95"), static_cast<qint64>(summary.p95)},
            {QStringLiteral("max"), static_cast<qint64>(summary.max)}};
}

}  // namespace

SessionRunner::SessionRunner(QObject* parent) : QObject(parent), timer_(new QTimer(this)) {
    timer_->setInterval(std::chrono::milliseconds(20));
    connect(timer_, &QTimer::timeout, this, &SessionRunner::tick);
}

SessionRunner::~SessionRunner() {
    if (session_) session_->stop();
}

void SessionRunner::start(od::Options options, QVariantMap restoreTokens) {
    if (session_ || busy_) return;
    busy_ = true;
    stopRequested_ = false;
    pendingMode_.reset();
    options_ = std::move(options);
    restoreTokens_ = std::move(restoreTokens);
    publishedToken_.clear();
    od::verboseLogging = options_.verbose;
    emit connecting(QStringLiteral("Looking for the receiver…"));
    try {
        endpoint_ = od::chooseEndpoint(options_);
        options_.virtualOutputName = od::virtualOutputNameFor(endpoint_);
        options_.restoreToken = restoreTokens_
            .value(od::restoreTokenKey(options_.mode, options_.virtualOutputName))
            .toString().toStdString();
        const auto name = QString::fromStdString(
            endpoint_.name.empty() ? endpoint_.udid : endpoint_.name);
        emit connecting(options_.restoreToken.empty()
                            ? QStringLiteral("Connecting to %1 — choose %2 in the share dialog")
                                  .arg(name, QString::fromStdString(options_.virtualOutputName))
                            : QStringLiteral("Connecting to %1…").arg(name));
        session_ = std::make_unique<od::Session>(options_,
                                                 od::makeDesktopBackend(options_.compositor));
        session_->start(endpoint_);
    } catch (const std::exception& error) {
        busy_ = false;
        if (session_) session_->stop();
        session_.reset();
        const std::string message = error.what();
        emit ended(QString::fromStdString(message), true, isCancellation(message));
        return;
    }
    busy_ = false;
    publishRestoreToken();
    emit streaming(streamingInfo());
    timer_->start();
    runDeferred();
}

void SessionRunner::stop() {
    if (busy_) {
        stopRequested_ = true;
        return;
    }
    finish(QStringLiteral("Disconnected"), false, false);
}

void SessionRunner::switchMode(const int mode) {
    const auto requested = static_cast<od::CaptureMode>(mode);
    if (busy_) {
        pendingMode_ = requested;
        return;
    }
    if (!session_ || requested == options_.mode) return;
    busy_ = true;
    options_.mode = requested;
    session_->setRestoreToken(
        restoreTokens_.value(od::restoreTokenKey(requested, options_.virtualOutputName))
            .toString().toStdString());
    try {
        session_->switchMode(requested);
    } catch (const std::exception& error) {
        busy_ = false;
        const std::string message = error.what();
        finish(QString::fromStdString(message), true, isCancellation(message));
        return;
    }
    busy_ = false;
    publishRestoreToken();
    emit modeChanged(mode);
    emit streaming(streamingInfo());
    runDeferred();
}

void SessionRunner::setWorkspace(const QString& workspace) {
    options_.display.workspace = workspace.toStdString();
    if (!session_ || busy_) return;  // applied at the next start / after the switch
    busy_ = true;
    try {
        session_->setWorkspace(options_.display.workspace);
    } catch (const std::exception& error) {
        od::log(std::string("Cannot change workspace: ") + error.what());
    }
    busy_ = false;
    runDeferred();
}

void SessionRunner::runDeferred() {
    if (stopRequested_) {
        stopRequested_ = false;
        pendingMode_.reset();
        finish(QStringLiteral("Disconnected"), false, false);
        return;
    }
    if (pendingMode_) {
        const auto mode = *pendingMode_;
        pendingMode_.reset();
        switchMode(static_cast<int>(mode));
    }
}

void SessionRunner::tick() {
    if (busy_) return;  // a nested portal loop is running inside start/switch
    if (!session_) {
        timer_->stop();
        return;
    }
    try {
        if (!session_->tick()) {
            finish(QStringLiteral("The receiver closed the connection"), false, false);
            return;
        }
    } catch (const std::exception& error) {
        finish(QString::fromUtf8(error.what()), true, false);
        return;
    }
    const auto [serial, stats] = session_->stats();
    if (serial != statsSerial_) {
        statsSerial_ = serial;
        emit statsUpdated({
            {QStringLiteral("fps"), stats.fps},
            {QStringLiteral("frames"), static_cast<qint64>(stats.sent.count)},
            {QStringLiteral("encode"), summaryJson(stats.encode)},
            {QStringLiteral("sent"), summaryJson(stats.sent)},
        });
    }
}

void SessionRunner::finish(const QString& reason, const bool failure, const bool cancelled) {
    timer_->stop();
    if (session_) {
        session_->stop();
        session_.reset();
    }
    emit ended(reason, failure, cancelled);
}

void SessionRunner::publishRestoreToken() {
    if (!session_) return;
    const auto token = session_->restoreToken();
    if (token.empty() || token == publishedToken_) return;
    publishedToken_ = token;
    const auto key = od::restoreTokenKey(options_.mode, options_.virtualOutputName);
    restoreTokens_.insert(key, QString::fromStdString(token));
    emit restoreTokenIssued(key, QString::fromStdString(token));
}

QJsonObject SessionRunner::streamingInfo() const {
    const auto receiver = session_ ? session_->receiver() : std::nullopt;
    return {
        {QStringLiteral("device"), QString::fromStdString(
             receiver ? receiver->device : endpoint_.name)},
        {QStringLiteral("transport"), endpoint_.kind == od::TransportKind::Usb
                                          ? QStringLiteral("usb") : QStringLiteral("wifi")},
        {QStringLiteral("output"), QString::fromStdString(options_.virtualOutputName)},
        {QStringLiteral("encoder"), QString::fromStdString(
             session_ ? session_->encoderName() : std::string{})},
        {QStringLiteral("mode"), od::captureModeName(options_.mode)},
        {QStringLiteral("resolution"), receiver
             ? QStringLiteral("%1x%2").arg(receiver->pixelsWide).arg(receiver->pixelsHigh)
             : QString()},
    };
}

}  // namespace od::daemon
