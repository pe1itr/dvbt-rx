/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Rob Hardenberg / PE1ITR */
#include "receiver_window.h"
#include <QApplication>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextDocument>
#include <QVBoxLayout>

static QComboBox *choices(const QStringList &items, const char *name)
{
    auto *box = new QComboBox;
    box->setObjectName(name);
    box->addItems(items);
    return box;
}

ReceiverWindow::ReceiverWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();
    loadSettings();
    clearTelemetry();
    setRunning(false);
    state_->setText("Ontvanger uit");
    connect(&visuals_, &QUdpSocket::readyRead, this, [this]() { readVisuals(); });
    retuneTimer_.setSingleShot(true);
    retuneTimer_.setInterval(350);
    connect(&retuneTimer_, &QTimer::timeout, this, [this]() {
        if (!running_)
            return;
        beginRetune();
    });
    retunePollTimer_.setInterval(40);
    connect(&retunePollTimer_, &QTimer::timeout, this, [this]() { finishRetune(); });
    retuneKillTimer_.setSingleShot(true);
    retuneKillTimer_.setInterval(1200);
    connect(&retuneKillTimer_, &QTimer::timeout, this, [this]() {
        for (QProcess *process : {rtl_, decoder_})
            if (process && process->state() != QProcess::NotRunning)
                process->kill();
    });
    qApp->installEventFilter(this);
    statusTimer_.setInterval(250);
    connect(&statusTimer_, &QTimer::timeout, this, [this]() { readStatus(); });
    statusTimer_.start();
}

ReceiverWindow::~ReceiverWindow()
{
    qApp->removeEventFilter(this);
    retuneTimer_.stop();
    stop();
    saveSettings();
}

