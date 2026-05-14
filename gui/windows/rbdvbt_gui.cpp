#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace {

#ifdef RBDVBT_GUI_VERSION
const char *kBuildVersion = RBDVBT_GUI_VERSION;
#else
const char *kBuildVersion = "0.1.1";
#endif

const char *kUdpTsBindUrl = "udp://@:10000";
const char *kUdpTsOut = "127.0.0.1:10000";

QString appDir()
{
    return QCoreApplication::applicationDirPath();
}

QString cleanPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return QString();
    return QDir::toNativeSeparators(QDir::cleanPath(trimmed));
}

QString firstExistingFile(const QStringList &paths)
{
    for (const QString &path : paths) {
        if (QFileInfo::exists(path) && QFileInfo(path).isFile())
            return cleanPath(path);
    }
    return QString();
}

QString findOnPath(const QString &name)
{
    const QString found = QStandardPaths::findExecutable(name);
    return found.isEmpty() ? QString() : cleanPath(found);
}

QString statusText(QProcess *process)
{
    if (!process)
        return "niet gestart";
    switch (process->state()) {
    case QProcess::NotRunning:
        return "gestopt";
    case QProcess::Starting:
        return "start";
    case QProcess::Running:
        return "actief";
    }
    return "onbekend";
}

QString processErrorText(QProcess::ProcessError error)
{
    switch (error) {
    case QProcess::FailedToStart:
        return "start mislukt";
    case QProcess::Crashed:
        return "proces crashte";
    case QProcess::Timedout:
        return "time-out";
    case QProcess::WriteError:
        return "schrijffout";
    case QProcess::ReadError:
        return "leesfout";
    case QProcess::UnknownError:
        return "onbekende fout";
    }
    return "onbekende fout";
}

bool isUsableExe(const QString &path)
{
    return !path.isEmpty() && QFileInfo(path).exists() && QFileInfo(path).isFile() && QFileInfo(path).isExecutable();
}

QStringList dllNameAlternatives(const QString &dll)
{
    if (dll.compare("rtlsdr.dll", Qt::CaseInsensitive) == 0 ||
        dll.compare("librtlsdr.dll", Qt::CaseInsensitive) == 0)
        return {"librtlsdr.dll", "rtlsdr.dll"};
    return {dll};
}

QStringList splitLines(const QByteArray &data)
{
    QString text = QString::fromLocal8Bit(data);
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    QStringList lines = text.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

struct DependencyReport {
    bool ok = false;
    QString rtlPath;
    QString decoderPath;
    QString vlcPath;
    QStringList missingDlls;
    QStringList lines;
};

struct Settings {
    QString rtlPath;
    QString decoderPath;
    QString vlcPath;
    QStringList dlls;
    QString inputMode = "rtl";
    QString iqPath;
    QString inputFormat = "u8";
    QString frequency = "437000000";
    QString rtlSampleRate = "1010526";
    QString gain = "30";
    QString symbolRate = "250000";
    QString guard = "1/32";
    QString fec = "2/3";
    QString loglevel = "info";
};

class ConfigDialog : public QDialog {
public:
    ConfigDialog(Settings settings, QWidget *parent = nullptr)
        : QDialog(parent), settings_(settings)
    {
        setWindowTitle("Configuratie");
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout();

        rtlEdit_ = addPathRow(form, "rtl_sdr.exe", settings_.rtlPath);
        decoderEdit_ = addPathRow(form, "rbdvbt_rx.exe", settings_.decoderPath);
        vlcEdit_ = addPathRow(form, "vlc.exe", settings_.vlcPath);

        dllEdit_ = new QTextEdit(this);
        dllEdit_->setPlainText(settings_.dlls.join("\n"));
        dllEdit_->setMinimumHeight(86);
        form->addRow("Verplichte DLL's", dllEdit_);

        layout->addLayout(form);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &ConfigDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &ConfigDialog::reject);
        layout->addWidget(buttons);
        resize(620, 280);
    }

    Settings settings() const
    {
        Settings result = settings_;
        result.rtlPath = cleanPath(rtlEdit_->text());
        result.decoderPath = cleanPath(decoderEdit_->text());
        result.vlcPath = cleanPath(vlcEdit_->text());
        result.dlls.clear();
        const QStringList lines = dllEdit_->toPlainText().split('\n');
        for (const QString &line : lines) {
            const QString item = line.trimmed();
            if (!item.isEmpty())
                result.dlls.append(item);
        }
        return result;
    }

private:
    QLineEdit *addPathRow(QFormLayout *form, const QString &label, const QString &value)
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *edit = new QLineEdit(value, row);
        auto *browse = new QPushButton("Bladeren", row);
        layout->addWidget(edit);
        layout->addWidget(browse);
        form->addRow(label, row);
        connect(browse, &QPushButton::clicked, this, [this, edit]() {
            const QString current = edit->text().isEmpty() ? appDir() : QFileInfo(edit->text()).absolutePath();
            const QString path = QFileDialog::getOpenFileName(this, "Kies executable", current, "Programma's (*.exe)");
            if (!path.isEmpty())
                edit->setText(cleanPath(path));
        });
        return edit;
    }

    Settings settings_;
    QLineEdit *rtlEdit_ = nullptr;
    QLineEdit *decoderEdit_ = nullptr;
    QLineEdit *vlcEdit_ = nullptr;
    QTextEdit *dllEdit_ = nullptr;
};

