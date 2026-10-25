#pragma once

#include "opendisplay/daemon_support.hpp"
#include "opendisplay/types.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QThread>

#include <optional>

class QTimer;

namespace od::daemon {

class SessionRunner;

/// Daemon brain: holds the desired state (should a session run, in which
/// mode, with which settings), drives the SessionRunner toward it, retries
/// with backoff when the receiver drops, and persists settings and portal
/// restore tokens. Lives on the main thread; all methods are main-thread only.
class Controller final : public QObject {
    Q_OBJECT

public:
    enum class Status { Idle, Connecting, Streaming, Reconnecting };

    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;

    [[nodiscard]] QJsonObject snapshot() const;
    [[nodiscard]] QJsonObject lastStats() const { return stats_; }

    /// Each returns an error message, or an empty string on success.
    QString connectRequest(std::optional<CaptureMode> mode, const QJsonObject& settings);
    QString disconnectRequest();
    QString setMode(CaptureMode mode);
    QString toggleMode();
    QString updateSettings(const QJsonObject& settings, std::optional<bool> autoconnect);

    /// Stops any session and waits briefly for the worker to finish.
    void shutdown();

signals:
    void stateChanged(const QJsonObject& state);
    void statsChanged(const QJsonObject& stats);

private slots:
    void onConnecting(const QString& detail);
    void onStreaming(const QJsonObject& info);
    void onModeChanged(int mode);
    void onEnded(const QString& reason, bool failure, bool cancelled);
    void onRestoreToken(const QString& key, const QString& token);
    void onStats(const QJsonObject& stats);
    void attempt();

private:
    QString mergeSettings(const QJsonObject& settings);
    void save();
    void setStatus(Status status, const QString& detail);
    void publish();

    QThread workerThread_;
    SessionRunner* runner_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    QString statePath_;
    PersistedState persisted_;
    Backoff backoff_;
    Status status_ = Status::Idle;
    bool desiredActive_ = false;
    QString detail_ = QStringLiteral("Ready");
    QJsonObject info_;
    QJsonObject stats_;
    QDateTime retryAt_;
};

}  // namespace od::daemon