void ReceiverWindow::buildUi()
{
    setWindowTitle("DVB-T · RTL-SDR — PE1ITR");
    resize(1220, 820);
    setStyleSheet(
        "QMainWindow,QDialog,QWidget { background:#10161b; color:#edf2f6; }"
        "QGroupBox { border:1px solid #34414b; border-radius:5px; margin-top:10px; padding-top:12px; }"
        "QGroupBox::title { subcontrol-origin:margin; left:12px; color:#9aa9b5; }"
        "QPushButton,QComboBox,QDoubleSpinBox,QSpinBox,QLineEdit { background:#1c2730; border:1px solid #455660; border-radius:4px; padding:6px; }"
        "QPushButton:hover { border-color:#43dde0; }"
        "QPushButton:disabled,QWidget:disabled { color:#798894; }"
        "QPlainTextEdit { background:#080b0e; border:1px solid #34414b; }"
        "QLabel#state { font-size:17px; font-weight:bold; color:#43dde0; }"
        "QDoubleSpinBox#frequency { font-family:monospace; font-size:23px; color:#43dde0; }"
        "QSplitter::handle { background:#34414b; }");

    auto *root = new QWidget;
    auto *layout = new QVBoxLayout(root);
    auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel("DVB-T / RTL-SDR");
    title->setStyleSheet("font-size:17px; font-weight:bold;");
    toolbar->addWidget(title);
    controls_ = new QWidget;
    auto *controls = new QHBoxLayout(controls_);
    controls->setContentsMargins(12, 0, 8, 0);
    frequency_ = new QDoubleSpinBox;
    frequency_->setObjectName("frequency");
    frequency_->setRange(24.0, 1766.0);
    frequency_->setDecimals(6);
    frequency_->setSingleStep(0.001);
    frequency_->setSuffix(" MHz");
    frequency_->setMinimumWidth(190);
    frequency_->setToolTip("← / →: afstemmen in stappen van 1 kHz; ontvangst heracquiret.");
    gain_ = new QDoubleSpinBox;
    gain_->setObjectName("gain");
    gain_->setRange(0, 49.6);
    gain_->setSingleStep(0.1);
    gain_->setDecimals(1);
    gain_->setSpecialValueText("Auto");
    gain_->setSuffix(" dB");
    gain_->setToolTip("↑ / ↓: SDR-gain 1 dB hoger of lager; ontvangst heracquiret.");
    bandwidth_ = choices({"35k", "40k", "125k", "150k", "250k", "333k", "500k"}, "bandwidth");
    bandwidth_->setToolTip("Nominale DVB-T-kanaalbandbreedte (decoderoptie --sr). Alle getoonde modes worden ondersteund.");
    fec_ = choices({"1/2", "2/3", "3/4", "5/6", "7/8", "auto"}, "fec");
    guard_ = choices({"1/32", "1/16", "1/8", "auto"}, "guard");
    const QList<QPair<QString, QWidget *>> fields = {
        {"Frequentie", frequency_}, {"Gain", gain_}, {"Bandbreedte", bandwidth_},
        {"FEC", fec_}, {"Guard", guard_}
    };
    for (const auto &field : fields) {
        auto *column = new QVBoxLayout;
        column->setSpacing(2);
        column->addWidget(new QLabel(field.first));
        column->addWidget(field.second);
        controls->addLayout(column);
    }
    toolbar->addWidget(controls_);
    toolbar->addStretch();
    config_ = new QPushButton("Instellingen");
    config_->setObjectName("settings");
    toolbar->addWidget(config_);
    playerButton_ = new QPushButton("Externe speler");
    toolbar->addWidget(playerButton_);
    auto *about = new QPushButton("Over");
    about->setObjectName("about");
    toolbar->addWidget(about);
    connect(about, &QPushButton::clicked, this, [this]() {
        QMessageBox::about(this, "Over DVB-T / RTL-SDR",
            "<h3>DVB-T / RTL-SDR ontvanger</h3>"
            "<p>Copyright &copy; 2026 Rob Hardenberg / PE1ITR</p>"
            "<p>Vrije software onder <b>GNU GPL versie 3 (GPL-3.0-only)</b>. "
            "Je mag de software wijzigen en verspreiden onder die licentie. "
            "Er wordt geen garantie gegeven.</p>"
            "<p>Zie LICENSE en NOTICE bij de distributie of de "
            "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">volledige GPL-tekst</a>. "
            "Externe componenten behouden hun eigen licenties.</p>");
    });
    power_ = new QPushButton;
    power_->setObjectName("power");
    power_->setMinimumWidth(135);
    toolbar->addWidget(power_);
    layout->addLayout(toolbar);
    connect(config_, &QPushButton::clicked, this, [this]() { configure(); });
    connect(power_, &QPushButton::clicked, this, [this]() { if (running_) stop(); else start(); });
    connect(playerButton_, &QPushButton::clicked, this, [this]() { openPlayer(); });

    spectrum_ = new ReceiverPlot(ReceiverPlot::Spectrum);
    waterfall_ = new ReceiverPlot(ReceiverPlot::Waterfall);
    auto *status = new QGroupBox("Ontvangststatus / service-info");
    status->setObjectName("statusPanel");
    status->setMinimumWidth(270);
    auto *statusLayout = new QVBoxLayout(status);
    state_ = new QLabel;
    state_->setObjectName("state");
    metrics_ = new QLabel;
    metrics_->setObjectName("metrics");
    service_ = new QLabel;
    service_->setObjectName("service");
    for (QLabel *label : {state_, metrics_, service_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        statusLayout->addWidget(label);
    }
    statusLayout->addStretch();

    constellation_ = new ReceiverPlot(ReceiverPlot::Constellation);
    auto *videoBox = new QGroupBox("Video / audio");
    videoBox->setObjectName("videoPanel");
    videoBox->setMinimumWidth(300);
    auto *videoLayout = new QVBoxLayout(videoBox);
    videoSurface_ = new QWidget;
    videoSurface_->setObjectName("videoSurface");
    videoSurface_->setMinimumSize(260, 170);
    videoSurface_->setStyleSheet("background:black;");
    videoMessage_ = new QLabel("Video verschijnt zodra een bruikbare transportstream wordt ontvangen.");
    videoMessage_->setWordWrap(true);
    videoLayout->addWidget(videoSurface_, 1);
    videoLayout->addWidget(videoMessage_);

    auto *receiverRow = new QSplitter(Qt::Horizontal);
    receiverRow->setObjectName("receiverRow");
    receiverRow->addWidget(constellation_);
    receiverRow->addWidget(status);
    receiverRow->addWidget(videoBox);
    receiverRow->setStretchFactor(0, 3);
    receiverRow->setStretchFactor(1, 3);
    receiverRow->setStretchFactor(2, 4);
    receiverRow->setSizes({340, 360, 460});

    auto *dashboard = new QSplitter(Qt::Vertical);
    dashboard->setObjectName("dashboard");
    dashboard->addWidget(waterfall_);
    dashboard->addWidget(spectrum_);
    dashboard->addWidget(receiverRow);
    dashboard->setStretchFactor(0, 4);
    dashboard->setStretchFactor(1, 3);
    dashboard->setStretchFactor(2, 5);
    dashboard->setSizes({220, 185, 315});
    layout->addWidget(dashboard, 1);
    auto *logButton = new QPushButton("Diagnose tonen / verbergen");
    logButton->setMaximumWidth(250);
    layout->addWidget(logButton);
    logs_ = new QPlainTextEdit;
    logs_->setObjectName("diagnostics");
    logs_->setReadOnly(true);
    logs_->setMaximumBlockCount(1000);
    logs_->setMaximumHeight(160);
    logs_->hide();
    layout->addWidget(logs_);
    connect(logButton, &QPushButton::clicked, this, [this]() { logs_->setVisible(!logs_->isVisible()); });
    setCentralWidget(root);
}

void ReceiverWindow::loadSettings()
{
    frequency_->setValue(settings_.value("receiver/frequencyMHz", 436.0).toDouble());
    gain_->setValue(settings_.value("receiver/gain", 30.0).toDouble());
    bandwidth_->setCurrentText(settings_.value("receiver/bandwidth", "333k").toString());
    fec_->setCurrentText(settings_.value("receiver/fec", "2/3").toString());
    guard_->setCurrentText(settings_.value("receiver/guard", "1/32").toString());
    rtlPath_ = settings_.value("rtl/program", "").toString();
    device_ = settings_.value("rtl/device", "0").toString();
    sampleRate_ = settings_.value("rtl/sampleRate", 1010526).toInt();
    ppm_ = settings_.value("rtl/ppm", 0).toInt();
    outputPort_ = settings_.value("output/port", 10000).toInt();
    playerPath_ = settings_.value("output/player", "").toString();
    embeddedVideo_ = settings_.value("output/embeddedVideo", true).toBool();
}

void ReceiverWindow::saveSettings()
{
    settings_.setValue("receiver/frequencyMHz", frequency_->value());
    settings_.setValue("receiver/gain", gain_->value());
    settings_.setValue("receiver/bandwidth", bandwidth_->currentText());
    settings_.setValue("receiver/fec", fec_->currentText());
    settings_.setValue("receiver/guard", guard_->currentText());
    settings_.setValue("rtl/program", rtlPath_);
    settings_.setValue("rtl/device", device_);
    settings_.setValue("rtl/sampleRate", sampleRate_);
    settings_.setValue("rtl/ppm", ppm_);
    settings_.setValue("output/port", outputPort_);
    settings_.setValue("output/player", playerPath_);
    settings_.setValue("output/embeddedVideo", embeddedVideo_);
}

void ReceiverWindow::configure()
{
    QDialog dialog(this);
    dialog.setWindowTitle("RTL-SDR en uitvoer");
    auto *layout = new QFormLayout(&dialog);
    auto *device = new QLineEdit(device_);
    device->setToolTip("RTL-SDR index of serienummer zoals geaccepteerd door rtl_sdr -d.");
    auto *rate = choices({"250000", "1000000", "1010526", "1024000", "1200000", "2048000", "2400000"}, "sampleRate");
    rate->setEditable(true);
    rate->setCurrentText(QString::number(sampleRate_));
    auto *ppm = new QSpinBox;
    ppm->setRange(-200, 200);
    ppm->setValue(ppm_);
    auto *port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(outputPort_);
    auto *embedded = new QCheckBox("Video in hoofdvenster (libVLC)");
    embedded->setChecked(embeddedVideo_);
    auto pathRow = [&](const QString &caption, const QString &path) {
        auto *row = new QWidget;
        auto *box = new QHBoxLayout(row);
        box->setContentsMargins(0, 0, 0, 0);
        auto *edit = new QLineEdit(path);
        edit->setPlaceholderText("Automatisch zoeken naast programma en in PATH");
        auto *browse = new QPushButton("…");
        box->addWidget(edit);
        box->addWidget(browse);
        connect(browse, &QPushButton::clicked, &dialog, [&, edit]() {
            const QString file = QFileDialog::getOpenFileName(&dialog, "Kies programma", edit->text());
            if (!file.isEmpty()) edit->setText(file);
        });
        layout->addRow(caption, row);
        return edit;
    };
    layout->addRow("Device", device);
    layout->addRow("IQ samplerate (samples/s)", rate);
    layout->addRow("Frequentiecorrectie (ppm)", ppm);
    layout->addRow("Externe MPEG-TS UDP-poort", port);
    layout->addRow(embedded);
    QLineEdit *rtl = pathRow("rtl_sdr", rtlPath_);
    QLineEdit *player = pathRow("VLC", playerPath_);
    auto *note = new QLabel("De decoder wordt naast de GUI geladen.\nStop de ontvangst om instellingen te wijzigen.");
    layout->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    device_ = device->text().trimmed();
    sampleRate_ = rate->currentText().toInt();
    ppm_ = ppm->value();
    outputPort_ = port->value();
    embeddedVideo_ = embedded->isChecked();
    rtlPath_ = rtl->text().trimmed();
    playerPath_ = player->text().trimmed();
    saveSettings();
}

QString ReceiverWindow::executable(const QString &name) const
{
#ifdef Q_OS_WIN
    const QString filename = name + ".exe";
#else
    const QString filename = name;
#endif
    const QString local = QCoreApplication::applicationDirPath() + "/" + filename;
    if (QFileInfo(local).isExecutable())
        return local;
    // The decoder is always the binary built alongside this GUI, never a stale PATH copy.
    return name == "rbdvbt_rx" ? QString() : QStandardPaths::findExecutable(filename);
}

void ReceiverWindow::log(const QString &message)
{
    logs_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + message.right(16000));
}

