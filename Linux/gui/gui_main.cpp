#include "gui_controller.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QVariant>

#include <unistd.h>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    // Single instance: closing the window only hides it to the tray while a
    // session may still be running. A second launch asks the running instance
    // to show its window instead of starting a fresh one that knows nothing
    // about that session.
    const QString instanceName = QStringLiteral("opendisplay-gui-%1").arg(::getuid());
    {
        QLocalSocket existing;
        existing.connectToServer(instanceName);
        if (existing.waitForConnected(500)) {
            existing.write("show\n");
            existing.waitForBytesWritten(500);
            existing.disconnectFromServer();
            return 0;
        }
    }
    QLocalServer::removeServer(instanceName);  // clear a stale socket from a crash
    QLocalServer instanceServer;
    instanceServer.listen(instanceName);
    QCoreApplication::setApplicationName(QStringLiteral("OpenDisplay"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("OpenDisplay"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.opendisplay.desktop"));
    QApplication::setQuitOnLastWindowClosed(false);
    // Bundled app icon; icon themes often lack a "video-display" entry.
    const QIcon applicationIcon(QStringLiteral(":/opendisplay/icon.png"));
    QApplication::setWindowIcon(applicationIcon);

    od::gui::GuiController controller;
    QObject::connect(&instanceServer, &QLocalServer::newConnection, &controller, [&] {
        while (auto* client = instanceServer.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::disconnected,
                             client, &QObject::deleteLater);
        }
        emit controller.showWindowRequested();
    });
    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
    });
    engine.loadFromModule(QStringLiteral("org.opendisplay.desktop"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;
    return application.exec();
}
