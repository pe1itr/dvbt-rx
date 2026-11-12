#include "video.h"

#include <QGuiApplication>
#ifdef RBDVBT_HAVE_VLC
#include <vlc/vlc.h>
#endif

ReceiverVideo::~ReceiverVideo() { stop(); }

bool ReceiverVideo::start(quint16 port, QWidget *surface, QString *error)
{
    stop();
#ifdef RBDVBT_HAVE_VLC
#ifndef Q_OS_WIN
    if (QGuiApplication::platformName() != "xcb") {
        *error = "Embedded video vereist X11/XWayland. Gebruik de knop Externe speler of start met QT_QPA_PLATFORM=xcb.";
        return false;
    }
#endif
    if (!surface) {
        *error = "VLC heeft geen videovak om in weer te geven.";
        return false;
    }
    const char *args[] = {"--no-video-title-show", "--quiet", "--network-caching=500"};
    instance_ = libvlc_new(3, args);
    if (instance_)
        player_ = libvlc_media_player_new(instance_);
    if (!player_) {
        *error = "VLC kon niet initialiseren; controleer de VLC-installatie en plugins.";
        stop();
        return false;
    }
#ifdef Q_OS_WIN
    libvlc_media_player_set_hwnd(player_, reinterpret_cast<void *>(surface->winId()));
#else
    libvlc_media_player_set_xwindow(player_, uint32_t(surface->winId()));
#endif
    const QByteArray url = QString("udp://@127.0.0.1:%1").arg(port).toUtf8();
    libvlc_media_t *media = libvlc_media_new_location(instance_, url.constData());
    if (!media) {
        *error = "VLC kon de UDP-stream niet openen.";
        stop();
        return false;
    }
    libvlc_media_player_set_media(player_, media);
    libvlc_media_release(media);
    if (libvlc_media_player_play(player_) != 0) {
        *error = "VLC kon de videoweergave niet starten.";
        stop();
        return false;
    }
    return true;
#else
    Q_UNUSED(port)
    Q_UNUSED(surface)
    *error = "Gebouwd zonder libVLC. Gebruik Externe speler voor video en audio.";
    return false;
#endif
}

void ReceiverVideo::stop()
{
#ifdef RBDVBT_HAVE_VLC
    if (player_) {
        libvlc_media_player_stop(player_);
        libvlc_media_player_release(player_);
    }
    if (instance_)
        libvlc_release(instance_);
#endif
    player_ = nullptr;
    instance_ = nullptr;
}