void ReceiverWindow::fail(const QString &message)
{
    log(message);
    stop();
    state_->setText("Fout · " + message);
    logs_->show();
}

void ReceiverWindow::setRunning(bool running)
{
    running_ = running;
    controls_->setEnabled(!running);
    config_->setEnabled(!running);
    playerButton_->setEnabled(running);
    power_->setText(running ? "■ Stop ontvangst" : "▶ Start ontvangst");
    power_->setStyleSheet(running ? "background:#18533a; border-color:#60d898;" : "");
}

void ReceiverWindow::watchProcess(QProcess *process, const QString &name)
{
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardError, this, [this, process, name]() {
        log(name + ": " + QString::fromLocal8Bit(process->readAllStandardError()).trimmed());
    });
    const QPointer<QProcess> guarded(process);
    auto exited = [this, guarded, name](const QString &reason) {
        log(name + " " + reason);
        // Defer cleanup: QProcess can emit error/finished inside waitForStarted().
        QTimer::singleShot(0, this, [this, guarded, name, reason]() {
            if (guarded && !stopping_ && !retuning_ && (guarded == rtl_ || guarded == decoder_))
                fail(name + " " + reason);
        });
    };
    connect(process, &QProcess::errorOccurred, this, [process, exited](QProcess::ProcessError) {
        exited(process->errorString());
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [exited](int code, QProcess::ExitStatus) { exited(QString("gestopt (exit %1)").arg(code)); });
}

