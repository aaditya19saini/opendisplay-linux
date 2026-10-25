#include "control_server.hpp"
#include "controller.hpp"

#include "opendisplay/daemon_support.hpp"
#include "opendisplay/hyprland_output_controller.hpp"
#include "opendisplay/log.hpp"

#include <QGuiApplication>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>

#include <sys/socket.h>
#include <unistd.h>

namespace {

int signalPipe[2] = {-1, -1};

void onSignal(int) {
    const char byte = 1;
    [[maybe_unused]] const auto ignored = ::write(signalPipe[1], &byte, 1);
}

bool pidAlive(const int pid) {
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

/// Removes virtual outputs left behind by a crashed daemon or CLI run; they
/// otherwise break reference-monitor detection ("more than one monitor").
void removeStaleOutputs() {
    if (std::getenv("HYPRLAND_INSTANCE_SIGNATURE") == nullptr) return;
    try {
        od::HyprlandOutputController outputs;
        for (const auto& output : outputs.outputs()) {
            if (od::isStaleVirtualOutput(output.name, pidAlive)) {
                od::log("Removing stale virtual monitor " + output.name);
                outputs.remove(output.name);
            }
        }
    } catch (const std::exception& error) {
        od::log(std::string("Stale-output cleanup skipped: ") + error.what());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication::setDesktopSettingsAware(false);
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("opendisplayd"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    std::signal(SIGPIPE, SIG_IGN);
    od::verboseLogging = application.arguments().contains(QStringLiteral("--verbose"));

    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signalPipe) != 0) {
        std::cerr << "opendisplayd: cannot create signal pipe\n";
        return 1;
    }
    QSocketNotifier signalNotifier(signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &application, [&] {
        char byte = 0;
        [[maybe_unused]] const auto ignored = ::read(signalPipe[0], &byte, 1);
        od::log("Stopping");
        application.quit();
    });
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    od::daemon::Controller controller;
    od::daemon::ControlServer server(controller);
    QString error;
    if (!server.listen(&error)) {
        std::cerr << "opendisplayd: " << error.toStdString() << '\n';
        return error.contains(QStringLiteral("already running")) ? 0 : 1;
    }
    removeStaleOutputs();
    od::log("opendisplayd ready");

    QObject::connect(&application, &QCoreApplication::aboutToQuit, &controller,
                     [&] { controller.shutdown(); });
    return application.exec();
}