class MainWindow : public QMainWindow {
public:
    MainWindow()
    {
        setWindowTitle("rbdvbt_rx Windows GUI");
        settingsStore_ = new QSettings("rbdvbt", "rbdvbt_gui", this);
        loadSettings();
        buildUi();
        updateStatus();
        statusTimer_ = new QTimer(this);
        connect(statusTimer_, &QTimer::timeout, this, &MainWindow::updateStatus);
        statusTimer_->start(500);
    }

    ~MainWindow() override
    {
        stopPipeline();
        saveSettings();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        stopPipeline();
        saveSettings();
        QMainWindow::closeEvent(event);
    }

private:
    void loadSettings()
    {
        const QString dir = appDir();
        settings_.rtlPath = settingsStore_->value("paths/rtl", firstExistingFile({dir + "/rtl_sdr.exe"})).toString();
        settings_.decoderPath = settingsStore_->value("paths/decoder", firstExistingFile({dir + "/rbdvbt_rx.exe"})).toString();
        settings_.vlcPath = settingsStore_->value("paths/vlc",
            firstExistingFile({dir + "/vlc.exe",
                               "C:/Program Files/VideoLAN/VLC/vlc.exe",
                               "C:/Program Files (x86)/VideoLAN/VLC/vlc.exe"})).toString();
        settings_.dlls = settingsStore_->value("paths/dlls", QStringList({"librtlsdr.dll", "libusb-1.0.dll"})).toStringList();
        settings_.inputMode = settingsStore_->value("input/mode", settings_.inputMode).toString();
        settings_.iqPath = settingsStore_->value("input/iqPath", settings_.iqPath).toString();
        settings_.inputFormat = settingsStore_->value("input/format", settings_.inputFormat).toString();
        settings_.frequency = settingsStore_->value("rx/frequency", settings_.frequency).toString();
        settings_.rtlSampleRate = settingsStore_->value("rx/rtlSampleRate", settings_.rtlSampleRate).toString();
        settings_.gain = settingsStore_->value("rx/gain", settings_.gain).toString();
        settings_.symbolRate = settingsStore_->value("rx/symbolRate", settings_.symbolRate).toString();
        settings_.guard = settingsStore_->value("rx/guard", settings_.guard).toString();
        settings_.fec = settingsStore_->value("rx/fec", settings_.fec).toString();
        settings_.loglevel = settingsStore_->value("rx/loglevel", settings_.loglevel).toString();
        restoreGeometry(settingsStore_->value("window/geometry").toByteArray());
    }

    void saveSettings()
    {
        readSettingsFromUi();
        settingsStore_->setValue("paths/rtl", settings_.rtlPath);
        settingsStore_->setValue("paths/decoder", settings_.decoderPath);
        settingsStore_->setValue("paths/vlc", settings_.vlcPath);
        settingsStore_->setValue("paths/dlls", settings_.dlls);
        settingsStore_->setValue("input/mode", settings_.inputMode);
        settingsStore_->setValue("input/iqPath", settings_.iqPath);
        settingsStore_->setValue("input/format", settings_.inputFormat);
        settingsStore_->setValue("rx/frequency", settings_.frequency);
        settingsStore_->setValue("rx/rtlSampleRate", settings_.rtlSampleRate);
        settingsStore_->setValue("rx/gain", settings_.gain);
        settingsStore_->setValue("rx/symbolRate", settings_.symbolRate);
        settingsStore_->setValue("rx/guard", settings_.guard);
        settingsStore_->setValue("rx/fec", settings_.fec);
        settingsStore_->setValue("rx/loglevel", settings_.loglevel);
        settingsStore_->setValue("window/geometry", saveGeometry());
        settingsStore_->sync();
    }

    void buildUi()
    {
        auto *fileMenu = menuBar()->addMenu("Bestand");
        auto *openAction = fileMenu->addAction("Open IQ bestand...");
        connect(openAction, &QAction::triggered, this, &MainWindow::openIqFile);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        auto *top = new QGridLayout();
        top->addWidget(buildRxBox(), 0, 0);
        top->addWidget(buildStatusBox(), 0, 1);
        top->setColumnStretch(0, 2);
        top->setColumnStretch(1, 1);
        root->addLayout(top);

        videoWidget_ = new QWidget(central);
        videoWidget_->setAutoFillBackground(true);
        videoWidget_->setMinimumHeight(300);
        videoWidget_->setStyleSheet("background: black;");
        root->addWidget(videoWidget_, 1);

        tabs_ = new QTabWidget(central);
        allLog_ = makeLogTab();
        rtlLog_ = makeLogTab();
        decoderLog_ = makeLogTab();
        vlcLog_ = makeLogTab();
        diagLog_ = makeLogTab();
        tabs_->addTab(allLog_, "Alles");
        tabs_->addTab(rtlLog_, "RTL-SDR");
        tabs_->addTab(decoderLog_, "Decoder");
        tabs_->addTab(vlcLog_, "VLC");
        tabs_->addTab(diagLog_, "Diagnose");
        root->addWidget(tabs_);

        setCentralWidget(central);
        resize(1120, 780);
    }