bool ReceiverWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (!running_ || event->type() != QEvent::KeyPress || QApplication::activeModalWidget() || !isActiveWindow())
        return QMainWindow::eventFilter(watched, event);
    QWidget *target = qobject_cast<QWidget *>(watched);
    if (!target || (target != this && !isAncestorOf(target)))
        return QMainWindow::eventFilter(watched, event);
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->modifiers() != Qt::NoModifier)
        return QMainWindow::eventFilter(watched, event);

    const double oldFrequency = frequency_->value();
    const double oldGain = gain_->value();
    switch (key->key()) {
    case Qt::Key_Left:
        frequency_->setValue(qMax(frequency_->minimum(), frequency_->value() - frequency_->singleStep()));
        break;
    case Qt::Key_Right:
        frequency_->setValue(qMin(frequency_->maximum(), frequency_->value() + frequency_->singleStep()));
        break;
    case Qt::Key_Up:
        gain_->setValue(qMin(gain_->maximum(), gain_->value() + 1.0));
        break;
    case Qt::Key_Down:
        gain_->setValue(qMax(gain_->minimum(), gain_->value() - 1.0));
        break;
    default:
        return QMainWindow::eventFilter(watched, event);
    }
    if (frequency_->value() != oldFrequency || gain_->value() != oldGain) {
        key->accept();
        scheduleRetune();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void ReceiverWindow::scheduleRetune()
{
    state_->setText("Afstemmen · decoder acquiret opnieuw");
    retuneTimer_.start();
}

void ReceiverWindow::beginRetune()
{
    if (!running_ || retuning_)
        return;
    retuning_ = true;
    clearTelemetry();
    state_->setText("Afstemmen · RTL-SDR en decoder opnieuw starten");
    log(QString("Afstemmen op %1 MHz, gain %2 dB; decoder acquiret opnieuw.")
        .arg(frequency_->value(), 0, 'f', 6).arg(gain_->value(), 0, 'f', 1));
    // Leave libVLC and the GUI UDP socket up while the two pipeline processes
    // exit. Their shutdown is asynchronous so a slow USB device cannot freeze Qt.
    if (rtl_ && rtl_->state() != QProcess::NotRunning)
        rtl_->terminate();
    if (decoder_ && decoder_->state() != QProcess::NotRunning)
        decoder_->terminate();
    retuneKillTimer_.start();
    retunePollTimer_.start();
}

void ReceiverWindow::finishRetune()
{
    if (!retuning_)
        return;
    if ((rtl_ && rtl_->state() != QProcess::NotRunning) ||
        (decoder_ && decoder_->state() != QProcess::NotRunning))
        return;

    retunePollTimer_.stop();
    retuneKillTimer_.stop();
    for (QProcess **slot : {&rtl_, &decoder_}) {
        if (!*slot)
            continue;
        (*slot)->disconnect(this);
        (*slot)->deleteLater();
        *slot = nullptr;
    }
    retuning_ = false;
    running_ = false;
    start();
}

void ReceiverWindow::start()
{
    if ((running_ && !retuning_) || starting_)
        return;
    const bool validRate = (sampleRate_ >= 225001 && sampleRate_ <= 300000) ||
                           (sampleRate_ >= 900001 && sampleRate_ <= 2400000);
    const int bandwidthHz = bandwidth_->currentText().chopped(1).toInt() * 1000;
    if (!validRate || sampleRate_ < bandwidthHz * 8.0 / 7.0) {
        fail("Kies een geldige RTL-SDR samplerate (225001–300000 of 900001–2400000) passend bij de bandbreedte.");
        return;
    }
    if (device_.isEmpty() || device_.startsWith('-') || outputPort_ < 1024 || outputPort_ > 65535 ||
        ppm_ < -200 || ppm_ > 200) {
        fail("Controleer RTL-SDR device, ppm en UDP-poort in Instellingen.");
        return;
    }
    const QString rtl = rtlPath_.isEmpty() ? executable("rtl_sdr") : QStandardPaths::findExecutable(rtlPath_);
    const QString decoder = executable("rbdvbt_rx");
    if (rtl.isEmpty() || decoder.isEmpty()) {
        fail("rtl_sdr of rbdvbt_rx ontbreekt. Controleer Instellingen en plaats de decoder naast de GUI.");
        return;
    }
    if (!runtime_.isValid()) {
        fail("Kan geen tijdelijke map voor ontvangststatus maken.");
        return;
    }
    clearTelemetry();
    QFile::remove(runtime_.filePath("status.json"));
    if (visuals_.state() != QAbstractSocket::BoundState &&
        !visuals_.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress)) {
        fail("Kan de lokale poort voor spectrum en constellatie niet openen.");
        return;
    }
    if (videoPort_ == 0) {
        // Separate ports prevent an external player from consuming embedded video packets.
        QUdpSocket reservation;
        if (!reservation.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress)) {
            fail("Kan geen lokale videopoort reserveren.");
            return;
        }
        videoPort_ = reservation.localPort();
        if (videoPort_ == outputPort_) {
            fail("Videopoort conflicteert met externe uitvoer; probeer opnieuw.");
            return;
        }
        reservation.close();
    }
    starting_ = true;
    setRunning(true);
    saveSettings();
    state_->setText("Starten · RTL-SDR → DVB-T");
    rtl_ = new QProcess(this);
    rtl_->setObjectName("rtlProcess");
    decoder_ = new QProcess(this);
    decoder_->setObjectName("decoderProcess");
    watchProcess(rtl_, "RTL-SDR");
    watchProcess(decoder_, "Decoder");
    // Native OS pipe: IQ never enters Qt's event loop or an unbounded QByteArray queue.
    rtl_->setStandardOutputProcess(decoder_);
    decoder_->setStandardOutputFile(QProcess::nullDevice());
    const QString block = bandwidthHz == 150000 ? "128" : "64";
    const QStringList decoderArgs = {
        "--stdin", "--input-format", "u8", "--sample-rate", QString::number(sampleRate_),
        "--probe-constellation", "--resample-to-dvbt-rate", "--dvbt-ir", "1", "--live", "--afc",
        "--sr", bandwidth_->currentText(), "--fec", fec_->currentText(), "--gi", guard_->currentText(),
        "--live-symbols", block, "--probe-symbols", block, "--wait-video-start",
        "--udp-ts", QString("127.0.0.1:%1").arg(videoPort_),
        "--udp-copy", QString("127.0.0.1:%1").arg(outputPort_),
        "--visualizer-udp", QString("127.0.0.1:%1").arg(visuals_.localPort()),
        "--status-json", runtime_.filePath("status.json"), "--status-period-packets", "50"
    };
    const QStringList rtlArgs = {
        "-d", device_, "-f", QString::number(qRound64(frequency_->value() * 1000000)),
        "-s", QString::number(sampleRate_), "-g", QString::number(gain_->value(), 'f', 1),
        "-p", QString::number(ppm_), "-"
    };
    spectrum_->setFrequency(frequency_->value());
    waterfall_->setFrequency(frequency_->value());
    videoMessage_->setText(QString("Externe stream: udp://@:%1").arg(outputPort_));
    if (embeddedVideo_ && !videoRunning_) {
        QString error;
        if (!video_.start(videoPort_, videoSurface_, &error)) {
            log(error);
            videoMessage_->setText(error);
        } else {
            videoRunning_ = true;
        }
    }
    log(decoder + " " + decoderArgs.join(' '));
    log(rtl + " " + rtlArgs.join(' '));
    decoder_->start(decoder, decoderArgs);
    rtl_->start(rtl, rtlArgs);
    starting_ = false;
    state_->setText("Zoeken · wachten op DVB-T-signaal");
    statusAge_.start();
}

