#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>

class QLocalServer;
class QLocalSocket;

namespace od::daemon {

class Controller;

/// JSON-lines control socket at $XDG_RUNTIME_DIR/opendisplay/control.sock.
/// Requests: {"cmd": "status" | "connect" | "disconnect" | "mode" |
/// "toggle-mode" | "settings" | "devices" | "watch" | "ping", ...}.
/// Replies: {"ok": true, ...} or {"ok": false, "error": "..."}. A "watch"
/// client then receives {"event": "state" | "stats", ...} lines.
class ControlServer final : public QObject {
    Q_OBJECT

public:
    ControlServer(Controller& controller, QObject* parent = nullptr);

    /// Binds the socket. Returns false if another daemon is already serving it.
    bool listen(QString* error);

private slots:
    void accept();
    void broadcastState(const QJsonObject& state);
    void broadcastStats(const QJsonObject& stats);

private:
    void readClient(QLocalSocket* client);
    void handle(QLocalSocket* client, const QJsonObject& request);
    void reply(QLocalSocket* client, QJsonObject message);
    void listDevices(QLocalSocket* client);

    Controller& controller_;
    QLocalServer* server_ = nullptr;
    QSet<QLocalSocket*> watchers_;
};

}  // namespace od::daemon
