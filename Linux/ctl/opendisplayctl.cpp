// opendisplayctl: command-line client for opendisplayd's control socket.

#include "opendisplay/daemon_support.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr auto usage = R"(Usage: opendisplayctl <command> [options]

Commands:
  status [--json]        Show the current state
  extend                 Connect (or switch) to extend mode
  mirror                 Connect (or switch) to mirror mode
  toggle-mode            Switch between extend and mirror
  connect                Connect using the saved mode and settings
  disconnect             Stop streaming
  devices                List visible receivers
  watch [--json]         Follow state and stats changes
  set KEY=VALUE...       Change saved settings (e.g. encoder=nvenc fps=60)
  autoconnect on|off     Reconnect automatically when the daemon starts

The daemon (opendisplayd) is started through systemd when it is not running.
)";

int connectSocket() {
    const auto path = od::controlSocketPath().toStdString();
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connectOrStartDaemon() {
    if (const int fd = connectSocket(); fd >= 0) return fd;
    if (std::system("systemctl --user start opendisplayd.service >/dev/null 2>&1") != 0) {
        return -1;
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (const int fd = connectSocket(); fd >= 0) return fd;
    }
    return -1;
}

bool writeAll(const int fd, const QByteArray& bytes) {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.constData() + offset,
                                   static_cast<std::size_t>(bytes.size() - offset));
        if (count <= 0) return false;
        offset += count;
    }
    return true;
}

/// Reads one line; nullopt on EOF or timeout (negative timeout waits forever).
std::optional<QJsonObject> readMessage(const int fd, QByteArray& buffer, const int timeoutMs) {
    for (;;) {
        if (const auto newline = buffer.indexOf('\n'); newline >= 0) {
            const auto line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            if (auto message = od::decodeControlMessage(line)) return message;
            continue;
        }
        pollfd descriptor{fd, POLLIN, 0};
        if (::poll(&descriptor, 1, timeoutMs) <= 0) return std::nullopt;
        char chunk[4096];
        const auto count = ::read(fd, chunk, sizeof(chunk));
        if (count <= 0) return std::nullopt;
        buffer.append(chunk, count);
    }
}

std::string latencyText(const QJsonObject& stats) {
    const auto sent = stats.value(QStringLiteral("sent")).toObject();
    char text[160];
    std::snprintf(text, sizeof(text), "%.0f fps, capture→sent p50 %lld ms / p95 %lld ms",
                  stats.value(QStringLiteral("fps")).toDouble(),
                  static_cast<long long>(sent.value(QStringLiteral("p50")).toInteger()),
                  static_cast<long long>(sent.value(QStringLiteral("p95")).toInteger()));
    return text;
}

void printState(const QJsonObject& state) {
    const auto status = state.value(QStringLiteral("status")).toString().toStdString();
    const auto mode = state.value(QStringLiteral("mode")).toString().toStdString();
    std::cout << status << " (" << mode << ")";
    const auto device = state.value(QStringLiteral("device")).toString();
    if (!device.isEmpty()) {
        std::cout << " — " << device.toStdString() << " over "
                  << state.value(QStringLiteral("transport")).toString().toStdString();
    }
    std::cout << '\n';
    const auto detail = state.value(QStringLiteral("detail")).toString();
    if (!detail.isEmpty()) std::cout << "  " << detail.toStdString() << '\n';
    const auto encoder = state.value(QStringLiteral("encoder")).toString();
    if (!encoder.isEmpty()) {
        std::cout << "  encoder " << encoder.toStdString() << ", output "
                  << state.value(QStringLiteral("output")).toString().toStdString() << '\n';
    }
    if (state.contains(QStringLiteral("retryInMs"))) {
        std::cout << "  retrying in "
                  << state.value(QStringLiteral("retryInMs")).toInteger() / 1000 << " s\n";
    }
}

QJsonValue settingValue(const QString& text) {
    if (text == QStringLiteral("true")) return true;
    if (text == QStringLiteral("false")) return false;
    bool isNumber = false;
    const double number = text.toDouble(&isNumber);
    if (isNumber) return number;
    return text;
}

}  // namespace