void ReceiverWindow::stop()
{
    if (stopping_)
        return;
    stopping_ = true;
    retuneTimer_.stop();
    retunePollTimer_.stop();
    retuneKillTimer_.stop();
    retuning_ = false;
    // Stop source first, then decoder. No hardware or decoder process survives the window.
    for (QProcess **slot : {&rtl_, &decoder_, &externalPlayer_}) {
        QProcess *process = *slot;
        *slot = nullptr;
        if (!process)
            continue;
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->terminate();
            if (!process->waitForFinished(1200)) {
                process->kill();
                process->waitForFinished(1200);
            }
        }
        process->deleteLater();
    }
    video_.stop();
    videoRunning_ = false;
    videoPort_ = 0;
    visuals_.close();
    clearTelemetry();
    state_->setText("Ontvanger uit");
    videoMessage_->setText("Start de ontvangst om video en audio te ontvangen.");
    setRunning(false);
    starting_ = false;
    stopping_ = false;
}

void ReceiverWindow::clearTelemetry()
{
    lastStatus_.clear();
    statusAge_.invalidate();
    spectrumAge_.invalidate();
    constellationAge_.invalidate();
    spectrum_->clear();
    waterfall_->clear();
    constellation_->clear();
    metrics_->setText("SNR —     CFO —     IQ —\nOFDM —     FEC —     TS —");
    service_->setText("Service —     Provider —");
}

