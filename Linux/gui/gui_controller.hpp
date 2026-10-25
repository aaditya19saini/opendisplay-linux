#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QVariantMap>

class QAction;
class QLocalSocket;
class QMenu;
class QSystemTrayIcon;
class QTimer;

namespace od::gui {

/// Thin client of opendisplayd: the daemon owns the session, so closing or
/// restarting the GUI never interrupts streaming. State arrives over a
/// persistent "watch" connection on the daemon's control socket.
class GuiController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(QString statsText READ statsText NOTIFY statsChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)
    Q_PROPERTY(bool quitting READ quitting NOTIFY quittingChanged)
    Q_PROPERTY(QVariantMap savedSettings READ savedSettings CONSTANT)

public:
    explicit GuiController(QObject* parent = nullptr);
    ~GuiController() override;

    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString detail() const { return detail_; }
    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] QString mode() const { return mode_; }
    [[nodiscard]] QString statsText() const { return statsText_; }
    [[nodiscard]] bool trayAvailable() const { return trayAvailable_; }
    [[nodiscard]] bool quitting() const { return quitting_; }
    [[nodiscard]] QVariantMap savedSettings() const { return savedSettings_; }

    Q_INVOKABLE void connectDevice(const QVariantMap& values);
    Q_INVOKABLE void connectLast();
    Q_INVOKABLE void disconnectDevice();
    Q_INVOKABLE void setMode(const QString& mode);
    Q_INVOKABLE void quit();

signals:
    void stateChanged();
    void statsChanged();
    void trayAvailableChanged();
    void quittingChanged();
    void showWindowRequested();

private slots:
    void updateTrayAvailability();
    void openWatch();
    void readWatch();
    void watchLost();

private:
    void createTray();
    void send(const QJsonObject& request);
    void applyState(const QJsonObject& state);
    void applyStats(const QJsonObject& stats);
    void setLocalError(const QString& message);
    void loadInitialSettings();

    QSettings settings_;
    QVariantMap savedSettings_;
    QLocalSocket* watch_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QByteArray buffer_;
    bool daemonStartRequested_ = false;
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* connectAction_ = nullptr;
    QAction* disconnectAction_ = nullptr;
    QAction* modeAction_ = nullptr;
    QTimer* trayPoll_ = nullptr;
    QString status_ = QStringLiteral("Starting");
    QString detail_ = QStringLiteral("Contacting the OpenDisplay service…");
    QString mode_ = QStringLiteral("extend");
    QString statsText_;
    bool connected_ = false;
    bool busy_ = false;
    bool trayAvailable_ = false;
    bool quitting_ = false;
};

}  // namespace od::gui