    QWidget *buildRxBox()
    {
        auto *box = new QGroupBox("Ontvangstinstellingen", this);
        auto *layout = new QGridLayout(box);
        inputModeCombo_ = addCombo(layout, "Input", {"RTL-SDR live", "IQ bestand"}, settings_.inputMode == "file" ? "IQ bestand" : "RTL-SDR live", 0);
        inputFormatCombo_ = addCombo(layout, "IQ formaat", {"u8", "s16"}, settings_.inputFormat, 1);
        frequencyEdit_ = addLine(layout, "Frequentie Hz", settings_.frequency, 2);
        rtlRateEdit_ = addLine(layout, "Sample rate", settings_.rtlSampleRate, 3);
        gainEdit_ = addLine(layout, "RTL gain", settings_.gain, 4);
        iqPathEdit_ = addFileLine(layout, "IQ bestand", settings_.iqPath, 5);
        symbolRateEdit_ = addLine(layout, "DVB-T symbol rate", settings_.symbolRate, 6);
        guardCombo_ = addCombo(layout, "Guard interval", {"1/32", "1/16", "1/8", "auto"}, settings_.guard, 7);
        fecCombo_ = addCombo(layout, "FEC", {"1/2", "2/3", "3/4", "5/6", "7/8", "auto"}, settings_.fec, 8);
        loglevelCombo_ = addCombo(layout, "Decoder loglevel", {"quiet", "error", "warn", "info", "debug", "trace"}, settings_.loglevel, 9);

        startButton_ = new QPushButton("START", box);
        stopButton_ = new QPushButton("STOP", box);
        auto *checkButton = new QPushButton("Check installatie", box);
        auto *copyButton = new QPushButton("Kopieer diagnose", box);
        auto *configButton = new QPushButton("Configuratie", box);
        layout->addWidget(startButton_, 10, 0);
        layout->addWidget(stopButton_, 10, 1);
        layout->addWidget(checkButton, 11, 0);
        layout->addWidget(copyButton, 11, 1);
        layout->addWidget(configButton, 11, 2);
        stopButton_->setEnabled(false);
        connect(inputModeCombo_, &QComboBox::currentTextChanged, this, &MainWindow::updateInputUi);
        updateInputUi();

        connect(startButton_, &QPushButton::clicked, this, &MainWindow::startPipeline);
        connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopPipeline);
        connect(checkButton, &QPushButton::clicked, this, &MainWindow::checkInstallation);
        connect(copyButton, &QPushButton::clicked, this, &MainWindow::copyDiagnosis);
        connect(configButton, &QPushButton::clicked, this, &MainWindow::openConfig);
        return box;
    }

    QWidget *buildStatusBox()
    {
        auto *box = new QGroupBox("Status", this);
        auto *layout = new QVBoxLayout(box);
        statusTable_ = new QTableWidget(4, 5, box);
        statusTable_->setHorizontalHeaderLabels({"Proces", "Status", "PID", "Exit", "Laatste fout"});
        statusTable_->verticalHeader()->hide();
        statusTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        const QStringList names = {"RTL", "Decoder", "TS output", "VLC"};
        for (int i = 0; i < names.size(); ++i)
            statusTable_->setItem(i, 0, new QTableWidgetItem(names[i]));
        layout->addWidget(statusTable_);
        countersLabel_ = new QLabel(box);
        countersLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(countersLabel_);
        return box;
    }

    QLineEdit *addLine(QGridLayout *layout, const QString &label, const QString &value, int row)
    {
        auto *edit = new QLineEdit(value, this);
        layout->addWidget(new QLabel(label, this), row, 0);
        layout->addWidget(edit, row, 1, 1, 2);
        return edit;
    }

    QLineEdit *addFileLine(QGridLayout *layout, const QString &label, const QString &value, int row)
    {
        auto *rowWidget = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto *edit = new QLineEdit(value, rowWidget);
        auto *browse = new QPushButton("Open", rowWidget);
        rowLayout->addWidget(edit);
        rowLayout->addWidget(browse);
        layout->addWidget(new QLabel(label, this), row, 0);
        layout->addWidget(rowWidget, row, 1, 1, 2);
        connect(browse, &QPushButton::clicked, this, &MainWindow::openIqFile);
        return edit;
    }

    QComboBox *addCombo(QGridLayout *layout, const QString &label, const QStringList &items, const QString &value, int row)
    {
        auto *combo = new QComboBox(this);
        combo->addItems(items);
        const int idx = combo->findText(value);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
        layout->addWidget(new QLabel(label, this), row, 0);
        layout->addWidget(combo, row, 1, 1, 2);
        return combo;
    }

    QPlainTextEdit *makeLogTab()
    {
        auto *log = new QPlainTextEdit(this);
        log->setReadOnly(true);
        log->setMaximumBlockCount(1000);
        return log;
    }

    void readSettingsFromUi()
    {
        if (!frequencyEdit_)
            return;
        settings_.inputMode = inputModeCombo_->currentText() == "IQ bestand" ? "file" : "rtl";
        settings_.inputFormat = inputFormatCombo_->currentText();
        settings_.iqPath = cleanPath(iqPathEdit_->text());
        settings_.frequency = frequencyEdit_->text().trimmed();
        settings_.rtlSampleRate = rtlRateEdit_->text().trimmed();
        settings_.gain = gainEdit_->text().trimmed();
        settings_.symbolRate = symbolRateEdit_->text().trimmed();
        settings_.guard = guardCombo_->currentText();
        settings_.fec = fecCombo_->currentText();
        settings_.loglevel = loglevelCombo_->currentText();
    }

    QString resolveConfiguredExe(const QString &configured, const QString &name, const QStringList &extra = {}) const
    {
        QStringList candidates;
        candidates << appDir() + "/" + name;
        if (!configured.isEmpty())
            candidates << configured;
        candidates << extra;
        candidates << findOnPath(name);
        return firstExistingFile(candidates);
    }

    DependencyReport dependencyReport()
    {
        readSettingsFromUi();
        DependencyReport report;
        const bool fileMode = settings_.inputMode == "file";
        report.rtlPath = fileMode ? QString() : resolveConfiguredExe(settings_.rtlPath, "rtl_sdr.exe");
        report.decoderPath = resolveConfiguredExe(settings_.decoderPath, "rbdvbt_rx.exe");
        report.vlcPath = resolveConfiguredExe(settings_.vlcPath, "vlc.exe",
            {"C:/Program Files/VideoLAN/VLC/vlc.exe", "C:/Program Files (x86)/VideoLAN/VLC/vlc.exe"});

        if (fileMode)
            report.lines << "rtl_sdr.exe: niet nodig voor IQ-bestand";
        else
            appendDependencyLine(report, "rtl_sdr.exe", report.rtlPath);
        appendDependencyLine(report, "rbdvbt_rx.exe", report.decoderPath);
        appendDependencyLine(report, "vlc.exe", report.vlcPath);
        if (!report.rtlPath.isEmpty())
            settings_.rtlPath = report.rtlPath;
        if (!report.decoderPath.isEmpty())
            settings_.decoderPath = report.decoderPath;
        if (!report.vlcPath.isEmpty())
            settings_.vlcPath = report.vlcPath;

        if (!fileMode) {
            QStringList dllDirs;
            if (!report.rtlPath.isEmpty())
                dllDirs << QFileInfo(report.rtlPath).absolutePath();
            dllDirs << appDir();
            for (const QString &dll : settings_.dlls) {
                QString foundDll;
                if (!findDll(dll, dllDirs, &foundDll)) {
                    report.missingDlls << dll;
                    report.lines << QString("%1: DLL ontbreekt").arg(dll);
                } else {
                    report.lines << QString("%1: OK%2").arg(dll, foundDll != dll ? QString(" (gevonden: %1)").arg(foundDll) : QString());
                }
            }
        } else if (settings_.iqPath.isEmpty() || !QFileInfo(settings_.iqPath).isFile()) {
            report.lines << "IQ bestand: niet gevonden";
        }
        report.ok = (fileMode || isUsableExe(report.rtlPath)) && isUsableExe(report.decoderPath) &&
                    isUsableExe(report.vlcPath) && report.missingDlls.isEmpty();
        if (fileMode)
            report.ok = report.ok && QFileInfo(settings_.iqPath).isFile();
        lastDependencyReport_ = report;
        return report;
    }

    void appendDependencyLine(DependencyReport &report, const QString &name, const QString &path)
    {
        if (path.isEmpty())
            report.lines << QString("%1: niet gevonden").arg(name);
        else if (!isUsableExe(path))
            report.lines << QString("%1: pad ongeldig").arg(path);
        else
            report.lines << QString("%1: OK (%2)").arg(name, path);
    }

    bool findDll(const QString &dll, const QStringList &dirs, QString *foundName = nullptr) const
    {
        const QStringList names = dllNameAlternatives(dll);
        for (const QString &name : names) {
            for (const QString &dir : dirs) {
                if (QFileInfo::exists(QDir(dir).filePath(name))) {
                    if (foundName)
                        *foundName = name;
                    return true;
                }
            }
            const QStringList pathDirs = QString::fromLocal8Bit(qgetenv("PATH")).split(';', Qt::SkipEmptyParts);
            for (const QString &dir : pathDirs) {
                if (QFileInfo::exists(QDir(dir).filePath(name))) {
                    if (foundName)
                        *foundName = name;
                    return true;
                }
            }
        }
        return false;
    }

    void checkInstallation()
    {
        const DependencyReport report = dependencyReport();
        log("Diagnose", report.lines.join("\n"));
        QMessageBox::information(this, "Installatiecheck", report.lines.join("\n"));
    }

    void openConfig()
    {
        readSettingsFromUi();
        ConfigDialog dialog(settings_, this);
        if (dialog.exec() == QDialog::Accepted) {
            settings_ = dialog.settings();
            saveSettings();
            log("Diagnose", "Configuratie opgeslagen.");
        }
    }

    void openIqFile()
    {
        const QString current = iqPathEdit_ && !iqPathEdit_->text().isEmpty() ? QFileInfo(iqPathEdit_->text()).absolutePath() : appDir();
        const QString path = QFileDialog::getOpenFileName(this, "Open IQ bestand", current, "IQ bestanden (*.iq *.bin *.raw);;Alle bestanden (*.*)");
        if (path.isEmpty())
            return;
        iqPathEdit_->setText(cleanPath(path));
        inputModeCombo_->setCurrentText("IQ bestand");
        saveSettings();
        log("Diagnose", QString("IQ bestand gekozen: %1").arg(cleanPath(path)));
    }

    void updateInputUi()
    {
        if (!inputModeCombo_)
            return;
        const bool fileMode = inputModeCombo_->currentText() == "IQ bestand";
        frequencyEdit_->setEnabled(!fileMode);
        gainEdit_->setEnabled(!fileMode);
        iqPathEdit_->setEnabled(fileMode);
    }

    void startPipeline()
    {
        if (rtlProcess_ || decoderProcess_ || vlcProcess_)
            return;
        saveSettings();
        const DependencyReport report = dependencyReport();
        if (!report.ok) {
            log("Diagnose", report.lines.join("\n"));
            QString message;
            if (settings_.inputMode != "file" && !isUsableExe(report.rtlPath))
                message += "rtl_sdr.exe niet gevonden. Controleer het pad in Configuratie.\n";
            if (settings_.inputMode == "file" && !QFileInfo(settings_.iqPath).isFile())
                message += "IQ bestand niet gevonden. Kies een geldig bestand via Bestand > Open IQ bestand.\n";
            if (!isUsableExe(report.decoderPath))
                message += "rbdvbt_rx.exe niet gevonden. Plaats deze naast de GUI of stel het pad in.\n";
            if (!isUsableExe(report.vlcPath))
                message += "VLC kon niet worden gestart. Controleer het pad naar vlc.exe.\n";
            for (const QString &dll : report.missingDlls)
                message += QString("DLL ontbreekt: %1. Plaats deze naast rtl_sdr.exe of naast de GUI.\n").arg(dll);
            QMessageBox::warning(this, "Start geblokkeerd", message.trimmed());
            return;
        }

        rtlBytes_ = 0;
        tsBytes_ = 0;
        lastRtlData_ = QDateTime();
        lastTsData_ = QDateTime();
        startTime_ = QDateTime::currentDateTime();
        lastProcessError_.clear();
        loggedWarnings_.clear();

        rtlProcess_ = settings_.inputMode == "file" ? nullptr : new QProcess(this);
        decoderProcess_ = new QProcess(this);
        vlcProcess_ = new QProcess(this);
        if (rtlProcess_)
            configureProcess("RTL-SDR", rtlProcess_);
        configureProcess("Decoder", decoderProcess_);
        configureProcess("VLC", vlcProcess_);

        if (rtlProcess_)
            connect(rtlProcess_, &QProcess::readyReadStandardError, this, [this]() { drainStderr("RTL-SDR", rtlProcess_, rtlErr_); });
        connect(decoderProcess_, &QProcess::readyReadStandardError, this, [this]() { drainStderr("Decoder", decoderProcess_, decoderErr_); });
        connect(vlcProcess_, &QProcess::readyReadStandardError, this, [this]() { drainStderr("VLC", vlcProcess_, vlcErr_); });
        if (rtlProcess_)
            connect(rtlProcess_, &QProcess::readyReadStandardOutput, this, [this]() { forwardRtlToDecoder(); });

        if (rtlProcess_) {
            connect(rtlProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                    [this](int code, QProcess::ExitStatus status) {
                        if (decoderProcess_)
                            decoderProcess_->closeWriteChannel();
                        processFinished("RTL-SDR", code, status);
                    });
        }
        connect(decoderProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) {
                    processFinished("Decoder", code, status);
                });
        connect(vlcProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) { processFinished("VLC", code, status); });

        rtlCommand_ = report.rtlPath;
        rtlArgs_ = settings_.inputMode == "file" ? QStringList({settings_.iqPath}) :
            QStringList({"-f", settings_.frequency, "-s", settings_.rtlSampleRate, "-g", settings_.gain, "-"});
        decoderCommand_ = report.decoderPath;
        decoderArgs_ = {"--stdin", "--live", "--resample-to-dvbt-rate",
                        "--input-format", settings_.inputFormat, "--sample-rate", settings_.rtlSampleRate,
                        "--sr", settings_.symbolRate, "--gi", settings_.guard, "--fec", settings_.fec,
                        "--udp-out", kUdpTsOut, "--wait-video-start", "--loglevel", settings_.loglevel};
        vlcCommand_ = report.vlcPath;
        const quintptr hwnd = quintptr(videoWidget_->winId());
        vlcArgs_ = {"--drawable-hwnd", QString::number(hwnd),
                    "--no-video-title-show", "--quiet", kUdpTsBindUrl};

        log("Diagnose", "Pipeline starten zonder shell; TS loopt via UDP naar VLC.");
        if (settings_.inputMode == "file")
            log("Diagnose", QString("IQ bestand: %1").arg(settings_.iqPath));
        else
            log("Diagnose", commandLine("RTL-SDR", rtlCommand_, rtlArgs_));
        log("Diagnose", commandLine("Decoder", decoderCommand_, decoderArgs_));
        log("Diagnose", commandLine("VLC", vlcCommand_, vlcArgs_));

        if (!startProcessChecked("VLC", vlcProcess_, vlcCommand_, vlcArgs_) ||
            !startProcessChecked("Decoder", decoderProcess_, decoderCommand_, decoderArgs_) ||
            (rtlProcess_ && !startProcessChecked("RTL-SDR", rtlProcess_, rtlCommand_, rtlArgs_))) {
            QMessageBox::warning(this, "Start mislukt", "Een proces kon niet worden gestart. Zie Diagnose voor details.");
            stopPipeline();
            return;
        }
        if (settings_.inputMode == "file" && !startIqFilePlayback()) {
            QMessageBox::warning(this, "Start mislukt", "IQ bestand kon niet worden geopend. Zie Diagnose voor details.");
            stopPipeline();
            return;
        }

        startButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        updateStatus();
    }

    void configureProcess(const QString &name, QProcess *process)
    {
        process->setProcessChannelMode(QProcess::SeparateChannels);
        connect(process, &QProcess::errorOccurred, this, [this, name](QProcess::ProcessError error) {
            const QString text = QString("%1: %2").arg(name, processErrorText(error));
            lastProcessError_[name] = text;
            log("Diagnose", text);
            if (name == "RTL-SDR" && error == QProcess::FailedToStart)
                log("Diagnose", "RTL-SDR kon niet starten. Controleer USB-stick, Zadig-driver en pad.");
            if (name == "Decoder" && error == QProcess::FailedToStart)
                log("Diagnose", "Decoder kon niet starten. Controleer het pad naar rbdvbt_rx.exe.");
            if (name == "VLC" && error == QProcess::FailedToStart)
                log("Diagnose", "VLC kon niet worden gestart. Controleer het pad naar vlc.exe.");
        });
    }

    bool startProcessChecked(const QString &name, QProcess *process, const QString &program, const QStringList &args)
    {
        process->start(program, args);
        if (!process->waitForStarted(3000)) {
            const QString text = QString("%1 start mislukt: %2").arg(name, process->errorString());
            lastProcessError_[name] = text;
            log("Diagnose", text);
            return false;
        }
        log("Diagnose", QString("%1 gestart met PID %2.").arg(name).arg(process->processId()));
        return true;
    }

    void stopPipeline()
    {
        stopProcess("VLC", vlcProcess_);
        stopProcess("Decoder", decoderProcess_);
        stopProcess("RTL-SDR", rtlProcess_);
        stopIqFilePlayback();
        deleteLaterAndClear(vlcProcess_);
        deleteLaterAndClear(decoderProcess_);
        deleteLaterAndClear(rtlProcess_);
        startButton_->setEnabled(true);
        stopButton_->setEnabled(false);
        updateStatus();
    }

    void stopProcess(const QString &name, QProcess *process)
    {
        if (!process)
            return;
        if (process->state() == QProcess::NotRunning)
            return;
        process->closeWriteChannel();
        process->terminate();
        if (!process->waitForFinished(1500)) {
            log("Diagnose", QString("%1 reageert niet op terminate, kill wordt gebruikt.").arg(name));
            process->kill();
            process->waitForFinished(1500);
        }
    }

    void deleteLaterAndClear(QProcess *&process)
    {
        if (process) {
            process->deleteLater();
            process = nullptr;
        }
    }

    bool startIqFilePlayback()
    {
        iqFile_ = new QFile(settings_.iqPath, this);
        if (!iqFile_->open(QIODevice::ReadOnly)) {
            log("Diagnose", QString("IQ bestand openen mislukt: %1").arg(iqFile_->errorString()));
            delete iqFile_;
            iqFile_ = nullptr;
            return false;
        }
        iqFileTimer_ = new QTimer(this);
        connect(iqFileTimer_, &QTimer::timeout, this, &MainWindow::feedIqFile);
        iqPlaybackClock_.restart();
        iqFileTimer_->start(10);
        log("Diagnose", QString("IQ bestand afspelen gestart: %1").arg(settings_.iqPath));
        return true;
    }

    void stopIqFilePlayback()
    {
        if (iqFileTimer_) {
            iqFileTimer_->stop();
            iqFileTimer_->deleteLater();
            iqFileTimer_ = nullptr;
        }
        if (iqFile_) {
            iqFile_->close();
            iqFile_->deleteLater();
            iqFile_ = nullptr;
        }
    }

    void feedIqFile()
    {
        if (!iqFile_ || !decoderProcess_ || decoderProcess_->state() != QProcess::Running)
            return;
        if (decoderProcess_->bytesToWrite() > 1024 * 1024)
            return;
        bool ok = false;
        const qint64 sampleRate = settings_.rtlSampleRate.toLongLong(&ok);
        const qint64 bytesPerSample = settings_.inputFormat == "s16" ? 4 : 2;
        const qint64 allowedBytes = ok && sampleRate > 0 ?
            (iqPlaybackClock_.elapsed() * sampleRate * bytesPerSample) / 1000 : rtlBytes_ + 256 * 1024;
        const qint64 budget = allowedBytes - rtlBytes_;
        if (budget <= 0)
            return;
        const QByteArray data = iqFile_->read(qMin<qint64>(256 * 1024, budget));
        if (!data.isEmpty()) {
            rtlBytes_ += data.size();
            lastRtlData_ = QDateTime::currentDateTime();
            decoderProcess_->write(data);
            return;
        }
        if (iqFile_->atEnd()) {
            log("Diagnose", "Einde IQ bestand bereikt.");
            decoderProcess_->closeWriteChannel();
            stopIqFilePlayback();
        }
    }

    void drainStderr(const QString &source, QProcess *process, QStringList &tail)
    {
        for (const QString &line : splitLines(process->readAllStandardError())) {
            appendTail(tail, line, 40);
            if (source == "Decoder")
                noteDecoderTsProgress(line);
            log(source, line);
        }
    }

    void noteDecoderTsProgress(const QString &line)
    {
        static const QRegularExpression re("written_packets=(\\d+)");
        const QRegularExpressionMatch match = re.match(line);
        if (!match.hasMatch())
            return;
        bool ok = false;
        const qint64 packets = match.captured(1).toLongLong(&ok);
        if (!ok || packets <= 0)
            return;
        tsBytes_ += packets * 188;
        lastTsData_ = QDateTime::currentDateTime();
    }

    void forwardRtlToDecoder()
    {
        if (!rtlProcess_)
            return;
        const QByteArray data = rtlProcess_->readAllStandardOutput();
        if (!data.isEmpty()) {
            rtlBytes_ += data.size();
            lastRtlData_ = QDateTime::currentDateTime();
            if (decoderProcess_ && decoderProcess_->state() == QProcess::Running)
                decoderProcess_->write(data);
        }
    }

    void processFinished(const QString &name, int code, QProcess::ExitStatus status)
    {
        log("Diagnose", QString("%1 gestopt: exit code %2, status %3")
            .arg(name).arg(code).arg(status == QProcess::NormalExit ? "normaal" : "crash"));
        updateStatus();
    }

    void updateStatus()
    {
        if (settings_.inputMode == "file")
            fillInputFileRow();
        else
            fillProcessRow(0, "RTL", rtlProcess_, rtlBytes_ > 0);
        fillProcessRow(1, "Decoder", decoderProcess_, decoderProcess_ && decoderProcess_->state() == QProcess::Running);
        fillProcessRow(2, "TS output", decoderProcess_, tsBytes_ > 0);
        fillProcessRow(3, "VLC", vlcProcess_, vlcProcess_ && vlcProcess_->state() == QProcess::Running);

        const qint64 rtlAge = lastRtlData_.isValid() ? lastRtlData_.msecsTo(QDateTime::currentDateTime()) / 1000 : -1;
        const qint64 tsAge = lastTsData_.isValid() ? lastTsData_.msecsTo(QDateTime::currentDateTime()) / 1000 : -1;
        countersLabel_->setText(QString("Input levert bytes: %1\nDecoder levert TS bytes: %2\nTijd sinds input data: %3\nTijd sinds TS data: %4\nTotaal input bytes: %5\nTotaal TS bytes: %6")
            .arg(rtlBytes_ > 0 ? "ja" : "nee")
            .arg(tsBytes_ > 0 ? "ja" : "nee")
            .arg(rtlAge >= 0 ? QString::number(rtlAge) + " s" : "n.v.t.")
            .arg(tsAge >= 0 ? QString::number(tsAge) + " s" : "n.v.t.")
            .arg(rtlBytes_)
            .arg(tsBytes_));

        if (settings_.inputMode != "file" && rtlProcess_ && rtlProcess_->state() == QProcess::Running && rtlBytes_ == 0 && startTime_.isValid() &&
            startTime_.msecsTo(QDateTime::currentDateTime()) > 5000)
            logOnce("no_rtl", "RTL-SDR levert geen samples. Controleer USB-stick, Zadig-driver en gain.");
        if (decoderProcess_ && decoderProcess_->state() == QProcess::Running && rtlBytes_ > 0 && tsBytes_ == 0 &&
            lastRtlData_.isValid() && lastRtlData_.msecsTo(QDateTime::currentDateTime()) > 8000)
            logOnce("no_ts", "Decoder gestart, maar levert geen transportstream. Controleer frequentie, sample rate, SR, guard en FEC.");
        if (vlcProcess_ && vlcProcess_->state() == QProcess::Running && tsBytes_ == 0 &&
            startTime_.isValid() && startTime_.msecsTo(QDateTime::currentDateTime()) > 10000)
            logOnce("vlc_no_video", "VLC actief, maar geen video. Controleer of de decoder TS levert.");
    }

    void fillProcessRow(int row, const QString &name, QProcess *process, bool lamp)
    {
        setCell(row, 0, QString("%1 %2").arg(lamp ? "[OK]" : "[--]", name));
        setCell(row, 1, statusText(process));
        setCell(row, 2, process && process->processId() > 0 ? QString::number(process->processId()) : "-");
        setCell(row, 3, process ? QString("%1/%2").arg(process->exitCode()).arg(process->exitStatus() == QProcess::NormalExit ? "normaal" : "crash") : "-");
        setCell(row, 4, lastProcessError_.value(name == "RTL" ? "RTL-SDR" : name, "-"));
    }

    void fillInputFileRow()
    {
        const bool active = iqFile_ && iqFile_->isOpen();
        setCell(0, 0, QString("%1 IQ bestand").arg(rtlBytes_ > 0 ? "[OK]" : "[--]"));
        setCell(0, 1, active ? "actief" : (rtlBytes_ > 0 ? "gelezen" : "niet gestart"));
        setCell(0, 2, "-");
        setCell(0, 3, "-");
        setCell(0, 4, QFileInfo(settings_.iqPath).fileName().isEmpty() ? "-" : QFileInfo(settings_.iqPath).fileName());
    }

    void setCell(int row, int col, const QString &text)
    {
        QTableWidgetItem *item = statusTable_->item(row, col);
        if (!item) {
            item = new QTableWidgetItem();
            statusTable_->setItem(row, col, item);
        }
        item->setText(text);
    }

    void log(const QString &source, const QString &text)
    {
        const QStringList lines = text.split('\n');
        for (const QString &line : lines) {
            if (line.isEmpty())
                continue;
            const QString entry = QString("%1 [%2] %3").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), source, line);
            appendTail(lastLogs_, entry, 100);
            allLog_->appendPlainText(entry);
            if (source == "RTL-SDR")
                rtlLog_->appendPlainText(entry);
            else if (source == "Decoder")
                decoderLog_->appendPlainText(entry);
            else if (source == "VLC")
                vlcLog_->appendPlainText(entry);
            else
                diagLog_->appendPlainText(entry);
        }
    }

    void logOnce(const QString &key, const QString &text)
    {
        if (loggedWarnings_.contains(key))
            return;
        loggedWarnings_.insert(key);
        log("Diagnose", text);
    }

    void appendTail(QStringList &list, const QString &line, int maxLines)
    {
        list.append(line);
        while (list.size() > maxLines)
            list.removeFirst();
    }

    QString commandLine(const QString &name, const QString &program, const QStringList &args) const
    {
        QStringList quoted;
        quoted << QDir::toNativeSeparators(program);
        for (const QString &arg : args)
            quoted << (arg.contains(' ') ? QString("\"%1\"").arg(arg) : arg);
        return QString("%1: %2").arg(name, quoted.join(' '));
    }

    QString diagnosisText() const
    {
        QString text;
        text += QString("GUI versie: %1\n").arg(kBuildVersion);
        text += QString("Windows versie: %1\n").arg(QSysInfo::prettyProductName());
        text += QString("rtl_sdr.exe: %1\n").arg(settings_.rtlPath);
        text += QString("rbdvbt_rx.exe: %1\n").arg(settings_.decoderPath);
        text += QString("vlc.exe: %1\n\n").arg(settings_.vlcPath);
        text += QString("Input mode: %1\n").arg(settings_.inputMode);
        text += settings_.inputMode == "file" ? QString("IQ bestand: %1\n").arg(settings_.iqPath) : commandLine("RTL-SDR", rtlCommand_, rtlArgs_) + "\n";
        text += commandLine("Decoder", decoderCommand_, decoderArgs_) + "\n";
        text += commandLine("VLC", vlcCommand_, vlcArgs_) + "\n\n";
        text += "Dependency check:\n" + lastDependencyReport_.lines.join("\n") + "\n\n";
        text += QString("Instellingen: input_format=%1 frequentie=%2 sample_rate=%3 gain=%4 symbol_rate=%5 gi=%6 fec=%7 loglevel=%8\n")
            .arg(settings_.inputFormat, settings_.frequency, settings_.rtlSampleRate, settings_.gain, settings_.symbolRate, settings_.guard, settings_.fec, settings_.loglevel);
        text += QString("Bytes: rtl=%1 ts=%2\n").arg(rtlBytes_).arg(tsBytes_);
        text += "Processen:\n";
        text += processSummary("RTL-SDR", rtlProcess_) + "\n";
        text += processSummary("Decoder", decoderProcess_) + "\n";
        text += processSummary("VLC", vlcProcess_) + "\n\n";
        text += "Laatste RTL stderr:\n" + rtlErr_.join("\n") + "\n\n";
        text += "Laatste decoder stderr:\n" + decoderErr_.join("\n") + "\n\n";
        text += "Laatste VLC stderr:\n" + vlcErr_.join("\n") + "\n\n";
        text += "Laatste logregels:\n" + lastLogs_.join("\n") + "\n";
        return text;
    }

    QString processSummary(const QString &name, QProcess *process) const
    {
        if (!process)
            return QString("%1: niet gestart").arg(name);
        return QString("%1: status=%2 pid=%3 exit_code=%4 exit_status=%5 laatste_fout=%6")
            .arg(name)
            .arg(statusText(process))
            .arg(process->processId())
            .arg(process->exitCode())
            .arg(process->exitStatus() == QProcess::NormalExit ? "normaal" : "crash")
            .arg(lastProcessError_.value(name, "-"));
    }

    void copyDiagnosis()
    {
        readSettingsFromUi();
        lastDependencyReport_ = dependencyReport();
        QGuiApplication::clipboard()->setText(diagnosisText());
        log("Diagnose", "Diagnose naar clipboard gekopieerd.");
    }

    Settings settings_;
    QSettings *settingsStore_ = nullptr;
    QLineEdit *frequencyEdit_ = nullptr;
    QLineEdit *rtlRateEdit_ = nullptr;
    QLineEdit *gainEdit_ = nullptr;
    QLineEdit *iqPathEdit_ = nullptr;
    QLineEdit *symbolRateEdit_ = nullptr;
    QComboBox *inputModeCombo_ = nullptr;
    QComboBox *inputFormatCombo_ = nullptr;
    QComboBox *guardCombo_ = nullptr;
    QComboBox *fecCombo_ = nullptr;
    QComboBox *loglevelCombo_ = nullptr;
    QPushButton *startButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QWidget *videoWidget_ = nullptr;
    QTableWidget *statusTable_ = nullptr;
    QLabel *countersLabel_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QPlainTextEdit *allLog_ = nullptr;
    QPlainTextEdit *rtlLog_ = nullptr;
    QPlainTextEdit *decoderLog_ = nullptr;
    QPlainTextEdit *vlcLog_ = nullptr;
    QPlainTextEdit *diagLog_ = nullptr;
    QTimer *statusTimer_ = nullptr;
    QProcess *rtlProcess_ = nullptr;
    QProcess *decoderProcess_ = nullptr;
    QProcess *vlcProcess_ = nullptr;
    QFile *iqFile_ = nullptr;
    QTimer *iqFileTimer_ = nullptr;
    QElapsedTimer iqPlaybackClock_;
    qint64 rtlBytes_ = 0;
    qint64 tsBytes_ = 0;
    QDateTime lastRtlData_;
    QDateTime lastTsData_;
    QDateTime startTime_;
    QString rtlCommand_;
    QString decoderCommand_;
    QString vlcCommand_;
    QStringList rtlArgs_;
    QStringList decoderArgs_;
    QStringList vlcArgs_;
    QStringList rtlErr_;
    QStringList decoderErr_;
    QStringList vlcErr_;
    QStringList lastLogs_;
    QSet<QString> loggedWarnings_;
    QHash<QString, QString> lastProcessError_;
    DependencyReport lastDependencyReport_;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("rbdvbt_gui");
    QCoreApplication::setOrganizationName("rbdvbt");
    QCoreApplication::setApplicationVersion(kBuildVersion);
    MainWindow window;
    window.show();
    return app.exec();
}
