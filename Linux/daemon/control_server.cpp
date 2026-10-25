#include "control_server.hpp"

#include "controller.hpp"

#include "opendisplay/daemon_support.hpp"
#include "opendisplay/discovery.hpp"
#include "opendisplay/log.hpp"
#include "opendisplay/settings.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>

#include <sys/stat.h>

#include <chrono>
#include <exception>
#include <thread>

namespace od::daemon {
namespace {

QJsonObject failure(const QString& message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

QJsonObject endpointJson(const od::Endpoint& endpoint) {
    const bool usb = endpoint.kind == od::TransportKind::Usb;
    return {
        {QStringLiteral("transport"), usb ? QStringLiteral("usb") : QStringLiteral("wifi")},
        {QStringLiteral("name"), QString::fromStdString(endpoint.name)},
        {QStringLiteral("id"), QString::fromStdString(
             usb ? endpoint.udid : endpoint.host + ':' + std::to_string(endpoint.port))},
    };
}

}  // namespace

ControlServer::ControlServer(Controller& controller, QObject* parent)
    : QObject(parent), controller_(controller), server_(new QLocalServer(this)) {
    connect(server_, &QLocalServer::newConnection, this, &ControlServer::accept);
    connect(&controller_, &Controller::stateChanged, this, &ControlServer::broadcastState);
    connect(&controller_, &Controller::statsChanged, this, &ControlServer::broadcastStats);
}

bool ControlServer::listen(QString* error) {
    const auto path = controlSocketPath();
    const auto directory = QFileInfo(path).absolutePath();
    QDir().mkpath(directory);
    ::chmod(directory.toLocal8Bit().constData(), 0700);

    // Refuse to steal the socket from a live daemon; clear a stale one.
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(300)) {
        if (error) *error = QStringLiteral("another opendisplayd is already running");
        return false;
    }
    QLocalServer::removeServer(path);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(path)) {
        if (error) *error = server_->errorString();
        return false;
    }
    log("Control socket: " + path.toStdString());
    return true;
}

void ControlServer::accept() {
    while (auto* client = server_->nextPendingConnection()) {
        connect(client, &QLocalSocket::readyRead, this, [this, client] { readClient(client); });
        connect(client, &QLocalSocket::disconnected, this, [this, client] {
            watchers_.remove(client);
            client->deleteLater();
        });
    }
}

void ControlServer::readClient(QLocalSocket* client) {
    while (client->canReadLine()) {
        const auto request = decodeControlMessage(client->readLine());
        if (!request) {
            reply(client, failure(QStringLiteral("invalid JSON request")));
            continue;
        }
        handle(client, *request);
    }
    if (client->bytesAvailable() > 64 * 1024) {
        reply(client, failure(QStringLiteral("request too large")));
        client->disconnectFromServer();
    }
}

void ControlServer::handle(QLocalSocket* client, const QJsonObject& request) {
    const auto command = request.value(QStringLiteral("cmd")).toString();
    const auto modeText = request.value(QStringLiteral("mode")).toString();
    const auto mode = parseCaptureMode(modeText);
    if (!modeText.isEmpty() && !mode) {
        reply(client, failure(QStringLiteral("mode must be extend or mirror")));
        return;
    }

    QString error;
    if (command == QStringLiteral("ping")) {
        reply(client, {{QStringLiteral("ok"), true}, {QStringLiteral("version"), QStringLiteral("0.1.0")}});
        return;
    }
    if (command == QStringLiteral("status")) {
        // fall through to the state reply
    } else if (command == QStringLiteral("connect")) {
        error = controller_.connectRequest(mode, request.value(QStringLiteral("settings")).toObject());
    } else if (command == QStringLiteral("disconnect")) {
        error = controller_.disconnectRequest();
    } else if (command == QStringLiteral("mode")) {
        if (!mode) {
            reply(client, failure(QStringLiteral("mode is required (extend or mirror)")));
            return;
        }
        error = controller_.setMode(*mode);
    } else if (command == QStringLiteral("toggle-mode")) {
        error = controller_.toggleMode();
    } else if (command == QStringLiteral("settings")) {
        const auto autoconnect = request.value(QStringLiteral("autoconnect"));
        error = controller_.updateSettings(
            request.value(QStringLiteral("settings")).toObject(),
            autoconnect.isBool() ? std::optional<bool>(autoconnect.toBool()) : std::nullopt);
    } else if (command == QStringLiteral("devices")) {
        listDevices(client);
        return;
    } else if (command == QStringLiteral("watch")) {
        watchers_.insert(client);
        QJsonObject message{{QStringLiteral("ok"), true},
                            {QStringLiteral("event"), QStringLiteral("state")},
                            {QStringLiteral("state"), controller_.snapshot()}};
        reply(client, message);
        if (const auto stats = controller_.lastStats(); !stats.isEmpty()) {
            reply(client, {{QStringLiteral("event"), QStringLiteral("stats")},
                           {QStringLiteral("stats"), stats}});
        }
        return;
    } else {
        reply(client, failure(QStringLiteral("unknown command '%1'").arg(command)));
        return;
    }
    if (!error.isEmpty()) {
        reply(client, failure(error));
        return;
    }
    reply(client, {{QStringLiteral("ok"), true}, {QStringLiteral("state"), controller_.snapshot()}});
}

void ControlServer::listDevices(QLocalSocket* client) {
    // Discovery blocks for a few seconds; keep the control loop responsive.
    QPointer<QLocalSocket> target(client);
    std::thread([this, target] {
        QJsonArray devices;
        QStringList problems;
        try {
            for (const auto& endpoint : discoverUsb(std::chrono::seconds(1))) {
                devices.append(endpointJson(endpoint));
            }
        } catch (const std::exception& error) {
            problems << QString::fromUtf8(error.what());
        }
        try {
            for (const auto& endpoint : discoverWifi(std::chrono::seconds(2))) {
                devices.append(endpointJson(endpoint));
            }
        } catch (const std::exception& error) {
            problems << QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(this, [this, target, devices, problems] {
            if (!target) return;
            QJsonObject message{{QStringLiteral("ok"), true},
                                {QStringLiteral("devices"), devices}};
            if (!problems.isEmpty()) {
                message.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(problems));
            }
            reply(target, message);
        }, Qt::QueuedConnection);
    }).detach();
}

void ControlServer::reply(QLocalSocket* client, QJsonObject message) {
    if (client == nullptr || client->state() != QLocalSocket::ConnectedState) return;
    client->write(encodeControlMessage(message));
    client->flush();
}

void ControlServer::broadcastState(const QJsonObject& state) {
    const QJsonObject message{{QStringLiteral("event"), QStringLiteral("state")},
                              {QStringLiteral("state"), state}};
    for (auto* client : std::as_const(watchers_)) reply(client, message);
}

void ControlServer::broadcastStats(const QJsonObject& stats) {
    const QJsonObject message{{QStringLiteral("event"), QStringLiteral("stats")},
                              {QStringLiteral("stats"), stats}};
    for (auto* client : std::as_const(watchers_)) reply(client, message);
}

}  // namespace od::daemon
