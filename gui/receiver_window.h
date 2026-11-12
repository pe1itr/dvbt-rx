#pragma once

#include "plots.h"
#include "video.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>

class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class ReceiverWindow : public QMainWindow {
public:
    explicit ReceiverWindow(QWidget *parent = nullptr);
    ~ReceiverWindow() override;
private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void configure();
    void start();
    void stop();
    void fail(const QString &message);
    void log(const QString &message);
    void setRunning(bool running);
    void readStatus();
    void readVisuals();
    void clearTelemetry();
    void openPlayer();
    void beginRetune();
    void finishRetune();
    QString executable(const QString &name) const;
    void watchProcess(QProcess *process, const QString &name);
    bool eventFilter(QObject *watched, QEvent *event) override;
    void scheduleRetune();

    QSettings settings_;
    QTemporaryDir runtime_;
    QProcess *rtl_ = nullptr;
    QProcess *decoder_ = nullptr;
    QProcess *externalPlayer_ = nullptr;
    QUdpSocket visuals_;
    ReceiverVideo video_;
    QTimer statusTimer_;
    QTimer retuneTimer_;
    QTimer retunePollTimer_;
    QTimer retuneKillTimer_;
    QElapsedTimer statusAge_, spectrumAge_, constellationAge_;
    QByteArray lastStatus_;
    QString rtlPath_, device_, playerPath_;
    int sampleRate_ = 1010526;
    int ppm_ = 0;
    int outputPort_ = 10000;
    quint16 videoPort_ = 0;
    bool embeddedVideo_ = true;
    bool running_ = false;
    bool stopping_ = false;
    bool starting_ = false;
    bool retuning_ = false;
    bool videoRunning_ = false;

    QWidget *controls_ = nullptr;
    QDoubleSpinBox *frequency_ = nullptr;
    QDoubleSpinBox *gain_ = nullptr;
    QComboBox *bandwidth_ = nullptr;
    QComboBox *fec_ = nullptr;
    QComboBox *guard_ = nullptr;
    QPushButton *power_ = nullptr;
    QPushButton *config_ = nullptr;
    QPushButton *playerButton_ = nullptr;
    QLabel *state_ = nullptr;
    QLabel *metrics_ = nullptr;
    QLabel *service_ = nullptr;
    QLabel *videoMessage_ = nullptr;
    QWidget *videoSurface_ = nullptr;
    QPlainTextEdit *logs_ = nullptr;
    ReceiverPlot *spectrum_ = nullptr;
    ReceiverPlot *waterfall_ = nullptr;
    ReceiverPlot *constellation_ = nullptr;
};
