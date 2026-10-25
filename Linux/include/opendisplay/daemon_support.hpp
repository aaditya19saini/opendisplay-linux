#pragma once

#include "opendisplay/types.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace od {

/// Exponential reconnect delay: 1 s, 2 s, 4 s ... capped. Reset after a
/// successful connection.
class Backoff {
public:
    explicit Backoff(std::chrono::milliseconds initial = std::chrono::seconds(1),
                     std::chrono::milliseconds cap = std::chrono::seconds(30))
        : initial_(initial), cap_(cap) {}

    std::chrono::milliseconds next();
    void reset() { attempts_ = 0; }
    [[nodiscard]] int attempts() const { return attempts_; }

private:
    std::chrono::milliseconds initial_;
    std::chrono::milliseconds cap_;
    int attempts_ = 0;
};

/// Stable, compositor-safe virtual output name for a receiver, so portal
/// restore tokens (which XDPH matches by output name) survive reconnects:
/// `OpenDisplay-u<8 of UDID>` for USB, `OpenDisplay-w<8 hex of hash>` for Wi-Fi.
std::string virtualOutputNameFor(const Endpoint& endpoint);

/// True for OpenDisplay virtual outputs no live process owns: daemon-style
/// names (`OpenDisplay-u…`/`-w…`) always, PID-style names (`OpenDisplay-<pid>`)
/// when that PID is gone. Other outputs are never stale.
bool isStaleVirtualOutput(std::string_view name, const std::function<bool(int)>& pidAlive);

/// Portal restore tokens are per captured output and mode.
QString restoreTokenKey(CaptureMode mode, const std::string& outputName);

QString controlSocketPath();
QString stateFilePath();

/// Daemon state that survives restarts.
struct PersistedState {
    QVariantMap settings;
    CaptureMode mode = CaptureMode::Extend;
    bool autoconnect = false;
    /// A session was wanted when the daemon last exited without a clean stop
    /// (crash, kill -9); the restarted daemon resumes it.
    bool resume = false;
    QVariantMap restoreTokens;
};

QJsonObject persistedStateToJson(const PersistedState& state);
PersistedState persistedStateFromJson(const QJsonObject& json);
PersistedState loadPersistedState(const QString& path);
bool savePersistedState(const QString& path, const PersistedState& state);

/// Control protocol framing: one compact JSON object per line.
QByteArray encodeControlMessage(const QJsonObject& message);
std::optional<QJsonObject> decodeControlMessage(QByteArray line);

}  // namespace od
