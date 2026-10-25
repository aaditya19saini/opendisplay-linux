#pragma once

#include "opendisplay/session.hpp"
#include "opendisplay/types.hpp"

#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class QTimer;

namespace od::daemon {

/// Owns one od::Session on a worker thread. Session start-up and mode
/// switches block inside nested portal event loops (the share dialog can stay
/// open for minutes), so requests that arrive meanwhile are deferred until
/// the blocking step returns instead of re-entering the session.
class SessionRunner final : public QObject {
    Q_OBJECT

public:
    explicit SessionRunner(QObject* parent = nullptr);
    ~SessionRunner() override;

public slots:
    void start(od::Options options, QVariantMap restoreTokens);
    void stop();
    void switchMode(int mode);
    void setWorkspace(const QString& workspace);

signals:
    void connecting(const QString& detail);
    /// device, transport, output, encoder, mode
    void streaming(const QJsonObject& info);
    void modeChanged(int mode);
    /// failure: an error (as opposed to a requested stop or receiver close).
    /// cancelled: the user dismissed the share dialog.
    void ended(const QString& reason, bool failure, bool cancelled);
    void restoreTokenIssued(const QString& key, const QString& token);
    void statsUpdated(const QJsonObject& stats);

private slots:
    void tick();

private:
    void finish(const QString& reason, bool failure, bool cancelled);
    void publishRestoreToken();
    void runDeferred();
    QJsonObject streamingInfo() const;

    QTimer* timer_ = nullptr;
    std::unique_ptr<od::Session> session_;
    od::Options options_;
    od::Endpoint endpoint_;
    QVariantMap restoreTokens_;
    std::string publishedToken_;
    std::uint64_t statsSerial_ = 0;
    bool busy_ = false;
    bool stopRequested_ = false;
    std::optional<od::CaptureMode> pendingMode_;
};

}  // namespace od::daemon
