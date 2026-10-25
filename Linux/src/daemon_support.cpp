#include "opendisplay/daemon_support.hpp"

#include "opendisplay/settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

namespace od {
namespace {

QString envOr(const char* name, const QString& fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? QString::fromLocal8Bit(value) : fallback;
}

std::string sanitizedPrefix(std::string_view value, const std::size_t length) {
    std::string result;
    for (const char character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0) continue;
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        if (result.size() == length) break;
    }
    return result;
}

std::uint32_t fnv1a(std::string_view value) {
    std::uint32_t hash = 2166136261U;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 16777619U;
    }
    return hash;
}

}  // namespace

std::chrono::milliseconds Backoff::next() {
    auto delay = initial_;
    for (int index = 0; index < attempts_ && delay < cap_; ++index) delay *= 2;
    attempts_ += 1;
    return std::min(delay, cap_);
}

std::string virtualOutputNameFor(const Endpoint& endpoint) {
    if (endpoint.kind == TransportKind::Usb && !endpoint.udid.empty()) {
        const auto id = sanitizedPrefix(endpoint.udid, 8);
        if (!id.empty()) return "OpenDisplay-u" + id;
    }
    const auto& key = !endpoint.name.empty() ? endpoint.name : endpoint.host;
    char hex[9]{};
    std::snprintf(hex, sizeof(hex), "%08x", fnv1a(key));
    return std::string("OpenDisplay-w") + hex;
}

bool isStaleVirtualOutput(std::string_view name, const std::function<bool(int)>& pidAlive) {
    constexpr std::string_view prefix = "OpenDisplay-";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) return false;
    const auto suffix = name.substr(prefix.size());
    if (std::all_of(suffix.begin(), suffix.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        if (suffix.size() > 9) return false;
        return !pidAlive(std::stoi(std::string(suffix)));
    }
    return (suffix.front() == 'u' || suffix.front() == 'w') && suffix.size() > 1;
}

QString restoreTokenKey(const CaptureMode mode, const std::string& outputName) {
    return captureModeName(mode) + ':' + QString::fromStdString(outputName);
}

QString controlSocketPath() {
    return envOr("XDG_RUNTIME_DIR", QStringLiteral("/tmp")) + QStringLiteral("/opendisplay/control.sock");
}

QString stateFilePath() {
    return envOr("XDG_STATE_HOME", QDir::homePath() + QStringLiteral("/.local/state"))
        + QStringLiteral("/opendisplay/state.json");
}

QJsonObject persistedStateToJson(const PersistedState& state) {
    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("settings"), QJsonObject::fromVariantMap(state.settings)},
        {QStringLiteral("mode"), captureModeName(state.mode)},
        {QStringLiteral("autoconnect"), state.autoconnect},
        {QStringLiteral("resume"), state.resume},
        {QStringLiteral("restoreTokens"), QJsonObject::fromVariantMap(state.restoreTokens)},
    };
}

PersistedState persistedStateFromJson(const QJsonObject& json) {
    PersistedState state;
    state.settings = defaultSettings();
    const auto stored = json.value(QStringLiteral("settings")).toObject().toVariantMap();
    for (auto it = stored.cbegin(); it != stored.cend(); ++it) state.settings.insert(it.key(), it.value());
    // The mode lives at the top level; older/GUI settings maps may carry one.
    state.settings.remove(QStringLiteral("mode"));
    state.mode = parseCaptureMode(json.value(QStringLiteral("mode")).toString())
                     .value_or(CaptureMode::Extend);
    state.autoconnect = json.value(QStringLiteral("autoconnect")).toBool(false);
    state.resume = json.value(QStringLiteral("resume")).toBool(false);
    state.restoreTokens = json.value(QStringLiteral("restoreTokens")).toObject().toVariantMap();
    return state;
}

PersistedState loadPersistedState(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return persistedStateFromJson({});
    const auto document = QJsonDocument::fromJson(file.readAll());
    return persistedStateFromJson(document.isObject() ? document.object() : QJsonObject{});
}

bool savePersistedState(const QString& path, const PersistedState& state) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    // Restore tokens grant screen capture without a prompt; keep them private.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(persistedStateToJson(state)).toJson(QJsonDocument::Indented));
    return file.commit();
}

QByteArray encodeControlMessage(const QJsonObject& message) {
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<QJsonObject> decodeControlMessage(QByteArray line) {
    line = line.trimmed();
    if (line.isEmpty()) return std::nullopt;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return document.object();
}

}  // namespace od
