#include "receiver_window.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QtEndian>
#include <QtTest>
#include <cstring>
#include <limits>

class ReceiverUiTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir_;
    QTemporaryDir fixtureDir_;
private:
    QString fixturePath() const {
        return QCoreApplication::applicationDirPath() + "/test_rtl_fixture"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
    }
    void click(ReceiverWindow &window) {
        window.findChild<QPushButton *>("power")->click();
    }
    QString state(ReceiverWindow &window) {
        return window.findChild<QLabel *>("state")->text();
    }
    QString args(const char *name) {
        QFile file(fixtureDir_.filePath(name));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }
private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(fixtureDir_.isValid());
        QCoreApplication::setOrganizationName("rbdvbt-test");
        QCoreApplication::setApplicationName("receiver-ui-test");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        qputenv("RBDVBT_TEST_DIR", fixtureDir_.path().toUtf8());
    }
    void init() {
        QSettings settings;
        settings.clear();
        settings.setValue("rtl/program", fixturePath());
        settings.setValue("output/embeddedVideo", false);
        settings.sync();
        qputenv("RBDVBT_TEST_MODE", "normal");
        QFile::remove(fixtureDir_.filePath("rtl-args"));
        QFile::remove(fixtureDir_.filePath("decoder-args"));
    }
    void visualProtocol() {
        VisualFrame frame;
        QByteArray bytes(48, '\0');
        std::memcpy(bytes.data(), "RBV1", 4);
        qToLittleEndian<quint16>(2, bytes.data() + 4);
        qToLittleEndian<quint32>(1010526, bytes.data() + 12);
        qToLittleEndian<quint32>(2, bytes.data() + 16);
        QVERIFY(decodeVisualFrame(bytes, &frame));
        QCOMPARE(frame.values.size(), 4);
        QCOMPARE(frame.type, 2);
        QVERIFY(!decodeVisualFrame(bytes.left(47), &frame));
        qToLittleEndian<quint32>(0xffffffff, bytes.data() + 16);
        QVERIFY(!decodeVisualFrame(bytes, &frame));
        qToLittleEndian<quint32>(2, bytes.data() + 16);
        qToLittleEndian<quint32>(0x7fc00000, bytes.data() + 32); // NaN
        QVERIFY(!decodeVisualFrame(bytes, &frame));
        QVERIFY(!decodeVisualFrame(QByteArray("bad"), &frame));
    }
    void linradDashboardLayout() {
        ReceiverWindow window;
        auto *dashboard = window.findChild<QSplitter *>("dashboard");
        auto *row = window.findChild<QSplitter *>("receiverRow");
        QVERIFY(dashboard && row);
        QCOMPARE(dashboard->count(), 3);
        QCOMPARE(dashboard->widget(0)->objectName(), QString("waterfall"));
        QCOMPARE(dashboard->widget(1)->objectName(), QString("spectrum"));
        QCOMPARE(dashboard->widget(2), static_cast<QWidget *>(row));
        QCOMPARE(row->count(), 3);
        QCOMPARE(row->widget(0)->objectName(), QString("constellation"));
        QCOMPARE(row->widget(1)->objectName(), QString("statusPanel"));
        QCOMPARE(row->widget(2)->objectName(), QString("videoPanel"));
    }
    void receiveStopRestart() {
        ReceiverWindow window;
        window.show();
        QCOMPARE(state(window), QString("Ontvanger uit"));
        QVERIFY(!window.findChild<QWidget *>("videoSurface")->testAttribute(Qt::WA_NativeWindow));
        click(window);
        QTRY_COMPARE_WITH_TIMEOUT(state(window), QString("DVB-T lock"), 4000);
        QVERIFY(window.findChild<QLabel *>("service")->text().contains("PIPE VERIFIED"));
        QVERIFY(window.findChild<QLabel *>("service")->text().contains("<b>plain text</b>"));
        QVERIFY(!window.findChild<QDoubleSpinBox *>("frequency")->isEnabled());
        QVERIFY(args("rtl-args").contains("-d\n0\n-f\n436000000"));
        QVERIFY(args("decoder-args").contains("--input-format\nu8"));
        QVERIFY(args("decoder-args").contains("--probe-constellation"));
        QVERIFY(args("decoder-args").contains("--sr\n333k"));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/receiver-test.png"));
        auto retune = [&](Qt::Key key, const QString &expectedArgs) {
            QTest::keyClick(&window, key);
            QTRY_VERIFY_WITH_TIMEOUT(state(window).startsWith("Afstemmen"), 1000);
            QTRY_VERIFY_WITH_TIMEOUT(args("rtl-args").contains(expectedArgs), 4000);
            QTRY_COMPARE_WITH_TIMEOUT(state(window), QString("DVB-T lock"), 4000);
            QVERIFY(!window.findChild<QDoubleSpinBox *>("frequency")->isEnabled());
        };
            retune(Qt::Key_Right, "-f\n436001000");
        retune(Qt::Key_Left, "-f\n436000000");
        retune(Qt::Key_Up, "-g\n31.0");
        retune(Qt::Key_Down, "-g\n30.0");
        // Both children must be reaped immediately, without depending on deferred deletion.
        QProcess *rtl = window.findChild<QProcess *>("rtlProcess");
        QProcess *decoder = window.findChild<QProcess *>("decoderProcess");
        QVERIFY(rtl && decoder);
        click(window);
        QCOMPARE(rtl->state(), QProcess::NotRunning);
        QCOMPARE(decoder->state(), QProcess::NotRunning);
        QCOMPARE(state(window), QString("Ontvanger uit"));
        QVERIFY(!window.findChild<QLabel *>("metrics")->text().contains("LOCK"));
        window.findChild<QComboBox *>("bandwidth")->setCurrentText("150k");
        click(window);
        QTRY_COMPARE_WITH_TIMEOUT(state(window), QString("DVB-T lock"), 4000);
        QVERIFY(args("decoder-args").contains("--live-symbols\n128"));
        click(window);
    }
    void processExit_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("source unplugged") << QByteArray("rtl-exit");
        QTest::newRow("decoder stopped") << QByteArray("decoder-exit");
    }
    void processExit() {
        QFETCH(QByteArray, mode);
        qputenv("RBDVBT_TEST_MODE", mode);
        ReceiverWindow window;
        click(window);
        QTRY_VERIFY_WITH_TIMEOUT(state(window).startsWith("Fout"), 4000);
        // An RTL-SDR exit also closes decoder stdin; either child can report EOF first.
        QVERIFY(state(window).contains("gestopt") || state(window).contains("fout"));
        QVERIFY(!args(mode == "rtl-exit" ? "rtl-args" : "decoder-args").isEmpty());
        QVERIFY(window.findChild<QDoubleSpinBox *>("frequency")->isEnabled());
        for (QProcess *process : window.findChildren<QProcess *>())
            QCOMPARE(process->state(), QProcess::NotRunning);
    }
    void invalidSampleRate() {
        QSettings().setValue("rtl/sampleRate", 250000);
        ReceiverWindow window;
        click(window); // 333 kHz channel cannot fit in 250 ksps.
        QVERIFY(state(window).startsWith("Fout"));
        QVERIFY(args("rtl-args").isEmpty());
    }
    void missingRtl() {
        QSettings().setValue("rtl/program", "/missing/rtl_sdr");
        ReceiverWindow window;
        click(window);
        QVERIFY(state(window).startsWith("Fout"));
        QVERIFY(args("decoder-args").isEmpty());
    }
    void staleLock() {
        qputenv("RBDVBT_TEST_MODE", "stale");
        ReceiverWindow window;
        click(window);
        QTRY_COMPARE_WITH_TIMEOUT(state(window), QString("DVB-T lock"), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(state(window).startsWith("Geen actuele"), 8000);
        QVERIFY(!window.findChild<QLabel *>("metrics")->text().contains("LOCK"));
        click(window);
    }
};

QTEST_MAIN(ReceiverUiTest)
#include "test_receiver_ui.moc"
