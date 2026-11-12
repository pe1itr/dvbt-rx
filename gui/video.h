#pragma once

#include <QString>
#include <QWidget>

struct libvlc_instance_t;
struct libvlc_media_player_t;

class ReceiverVideo {
public:
    ~ReceiverVideo();
    bool start(quint16 port, QWidget *surface, QString *error);
    void stop();
private:
    libvlc_instance_t *instance_ = nullptr;
    libvlc_media_player_t *player_ = nullptr;
};
