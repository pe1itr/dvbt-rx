/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Rob Hardenberg / PE1ITR */
#include "plots.h"

#include <QPainter>
#include <QPainterPath>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

bool decodeVisualFrame(const QByteArray &packet, VisualFrame *frame)
{
    if (!frame || packet.size() < 32 || packet.size() > 8192 || packet.left(4) != "RBV1")
        return false;
    const auto *p = reinterpret_cast<const uchar *>(packet.constData());
    const quint16 type = qFromLittleEndian<quint16>(p + 4);
    const quint32 count = qFromLittleEndian<quint32>(p + 16);
    const quint32 rate = qFromLittleEndian<quint32>(p + 12);
    if ((type != 1 && type != 2) || !count || !rate)
        return false;
    const quint32 floats = count * (type == 2 ? 2u : 1u);
    if (count > 2040 || floats > 2040 || packet.size() != 32 + floats * 4)
        return false;
    VisualFrame result;
    result.type = type;
    result.sampleRate = rate;
    for (quint32 i = 0; i < floats; ++i) {
        const quint32 bits = qFromLittleEndian<quint32>(p + 32 + i * 4);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
            return false;
        result.values.append(value);
    }
    *frame = result;
    return true;
}

ReceiverPlot::ReceiverPlot(Kind kind, QWidget *parent) : QWidget(parent), kind_(kind)
{
    setObjectName(kind == Spectrum ? "spectrum" : kind == Waterfall ? "waterfall" : "constellation");
    setMinimumSize(220, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ReceiverPlot::clear()
{
    frame_ = VisualFrame();
    waterfall_ = QImage();
    update();
}

void ReceiverPlot::setFrame(const VisualFrame &frame)
{
    frame_ = frame;
    if (kind_ == Waterfall) {
        if (waterfall_.width() != frame.values.size()) {
            waterfall_ = QImage(frame.values.size(), 200, QImage::Format_RGB32);
            waterfall_.fill(Qt::black);
        }
        std::memmove(waterfall_.scanLine(1), waterfall_.constScanLine(0),
                     size_t(waterfall_.bytesPerLine()) * (waterfall_.height() - 1));
        QVector<float> sorted = frame.values;
        std::sort(sorted.begin(), sorted.end());
        const float floor = sorted[sorted.size() / 5];
        auto *row = reinterpret_cast<QRgb *>(waterfall_.scanLine(0));
        for (int i = 0; i < frame.values.size(); ++i) {
            const double v = qBound(0.0, (frame.values[i] - floor) / 40.0, 1.0);
            row[i] = QColor::fromHsvF(0.66 * (1.0 - v), 0.9, 0.12 + 0.88 * v).rgb();
        }
    }
    update();
}

void ReceiverPlot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#080b0e"));
    const QRect area = rect().adjusted(48, 25, -15, -28);
    if (area.width() < 1 || area.height() < 1)
        return;
    p.setPen(QColor("#9aa9b5"));
    p.drawText(12, 17, kind_ == Spectrum ? "SPECTRUM · dB boven ruisvloer" :
               kind_ == Waterfall ? "WATERVAL · nieuwste bovenaan" : "CONSTELLATIE · QPSK");
    if (frame_.values.isEmpty()) {
        p.drawText(area, Qt::AlignCenter, "Wachten op ontvangstdata");
        return;
    }
    if (kind_ == Waterfall) {
        p.drawImage(area, waterfall_);
    } else if (kind_ == Spectrum) {
        QVector<float> sorted = frame_.values;
        std::sort(sorted.begin(), sorted.end());
        const float floor = sorted[sorted.size() / 5];
        for (int db = 0; db <= 40; db += 10) {
            const int y = area.bottom() - db * area.height() / 40;
            p.setPen(QColor("#29333c"));
            p.drawLine(area.left(), y, area.right(), y);
            p.setPen(QColor("#9aa9b5"));
            p.drawText(12, y + 4, QString::number(db));
        }
        QPainterPath path;
        for (int x = 0; x < area.width(); ++x) {
            const int i = x * frame_.values.size() / area.width();
            const double v = qBound(0.0, double(frame_.values[i] - floor), 40.0);
            const QPointF point(area.left() + x, area.bottom() - v * area.height() / 40.0);
            if (!x) path.moveTo(point); else path.lineTo(point);
        }
        p.setPen(QPen(QColor("#43dde0"), 1.3));
        p.drawPath(path);
    } else {
        const int side = qMin(area.width(), area.height());
        const QRect square(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
        p.setPen(QColor("#29333c"));
        p.drawRect(square);
        p.drawLine(square.center().x(), square.top(), square.center().x(), square.bottom());
        p.drawLine(square.left(), square.center().y(), square.right(), square.center().y());
        p.setClipRect(square);
        p.setPen(QColor("#ffe07a"));
        for (int i = 0; i + 1 < frame_.values.size(); i += 2)
            p.drawPoint(QPointF(square.center().x() + frame_.values[i] * side / 4.0,
                               square.center().y() - frame_.values[i + 1] * side / 4.0));
        return;
    }
    p.setPen(QColor("#9aa9b5"));
    const double halfSpan = frame_.sampleRate / 2000000.0;
    p.drawText(area.left(), height() - 8, QString::number(frequency_ - halfSpan, 'f', 3));
    p.drawText(area.center().x() - 45, height() - 8, QString("%1 MHz").arg(frequency_, 0, 'f', 3));
    p.drawText(area.right() - 60, height() - 8, QString::number(frequency_ + halfSpan, 'f', 3));
}
