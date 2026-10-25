#include "gui_controller.hpp"

#include "opendisplay/daemon_support.hpp"
#include "opendisplay/settings.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QLocalSocket>
#include <QMenu>
#include <QProcess>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>

#include <exception>

namespace od::gui {
namespace {

QString capitalized(QString text) {
    if (!text.isEmpty()) text[0] = text[0].toUpper();
    return text;
}

void startDaemon() {
    QProcess::startDetached(QStringLiteral("systemctl"),
                            {QStringLiteral("--user"), QStringLiteral("start"),
                             QStringLiteral("opendisplayd.service")});
}

}  // namespace

GuiController::GuiController(QObject* parent)
    : QObject(parent),
      settings_(QStringLiteral("OpenDisplay"), QStringLiteral("OpenDisplay")),
      watch_(new QLocalSocket(this)), reconnectTimer_(new QTimer(this)) {
    loadInitialSettings();
    createTray();
    connect(watch_, &QLocalSocket::connected, this, [this] {
        buffer_.clear();
        send({{QStringLiteral("cmd"), QStringLiteral("watch")}});
    });
    connect(watch_, &QLocalSocket::readyRead, this, &GuiController::readWatch);
    connect(watch_, &QLocalSocket::disconnected, this, &GuiController::watchLost);
    connect(watch_, &QLocalSocket::errorOccurred, this, [this] { watchLost(); });
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &GuiController::openWatch);
    openWatch();
}

GuiController::~GuiController() {
    tray_->setContextMenu(nullptr);
    delete trayMenu_;
    trayMenu_ = nullptr;
}

void GuiController::loadInitialSettings() {
    // QML reads savedSettings once at load, so fetch the daemon's settings
    // synchronously (starting the daemon if needed); fall back to the GUI's
    // own last-used values.
    savedSettings_ = settings_.value(QStringLiteral("connection")).toMap();
    for (int attempt = 0; attempt < 2; ++attempt) {
        QLocalSocket probe;
        probe.connectToServer(controlSocketPath());
        if (!probe.waitForConnected(500)) {
            if (attempt == 0) {
                startDaemon();
                QThread::msleep(500);
            }
            continue;
        }
        probe.write(encodeControlMessage({{QStringLiteral("cmd"), QStringLiteral("status")}}));
        probe.flush();
        QByteArray line;
        while (!line.contains('\n') && probe.waitForReadyRead(1'000)) line += probe.readAll();
        const auto reply = decodeControlMessage(line);
        if (!reply) return;
        const auto state = reply->value(QStringLiteral("state")).toObject();
        auto merged = state.value(QStringLiteral("settings")).toObject().toVariantMap();
        merged.insert(QStringLiteral("mode"), state.value(QStringLiteral("mode")).toString());
        for (auto it = merged.cbegin(); it != merged.cend(); ++it) savedSettings_.insert(it.key(), it.value());
        return;
    }
}

void GuiController::openWatch() {
    if (watch_->state() != QLocalSocket::UnconnectedState) return;
    watch_->connectToServer(controlSocketPath());
}

void GuiController::watchLost() {
    if (quitting_) return;
    if (watch_->state() == QLocalSocket::ConnectedState) return;
    connected_ = false;
    busy_ = false;
    status_ = QStringLiteral("Service unavailable");
    detail_ = QStringLiteral("Waiting for opendisplayd… (systemctl --user status opendisplayd)");
    connectAction_->setEnabled(false);
    disconnectAction_->setEnabled(false);
    emit stateChanged();
    if (!daemonStartRequested_) {
        daemonStartRequested_ = true;
        startDaemon();
    }
    watch_->abort();
    reconnectTimer_->start(1'000);
}

void GuiController::readWatch() {
    buffer_ += watch_->readAll();
    qsizetype newline = 0;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const auto message = decodeControlMessage(buffer_.left(newline));
        buffer_.remove(0, newline + 1);
        if (!message) continue;
        if (message->contains(QStringLiteral("ok")) && !message->value(QStringLiteral("ok")).toBool()) {
            setLocalError(message->value(QStringLiteral("error")).toString());
            continue;
        }
        const auto event = message->value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("stats")) {
            applyStats(message->value(QStringLiteral("stats")).toObject());
        } else if (message->contains(QStringLiteral("state"))) {
            daemonStartRequested_ = false;
            applyState(message->value(QStringLiteral("state")).toObject());
        }
    }
}

void GuiController::send(const QJsonObject& request) {
    if (watch_->state() != QLocalSocket::ConnectedState) {
        setLocalError(QStringLiteral("The OpenDisplay service is not running."));
        return;
    }
    watch_->write(encodeControlMessage(request));
    watch_->flush();
}

void GuiController::applyState(const QJsonObject& state) {
    const auto status = state.value(QStringLiteral("status")).toString();
    const bool wasConnected = connected_;
    connected_ = status == QStringLiteral("streaming");
    busy_ = status == QStringLiteral("connecting") || status == QStringLiteral("reconnecting");
    mode_ = state.value(QStringLiteral("mode")).toString(QStringLiteral("extend"));
    if (connected_) {
        status_ = QStringLiteral("Connected");
    } else if (status == QStringLiteral("idle")) {
        status_ = QStringLiteral("Disconnected");
    } else {
        status_ = capitalized(status);
    }
    detail_ = state.value(QStringLiteral("detail")).toString();
    if (connected_) {
        const auto encoder = state.value(QStringLiteral("encoder")).toString();
        const auto output = state.value(QStringLiteral("output")).toString();
        detail_ += QStringLiteral(" · %1 mode").arg(mode_);
        if (!encoder.isEmpty()) detail_ += QStringLiteral(" · %1").arg(encoder);
        if (mode_ == QStringLiteral("extend") && !output.isEmpty()) {
            detail_ += QStringLiteral(" · %1").arg(output);
        }
    }
    if (!connected_ && !busy_) {
        statsText_.clear();
        emit statsChanged();
    }
    const bool active = state.value(QStringLiteral("active")).toBool();
    connectAction_->setEnabled(!active);
    disconnectAction_->setEnabled(active || connected_ || busy_);
    modeAction_->setText(mode_ == QStringLiteral("extend") ? QStringLiteral("Switch to Mirror")
                                                           : QStringLiteral("Switch to Extend"));
    tray_->setToolTip(QStringLiteral("OpenDisplay — %1").arg(status_));
    emit stateChanged();

    if (trayAvailable_ && connected_ && !wasConnected) {
        tray_->showMessage(QStringLiteral("OpenDisplay"), detail_,
                           QSystemTrayIcon::Information, 3000);
    }
}

void GuiController::applyStats(const QJsonObject& stats) {
    const auto sent = stats.value(QStringLiteral("sent")).toObject();
    statsText_ = QStringLiteral("%1 fps · capture→sent p50 %2 ms, p95 %3 ms")
                     .arg(qRound(stats.value(QStringLiteral("fps")).toDouble()))
                     .arg(sent.value(QStringLiteral("p50")).toInteger())
                     .arg(sent.value(QStringLiteral("p95")).toInteger());
    emit statsChanged();
}

void GuiController::connectDevice(const QVariantMap& values) {
    try {
        od::optionsFromSettings(values);  // validate locally for instant feedback
    } catch (const std::exception& error) {
        setLocalError(QString::fromUtf8(error.what()));
        return;
    }
    settings_.setValue(QStringLiteral("connection"), values);
    auto settings = QJsonObject::fromVariantMap(values);
    const auto mode = settings.take(QStringLiteral("mode")).toString();
    QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("connect")},
                        {QStringLiteral("settings"), settings}};
    if (!mode.isEmpty()) request.insert(QStringLiteral("mode"), mode);
    send(request);
}