void ReceiverWindow::readStatus()
{
    if (!running_ || starting_)
        return;
    QFile file(runtime_.filePath("status.json"));
    if (file.open(QIODevice::ReadOnly) && file.size() <= 65536) {
        const QByteArray data = file.readAll();
        const QJsonDocument document = QJsonDocument::fromJson(data);
        if (document.isObject() && data != lastStatus_) {
            lastStatus_ = data;
            statusAge_.start();
            const QJsonObject object = document.object();
            auto number = [&](const char *key, int decimals = 1) {
                const QJsonValue value = object.value(QLatin1String(key));
                return value.isDouble() ? QString::number(value.toDouble(), 'f', decimals) : QString("—");
            };
            auto lamp = [&](const char *key) { return object.value(QLatin1String(key)).toBool() ? "LOCK" : "—"; };
            state_->setText(object.value("locked").toBool() ? "DVB-T lock" : "Zoeken · " + object.value("stage").toString());
            metrics_->setText(QString("SNR %1 dB     CFO %2 Hz     IQ %3 dBFS\nOFDM %4     FEC %5     TS %6\nTS %7 pakketten     RS onherstelbaar %8     CC fouten %9\nFEC %10     GI %11     FIFO %12%")
                .arg(number("snr_db"), number("cfo_hz"), number("iq_rms_dbfs"),
                     lamp("lamp_ofdm_sync"), lamp("lamp_inner_fec"), lamp("lamp_ts_sync"),
                     number("ts_session_written_packets", 0), number("ts_session_rs_uncorrectable", 0),
                     number("ts_session_cc_errors", 0), object.value("fec").toString(),
                     object.value("guard").toString(), number("fifo1_load_percent", 0)));
            service_->setText(QString("Service: %1\nProvider: %2")
                .arg(object.value("service_name").toString("—"), object.value("service_provider").toString("—")));
        }
    }
    // Narrowband OFDM blocks take several seconds; do not show a stale lock forever.
    const int staleMs = bandwidth_->currentText() == "35k" || bandwidth_->currentText() == "40k" ? 12000 : 6000;
    if (statusAge_.isValid() && statusAge_.elapsed() > staleMs) {
        state_->setText("Geen actuele decoderstatus · controleer Diagnose");
        metrics_->setText("SNR —     CFO —     IQ —\nOFDM —     FEC —     TS —");
        service_->setText("Service —     Provider —");
    }
    if (spectrumAge_.isValid() && spectrumAge_.elapsed() > staleMs) {
        spectrum_->clear();
        waterfall_->clear();
        spectrumAge_.invalidate();
    }
    if (constellationAge_.isValid() && constellationAge_.elapsed() > staleMs) {
        constellation_->clear();
        constellationAge_.invalidate();
    }
}