int main(int argc, char* argv[]) {
    QStringList args;
    for (int index = 1; index < argc; ++index) args << QString::fromLocal8Bit(argv[index]);
    const bool json = args.removeAll(QStringLiteral("--json")) > 0;
    if (args.isEmpty() || args.front() == QStringLiteral("-h")
        || args.front() == QStringLiteral("--help") || args.front() == QStringLiteral("help")) {
        std::cout << usage;
        return args.isEmpty() ? 1 : 0;
    }
    const auto command = args.takeFirst();

    QJsonObject request;
    if (command == QStringLiteral("status") || command == QStringLiteral("watch")
        || command == QStringLiteral("disconnect") || command == QStringLiteral("toggle-mode")
        || command == QStringLiteral("devices") || command == QStringLiteral("connect")) {
        request.insert(QStringLiteral("cmd"), command);
    } else if (command == QStringLiteral("extend") || command == QStringLiteral("mirror")) {
        request = {{QStringLiteral("cmd"), QStringLiteral("connect")},
                   {QStringLiteral("mode"), command}};
    } else if (command == QStringLiteral("set")) {
        QJsonObject settings;
        for (const auto& pair : args) {
            const auto separator = pair.indexOf('=');
            if (separator <= 0) {
                std::cerr << "opendisplayctl: expected KEY=VALUE, got '" << pair.toStdString()
                          << "'\n";
                return 2;
            }
            settings.insert(pair.left(separator), settingValue(pair.mid(separator + 1)));
        }
        request = {{QStringLiteral("cmd"), QStringLiteral("settings")},
                   {QStringLiteral("settings"), settings}};
    } else if (command == QStringLiteral("autoconnect") && args.size() == 1
               && (args.front() == QStringLiteral("on") || args.front() == QStringLiteral("off"))) {
        request = {{QStringLiteral("cmd"), QStringLiteral("settings")},
                   {QStringLiteral("autoconnect"), args.front() == QStringLiteral("on")}};
    } else {
        std::cerr << usage;
        return 2;
    }

    const int fd = connectOrStartDaemon();
    if (fd < 0) {
        std::cerr << "opendisplayctl: cannot reach opendisplayd at "
                  << od::controlSocketPath().toStdString()
                  << ". Start it with `systemctl --user start opendisplayd`.\n";
        return 3;
    }
    if (!writeAll(fd, od::encodeControlMessage(request))) {
        std::cerr << "opendisplayctl: lost connection to opendisplayd\n";
        return 3;
    }

    QByteArray buffer;
    const int timeout = command == QStringLiteral("devices") ? 15'000 : 5'000;
    const auto reply = readMessage(fd, buffer, timeout);
    if (!reply) {
        std::cerr << "opendisplayctl: no reply from opendisplayd\n";
        return 3;
    }
    if (!reply->value(QStringLiteral("ok")).toBool()) {
        if (json) {
            std::cout << QJsonDocument(*reply).toJson(QJsonDocument::Compact).toStdString() << '\n';
        } else {
            std::cerr << "opendisplayctl: "
                      << reply->value(QStringLiteral("error")).toString().toStdString() << '\n';
        }
        return 1;
    }

    if (command == QStringLiteral("watch")) {
        auto message = reply;
        do {
            if (json) {
                std::cout << QJsonDocument(*message).toJson(QJsonDocument::Compact).toStdString()
                          << std::endl;
            } else if (message->value(QStringLiteral("event")).toString()
                       == QStringLiteral("stats")) {
                std::cout << "  " << latencyText(message->value(QStringLiteral("stats")).toObject())
                          << std::endl;
            } else {
                printState(message->value(QStringLiteral("state")).toObject());
                std::cout.flush();
            }
        } while ((message = readMessage(fd, buffer, -1)));
        return 0;
    }

    if (json) {
        std::cout << QJsonDocument(*reply).toJson(QJsonDocument::Compact).toStdString() << '\n';
    } else if (command == QStringLiteral("devices")) {
        const auto devices = reply->value(QStringLiteral("devices")).toArray();
        if (devices.isEmpty()) std::cout << "No receivers found\n";
        for (const auto& value : devices) {
            const auto device = value.toObject();
            std::cout << device.value(QStringLiteral("transport")).toString().toStdString()
                      << "  " << device.value(QStringLiteral("id")).toString().toStdString()
                      << "  " << device.value(QStringLiteral("name")).toString().toStdString()
                      << '\n';
        }
    } else {
        printState(reply->value(QStringLiteral("state")).toObject());
    }
    ::close(fd);
    return 0;
}
