/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Rob Hardenberg / PE1ITR */
// Process fixture for real OS-pipe/lifecycle tests. Never opens SDR hardware.
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <QUdpSocket>
#include <QtEndian>
#include <cstdio>
#include <cstring>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const QStringList args = app.arguments();
    const bool decoder = args.contains("--stdin");
    const QString mode = qEnvironmentVariable("RBDVBT_TEST_MODE");
    QFile arguments(qEnvironmentVariable("RBDVBT_TEST_DIR") + (decoder ? "/decoder-args" : "/rtl-args"));
    if (arguments.open(QIODevice::WriteOnly))
        arguments.write(args.join('\n').toUtf8());
    arguments.close();
    if ((decoder && mode == "decoder-exit") || (!decoder && mode == "rtl-exit"))
        return 7;
    if (!decoder) {
        QByteArray bytes(4096, Qt::Uninitialized);
        for (int i = 0; i < bytes.size(); ++i)
            bytes[i] = char(i & 255);
        for (;;) {
            if (std::fwrite(bytes.constData(), 1, bytes.size(), stdout) != size_t(bytes.size()) || std::fflush(stdout))
                return 0;
            QThread::msleep(10);
        }
    }
    auto option = [&](const char *name) { return args.value(args.indexOf(QLatin1String(name)) + 1); };
    const quint16 port = option("--visualizer-udp").section(':', -1).toUShort();
    const QString path = option("--status-json");
    QUdpSocket socket;
    int sequence = 0;
    char data[4096];
    while (std::fread(data, 1, sizeof(data), stdin) == sizeof(data)) {
        for (int i = 0; i < int(sizeof(data)); ++i)
            if (static_cast<unsigned char>(data[i]) != (i & 255))
                return 9;
        ++sequence;
        if (mode != "stale" || sequence == 1) {
            QJsonObject obj{{"status_update", sequence}, {"locked", true}, {"snr_db", 12.5},
                {"lamp_ofdm_sync", true}, {"lamp_inner_fec", true}, {"lamp_ts_sync", true},
                {"ts_session_written_packets", sequence}, {"service_name", "PIPE VERIFIED"},
                {"service_provider", "<b>plain text</b>"}, {"fec", "2/3"}, {"guard", "1/32"}};
            QSaveFile status(path);
            if (status.open(QIODevice::WriteOnly)) {
                status.write(QJsonDocument(obj).toJson());
                status.commit();
            }
            QByteArray frame(48, '\0');
            std::memcpy(frame.data(), "RBV1", 4);
            qToLittleEndian<quint16>(1, frame.data() + 4);
            qToLittleEndian<quint32>(1010526, frame.data() + 12);
            qToLittleEndian<quint32>(4, frame.data() + 16);
            socket.writeDatagram(frame, QHostAddress::LocalHost, port);
        }
    }
    return 0;
}