void GuiController::connectLast() { send({{QStringLiteral("cmd"), QStringLiteral("connect")}}); }

void GuiController::disconnectDevice() {
    send({{QStringLiteral("cmd"), QStringLiteral("disconnect")}});
}

void GuiController::setMode(const QString& mode) {
    if (mode == mode_) return;
    send({{QStringLiteral("cmd"), QStringLiteral("mode")}, {QStringLiteral("mode"), mode}});
}

void GuiController::quit() {
    // Quitting the GUI leaves the daemon (and any stream) running.
    if (quitting_) return;
    quitting_ = true;
    emit quittingChanged();
    QCoreApplication::quit();
}

void GuiController::createTray() {
    tray_ = new QSystemTrayIcon(this);
    QIcon trayIcon = QIcon::fromTheme(QStringLiteral("video-display"));
    if (trayIcon.isNull()) {
        trayIcon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    tray_->setIcon(trayIcon);
    tray_->setToolTip(QStringLiteral("OpenDisplay"));
    trayMenu_ = new QMenu;
    auto* showAction = trayMenu_->addAction(QIcon::fromTheme(QStringLiteral("window")),
                                            QStringLiteral("Show OpenDisplay"));
    trayMenu_->addSeparator();
    connectAction_ = trayMenu_->addAction(QStringLiteral("Connect"));
    disconnectAction_ = trayMenu_->addAction(QStringLiteral("Disconnect"));
    modeAction_ = trayMenu_->addAction(QStringLiteral("Switch to Mirror"));
    disconnectAction_->setEnabled(false);
    trayMenu_->addSeparator();
    auto* quitAction = trayMenu_->addAction(QStringLiteral("Quit window (keeps streaming)"));
    tray_->setContextMenu(trayMenu_);

    connect(showAction, &QAction::triggered, this, &GuiController::showWindowRequested);
    connect(connectAction_, &QAction::triggered, this, &GuiController::connectLast);
    connect(disconnectAction_, &QAction::triggered, this, &GuiController::disconnectDevice);
    connect(modeAction_, &QAction::triggered, this, [this] {
        send({{QStringLiteral("cmd"), QStringLiteral("toggle-mode")}});
    });
    connect(quitAction, &QAction::triggered, this, &GuiController::quit);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](const QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    emit showWindowRequested();
                }
            });

    tray_->show();
    trayPoll_ = new QTimer(this);
    trayPoll_->setInterval(2000);
    connect(trayPoll_, &QTimer::timeout, this, &GuiController::updateTrayAvailability);
    trayPoll_->start();
    updateTrayAvailability();
}

void GuiController::updateTrayAvailability() {
    const bool available = QSystemTrayIcon::isSystemTrayAvailable();
    if (available == trayAvailable_) return;
    const bool disappeared = trayAvailable_ && !available;
    trayAvailable_ = available;
    emit trayAvailableChanged();
    if (disappeared) emit showWindowRequested();
}

void GuiController::setLocalError(const QString& message) {
    status_ = QStringLiteral("Invalid settings");
    detail_ = message;
    emit stateChanged();
}

}  // namespace od::gui
