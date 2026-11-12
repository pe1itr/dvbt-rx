/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Rob Hardenberg / PE1ITR */
#include "receiver_window.h"
#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("rbdvbt_gui");
    app.setOrganizationName("PE1ITR");
    app.setApplicationVersion(RBDVBT_GUI_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("DVB-T 2K QPSK ontvanger voor RTL-SDR");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);
    ReceiverWindow window;
    window.show();
    return app.exec();
}
