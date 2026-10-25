#include "opendisplay/daemon_support.hpp"
#include "opendisplay/settings.hpp"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cassert>
#include <chrono>
#include <set>

using namespace std::chrono_literals;

namespace {

void backoffDoublesAndCaps() {
    od::Backoff backoff(1s, 10s);
    assert(backoff.next() == 1s);
    assert(backoff.next() == 2s);
    assert(backoff.next() == 4s);
    assert(backoff.next() == 8s);
    assert(backoff.next() == 10s);
    assert(backoff.next() == 10s);
    assert(backoff.attempts() == 6);
    backoff.reset();
    assert(backoff.next() == 1s);
}

void namesOutputsStably() {
    od::Endpoint usb{.kind = od::TransportKind::Usb, .name = "iPad", .host = {}, .port = 9000,
                     .udid = "03BAB47093c1d0023035b444260c4e80cd6d9f9f", .usbHandle = 1};
    assert(od::virtualOutputNameFor(usb) == "OpenDisplay-u03bab470");
    usb.usbHandle = 7;  // handles change on replug; the name must not
    assert(od::virtualOutputNameFor(usb) == "OpenDisplay-u03bab470");

    const od::Endpoint wifi{.kind = od::TransportKind::Wifi, .name = "Aaditya's iPad",
                            .host = "192.168.1.5", .port = 9000, .udid = {}, .usbHandle = -1};
    const auto wifiName = od::virtualOutputNameFor(wifi);
    assert(wifiName.starts_with("OpenDisplay-w") && wifiName.size() == 21);
    assert(wifiName == od::virtualOutputNameFor(wifi));
    for (const char character : wifiName) {
        assert(std::isalnum(static_cast<unsigned char>(character)) || character == '-');
    }
}

void detectsStaleOutputs() {
    const std::set<int> alive{1234};
    const auto pidAlive = [&](int pid) { return alive.contains(pid); };
    assert(od::isStaleVirtualOutput("OpenDisplay-u03bab470", pidAlive));
    assert(od::isStaleVirtualOutput("OpenDisplay-w1a2b3c4d", pidAlive));
    assert(od::isStaleVirtualOutput("OpenDisplay-999", pidAlive));
    assert(!od::isStaleVirtualOutput("OpenDisplay-1234", pidAlive));
    assert(!od::isStaleVirtualOutput("eDP-1", pidAlive));
    assert(!od::isStaleVirtualOutput("OpenDisplay-", pidAlive));
    assert(!od::isStaleVirtualOutput("OpenDisplay-x", pidAlive));
    assert(!od::isStaleVirtualOutput("OpenDisplay-u", pidAlive));
}

void roundTripsState() {
    od::PersistedState state;
    state.settings = od::defaultSettings();
    state.settings.insert(QStringLiteral("encoder"), QStringLiteral("nvenc"));
    state.mode = od::CaptureMode::Mirror;
    state.autoconnect = true;
    state.resume = true;
    state.restoreTokens.insert(od::restoreTokenKey(od::CaptureMode::Extend, "OpenDisplay-uab"),
                               QStringLiteral("token-1"));

    QTemporaryDir directory;
    assert(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("nested/state.json"));
    assert(od::savePersistedState(path, state));
    assert((QFile(path).permissions() & QFileDevice::ReadOther) == 0);

    const auto loaded = od::loadPersistedState(path);
    assert(loaded.mode == od::CaptureMode::Mirror);
    assert(loaded.autoconnect);
    assert(loaded.resume);
    assert(loaded.settings.value(QStringLiteral("encoder")).toString() == QStringLiteral("nvenc"));
    assert(loaded.restoreTokens.value(QStringLiteral("extend:OpenDisplay-uab")).toString()
           == QStringLiteral("token-1"));

    // Missing file and missing keys fall back to defaults.
    const auto empty = od::loadPersistedState(directory.filePath(QStringLiteral("absent.json")));
    assert(empty.mode == od::CaptureMode::Extend && !empty.autoconnect);
    assert(empty.settings.value(QStringLiteral("fps")).toInt() == 60);
    assert(!empty.settings.contains(QStringLiteral("mode")));
    const auto legacy = od::persistedStateFromJson(QJsonObject{
        {QStringLiteral("settings"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("mirror")}}}});
    assert(!legacy.settings.contains(QStringLiteral("mode")));
    od::optionsFromSettings(empty.settings);  // defaults must validate
}

void framesControlMessages() {
    const QJsonObject message{{QStringLiteral("cmd"), QStringLiteral("status")}};
    const auto encoded = od::encodeControlMessage(message);
    assert(encoded.endsWith('\n') && encoded.count('\n') == 1);
    const auto decoded = od::decodeControlMessage(encoded);
    assert(decoded && decoded->value(QStringLiteral("cmd")).toString() == QStringLiteral("status"));
    assert(!od::decodeControlMessage("   \n"));
    assert(!od::decodeControlMessage("[1,2]"));
    assert(!od::decodeControlMessage("{broken"));
}

void parsesModes() {
    assert(od::parseCaptureMode(QStringLiteral("extend")) == od::CaptureMode::Extend);
    assert(od::parseCaptureMode(QStringLiteral("mirror")) == od::CaptureMode::Mirror);
    assert(!od::parseCaptureMode(QStringLiteral("Mirror")));
    assert(od::captureModeName(od::CaptureMode::Mirror) == QStringLiteral("mirror"));
    assert(od::restoreTokenKey(od::CaptureMode::Mirror, "OpenDisplay-u1")
           == QStringLiteral("mirror:OpenDisplay-u1"));
}

}  // namespace

int main() {
    backoffDoublesAndCaps();
    namesOutputsStably();
    detectsStaleOutputs();
    roundTripsState();
    framesControlMessages();
    parsesModes();
    return 0;
}