void ReceiverWindow::readVisuals()
{
    // Bound each event-loop turn even if the decoder sends many frames.
    int received = 0;
    while (visuals_.hasPendingDatagrams() && received++ < 32) {
        QByteArray packet(8192, Qt::Uninitialized);
        const qint64 size = visuals_.readDatagram(packet.data(), packet.size());
        if (size < 0) break;
        packet.resize(size);
        VisualFrame frame;
        if (!decodeVisualFrame(packet, &frame))
            continue;
        if (frame.type == 1) {
            spectrum_->setFrame(frame);
            waterfall_->setFrame(frame);
            spectrumAge_.start();
        } else {
            constellation_->setFrame(frame);
            constellationAge_.start();
        }
    }
    if (visuals_.hasPendingDatagrams())
        QTimer::singleShot(0, this, [this]() { readVisuals(); });
}

void ReceiverWindow::openPlayer()
{
    if (!running_ || externalPlayer_)
        return;
    const QString path = playerPath_.isEmpty() ? executable("vlc") : QStandardPaths::findExecutable(playerPath_);
    if (path.isEmpty()) {
        log("VLC niet gevonden; kies het programma bij Instellingen.");
        logs_->show();
        return;
    }
    externalPlayer_ = new QProcess(this);
    QProcess *player = externalPlayer_;
    player->setStandardOutputFile(QProcess::nullDevice());
    connect(player, &QProcess::readyReadStandardError, this, [this, player]() {
        log("VLC: " + QString::fromLocal8Bit(player->readAllStandardError()).trimmed());
    });
    auto cleanup = [this, player]() {
        if (externalPlayer_ == player) {
            externalPlayer_ = nullptr;
            player->deleteLater();
        }
    };
    connect(player, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [cleanup](int, QProcess::ExitStatus) { cleanup(); });
    connect(player, &QProcess::errorOccurred, this, [this, player, cleanup](QProcess::ProcessError) {
        log("VLC: " + player->errorString());
        logs_->show();
        cleanup();
    });
    player->start(path, {"--no-one-instance", "--network-caching=500", QString("udp://@:%1").arg(outputPort_)});
}
