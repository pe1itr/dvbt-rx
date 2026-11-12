#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

// RBV1 is the existing decoder's little-endian UDP visualization protocol.
struct VisualFrame {
    quint16 type = 0;
    quint32 sampleRate = 0;
    QVector<float> values;
};
bool decodeVisualFrame(const QByteArray &packet, VisualFrame *frame);

class ReceiverPlot : public QWidget {
public:
    enum Kind { Spectrum, Waterfall, Constellation };
    explicit ReceiverPlot(Kind kind, QWidget *parent = nullptr);
    void setFrame(const VisualFrame &frame);
    void clear();
    void setFrequency(double mhz) { frequency_ = mhz; update(); }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    Kind kind_;
    VisualFrame frame_;
    QImage waterfall_;
    double frequency_ = 436.0;
};
