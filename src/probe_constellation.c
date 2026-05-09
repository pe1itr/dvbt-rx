#include "probe_constellation.h"

#include "dvbt_2k_model.h"
#include "dvbt_outer.h"
#include "iq_input.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifdef RBDVBT_HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_PROBE_MAX_SAMPLES 2097152u
#define STATUS_INPUT_BLOCK_SAMPLES 4096u

typedef struct {
    float re;
    float im;
} complexf_t;

typedef struct {
    int valid;
    uint32_t sample_rate_hz;
    rbdvbt_symbol_rate_t symbol_rate;
    rbdvbt_guard_interval_t guard_interval;
    uint32_t fft_size;
    uint32_t symbol_len;
    uint32_t chunk_output_samples;
    uint32_t start;
    double cfo_hz;
    double cp_score;
} live_sync_hint_t;

static live_sync_hint_t live_sync_hint;
static int live_symbol_continuity_ok;
static size_t live_ofdm_prefix_count;

#define GRDVBT_ML_SYNC_RHO 0.9090909090909091

typedef struct {
    int valid;
    double pilot_lock;
    double snr_db;
    double cfo_hz;
    int32_t bin_shift;
    int32_t symbol_phase;
} live_status_hold_t;

static live_status_hold_t live_status_hold;

typedef struct {
    int valid;
    uint32_t sample_rate_hz;
    uint32_t symbol_len;
    complexf_t *tail;
    size_t tail_count;
    size_t tail_cap;
} live_ofdm_buffer_state_t;

static live_ofdm_buffer_state_t live_ofdm_buffer_state;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_t thread;
    int started;
    int eof;
    rbdvbt_input_format_t format;
    complexf_t *items;
    size_t cap;
    size_t head;
    size_t count;
    uint64_t overrun_samples;
} live_iq_ring_t;

static live_iq_ring_t live_iq_ring = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    0,
    0,
    RBDVBT_INPUT_S16,
    NULL,
    0u,
    0u,
    0u,
    0u
};

#define GUI_CONSTELLATION_MAX_POINTS 6000u
#define GUI_WINDOW_X 80u
#define GUI_WINDOW_Y 80u
#define GUI_WINDOW_GAP 30u
#define GUI_CONSTELLATION_W 420u
#define GUI_CONSTELLATION_H 420u
#define GUI_FIFO_HISTORY_POINTS 720u
#define GUI_FIFO_W 640u
#define GUI_FIFO_H 260u

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    int enabled;
    int started;
    int stop;
    int have_points;
    complexf_t *points;
    size_t point_count;
    char source[32];
    double snr_db;
    double pilot_lock;
    double cfo_hz;
} gui_constellation_state_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    int enabled;
    int started;
    int stop;
    int updated;
    uint32_t depth;
    uint32_t queued;
    uint32_t processing;
    uint32_t capacity;
    uint32_t history[GUI_FIFO_HISTORY_POINTS];
    size_t history_count;
    size_t history_head;
} gui_fifo_state_t;

static gui_constellation_state_t gui_constellation = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    0,
    0,
    0,
    0,
    NULL,
    0u,
    "",
    0.0,
    0.0,
    0.0
};

static gui_fifo_state_t gui_fifo = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    0,
    0,
    0,
    0,
    0u,
    0u,
    0u,
    0u,
    {0u},
    0u,
    0u
};

static complexf_t c_add(complexf_t a, complexf_t b)
{
    complexf_t r;
    r.re = a.re + b.re;
    r.im = a.im + b.im;
    return r;
}

static complexf_t c_mul(complexf_t a, complexf_t b)
{
    complexf_t r;
    r.re = a.re * b.re - a.im * b.im;
    r.im = a.re * b.im + a.im * b.re;
    return r;
}

static complexf_t c_conj(complexf_t a)
{
    complexf_t r;
    r.re = a.re;
    r.im = -a.im;
    return r;
}

static float c_abs2(complexf_t a)
{
    return a.re * a.re + a.im * a.im;
}

static double monotonic_seconds(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

#ifdef RBDVBT_HAVE_X11
static unsigned long gui_alloc_color(Display *display, int screen, const char *name, unsigned long fallback)
{
    XColor exact;
    XColor color;

    if (XAllocNamedColor(display, DefaultColormap(display, screen), name, &color, &exact)) {
        return color.pixel;
    }
    return fallback;
}

static int gui_plot_coord(float value, int span)
{
    const double scale = 1.6;
    double x = ((double)value / scale) * ((double)span * 0.43) + ((double)span * 0.5);

    if (x < 0.0) {
        return 0;
    }
    if (x > (double)(span - 1)) {
        return span - 1;
    }
    return (int)(x + 0.5);
}

static void gui_draw_cross(Display *display, Drawable drawable, GC gc, int x, int y, int radius)
{
    XDrawLine(display, drawable, gc, x - radius, y, x + radius, y);
    XDrawLine(display, drawable, gc, x, y - radius, x, y + radius);
}

static void gui_set_window_geometry(Display *display,
                                    Window window,
                                    int x,
                                    int y,
                                    unsigned int width,
                                    unsigned int height)
{
    XSizeHints hints;

    memset(&hints, 0, sizeof(hints));
    hints.flags = USPosition | PPosition | USSize | PSize;
    hints.x = x;
    hints.y = y;
    hints.width = (int)width;
    hints.height = (int)height;
    XSetWMNormalHints(display, window, &hints);
}

static void *gui_constellation_thread_main(void *arg)
{
    Display *display;
    int screen;
    Window window;
    GC gc;
    Pixmap pixmap;
    unsigned int width = GUI_CONSTELLATION_W;
    unsigned int height = GUI_CONSTELLATION_H;
    unsigned long black;
    unsigned long green;
    unsigned long grid;
    unsigned long white;
    unsigned long yellow;
    Atom wm_delete;
    complexf_t *points = NULL;
    size_t points_cap = 0u;

    (void)arg;

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "[gui] unable to open X11 display; constellation GUI disabled\n");
        pthread_mutex_lock(&gui_constellation.mutex);
        gui_constellation.enabled = 0;
        gui_constellation.started = 0;
        pthread_mutex_unlock(&gui_constellation.mutex);
        return NULL;
    }

    screen = DefaultScreen(display);
    black = BlackPixel(display, screen);
    white = WhitePixel(display, screen);
    green = gui_alloc_color(display, screen, "lime green", white);
    grid = gui_alloc_color(display, screen, "gray25", white);
    yellow = gui_alloc_color(display, screen, "gold", white);

    window = XCreateSimpleWindow(display,
                                 RootWindow(display, screen),
                                 GUI_WINDOW_X,
                                 GUI_WINDOW_Y,
                                 width,
                                 height,
                                 1,
                                 white,
                                 black);
    gui_set_window_geometry(display, window, GUI_WINDOW_X, GUI_WINDOW_Y, width, height);
    XStoreName(display, window, "rbdvbt_rx QPSK constellation");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask);
    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    XMapWindow(display, window);
    XMoveWindow(display, window, GUI_WINDOW_X, GUI_WINDOW_Y);
    gc = XCreateGC(display, window, 0, NULL);
    pixmap = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen));

    while (1) {
        struct timespec deadline;
        size_t point_count;
        char source[32];
        double snr_db;
        double pilot_lock;
        double cfo_hz;

        {
            struct timeval now;

            gettimeofday(&now, NULL);
            deadline.tv_sec = now.tv_sec;
            deadline.tv_nsec = (long)now.tv_usec * 1000L + 50000000L;
        }
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&gui_constellation.mutex);
        while (!gui_constellation.stop && !gui_constellation.have_points) {
            if (pthread_cond_timedwait(&gui_constellation.cond,
                                       &gui_constellation.mutex,
                                       &deadline) != 0) {
                break;
            }
        }
        if (gui_constellation.stop) {
            pthread_mutex_unlock(&gui_constellation.mutex);
            break;
        }
        point_count = gui_constellation.point_count;
        if (point_count > points_cap) {
            complexf_t *new_points = realloc(points, point_count * sizeof(*new_points));

            if (new_points != NULL) {
                points = new_points;
                points_cap = point_count;
            } else {
                point_count = 0u;
            }
        }
        if (point_count > 0u && points != NULL) {
            memcpy(points, gui_constellation.points, point_count * sizeof(*points));
        }
        snr_db = gui_constellation.snr_db;
        pilot_lock = gui_constellation.pilot_lock;
        cfo_hz = gui_constellation.cfo_hz;
        snprintf(source, sizeof(source), "%s", gui_constellation.source);
        gui_constellation.have_points = 0;
        pthread_mutex_unlock(&gui_constellation.mutex);

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == wm_delete) {
                pthread_mutex_lock(&gui_constellation.mutex);
                gui_constellation.enabled = 0;
                gui_constellation.stop = 1;
                pthread_mutex_unlock(&gui_constellation.mutex);
                point_count = 0u;
                break;
            }
            if (event.type == ConfigureNotify) {
                XConfigureEvent *cfg = &event.xconfigure;

                if (cfg->width > 32 && cfg->height > 32 &&
                    ((unsigned int)cfg->width != width || (unsigned int)cfg->height != height)) {
                    width = (unsigned int)cfg->width;
                    height = (unsigned int)cfg->height;
                    XFreePixmap(display, pixmap);
                    pixmap = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen));
                }
            }
        }
        if (gui_constellation.stop) {
            break;
        }

        XSetForeground(display, gc, black);
        XFillRectangle(display, pixmap, gc, 0, 0, width, height);
        XSetForeground(display, gc, grid);
        XDrawLine(display, pixmap, gc, (int)width / 2, 0, (int)width / 2, (int)height);
        XDrawLine(display, pixmap, gc, 0, (int)height / 2, (int)width, (int)height / 2);

        XSetForeground(display, gc, white);
        gui_draw_cross(display, pixmap, gc,
                       gui_plot_coord(0.70710678f, (int)width),
                       (int)height - 1 - gui_plot_coord(0.70710678f, (int)height),
                       5);
        gui_draw_cross(display, pixmap, gc,
                       gui_plot_coord(-0.70710678f, (int)width),
                       (int)height - 1 - gui_plot_coord(0.70710678f, (int)height),
                       5);
        gui_draw_cross(display, pixmap, gc,
                       gui_plot_coord(0.70710678f, (int)width),
                       (int)height - 1 - gui_plot_coord(-0.70710678f, (int)height),
                       5);
        gui_draw_cross(display, pixmap, gc,
                       gui_plot_coord(-0.70710678f, (int)width),
                       (int)height - 1 - gui_plot_coord(-0.70710678f, (int)height),
                       5);

        XSetForeground(display, gc, green);
        for (size_t i = 0; i < point_count; ++i) {
            int x = gui_plot_coord(points[i].re, (int)width);
            int y = (int)height - 1 - gui_plot_coord(points[i].im, (int)height);

            XDrawPoint(display, pixmap, gc, x, y);
        }

        {
            char label[128];

            snprintf(label,
                     sizeof(label),
                     "%s  points=%zu  SNR=%.1fdB  lock=%.3f  CFO=%.1fHz",
                     source[0] != '\0' ? source : "constellation",
                     point_count,
                     snr_db,
                     pilot_lock,
                     cfo_hz);
            XSetForeground(display, gc, yellow);
            XDrawString(display, pixmap, gc, 10, 18, label, (int)strlen(label));
        }

        XCopyArea(display, pixmap, window, gc, 0, 0, width, height, 0, 0);
        XFlush(display);
    }

    free(points);
    XFreePixmap(display, pixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    pthread_mutex_lock(&gui_constellation.mutex);
    gui_constellation.started = 0;
    gui_constellation.enabled = 0;
    pthread_mutex_unlock(&gui_constellation.mutex);
    return NULL;
}

static void *gui_fifo_thread_main(void *arg)
{
    Display *display;
    int screen;
    Window window;
    GC gc;
    Pixmap pixmap;
    unsigned int width = GUI_FIFO_W;
    unsigned int height = GUI_FIFO_H;
    unsigned long black;
    unsigned long green;
    unsigned long grid;
    unsigned long white;
    unsigned long yellow;
    Atom wm_delete;
    uint32_t history[GUI_FIFO_HISTORY_POINTS];
    size_t history_count = 0u;
    uint32_t depth = 0u;
    uint32_t queued = 0u;
    uint32_t processing = 0u;
    uint32_t capacity = 1u;

    (void)arg;

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "[gui] unable to open X11 display; FIFO1 GUI disabled\n");
        pthread_mutex_lock(&gui_fifo.mutex);
        gui_fifo.enabled = 0;
        gui_fifo.started = 0;
        pthread_mutex_unlock(&gui_fifo.mutex);
        return NULL;
    }

    screen = DefaultScreen(display);
    black = BlackPixel(display, screen);
    white = WhitePixel(display, screen);
    green = gui_alloc_color(display, screen, "lime green", white);
    grid = gui_alloc_color(display, screen, "gray25", white);
    yellow = gui_alloc_color(display, screen, "gold", white);

    window = XCreateSimpleWindow(display,
                                 RootWindow(display, screen),
                                 GUI_WINDOW_X + GUI_CONSTELLATION_W + GUI_WINDOW_GAP,
                                 GUI_WINDOW_Y,
                                 width,
                                 height,
                                 1,
                                 white,
                                 black);
    gui_set_window_geometry(display,
                            window,
                            GUI_WINDOW_X + GUI_CONSTELLATION_W + GUI_WINDOW_GAP,
                            GUI_WINDOW_Y,
                            width,
                            height);
    XStoreName(display, window, "rbdvbt_rx FIFO1 depth");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask);
    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    XMapWindow(display, window);
    XMoveWindow(display, window, GUI_WINDOW_X + GUI_CONSTELLATION_W + GUI_WINDOW_GAP, GUI_WINDOW_Y);
    gc = XCreateGC(display, window, 0, NULL);
    pixmap = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen));

    while (1) {
        struct timespec deadline;

        {
            struct timeval now;

            gettimeofday(&now, NULL);
            deadline.tv_sec = now.tv_sec;
            deadline.tv_nsec = (long)now.tv_usec * 1000L + 100000000L;
        }
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&gui_fifo.mutex);
        while (!gui_fifo.stop && !gui_fifo.updated) {
            if (pthread_cond_timedwait(&gui_fifo.cond,
                                       &gui_fifo.mutex,
                                       &deadline) != 0) {
                break;
            }
        }
        if (gui_fifo.stop) {
            pthread_mutex_unlock(&gui_fifo.mutex);
            break;
        }
        depth = gui_fifo.depth;
        queued = gui_fifo.queued;
        processing = gui_fifo.processing;
        capacity = gui_fifo.capacity != 0u ? gui_fifo.capacity : 1u;
        history_count = gui_fifo.history_count;
        if (history_count > GUI_FIFO_HISTORY_POINTS) {
            history_count = GUI_FIFO_HISTORY_POINTS;
        }
        for (size_t i = 0; i < history_count; ++i) {
            size_t pos = (gui_fifo.history_head + GUI_FIFO_HISTORY_POINTS - history_count + i) %
                GUI_FIFO_HISTORY_POINTS;

            history[i] = gui_fifo.history[pos];
        }
        gui_fifo.updated = 0;
        pthread_mutex_unlock(&gui_fifo.mutex);

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == wm_delete) {
                pthread_mutex_lock(&gui_fifo.mutex);
                gui_fifo.enabled = 0;
                gui_fifo.stop = 1;
                pthread_mutex_unlock(&gui_fifo.mutex);
                break;
            }
            if (event.type == ConfigureNotify) {
                XConfigureEvent *cfg = &event.xconfigure;

                if (cfg->width > 64 && cfg->height > 64 &&
                    ((unsigned int)cfg->width != width || (unsigned int)cfg->height != height)) {
                    width = (unsigned int)cfg->width;
                    height = (unsigned int)cfg->height;
                    XFreePixmap(display, pixmap);
                    pixmap = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen));
                }
            }
        }
        if (gui_fifo.stop) {
            break;
        }

        XSetForeground(display, gc, black);
        XFillRectangle(display, pixmap, gc, 0, 0, width, height);

        XSetForeground(display, gc, grid);
        for (unsigned int i = 1u; i < 4u; ++i) {
            int y = (int)((height - 34u) * i / 4u) + 24;
            XDrawLine(display, pixmap, gc, 0, y, (int)width, y);
        }
        XDrawRectangle(display, pixmap, gc, 0, 24, width > 1u ? width - 1u : 0u, height > 35u ? height - 35u : 0u);

        if (history_count > 1u) {
            unsigned int graph_h = height > 36u ? height - 36u : 1u;
            unsigned int graph_y = 24u;
            double x_scale = history_count > 1u ?
                (double)(width - 1u) / (double)(history_count - 1u) :
                1.0;

            XSetForeground(display, gc, green);
            for (size_t i = 1u; i < history_count; ++i) {
                double prev_fill = (double)history[i - 1u] / (double)capacity;
                double fill = (double)history[i] / (double)capacity;
                int x0 = (int)((double)(i - 1u) * x_scale + 0.5);
                int x1 = (int)((double)i * x_scale + 0.5);
                int y0;
                int y1;

                if (prev_fill > 1.0) {
                    prev_fill = 1.0;
                }
                if (fill > 1.0) {
                    fill = 1.0;
                }
                y0 = (int)(graph_y + (double)graph_h * (1.0 - prev_fill) + 0.5);
                y1 = (int)(graph_y + (double)graph_h * (1.0 - fill) + 0.5);
                XDrawLine(display, pixmap, gc, x0, y0, x1, y1);
            }
        }

        {
            char label[128];
            double fill_pct = capacity != 0u ? (100.0 * (double)depth / (double)capacity) : 0.0;

            if (fill_pct > 100.0) {
                fill_pct = 100.0;
            }
            snprintf(label,
                     sizeof(label),
                     "FIFO1 load: %u / %u  queued=%u processing=%u  %.0f%%",
                     depth,
                     capacity,
                     queued,
                     processing,
                     fill_pct);
            XSetForeground(display, gc, yellow);
            XDrawString(display, pixmap, gc, 10, 18, label, (int)strlen(label));
        }

        XCopyArea(display, pixmap, window, gc, 0, 0, width, height, 0, 0);
        XFlush(display);
    }

    XFreePixmap(display, pixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    pthread_mutex_lock(&gui_fifo.mutex);
    gui_fifo.started = 0;
    gui_fifo.enabled = 0;
    pthread_mutex_unlock(&gui_fifo.mutex);
    return NULL;
}
#endif

static void gui_fifo_submit(uint32_t queued, uint32_t processing, uint32_t capacity)
{
    uint32_t depth = queued + processing;

    pthread_mutex_lock(&gui_fifo.mutex);
    if (!gui_fifo.enabled) {
        pthread_mutex_unlock(&gui_fifo.mutex);
        return;
    }
    gui_fifo.depth = depth;
    gui_fifo.queued = queued;
    gui_fifo.processing = processing;
    gui_fifo.capacity = capacity;
    gui_fifo.history[gui_fifo.history_head] = depth;
    gui_fifo.history_head = (gui_fifo.history_head + 1u) % GUI_FIFO_HISTORY_POINTS;
    if (gui_fifo.history_count < GUI_FIFO_HISTORY_POINTS) {
        gui_fifo.history_count++;
    }
    gui_fifo.updated = 1;
    pthread_cond_signal(&gui_fifo.cond);
    pthread_mutex_unlock(&gui_fifo.mutex);
}

static int gui_fifo_start(int enabled)
{
    if (!enabled) {
        return 0;
    }
#ifndef RBDVBT_HAVE_X11
    return 0;
#else
    pthread_mutex_lock(&gui_fifo.mutex);
    if (gui_fifo.started || gui_fifo.enabled) {
        pthread_mutex_unlock(&gui_fifo.mutex);
        return 0;
    }
    gui_fifo.depth = 0u;
    gui_fifo.queued = 0u;
    gui_fifo.processing = 0u;
    gui_fifo.capacity = 1u;
    gui_fifo.history_count = 0u;
    gui_fifo.history_head = 0u;
    gui_fifo.updated = 1;
    gui_fifo.stop = 0;
    gui_fifo.enabled = 1;
    gui_fifo.started = 1;
    if (pthread_create(&gui_fifo.thread, NULL, gui_fifo_thread_main, NULL) != 0) {
        gui_fifo.enabled = 0;
        gui_fifo.started = 0;
        pthread_mutex_unlock(&gui_fifo.mutex);
        fprintf(stderr, "[gui] failed to start FIFO1 GUI thread\n");
        return -1;
    }
    pthread_detach(gui_fifo.thread);
    pthread_mutex_unlock(&gui_fifo.mutex);
    return 0;
#endif
}

static int gui_constellation_start(int enabled)
{
    if (!enabled) {
        return 0;
    }
#ifndef RBDVBT_HAVE_X11
    static int warned;

    if (!warned) {
        fprintf(stderr, "[gui] X11 support was not available at build time; --gui ignored\n");
        warned = 1;
    }
    return 0;
#else
    pthread_mutex_lock(&gui_constellation.mutex);
    if (gui_constellation.started || gui_constellation.enabled) {
        pthread_mutex_unlock(&gui_constellation.mutex);
        return 0;
    }
    gui_constellation.points = malloc(GUI_CONSTELLATION_MAX_POINTS * sizeof(*gui_constellation.points));
    if (gui_constellation.points == NULL) {
        pthread_mutex_unlock(&gui_constellation.mutex);
        fprintf(stderr, "[gui] failed to allocate constellation point buffer\n");
        return -1;
    }
    gui_constellation.point_count = 0u;
    gui_constellation.have_points = 0;
    gui_constellation.stop = 0;
    gui_constellation.enabled = 1;
    gui_constellation.started = 1;
    if (pthread_create(&gui_constellation.thread, NULL, gui_constellation_thread_main, NULL) != 0) {
        gui_constellation.enabled = 0;
        gui_constellation.started = 0;
        free(gui_constellation.points);
        gui_constellation.points = NULL;
        pthread_mutex_unlock(&gui_constellation.mutex);
        fprintf(stderr, "[gui] failed to start constellation GUI thread\n");
        return -1;
    }
    pthread_detach(gui_constellation.thread);
    pthread_mutex_unlock(&gui_constellation.mutex);
    return 0;
#endif
}

static void gui_constellation_submit(const complexf_t *points,
                                     size_t point_count,
                                     const char *source,
                                     double snr_db,
                                     double pilot_lock,
                                     double cfo_hz)
{
    size_t stride;
    size_t out_count = 0u;

    if (points == NULL || point_count == 0u) {
        return;
    }

    pthread_mutex_lock(&gui_constellation.mutex);
    if (!gui_constellation.enabled || gui_constellation.points == NULL) {
        pthread_mutex_unlock(&gui_constellation.mutex);
        return;
    }
    stride = point_count / GUI_CONSTELLATION_MAX_POINTS;
    if (stride == 0u) {
        stride = 1u;
    }
    for (size_t i = 0; i < point_count && out_count < GUI_CONSTELLATION_MAX_POINTS; i += stride) {
        gui_constellation.points[out_count++] = points[i];
    }
    gui_constellation.point_count = out_count;
    snprintf(gui_constellation.source,
             sizeof(gui_constellation.source),
             "%s",
             source != NULL ? source : "constellation");
    gui_constellation.snr_db = snr_db;
    gui_constellation.pilot_lock = pilot_lock;
    gui_constellation.cfo_hz = cfo_hz;
    gui_constellation.have_points = 1;
    pthread_cond_signal(&gui_constellation.cond);
    pthread_mutex_unlock(&gui_constellation.mutex);
}

static complexf_t c_rotate(complexf_t a, double phase)
{
    complexf_t r;
    double cs = cos(phase);
    double sn = sin(phase);
    r.re = (float)(a.re * cs - a.im * sn);
    r.im = (float)(a.re * sn + a.im * cs);
    return r;
}

static uint32_t guard_samples(uint32_t fft_size, rbdvbt_guard_interval_t gi)
{
    switch (gi) {
    case RBDVBT_GI_AUTO:
        return 0;
    case RBDVBT_GI_1_8:
        return fft_size / 8u;
    case RBDVBT_GI_1_16:
        return fft_size / 16u;
    case RBDVBT_GI_1_32:
        return fft_size / 32u;
    }

    return 0;
}

static uint32_t find_symbol_start(const complexf_t *samples,
                                  size_t count,
                                  uint32_t fft_size,
                                  uint32_t gi_len,
                                  uint32_t symbol_len,
                                  uint32_t max_symbols,
                                  double *out_score,
                                  complexf_t *out_corr);

static rbdvbt_guard_interval_t select_guard_interval_auto(const rbdvbt_config_t *cfg,
                                                          const complexf_t *samples,
                                                          size_t count,
                                                          uint32_t fft_size)
{
    static const rbdvbt_guard_interval_t candidates[] = {
        RBDVBT_GI_1_32,
        RBDVBT_GI_1_16,
        RBDVBT_GI_1_8
    };
    rbdvbt_guard_interval_t best_gi = RBDVBT_GI_1_32;
    double best_score = -1.0;
    uint32_t best_start = 0;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint32_t gi_len = guard_samples(fft_size, candidates[i]);
        uint32_t symbol_len = fft_size + gi_len;
        uint32_t scan_symbols = cfg->probe_symbols > 200u ? 200u : cfg->probe_symbols;
        complexf_t corr = {0.0f, 0.0f};
        double score = 0.0;
        uint32_t start;

        if (gi_len == 0 || count < (size_t)symbol_len * 4u) {
            continue;
        }
        if ((size_t)scan_symbols * symbol_len > count) {
            scan_symbols = (uint32_t)(count / symbol_len);
        }
        if (scan_symbols == 0u) {
            continue;
        }

        start = find_symbol_start(samples,
                                  count,
                                  fft_size,
                                  gi_len,
                                  symbol_len,
                                  scan_symbols,
                                  &score,
                                  &corr);

        fprintf(stderr,
                "[auto-gi] candidate=%s gi_samples=%u symbol_samples=%u start=%u cp_score=%.5f\n",
                rbdvbt_guard_interval_name(candidates[i]),
                gi_len,
                symbol_len,
                start,
                score);

        if (best_score < 0.0 || score > best_score * 1.02) {
            best_score = score;
            best_gi = candidates[i];
            best_start = start;
        }
    }

    fprintf(stderr,
            "[auto-gi] selected=%s start=%u cp_score=%.5f\n",
            rbdvbt_guard_interval_name(best_gi),
            best_start,
            best_score);

    return best_gi;
}

static void init_status_context_from_config(const rbdvbt_config_t *cfg,
                                            rbdvbt_status_context_t *status,
                                            char *symbol_rate,
                                            size_t symbol_rate_len)
{
    memset(status, 0, sizeof(*status));
    snprintf(symbol_rate, symbol_rate_len, "%uks", (unsigned)(cfg->symbol_rate / 1000u));
    status->status_json_path = cfg->status_json;
    status->status_period_packets = cfg->status_period_packets;
    status->symbol_rate = symbol_rate;
    status->guard_interval = rbdvbt_guard_interval_name(cfg->guard_interval);
    status->fec = rbdvbt_fec_name(cfg->fec);
    status->modulation = "dvb-t";
    status->fft_mode = "2k";
    status->constellation = "QPSK";
    status->snr_db = 0.0;
    status->pilot_lock = 0.0;
    status->cfo_hz = 0.0;
    status->bin_shift = 0;
    status->symbol_phase = 0;
    status->input_samples = 0u;
    status->lock_quality = 0u;
    status->ssi = 0u;
    status->wait_video_start = cfg->wait_video_start;
    status->live_mode = cfg->live_mode;
    status->gui_enabled = cfg->gui;

    if (cfg->live_mode && live_status_hold.valid) {
        status->pilot_lock = live_status_hold.pilot_lock;
        status->snr_db = live_status_hold.snr_db;
        status->cfo_hz = live_status_hold.cfo_hz;
        status->bin_shift = live_status_hold.bin_shift;
        status->symbol_phase = live_status_hold.symbol_phase;
        status->lock_quality = (uint32_t)(live_status_hold.pilot_lock * 100.0 + 0.5);
        if (status->lock_quality > 100u) {
            status->lock_quality = 100u;
        }
    }
}

static int live_iq_ring_start(rbdvbt_input_format_t format, size_t chunk_samples);
static size_t live_iq_ring_read(complexf_t *out, size_t want, uint64_t *overrun_delta);

static int read_probe_samples(const rbdvbt_config_t *cfg,
                              complexf_t **out_samples,
                              size_t *out_count,
                              rbdvbt_iq_stats_t *stats)
{
    uint64_t sample_limit = cfg->max_samples != 0 ? cfg->max_samples : DEFAULT_PROBE_MAX_SAMPLES;
    complexf_t *samples = calloc((size_t)sample_limit, sizeof(*samples));
    rbdvbt_complex_t block[4096];
    size_t count = 0;
    uint64_t status_period_samples = (uint64_t)cfg->status_period_packets * STATUS_INPUT_BLOCK_SAMPLES;
    uint64_t next_status_samples = status_period_samples;
    rbdvbt_status_context_t status;
    char status_symbol_rate[16];

    if (samples == NULL) {
        fprintf(stderr, "failed to allocate probe sample buffer\n");
        return -1;
    }
    if (cfg->live_mode && live_iq_ring_start(cfg->input_format, (size_t)sample_limit) != 0) {
        free(samples);
        return -1;
    }

    rbdvbt_iq_stats_init(stats);
    init_status_context_from_config(cfg, &status, status_symbol_rate, sizeof(status_symbol_rate));
    rbdvbt_status_publish_idle(&status, "start", 0);

    while (count < sample_limit) {
        size_t want = 4096;
        size_t got;
        size_t n;

        if (sample_limit - count < want) {
            want = (size_t)(sample_limit - count);
        }

        if (cfg->live_mode) {
            uint64_t overrun_delta = 0u;

            got = live_iq_ring_read(&samples[count], want, &overrun_delta);
            if (overrun_delta != 0u) {
                fprintf(stderr,
                        "[iqring] overrun dropped_samples=%llu\n",
                        (unsigned long long)overrun_delta);
            }
        } else {
            got = rbdvbt_read_iq(stdin, cfg->input_format, block, want);
            rbdvbt_iq_stats_update(stats, block, got);
            for (n = 0; n < got; ++n) {
                samples[count + n].re = block[n].i;
                samples[count + n].im = block[n].q;
            }
        }
        if (got == 0) {
            break;
        }

        if (cfg->live_mode) {
            rbdvbt_complex_t stats_block[4096];
            size_t stats_pos = 0u;

            while (stats_pos < got) {
                size_t stats_n = got - stats_pos;

                if (stats_n > 4096u) {
                    stats_n = 4096u;
                }
                for (n = 0; n < stats_n; ++n) {
                    stats_block[n].i = samples[count + stats_pos + n].re;
                    stats_block[n].q = samples[count + stats_pos + n].im;
                }
                rbdvbt_iq_stats_update(stats, stats_block, stats_n);
                stats_pos += stats_n;
            }
        }
        count += got;

        while (status_period_samples != 0u && (uint64_t)count >= next_status_samples) {
            rbdvbt_status_publish_idle(&status, "input", (uint64_t)count);
            next_status_samples += status_period_samples;
        }
    }

    rbdvbt_status_publish_idle(&status, "input-done", (uint64_t)count);
    *out_samples = samples;
    *out_count = count;
    return 0;
}

static void live_iq_ring_push(const rbdvbt_complex_t *samples, size_t count)
{
    size_t n;

    pthread_mutex_lock(&live_iq_ring.mutex);
    if (live_iq_ring.items == NULL || live_iq_ring.cap == 0u) {
        pthread_mutex_unlock(&live_iq_ring.mutex);
        return;
    }

    if (count > live_iq_ring.cap) {
        samples += count - live_iq_ring.cap;
        live_iq_ring.overrun_samples += count - live_iq_ring.cap;
        count = live_iq_ring.cap;
    }
    if (live_iq_ring.count + count > live_iq_ring.cap) {
        size_t drop = live_iq_ring.count + count - live_iq_ring.cap;

        live_iq_ring.head = (live_iq_ring.head + drop) % live_iq_ring.cap;
        live_iq_ring.count -= drop;
        live_iq_ring.overrun_samples += drop;
    }

    for (n = 0; n < count; ++n) {
        size_t pos = (live_iq_ring.head + live_iq_ring.count) % live_iq_ring.cap;

        live_iq_ring.items[pos].re = samples[n].i;
        live_iq_ring.items[pos].im = samples[n].q;
        live_iq_ring.count++;
    }
    pthread_cond_broadcast(&live_iq_ring.not_empty);
    pthread_mutex_unlock(&live_iq_ring.mutex);
}

static void *live_iq_ring_reader_main(void *arg)
{
    (void)arg;

    for (;;) {
        rbdvbt_complex_t block[4096];
        size_t got = rbdvbt_read_iq(stdin, live_iq_ring.format, block, 4096u);

        if (got == 0u) {
            pthread_mutex_lock(&live_iq_ring.mutex);
            live_iq_ring.eof = 1;
            pthread_cond_broadcast(&live_iq_ring.not_empty);
            pthread_mutex_unlock(&live_iq_ring.mutex);
            return NULL;
        }
        live_iq_ring_push(block, got);
    }
}

static int live_iq_ring_start(rbdvbt_input_format_t format, size_t chunk_samples)
{
    size_t cap = chunk_samples * 4u;

    if (cap < chunk_samples + 4096u) {
        cap = chunk_samples + 4096u;
    }
    if (cap < 16384u) {
        cap = 16384u;
    }

    pthread_mutex_lock(&live_iq_ring.mutex);
    if (live_iq_ring.started) {
        pthread_mutex_unlock(&live_iq_ring.mutex);
        return 0;
    }

    live_iq_ring.items = calloc(cap, sizeof(*live_iq_ring.items));
    if (live_iq_ring.items == NULL) {
        pthread_mutex_unlock(&live_iq_ring.mutex);
        fprintf(stderr, "failed to allocate live IQ ring buffer\n");
        return -1;
    }
    live_iq_ring.cap = cap;
    live_iq_ring.head = 0u;
    live_iq_ring.count = 0u;
    live_iq_ring.eof = 0;
    live_iq_ring.format = format;
    live_iq_ring.overrun_samples = 0u;
    live_iq_ring.started = 1;
    pthread_mutex_unlock(&live_iq_ring.mutex);

    if (pthread_create(&live_iq_ring.thread, NULL, live_iq_ring_reader_main, NULL) != 0) {
        pthread_mutex_lock(&live_iq_ring.mutex);
        live_iq_ring.started = 0;
        free(live_iq_ring.items);
        live_iq_ring.items = NULL;
        live_iq_ring.cap = 0u;
        pthread_mutex_unlock(&live_iq_ring.mutex);
        fprintf(stderr, "failed to start live IQ reader thread\n");
        return -1;
    }
    pthread_detach(live_iq_ring.thread);
    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        fprintf(stderr, "[iqring] started capacity_samples=%zu chunk_samples=%zu\n", cap, chunk_samples);
    }
    return 0;
}

static size_t live_iq_ring_read(complexf_t *out, size_t want, uint64_t *overrun_delta)
{
    size_t got = 0u;
    uint64_t overrun_before;

    pthread_mutex_lock(&live_iq_ring.mutex);
    overrun_before = live_iq_ring.overrun_samples;
    while (got < want) {
        while (live_iq_ring.count == 0u && !live_iq_ring.eof) {
            pthread_cond_wait(&live_iq_ring.not_empty, &live_iq_ring.mutex);
        }
        if (live_iq_ring.count == 0u && live_iq_ring.eof) {
            break;
        }
        while (got < want && live_iq_ring.count > 0u) {
            out[got++] = live_iq_ring.items[live_iq_ring.head];
            live_iq_ring.head = (live_iq_ring.head + 1u) % live_iq_ring.cap;
            live_iq_ring.count--;
        }
    }
    if (overrun_delta != NULL) {
        *overrun_delta = live_iq_ring.overrun_samples - overrun_before;
    }
    pthread_mutex_unlock(&live_iq_ring.mutex);
    return got;
}

static void remove_dc(complexf_t *samples, size_t count)
{
    double mean_re = 0.0;
    double mean_im = 0.0;
    size_t n;

    if (count == 0) {
        return;
    }

    for (n = 0; n < count; ++n) {
        mean_re += samples[n].re;
        mean_im += samples[n].im;
    }
    mean_re /= (double)count;
    mean_im /= (double)count;

    for (n = 0; n < count; ++n) {
        samples[n].re -= (float)mean_re;
        samples[n].im -= (float)mean_im;
    }
}

static double sinc_norm(double x)
{
    if (fabs(x) < 1e-12) {
        return 1.0;
    }

    return sin(M_PI * x) / (M_PI * x);
}

typedef struct {
    int valid;
    uint32_t input_rate_hz;
    uint32_t output_rate_hz;
    double ratio;
    double next_x;
    uint64_t output_seq;
    complexf_t tail[64];
    size_t tail_count;
} live_resampler_state_t;

static live_resampler_state_t live_resampler_state;
static uint64_t live_resampled_chunk_start_abs;
static size_t live_resampled_chunk_count;

typedef struct {
    int valid;
    uint32_t sample_rate_hz;
    uint32_t symbol_len;
    uint64_t symbol_seq;
    uint64_t next_symbol_sample_abs;
    uint64_t last_symbol_sample_abs;
    double cfo_hz;
    int bin_shift;
    int conjugate;
    int symbol_phase_mod4;
    double pilot_lock;
} live_frontend_cursor_t;

static live_frontend_cursor_t live_frontend_cursor;
static uint64_t live_soft_next_symbol_seq;
static const char *live_frontend_sync_mode = "init";

static int resample_sinc_ratio(const complexf_t *in,
                               size_t in_count,
                               double ratio,
                               complexf_t **out_samples,
                               size_t *out_count)
{
    const int radius = 12;
    double cutoff = ratio < 1.0 ? ratio * 0.5 : 0.5;
    size_t output_count;
    complexf_t *out;
    size_t m;

    if (in_count == 0 || ratio <= 0.0) {
        return -1;
    }

    output_count = (size_t)floor((double)in_count * ratio);
    if (output_count == 0) {
        return -1;
    }
    out = calloc(output_count, sizeof(*out));
    if (out == NULL) {
        fprintf(stderr, "failed to allocate resample buffer\n");
        return -1;
    }

    for (m = 0; m < output_count; ++m) {
        double x = (double)m / ratio;
        int center = (int)floor(x);
        double acc_re = 0.0;
        double acc_im = 0.0;
        double acc_w = 0.0;
        int k;

        for (k = center - radius; k <= center + radius; ++k) {
            double d;
            double wpos;
            double window;
            double h;

            if (k < 0 || k >= (int)in_count) {
                continue;
            }

            d = x - (double)k;
            wpos = ((double)(k - (center - radius))) / (double)(radius * 2);
            window = 0.5 - 0.5 * cos(2.0 * M_PI * wpos);
            h = 2.0 * cutoff * sinc_norm(2.0 * cutoff * d) * window;

            acc_re += (double)in[k].re * h;
            acc_im += (double)in[k].im * h;
            acc_w += h;
        }

        if (fabs(acc_w) > 1e-12) {
            out[m].re = (float)(acc_re / acc_w);
            out[m].im = (float)(acc_im / acc_w);
        }
    }

    *out_samples = out;
    *out_count = output_count;
    return 0;
}

static int resample_sinc_ratio_live(const complexf_t *in,
                                    size_t in_count,
                                    double ratio,
                                    uint32_t input_rate_hz,
                                    uint32_t output_rate_hz,
                                    complexf_t **out_samples,
                                    size_t *out_count)
{
    const int radius = 12;
    double cutoff = ratio < 1.0 ? ratio * 0.5 : 0.5;
    complexf_t *work = NULL;
    complexf_t *out = NULL;
    size_t work_count;
    size_t out_cap;
    size_t out_n = 0;

    if (in_count == 0 || ratio <= 0.0) {
        return -1;
    }

    if (!live_resampler_state.valid ||
        live_resampler_state.input_rate_hz != input_rate_hz ||
        live_resampler_state.output_rate_hz != output_rate_hz ||
        fabs(live_resampler_state.ratio - ratio) > 1e-12) {
        memset(&live_resampler_state, 0, sizeof(live_resampler_state));
        live_resampler_state.valid = 1;
        live_resampler_state.input_rate_hz = input_rate_hz;
        live_resampler_state.output_rate_hz = output_rate_hz;
        live_resampler_state.ratio = ratio;
        live_resampler_state.next_x = 0.0;
        live_resampler_state.output_seq = 0u;
        live_frontend_cursor.valid = 0;
        live_soft_next_symbol_seq = 0u;
    }

    work_count = live_resampler_state.tail_count + in_count;
    work = calloc(work_count, sizeof(*work));
    if (work == NULL) {
        fprintf(stderr, "failed to allocate live resample work buffer\n");
        return -1;
    }
    if (live_resampler_state.tail_count > 0u) {
        memcpy(work,
               live_resampler_state.tail,
               live_resampler_state.tail_count * sizeof(*work));
    }
    memcpy(&work[live_resampler_state.tail_count], in, in_count * sizeof(*work));

    out_cap = (size_t)ceil(((double)in_count + 128.0) * ratio) + 128u;
    if (out_cap == 0u) {
        out_cap = 1u;
    }
    out = calloc(out_cap, sizeof(*out));
    if (out == NULL) {
        fprintf(stderr, "failed to allocate live resample buffer\n");
        free(work);
        return -1;
    }

    while (live_resampler_state.next_x + (double)radius < (double)in_count) {
        double x = (double)live_resampler_state.tail_count + live_resampler_state.next_x;
        int center = (int)floor(x);
        double acc_re = 0.0;
        double acc_im = 0.0;
        double acc_w = 0.0;

        if (out_n >= out_cap) {
            size_t new_cap = out_cap * 2u;
            complexf_t *new_out = realloc(out, new_cap * sizeof(*out));

            if (new_out == NULL) {
                fprintf(stderr, "failed to grow live resample buffer\n");
                free(work);
                free(out);
                return -1;
            }
            out = new_out;
            memset(&out[out_cap], 0, (new_cap - out_cap) * sizeof(*out));
            out_cap = new_cap;
        }

        for (int k = center - radius; k <= center + radius; ++k) {
            double d;
            double wpos;
            double window;
            double h;

            if (k < 0 || k >= (int)work_count) {
                continue;
            }

            d = x - (double)k;
            wpos = ((double)(k - (center - radius))) / (double)(radius * 2);
            window = 0.5 - 0.5 * cos(2.0 * M_PI * wpos);
            h = 2.0 * cutoff * sinc_norm(2.0 * cutoff * d) * window;

            acc_re += (double)work[k].re * h;
            acc_im += (double)work[k].im * h;
            acc_w += h;
        }

        if (fabs(acc_w) > 1e-12) {
            out[out_n].re = (float)(acc_re / acc_w);
            out[out_n].im = (float)(acc_im / acc_w);
        }
        out_n++;
        live_resampler_state.next_x += 1.0 / ratio;
    }

    live_resampler_state.next_x -= (double)in_count;
    live_resampler_state.tail_count = in_count < (sizeof(live_resampler_state.tail) / sizeof(live_resampler_state.tail[0])) ?
        in_count :
        (sizeof(live_resampler_state.tail) / sizeof(live_resampler_state.tail[0]));
    if (live_resampler_state.tail_count > 0u) {
        memcpy(live_resampler_state.tail,
               &in[in_count - live_resampler_state.tail_count],
               live_resampler_state.tail_count * sizeof(live_resampler_state.tail[0]));
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        fprintf(stderr,
                "[resample] live-state out_abs=%llu tail=%zu next_x=%.6f emitted=%zu\n",
                (unsigned long long)live_resampler_state.output_seq,
                live_resampler_state.tail_count,
                live_resampler_state.next_x,
                out_n);
    }

    free(work);
    *out_samples = out;
    *out_count = out_n;
    live_resampled_chunk_start_abs = live_resampler_state.output_seq;
    live_resampled_chunk_count = out_n;
    live_resampler_state.output_seq += out_n;
    return out_n > 0u ? 0 : -1;
}

static int resample_x4_sinc(const complexf_t *in,
                            size_t in_count,
                            complexf_t **out_samples,
                            size_t *out_count)
{
    if (in_count > (SIZE_MAX / 4u)) {
        return -1;
    }

    return resample_sinc_ratio(in, in_count, 4.0, out_samples, out_count);
}

static int live_ofdm_prepend_tail(const rbdvbt_config_t *cfg,
                                  complexf_t **samples,
                                  size_t *count,
                                  uint32_t symbol_len)
{
    size_t keep;
    size_t prefix;
    complexf_t *merged = NULL;
    const complexf_t *src;

    live_ofdm_prefix_count = 0u;

    if (!cfg->live_mode || symbol_len == 0u || *samples == NULL || *count == 0u) {
        return 0;
    }

    keep = (size_t)symbol_len * 2u;
    if (!live_ofdm_buffer_state.valid ||
        live_ofdm_buffer_state.sample_rate_hz != cfg->sample_rate_hz ||
        live_ofdm_buffer_state.symbol_len != symbol_len) {
        live_ofdm_buffer_state.valid = 1;
        live_ofdm_buffer_state.sample_rate_hz = cfg->sample_rate_hz;
        live_ofdm_buffer_state.symbol_len = symbol_len;
        live_ofdm_buffer_state.tail_count = 0u;
    }

    if (live_ofdm_buffer_state.tail_cap < keep) {
        complexf_t *new_tail = realloc(live_ofdm_buffer_state.tail, keep * sizeof(*new_tail));

        if (new_tail == NULL) {
            fprintf(stderr, "failed to allocate live OFDM ring tail\n");
            return -1;
        }
        live_ofdm_buffer_state.tail = new_tail;
        live_ofdm_buffer_state.tail_cap = keep;
        if (live_ofdm_buffer_state.tail_count > keep) {
            live_ofdm_buffer_state.tail_count = keep;
        }
    }

    prefix = live_ofdm_buffer_state.tail_count;
    if (prefix > 0u) {
        merged = calloc(prefix + *count, sizeof(*merged));
        if (merged == NULL) {
            fprintf(stderr, "failed to allocate live OFDM ring buffer\n");
            return -1;
        }
        memcpy(merged, live_ofdm_buffer_state.tail, prefix * sizeof(*merged));
        memcpy(&merged[prefix], *samples, *count * sizeof(*merged));
        free(*samples);
        *samples = merged;
        *count += prefix;
        live_ofdm_prefix_count = prefix;
        if (live_resampled_chunk_start_abs >= (uint64_t)prefix) {
            live_resampled_chunk_start_abs -= (uint64_t)prefix;
        } else {
            live_resampled_chunk_start_abs = 0u;
        }
        live_resampled_chunk_count += prefix;
    }

    src = *samples;
    live_ofdm_buffer_state.tail_count = *count < keep ? *count : keep;
    if (live_ofdm_buffer_state.tail_count > 0u) {
        memcpy(live_ofdm_buffer_state.tail,
               &src[*count - live_ofdm_buffer_state.tail_count],
               live_ofdm_buffer_state.tail_count * sizeof(*src));
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        fprintf(stderr,
                "[ofdm-live] prefix=%zu tail=%zu symbol_len=%u\n",
                live_ofdm_prefix_count,
                live_ofdm_buffer_state.tail_count,
                symbol_len);
    }
    return 0;
}

static double cp_score_at(const complexf_t *samples,
                          size_t count,
                          uint32_t fft_size,
                          uint32_t gi_len,
                          uint32_t symbol_len,
                          uint32_t offset,
                          uint32_t max_symbols,
                          complexf_t *out_corr)
{
    complexf_t corr = {0.0f, 0.0f};
    double p0 = 0.0;
    double p1 = 0.0;
    uint32_t symbols = 0;
    uint32_t s;

    for (s = 0; s < max_symbols; ++s) {
        size_t base = (size_t)offset + (size_t)s * symbol_len;
        uint32_t k;

        if (base + fft_size + gi_len >= count) {
            break;
        }

        for (k = 0; k < gi_len; ++k) {
            complexf_t a = samples[base + k];
            complexf_t b = samples[base + fft_size + k];
            corr = c_add(corr, c_mul(a, c_conj(b)));
            p0 += c_abs2(a);
            p1 += c_abs2(b);
        }
        symbols++;
    }

    *out_corr = corr;
    if (symbols == 0 || p0 <= 0.0 || p1 <= 0.0) {
        return 0.0;
    }

    return sqrt((double)c_abs2(corr)) / sqrt(p0 * p1);
}

static double grdvbt_ml_metric_at(const complexf_t *samples,
                                  size_t count,
                                  uint32_t fft_size,
                                  uint32_t gi_len,
                                  uint32_t symbol_len,
                                  uint32_t offset,
                                  uint32_t max_symbols,
                                  double rho,
                                  double *out_score,
                                  complexf_t *out_corr)
{
    complexf_t corr = {0.0f, 0.0f};
    double phi = 0.0;
    double p0 = 0.0;
    double p1 = 0.0;
    uint32_t symbols = 0;
    uint32_t s;

    for (s = 0; s < max_symbols; ++s) {
        size_t base = (size_t)offset + (size_t)s * symbol_len;
        uint32_t k;

        if (base + fft_size + gi_len >= count) {
            break;
        }

        for (k = 0; k < gi_len; ++k) {
            complexf_t a = samples[base + k];
            complexf_t b = samples[base + fft_size + k];
            double a2 = c_abs2(a);
            double b2 = c_abs2(b);

            corr = c_add(corr, c_mul(a, c_conj(b)));
            p0 += a2;
            p1 += b2;
            phi += a2 + b2;
        }
        symbols++;
    }

    *out_corr = corr;
    if (symbols == 0 || p0 <= 0.0 || p1 <= 0.0) {
        *out_score = 0.0;
        return -INFINITY;
    }

    *out_score = sqrt((double)c_abs2(corr)) / sqrt(p0 * p1);
    return sqrt((double)c_abs2(corr)) - (rho * 0.5 * phi);
}

static uint32_t find_symbol_start(const complexf_t *samples,
                                  size_t count,
                                  uint32_t fft_size,
                                  uint32_t gi_len,
                                  uint32_t symbol_len,
                                  uint32_t max_symbols,
                                  double *out_score,
                                  complexf_t *out_corr)
{
    uint32_t best_offset = 0;
    double best_score = -1.0;
    complexf_t best_corr = {0.0f, 0.0f};
    uint32_t offset;

    for (offset = 0; offset < symbol_len; ++offset) {
        complexf_t corr = {0.0f, 0.0f};
        double score = cp_score_at(samples,
                                   count,
                                   fft_size,
                                   gi_len,
                                   symbol_len,
                                   offset,
                                   max_symbols,
                                   &corr);
        if (score > best_score) {
            best_score = score;
            best_offset = offset;
            best_corr = corr;
        }
    }

    *out_score = best_score;
    *out_corr = best_corr;
    return best_offset;
}

static uint32_t find_symbol_start_grdvbt_ml(const complexf_t *samples,
                                            size_t count,
                                            uint32_t fft_size,
                                            uint32_t gi_len,
                                            uint32_t symbol_len,
                                            uint32_t max_symbols,
                                            double *out_score,
                                            complexf_t *out_corr)
{
    uint32_t best_offset = 0;
    double best_metric = -INFINITY;
    double best_score = 0.0;
    complexf_t best_corr = {0.0f, 0.0f};
    uint32_t offset;

    for (offset = 0; offset < symbol_len; ++offset) {
        double score = 0.0;
        complexf_t corr = {0.0f, 0.0f};
        double metric = grdvbt_ml_metric_at(samples,
                                            count,
                                            fft_size,
                                            gi_len,
                                            symbol_len,
                                            offset,
                                            max_symbols,
                                            GRDVBT_ML_SYNC_RHO,
                                            &score,
                                            &corr);

        if (metric > best_metric) {
            best_metric = metric;
            best_score = score;
            best_offset = offset;
            best_corr = corr;
        }
    }

    *out_score = best_score;
    *out_corr = best_corr;
    return best_offset;
}

static uint64_t symbol_phase_delta(uint64_t a, uint64_t b, uint32_t symbol_len)
{
    uint64_t raw_delta;
    uint64_t rem;

    if (symbol_len == 0u) {
        return a > b ? a - b : b - a;
    }
    raw_delta = a > b ? a - b : b - a;
    rem = raw_delta % (uint64_t)symbol_len;
    if (rem > (uint64_t)symbol_len - rem) {
        rem = (uint64_t)symbol_len - rem;
    }
    return rem;
}

static uint32_t find_symbol_start_near_local_grdvbt_ml(const complexf_t *samples,
                                                       size_t count,
                                                       uint32_t fft_size,
                                                       uint32_t gi_len,
                                                       uint32_t symbol_len,
                                                       uint32_t max_symbols,
                                                       uint32_t predicted,
                                                       uint32_t radius,
                                                       double *out_score,
                                                       complexf_t *out_corr)
{
    uint32_t best_offset = predicted;
    double best_metric = -INFINITY;
    double best_score = 0.0;
    complexf_t best_corr = {0.0f, 0.0f};
    int32_t first = predicted > radius ? (int32_t)(predicted - radius) : 0;
    int32_t last = predicted + radius;

    if ((uint64_t)last + (uint64_t)fft_size + (uint64_t)gi_len >= (uint64_t)count) {
        if (count > (size_t)(fft_size + gi_len)) {
            last = (int32_t)(count - (size_t)fft_size - (size_t)gi_len - 1u);
        } else {
            last = first;
        }
    }

    for (int32_t offset_i = first; offset_i <= last; ++offset_i) {
        uint32_t offset = (uint32_t)offset_i;
        double score = 0.0;
        complexf_t corr = {0.0f, 0.0f};
        double metric = grdvbt_ml_metric_at(samples,
                                            count,
                                            fft_size,
                                            gi_len,
                                            symbol_len,
                                            offset,
                                            max_symbols,
                                            GRDVBT_ML_SYNC_RHO,
                                            &score,
                                            &corr);

        if (metric > best_metric) {
            best_metric = metric;
            best_score = score;
            best_offset = offset;
            best_corr = corr;
        }
    }

    *out_score = best_score;
    *out_corr = best_corr;
    return best_offset;
}

static int write_cp_timing(const rbdvbt_config_t *cfg,
                           const complexf_t *samples,
                           size_t count,
                           uint32_t fft_size,
                           uint32_t gi_len,
                           uint32_t symbol_len,
                           uint32_t max_symbols,
                           uint32_t *out_safe_start,
                           double *out_safe_score,
                           complexf_t *out_safe_corr)
{
    double *scores = NULL;
    complexf_t *corrs = NULL;
    FILE *csv = NULL;
    FILE *svg = NULL;
    uint32_t offset;
    uint32_t best = 0;
    uint32_t plateau_first = 0;
    uint32_t plateau_last = 0;
    uint32_t safe = 0;
    double best_score = -1.0;
    double threshold;
    int rc = -1;

    scores = calloc(symbol_len, sizeof(*scores));
    corrs = calloc(symbol_len, sizeof(*corrs));
    if (scores == NULL || corrs == NULL) {
        fprintf(stderr, "failed to allocate CP timing buffers\n");
        goto done;
    }

    for (offset = 0; offset < symbol_len; ++offset) {
        scores[offset] = cp_score_at(samples,
                                     count,
                                     fft_size,
                                     gi_len,
                                     symbol_len,
                                     offset,
                                     max_symbols,
                                     &corrs[offset]);
        if (scores[offset] > best_score) {
            best_score = scores[offset];
            best = offset;
        }
    }

    threshold = best_score * 0.85;
    plateau_first = best;
    while (plateau_first > 0 && scores[plateau_first - 1u] >= threshold) {
        plateau_first--;
    }
    plateau_last = best;
    while (plateau_last + 1u < symbol_len && scores[plateau_last + 1u] >= threshold) {
        plateau_last++;
    }

    /* A conservative FFT start is inside the CP plateau, not necessarily at its maximum. */
    safe = plateau_first + (uint32_t)((plateau_last - plateau_first) * 3u / 4u);
    if (safe >= symbol_len) {
        safe = best;
    }

    if (cfg->cp_timing_out != NULL) {
        csv = fopen(cfg->cp_timing_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open CP timing output: %s\n", cfg->cp_timing_out);
            goto done;
        }
        fprintf(csv, "offset,score,corr_phase,cfo_hz,is_peak,is_plateau,is_safe\n");
        for (offset = 0; offset < symbol_len; ++offset) {
            double phase = atan2(corrs[offset].im, corrs[offset].re);
            double cfo = -phase * (double)cfg->sample_rate_hz / (2.0 * M_PI * (double)fft_size);
            fprintf(csv,
                    "%u,%.9f,%.9f,%.6f,%d,%d,%d\n",
                    offset,
                    scores[offset],
                    phase,
                    cfo,
                    offset == best ? 1 : 0,
                    offset >= plateau_first && offset <= plateau_last ? 1 : 0,
                    offset == safe ? 1 : 0);
        }
        fclose(csv);
        csv = NULL;
    }

    if (cfg->cp_timing_svg != NULL) {
        svg = fopen(cfg->cp_timing_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open CP timing SVG: %s\n", cfg->cp_timing_svg);
            goto done;
        }
        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"420\" viewBox=\"0 0 1000 420\">\n");
        fprintf(svg, "<rect width=\"1000\" height=\"420\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"50\" y1=\"340\" x2=\"950\" y2=\"340\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        for (offset = 0; offset < symbol_len; ++offset) {
            double x = 50.0 + ((double)offset / (double)(symbol_len - 1u)) * 900.0;
            double h = best_score > 0.0 ? (scores[offset] / best_score) * 290.0 : 0.0;
            const char *color = "#7aa7ff";
            if (offset >= plateau_first && offset <= plateau_last) {
                color = "#48d597";
            }
            if (offset == best) {
                color = "#ff7777";
            }
            if (offset == safe) {
                color = "#ffcc66";
            }
            fprintf(svg, "<line x1=\"%.2f\" y1=\"340\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"%s\" stroke-width=\"1\"/>\n", x, x, 340.0 - h, color);
        }
        fprintf(svg, "</svg>\n");
        fclose(svg);
        svg = NULL;
    }

    *out_safe_start = safe;
    *out_safe_score = scores[safe];
    *out_safe_corr = corrs[safe];

    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        fprintf(stderr,
                "[cptiming] peak=%u peak_score=%.5f plateau=%u..%u safe=%u safe_score=%.5f csv=%s svg=%s\n",
                best,
                best_score,
                plateau_first,
                plateau_last,
                safe,
                scores[safe],
                cfg->cp_timing_out != NULL ? cfg->cp_timing_out : "-",
                cfg->cp_timing_svg != NULL ? cfg->cp_timing_svg : "-");
    }

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    free(scores);
    free(corrs);
    return rc;
}

static void fftshift_copy(fftwf_complex *in, complexf_t *out, uint32_t n)
{
    uint32_t k;
    uint32_t half = n / 2u;

    for (k = 0; k < n; ++k) {
        uint32_t src = (k + half) % n;
        out[k].re = in[src][0];
        out[k].im = in[src][1];
    }
}

static double c_abs(complexf_t a)
{
    return sqrt((double)c_abs2(a));
}

static void write_svg_header(FILE *svg)
{
    fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"640\" viewBox=\"0 0 640 640\">\n");
    fprintf(svg, "<rect width=\"640\" height=\"640\" fill=\"#111\"/>\n");
    fprintf(svg, "<line x1=\"320\" y1=\"32\" x2=\"320\" y2=\"608\" stroke=\"#555\" stroke-width=\"1\"/>\n");
    fprintf(svg, "<line x1=\"32\" y1=\"320\" x2=\"608\" y2=\"320\" stroke=\"#555\" stroke-width=\"1\"/>\n");
    fprintf(svg, "<circle cx=\"320\" cy=\"320\" r=\"222\" fill=\"none\" stroke=\"#333\" stroke-width=\"1\"/>\n");
}

static void write_svg_point(FILE *svg, complexf_t z, const char *color, double opacity)
{
    double x = 320.0 + (double)z.re * 220.0;
    double y = 320.0 - (double)z.im * 220.0;

    if (x >= 0.0 && x <= 640.0 && y >= 0.0 && y <= 640.0) {
        fprintf(svg,
                "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"1.4\" fill=\"%s\" fill-opacity=\"%.2f\"/>\n",
                x,
                y,
                color,
                opacity);
    }
}

static int write_spectrum_outputs(const rbdvbt_config_t *cfg,
                                  const double *bin_power,
                                  const int *active,
                                  const int *data_like,
                                  uint32_t fft_size)
{
    FILE *csv = NULL;
    FILE *svg = NULL;
    uint32_t k;
    double max_power = 0.0;

    for (k = 0; k < fft_size; ++k) {
        if (bin_power[k] > max_power) {
            max_power = bin_power[k];
        }
    }

    if (cfg->spectrum_out != NULL) {
        csv = fopen(cfg->spectrum_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open spectrum output: %s\n", cfg->spectrum_out);
            return -1;
        }
        fprintf(csv, "bin,power,power_db,active,data_like\n");
        for (k = 0; k < fft_size; ++k) {
            double p = bin_power[k];
            double db = 10.0 * log10((p + 1e-30) / (max_power + 1e-30));
            fprintf(csv,
                    "%d,%.9g,%.6f,%d,%d\n",
                    (int)k - (int)(fft_size / 2u),
                    p,
                    db,
                    active[k],
                    data_like[k]);
        }
        fclose(csv);
    }

    if (cfg->spectrum_svg != NULL) {
        svg = fopen(cfg->spectrum_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open spectrum SVG: %s\n", cfg->spectrum_svg);
            return -1;
        }
        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"900\" height=\"360\" viewBox=\"0 0 900 360\">\n");
        fprintf(svg, "<rect width=\"900\" height=\"360\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"40\" y1=\"300\" x2=\"860\" y2=\"300\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        fprintf(svg, "<line x1=\"450\" y1=\"24\" x2=\"450\" y2=\"320\" stroke=\"#333\" stroke-width=\"1\"/>\n");
        for (k = 0; k < fft_size; ++k) {
            double norm = max_power > 0.0 ? bin_power[k] / max_power : 0.0;
            double h = sqrt(norm) * 270.0;
            double x = 40.0 + ((double)k / (double)(fft_size - 1u)) * 820.0;
            const char *color = data_like[k] ? "#48d597" : (active[k] ? "#7aa7ff" : "#555");
            fprintf(svg,
                    "<line x1=\"%.2f\" y1=\"300\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"%s\" stroke-width=\"1\"/>\n",
                    x,
                    x,
                    300.0 - h,
                    color);
        }
        fprintf(svg, "</svg>\n");
        fclose(svg);
    }

    return 0;
}

static int write_carrier_metrics(const rbdvbt_config_t *cfg,
                                 const double *bin_power,
                                 const double *qpsk_score,
                                 const int *active,
                                 const int *data_like,
                                 uint32_t fft_size)
{
    FILE *out;
    uint32_t k;

    if (cfg->carrier_metrics_out == NULL) {
        return 0;
    }

    out = fopen(cfg->carrier_metrics_out, "w");
    if (out == NULL) {
        fprintf(stderr, "failed to open carrier metrics output: %s\n", cfg->carrier_metrics_out);
        return -1;
    }

    fprintf(out, "bin,power,qpsk4_score,active,data_like\n");
    for (k = 0; k < fft_size; ++k) {
        fprintf(out,
                "%d,%.9g,%.7f,%d,%d\n",
                (int)k - (int)(fft_size / 2u),
                bin_power[k],
                qpsk_score[k],
                active[k],
                data_like[k]);
    }

    fclose(out);
    return 0;
}

static int is_scattered_pilot_logical(int logical_carrier, int symbol_mod4)
{
    int kmin = -852;
    int rel = logical_carrier - kmin;
    int phase = 3 * symbol_mod4;

    if (rel < 0 || rel > 1704) {
        return 0;
    }

    rel -= phase;
    return rel >= 0 && (rel % 12) == 0;
}

static int is_continual_pilot_2k_abs(int abs_k)
{
    static const int pilots[] = {
        0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333,
        432, 450, 483, 525, 531, 618, 636, 714, 759, 765, 780,
        804, 873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107,
        1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704
    };
    size_t i;

    for (i = 0; i < sizeof(pilots) / sizeof(pilots[0]); ++i) {
        if (pilots[i] == abs_k) {
            return 1;
        }
    }
    return 0;
}

static int is_continual_pilot_logical(int logical_carrier)
{
    return is_continual_pilot_2k_abs(logical_carrier + 852);
}

static int dvbt_prbs_w_for_abs_carrier(int abs_k)
{
    static int initialized = 0;
    static int prbs[2047];

    if (abs_k < 0) {
        return 0;
    }

    if (!initialized) {
        uint16_t reg = 0x7ffu;
        int k;

        for (k = 0; k < 2047; ++k) {
            int w = reg & 0x1u;
            uint16_t new_bit = (uint16_t)(((reg >> 0) ^ (reg >> 2)) & 0x1u);

            prbs[k] = w;
            reg = (uint16_t)((reg >> 1) | (new_bit << 10));
        }
        initialized = 1;
    }

    return prbs[abs_k % 2047];
}

static int dvbt_prbs_w_for_abs_carrier_shifted(int abs_k, int shift)
{
    int idx = (abs_k + shift) % 2047;

    if (idx < 0) {
        idx += 2047;
    }
    return dvbt_prbs_w_for_abs_carrier(idx);
}

static double dvbt_pilot_ref_sign_logical_shifted(int logical_carrier, int shift)
{
    int abs_k = logical_carrier + 852;
    int w = dvbt_prbs_w_for_abs_carrier_shifted(abs_k, shift);

    return w ? -1.0 : 1.0;
}

static int write_mapping_scan(const rbdvbt_config_t *cfg,
                              const double *bin_power,
                              const double *qpsk_score,
                              const int *active,
                              uint32_t fft_size)
{
    FILE *csv = NULL;
    FILE *svg = NULL;
    double scale_candidates[] = {4.0, 5.0, 5.25, 5.3, 5.3333333333, 5.36, 5.5, 6.0};
    int offset_candidates[] = {-12, -8, -4, 0, 4, 8, 12};
    double scores[64];
    char labels[64][40];
    uint32_t result_count = 0;
    double best_score = -1.0;
    double max_score = 0.0;
    double best_scale = 0.0;
    int best_offset = 0;
    size_t si;
    size_t oi;
    uint32_t k;
    int rc = -1;

    (void)bin_power;

    if (cfg->mapping_scan_out == NULL && cfg->mapping_scan_svg == NULL) {
        return 0;
    }

    if (cfg->mapping_scan_out != NULL) {
        csv = fopen(cfg->mapping_scan_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open mapping scan output: %s\n", cfg->mapping_scan_out);
            goto done;
        }
        fprintf(csv, "scale,offset,score,active_bins,mapped_bins,scattered_hits,continual_hits,avg_hit_qpsk4,avg_miss_qpsk4\n");
    }

    for (si = 0; si < sizeof(scale_candidates) / sizeof(scale_candidates[0]); ++si) {
        for (oi = 0; oi < sizeof(offset_candidates) / sizeof(offset_candidates[0]); ++oi) {
            double scale = scale_candidates[si];
            int offset = offset_candidates[oi];
            int active_bins = 0;
            int mapped_bins = 0;
            int scattered_hits = 0;
            int continual_hits = 0;
            double hit_score_sum = 0.0;
            double miss_score_sum = 0.0;
            int hit_count = 0;
            int miss_count = 0;
            double score;

            for (k = 0; k < fft_size; ++k) {
                int physical_bin;
                int logical_carrier;
                int is_hit;

                if (!active[k]) {
                    continue;
                }

                active_bins++;
                physical_bin = (int)k - (int)(fft_size / 2u);
                logical_carrier = (int)lrint((double)physical_bin * scale) + offset;
                if (logical_carrier < -852 || logical_carrier > 852) {
                    continue;
                }

                mapped_bins++;
                is_hit = is_scattered_pilot_logical(logical_carrier, 0) ||
                         is_scattered_pilot_logical(logical_carrier, 1) ||
                         is_scattered_pilot_logical(logical_carrier, 2) ||
                         is_scattered_pilot_logical(logical_carrier, 3) ||
                         is_continual_pilot_logical(logical_carrier);

                if (is_scattered_pilot_logical(logical_carrier, 0) ||
                    is_scattered_pilot_logical(logical_carrier, 1) ||
                    is_scattered_pilot_logical(logical_carrier, 2) ||
                    is_scattered_pilot_logical(logical_carrier, 3)) {
                    scattered_hits++;
                }
                if (is_continual_pilot_logical(logical_carrier)) {
                    continual_hits++;
                }

                if (is_hit) {
                    hit_score_sum += qpsk_score[k];
                    hit_count++;
                } else {
                    miss_score_sum += qpsk_score[k];
                    miss_count++;
                }
            }

            score = 0.0;
            if (mapped_bins > 0) {
                double hit_fraction = (double)(scattered_hits + continual_hits) / (double)mapped_bins;
                double hit_avg = hit_count > 0 ? hit_score_sum / (double)hit_count : 0.0;
                double miss_avg = miss_count > 0 ? miss_score_sum / (double)miss_count : 0.0;
                score = hit_fraction * (hit_avg + 0.001) / (miss_avg + 0.001);
            }

            if (csv != NULL) {
                fprintf(csv,
                        "%.9g,%d,%.8f,%d,%d,%d,%d,%.8f,%.8f\n",
                        scale,
                        offset,
                        score,
                        active_bins,
                        mapped_bins,
                        scattered_hits,
                        continual_hits,
                        hit_count > 0 ? hit_score_sum / (double)hit_count : 0.0,
                        miss_count > 0 ? miss_score_sum / (double)miss_count : 0.0);
            }

            scores[result_count] = score;
            snprintf(labels[result_count], sizeof(labels[result_count]), "%.2f/%d", scale, offset);
            result_count++;
            if (score > best_score) {
                best_score = score;
                best_scale = scale;
                best_offset = offset;
            }
            if (score > max_score) {
                max_score = score;
            }
        }
    }

    if (cfg->mapping_scan_svg != NULL) {
        uint32_t i;

        svg = fopen(cfg->mapping_scan_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open mapping scan SVG: %s\n", cfg->mapping_scan_svg);
            goto done;
        }

        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"980\" height=\"460\" viewBox=\"0 0 980 460\">\n");
        fprintf(svg, "<rect width=\"980\" height=\"460\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"40\" y1=\"360\" x2=\"940\" y2=\"360\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        for (i = 0; i < result_count; ++i) {
            double bar_w = 860.0 / (double)result_count;
            double x = 55.0 + (double)i * bar_w;
            double h = max_score > 0.0 ? (scores[i] / max_score) * 300.0 : 0.0;
            fprintf(svg, "<rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"#ffcc66\"/>\n", x, 360.0 - h, bar_w * 0.72, h);
            fprintf(svg, "<text x=\"%.2f\" y=\"382\" fill=\"#bbb\" font-size=\"8\" transform=\"rotate(55 %.2f 382)\">%s</text>\n", x, x, labels[i]);
        }
        fprintf(svg, "</svg>\n");
        fclose(svg);
        svg = NULL;
    }

    fprintf(stderr,
            "[mapping] best_scale=%.6g best_offset=%d best_score=%.5f csv=%s svg=%s\n",
            best_scale,
            best_offset,
            best_score,
            cfg->mapping_scan_out != NULL ? cfg->mapping_scan_out : "-",
            cfg->mapping_scan_svg != NULL ? cfg->mapping_scan_svg : "-");

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    return rc;
}

static double qpsk4_score_for_offset(const rbdvbt_config_t *cfg,
                                     const complexf_t *samples,
                                     size_t count,
                                     uint32_t offset,
                                     uint32_t fft_size,
                                     uint32_t gi_len,
                                     uint32_t symbol_len,
                                     double cfo_hz,
                                     fftwf_complex *fft_in,
                                     fftwf_complex *fft_out,
                                     fftwf_plan plan,
                                     complexf_t *shifted)
{
    complexf_t qpsk4 = {0.0f, 0.0f};
    double qpsk4_power = 0.0;
    uint32_t used_symbols = 0;
    uint32_t symbols = cfg->probe_symbols > 80u ? 80u : cfg->probe_symbols;
    uint32_t low_bin = fft_size / 2u - 180u;
    uint32_t high_bin = fft_size / 2u + 180u;
    uint32_t s;

    if (high_bin > fft_size) {
        high_bin = fft_size;
    }

    for (s = 0; s < symbols; ++s) {
        size_t base = (size_t)offset + (size_t)s * symbol_len + gi_len;
        uint32_t k;

        if (base + fft_size >= count) {
            break;
        }

        for (k = 0; k < fft_size; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);
        fftshift_copy(fft_out, shifted, fft_size);

        for (k = low_bin; k < high_bin; ++k) {
            complexf_t z2;
            complexf_t z4;
            double p;

            if (k == fft_size / 2u) {
                continue;
            }

            z2 = c_mul(shifted[k], shifted[k]);
            z4 = c_mul(z2, z2);
            p = c_abs2(shifted[k]);
            qpsk4 = c_add(qpsk4, z4);
            qpsk4_power += p * p;
        }
        used_symbols++;
    }

    if (used_symbols == 0 || qpsk4_power <= 0.0) {
        return 0.0;
    }

    return c_abs(qpsk4) / qpsk4_power;
}

static uint32_t active_width_for_candidate(const rbdvbt_config_t *cfg,
                                           const complexf_t *samples,
                                           size_t count,
                                           uint32_t start,
                                           uint32_t fft_size,
                                           uint32_t gi_len,
                                           uint32_t symbol_len,
                                           double cfo_hz)
{
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    complexf_t *shifted = NULL;
    double *power = NULL;
    uint32_t symbols = cfg->probe_symbols > 80u ? 80u : cfg->probe_symbols;
    uint32_t used = 0;
    uint32_t first = fft_size;
    uint32_t last = 0;
    uint32_t k;
    uint32_t s;
    double max_power = 0.0;
    double mean_power = 0.0;
    double threshold;
    uint32_t width = 0;

    fft_in = fftwf_malloc(sizeof(*fft_in) * fft_size);
    fft_out = fftwf_malloc(sizeof(*fft_out) * fft_size);
    shifted = calloc(fft_size, sizeof(*shifted));
    power = calloc(fft_size, sizeof(*power));
    if (fft_in == NULL || fft_out == NULL || shifted == NULL || power == NULL) {
        goto done;
    }

    plan = fftwf_plan_dft_1d((int)fft_size, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        goto done;
    }

    for (s = 0; s < symbols; ++s) {
        size_t base = (size_t)start + (size_t)s * symbol_len + gi_len;
        if (base + fft_size >= count) {
            break;
        }

        for (k = 0; k < fft_size; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);
        fftshift_copy(fft_out, shifted, fft_size);

        for (k = 0; k < fft_size; ++k) {
            power[k] += c_abs2(shifted[k]);
        }
        used++;
    }

    if (used == 0) {
        goto done;
    }

    for (k = 0; k < fft_size; ++k) {
        power[k] /= used;
        mean_power += power[k];
        if (power[k] > max_power) {
            max_power = power[k];
        }
    }
    mean_power /= fft_size;
    threshold = mean_power * 2.0;
    if (threshold > max_power * 0.25) {
        threshold = max_power * 0.25;
    }

    for (k = 0; k < fft_size; ++k) {
        if (k != fft_size / 2u && power[k] >= threshold) {
            if (first == fft_size) {
                first = k;
            }
            last = k;
        }
    }

    if (first != fft_size && last >= first) {
        width = last - first + 1u;
    }

done:
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    free(shifted);
    free(power);
    return width;
}

static int write_acquisition_scan(const rbdvbt_config_t *cfg,
                                  const complexf_t *samples,
                                  size_t count)
{
    static const uint32_t fft_candidates[] = {512u, 768u, 1024u, 1536u, 2048u};
    static const rbdvbt_guard_interval_t gi_candidates[] = {
        RBDVBT_GI_1_8,
        RBDVBT_GI_1_16,
        RBDVBT_GI_1_32
    };
    FILE *csv = NULL;
    FILE *svg = NULL;
    double scores[15];
    char labels[15][32];
    uint32_t result_count = 0;
    uint32_t best_fft = 0;
    rbdvbt_guard_interval_t best_gi = RBDVBT_GI_1_32;
    uint32_t best_start = 0;
    double best_score = 0.0;
    double max_score = 0.0;
    size_t fi;
    size_t gi;
    int rc = -1;

    if (cfg->acquisition_scan_out == NULL && cfg->acquisition_scan_svg == NULL) {
        return 0;
    }

    if (cfg->acquisition_scan_out != NULL) {
        csv = fopen(cfg->acquisition_scan_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open acquisition scan output: %s\n", cfg->acquisition_scan_out);
            goto done;
        }
        fprintf(csv, "fft_size,gi,gi_samples,symbol_samples,best_start,cp_score,cfo_hz,active_width\n");
    }

    for (fi = 0; fi < sizeof(fft_candidates) / sizeof(fft_candidates[0]); ++fi) {
        for (gi = 0; gi < sizeof(gi_candidates) / sizeof(gi_candidates[0]); ++gi) {
            uint32_t fft_size = fft_candidates[fi];
            uint32_t gi_len = guard_samples(fft_size, gi_candidates[gi]);
            uint32_t symbol_len = fft_size + gi_len;
            uint32_t scan_symbols = cfg->probe_symbols > 120u ? 120u : cfg->probe_symbols;
            complexf_t corr = {0.0f, 0.0f};
            double score;
            double cfo_hz;
            uint32_t start;
            uint32_t active_width;

            if (count < (size_t)symbol_len * 4u || gi_len == 0) {
                continue;
            }
            if ((size_t)scan_symbols * symbol_len > count) {
                scan_symbols = (uint32_t)(count / symbol_len);
            }
            if (scan_symbols == 0) {
                continue;
            }

            start = find_symbol_start(samples,
                                      count,
                                      fft_size,
                                      gi_len,
                                      symbol_len,
                                      scan_symbols,
                                      &score,
                                      &corr);
            cfo_hz = -atan2(corr.im, corr.re) * (double)cfg->sample_rate_hz /
                     (2.0 * M_PI * (double)fft_size);
            active_width = active_width_for_candidate(cfg,
                                                      samples,
                                                      count,
                                                      start,
                                                      fft_size,
                                                      gi_len,
                                                      symbol_len,
                                                      cfo_hz);

            if (csv != NULL) {
                fprintf(csv,
                        "%u,%s,%u,%u,%u,%.8f,%.4f,%u\n",
                        fft_size,
                        rbdvbt_guard_interval_name(gi_candidates[gi]),
                        gi_len,
                        symbol_len,
                        start,
                        score,
                        cfo_hz,
                        active_width);
            }

            scores[result_count] = score;
            snprintf(labels[result_count], sizeof(labels[result_count]), "%u %s", fft_size, rbdvbt_guard_interval_name(gi_candidates[gi]));
            result_count++;

            if (score > best_score) {
                best_score = score;
                best_fft = fft_size;
                best_gi = gi_candidates[gi];
                best_start = start;
            }
            if (score > max_score) {
                max_score = score;
            }
        }
    }

    if (cfg->acquisition_scan_svg != NULL) {
        uint32_t i;

        svg = fopen(cfg->acquisition_scan_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open acquisition scan SVG: %s\n", cfg->acquisition_scan_svg);
            goto done;
        }

        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"940\" height=\"420\" viewBox=\"0 0 940 420\">\n");
        fprintf(svg, "<rect width=\"940\" height=\"420\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"50\" y1=\"340\" x2=\"900\" y2=\"340\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        for (i = 0; i < result_count; ++i) {
            double bar_w = 760.0 / (double)result_count;
            double x = 70.0 + (double)i * bar_w;
            double h = max_score > 0.0 ? (scores[i] / max_score) * 280.0 : 0.0;
            fprintf(svg, "<rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"#7aa7ff\"/>\n", x, 340.0 - h, bar_w * 0.72, h);
            fprintf(svg, "<text x=\"%.2f\" y=\"365\" fill=\"#bbb\" font-size=\"10\" transform=\"rotate(45 %.2f 365)\">%s</text>\n", x, x, labels[i]);
        }
        fprintf(svg, "</svg>\n");
        fclose(svg);
        svg = NULL;
    }

    fprintf(stderr,
            "[acqscan] best_fft=%u best_gi=%s best_start=%u best_cp=%.5f csv=%s svg=%s\n",
            best_fft,
            rbdvbt_guard_interval_name(best_gi),
            best_start,
            best_score,
            cfg->acquisition_scan_out != NULL ? cfg->acquisition_scan_out : "-",
            cfg->acquisition_scan_svg != NULL ? cfg->acquisition_scan_svg : "-");

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    return rc;
}

static int write_highres_spectrum(const rbdvbt_config_t *cfg,
                                  const complexf_t *samples,
                                  size_t count,
                                  uint32_t *out_center_bin,
                                  uint32_t *out_spacing_bins,
                                  double *out_center_freq_hz,
                                  double *out_spacing_hz)
{
    uint32_t nfft = cfg->highres_fft_size;
    uint32_t hop = nfft / 2u;
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    double *power = NULL;
    FILE *csv = NULL;
    FILE *svg = NULL;
    FILE *spacing = NULL;
    uint32_t blocks = 0;
    uint32_t k;
    size_t base;
    double max_power = 0.0;
    double mean_power = 0.0;
    double threshold;
    uint32_t first = nfft;
    uint32_t last = 0;
    uint32_t best_spacing_bins = 0;
    uint32_t best_phase = 0;
    uint32_t best_hits = 0;
    uint32_t best_carriers = 0;
    uint32_t center_bin = 0;
    double best_score = -1.0;
    double best_spacing_hz = 0.0;
    double center_freq_hz = 0.0;
    double occupied_hz = 0.0;
    int rc = -1;

    if (cfg->highres_spectrum_out == NULL &&
        cfg->highres_spectrum_svg == NULL &&
        cfg->carrier_spacing_out == NULL &&
        cfg->carrier_map_out == NULL &&
        cfg->carrier_map_svg == NULL &&
        cfg->logical_constellation_out == NULL &&
        cfg->logical_constellation_svg == NULL) {
        *out_center_bin = 0;
        *out_spacing_bins = 0;
        *out_center_freq_hz = 0.0;
        *out_spacing_hz = 0.0;
        return 0;
    }

    if (nfft < 1024u || count < nfft) {
        fprintf(stderr, "[highres] not enough samples for high-resolution FFT\n");
        return -1;
    }

    fft_in = fftwf_malloc(sizeof(*fft_in) * nfft);
    fft_out = fftwf_malloc(sizeof(*fft_out) * nfft);
    power = calloc(nfft, sizeof(*power));
    if (fft_in == NULL || fft_out == NULL || power == NULL) {
        fprintf(stderr, "failed to allocate highres spectrum buffers\n");
        goto done;
    }

    plan = fftwf_plan_dft_1d((int)nfft, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "failed to create highres FFTW plan\n");
        goto done;
    }

    for (base = 0; base + nfft <= count; base += hop) {
        for (k = 0; k < nfft; ++k) {
            double w = 0.5 - 0.5 * cos((2.0 * M_PI * (double)k) / (double)(nfft - 1u));
            fft_in[k][0] = (float)(samples[base + k].re * w);
            fft_in[k][1] = (float)(samples[base + k].im * w);
        }

        fftwf_execute(plan);

        for (k = 0; k < nfft; ++k) {
            uint32_t src = (k + nfft / 2u) % nfft;
            double re = fft_out[src][0];
            double im = fft_out[src][1];
            power[k] += re * re + im * im;
        }
        blocks++;

        if (blocks >= 64u) {
            break;
        }
    }

    if (blocks == 0) {
        goto done;
    }

    for (k = 0; k < nfft; ++k) {
        power[k] /= blocks;
        mean_power += power[k];
        if (power[k] > max_power) {
            max_power = power[k];
        }
    }
    mean_power /= nfft;
    threshold = mean_power * 3.0;
    if (threshold > max_power * 0.10) {
        threshold = max_power * 0.10;
    }

    for (k = 1; k + 1 < nfft; ++k) {
        if (power[k] >= threshold) {
            if (first == nfft) {
                first = k;
            }
            last = k;
        }
    }

    if (first != nfft && last >= first) {
        occupied_hz = ((double)(last - first + 1u) / (double)nfft) * (double)cfg->sample_rate_hz;
    }

    if (cfg->highres_spectrum_out != NULL) {
        csv = fopen(cfg->highres_spectrum_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open highres spectrum output: %s\n", cfg->highres_spectrum_out);
            goto done;
        }
        fprintf(csv, "freq_hz,power,power_db,active\n");
        for (k = 0; k < nfft; ++k) {
            double freq_hz = (((double)k / (double)nfft) - 0.5) * (double)cfg->sample_rate_hz;
            double db = 10.0 * log10((power[k] + 1e-30) / (max_power + 1e-30));
            fprintf(csv, "%.6f,%.9g,%.6f,%d\n", freq_hz, power[k], db, power[k] >= threshold ? 1 : 0);
        }
        fclose(csv);
        csv = NULL;
    }

    if (cfg->carrier_spacing_out != NULL) {
        double expected_2k_spacing_hz = occupied_hz > 0.0 ? occupied_hz / 1705.0 : 0.0;
        uint32_t min_spacing = (uint32_t)floor(80.0 / ((double)cfg->sample_rate_hz / (double)nfft));
        uint32_t max_spacing = (uint32_t)ceil(350.0 / ((double)cfg->sample_rate_hz / (double)nfft));
        uint32_t d;

        spacing = fopen(cfg->carrier_spacing_out, "w");
        if (spacing == NULL) {
            fprintf(stderr, "failed to open carrier spacing output: %s\n", cfg->carrier_spacing_out);
            goto done;
        }

        if (min_spacing < 2u) {
            min_spacing = 2u;
        }
        if (max_spacing >= nfft / 8u) {
            max_spacing = nfft / 8u;
        }

        fprintf(spacing, "section,key,value\n");
        fprintf(spacing, "summary,fft_size,%u\n", nfft);
        fprintf(spacing, "summary,blocks,%u\n", blocks);
        fprintf(spacing, "summary,bin_hz,%.9f\n", (double)cfg->sample_rate_hz / (double)nfft);
        fprintf(spacing, "summary,occupied_hz,%.6f\n", occupied_hz);
        fprintf(spacing, "summary,expected_2k_spacing_hz,%.6f\n", expected_2k_spacing_hz);
        fprintf(spacing, "summary,threshold,%.9g\n", threshold);
        fprintf(spacing, "scan,spacing_bins,spacing_hz,phase,hits,carrier_slots,hit_fraction,mean_hit_power,cv,score\n");

        for (d = min_spacing; d <= max_spacing; ++d) {
            uint32_t phase;
            for (phase = 0; phase < d; ++phase) {
                uint32_t pos;
                uint32_t hits = 0;
                uint32_t carriers = 0;
                double sum = 0.0;
                double sum2 = 0.0;
                double mean = 0.0;
                double cv = 99.0;
                double hit_fraction;
                double score;

                if (first == nfft || last <= first) {
                    continue;
                }

                pos = first + ((phase + d - (first % d)) % d);
                for (; pos <= last; pos += d) {
                    uint32_t w0 = pos > 1u ? pos - 1u : 0u;
                    uint32_t w1 = pos + 1u < nfft ? pos + 1u : nfft - 1u;
                    uint32_t wk;
                    double local_max = 0.0;

                    carriers++;
                    for (wk = w0; wk <= w1; ++wk) {
                        if (power[wk] > local_max) {
                            local_max = power[wk];
                        }
                    }

                    if (local_max >= threshold) {
                        hits++;
                        sum += local_max;
                        sum2 += local_max * local_max;
                    }
                }

                if (carriers == 0 || hits == 0) {
                    continue;
                }

                mean = sum / (double)hits;
                if (hits > 1 && mean > 0.0) {
                    double var = (sum2 / (double)hits) - mean * mean;
                    if (var < 0.0) {
                        var = 0.0;
                    }
                    cv = sqrt(var) / mean;
                }
                hit_fraction = (double)hits / (double)carriers;
                score = hit_fraction * hit_fraction * log(1.0 + mean / (threshold + 1e-30)) / (1.0 + cv);
                if (expected_2k_spacing_hz > 0.0) {
                    double spacing_hz = ((double)d * (double)cfg->sample_rate_hz) / (double)nfft;
                    double rel = fabs(spacing_hz - expected_2k_spacing_hz) / expected_2k_spacing_hz;
                    score /= 1.0 + 8.0 * rel;
                }

                fprintf(spacing,
                        "scan,%u,%.6f,%u,%u,%u,%.8f,%.9g,%.8f,%.8f\n",
                        d,
                        ((double)d * (double)cfg->sample_rate_hz) / (double)nfft,
                        phase,
                        hits,
                        carriers,
                        hit_fraction,
                        mean,
                        cv,
                        score);

                if (score > best_score) {
                    best_score = score;
                    best_spacing_bins = d;
                    best_phase = phase;
                    best_hits = hits;
                    best_carriers = carriers;
                    best_spacing_hz = ((double)d * (double)cfg->sample_rate_hz) / (double)nfft;
                }
            }
        }

        fprintf(spacing, "best,spacing_bins,%u\n", best_spacing_bins);
        fprintf(spacing, "best,spacing_hz,%.6f\n", best_spacing_hz);
        fprintf(spacing, "best,phase,%u\n", best_phase);
        fprintf(spacing, "best,hits,%u\n", best_hits);
        fprintf(spacing, "best,carrier_slots,%u\n", best_carriers);
        fprintf(spacing, "best,score,%.8f\n", best_score);
        fclose(spacing);
        spacing = NULL;
    }

    if (best_spacing_bins > 0) {
        int nearest_center_n = (int)lrint(((double)(nfft / 2u) - (double)best_phase) / (double)best_spacing_bins);
        int center_bin_i = (int)best_phase + nearest_center_n * (int)best_spacing_bins;

        if (center_bin_i < 0) {
            center_bin_i = (int)nfft / 2;
        }
        if (center_bin_i >= (int)nfft) {
            center_bin_i = (int)nfft / 2;
        }
        center_bin = (uint32_t)center_bin_i;
        center_freq_hz = (((double)center_bin / (double)nfft) - 0.5) * (double)cfg->sample_rate_hz;
    }

    if (cfg->carrier_map_out != NULL && best_spacing_bins > 0) {
        FILE *map = fopen(cfg->carrier_map_out, "w");
        int logical;

        if (map == NULL) {
            fprintf(stderr, "failed to open carrier map output: %s\n", cfg->carrier_map_out);
            goto done;
        }

        fprintf(map, "logical_carrier,physical_bin,freq_hz,power,power_db,in_highres_fft,in_active_mask,role_hint\n");
        for (logical = -852; logical <= 852; ++logical) {
            int bin_i = (int)center_bin + logical * (int)best_spacing_bins;
            int in_fft = bin_i >= 0 && bin_i < (int)nfft;
            double p = in_fft ? power[bin_i] : 0.0;
            double db = 10.0 * log10((p + 1e-30) / (max_power + 1e-30));
            double freq_hz = center_freq_hz + (double)logical * best_spacing_hz;
            const char *role = "data_or_unknown";

            if (logical == 0) {
                role = "dc";
            } else if (is_continual_pilot_logical(logical)) {
                role = "continual_pilot";
            }

            fprintf(map,
                    "%d,%d,%.6f,%.9g,%.6f,%d,%d,%s\n",
                    logical,
                    bin_i,
                    freq_hz,
                    p,
                    db,
                    in_fft,
                    p >= threshold ? 1 : 0,
                    role);
        }
        fclose(map);
    }

    if (cfg->carrier_map_svg != NULL && best_spacing_bins > 0) {
        FILE *map_svg = fopen(cfg->carrier_map_svg, "w");
        int logical;

        if (map_svg == NULL) {
            fprintf(stderr, "failed to open carrier map SVG: %s\n", cfg->carrier_map_svg);
            goto done;
        }

        fprintf(map_svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(map_svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1100\" height=\"460\" viewBox=\"0 0 1100 460\">\n");
        fprintf(map_svg, "<rect width=\"1100\" height=\"460\" fill=\"#111\"/>\n");
        fprintf(map_svg, "<line x1=\"60\" y1=\"360\" x2=\"1040\" y2=\"360\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        fprintf(map_svg, "<line x1=\"550\" y1=\"40\" x2=\"550\" y2=\"380\" stroke=\"#444\" stroke-width=\"1\"/>\n");
        for (k = 0; k < nfft; k += nfft / 1800u == 0 ? 1u : nfft / 1800u) {
            double norm = max_power > 0.0 ? power[k] / max_power : 0.0;
            double h = sqrt(norm) * 280.0;
            double x = 60.0 + ((double)k / (double)(nfft - 1u)) * 980.0;
            fprintf(map_svg, "<line x1=\"%.2f\" y1=\"360\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#46536c\" stroke-width=\"1\"/>\n", x, x, 360.0 - h);
        }
        for (logical = -852; logical <= 852; logical += 12) {
            int bin_i = (int)center_bin + logical * (int)best_spacing_bins;
            double x;
            const char *color = "#48d597";

            if (bin_i < 0 || bin_i >= (int)nfft) {
                continue;
            }
            x = 60.0 + ((double)bin_i / (double)(nfft - 1u)) * 980.0;
            if (logical == 0) {
                color = "#ff7777";
            } else if (is_continual_pilot_logical(logical)) {
                color = "#ffcc66";
            }
            fprintf(map_svg, "<line x1=\"%.2f\" y1=\"40\" x2=\"%.2f\" y2=\"360\" stroke=\"%s\" stroke-width=\"0.7\" stroke-opacity=\"0.55\"/>\n", x, x, color);
        }
        fprintf(map_svg, "</svg>\n");
        fclose(map_svg);
    }

    if (cfg->highres_spectrum_svg != NULL) {
        svg = fopen(cfg->highres_spectrum_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open highres spectrum SVG: %s\n", cfg->highres_spectrum_svg);
            goto done;
        }
        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"420\" viewBox=\"0 0 1000 420\">\n");
        fprintf(svg, "<rect width=\"1000\" height=\"420\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"50\" y1=\"340\" x2=\"950\" y2=\"340\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        fprintf(svg, "<line x1=\"500\" y1=\"40\" x2=\"500\" y2=\"360\" stroke=\"#333\" stroke-width=\"1\"/>\n");
        for (k = 0; k < nfft; k += nfft / 1800u == 0 ? 1u : nfft / 1800u) {
            double norm = max_power > 0.0 ? power[k] / max_power : 0.0;
            double h = sqrt(norm) * 290.0;
            double x = 50.0 + ((double)k / (double)(nfft - 1u)) * 900.0;
            const char *color = power[k] >= threshold ? "#48d597" : "#63708a";
            fprintf(svg, "<line x1=\"%.2f\" y1=\"340\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"%s\" stroke-width=\"1\"/>\n", x, x, 340.0 - h, color);
        }
        fprintf(svg, "</svg>\n");
        fclose(svg);
        svg = NULL;
    }

    fprintf(stderr,
            "[highres] fft=%u blocks=%u bin_hz=%.3f occupied=%.1fHz best_spacing=%.3fHz bins=%u center=%.2fHz hits=%u/%u score=%.5f csv=%s svg=%s spacing=%s map=%s\n",
            nfft,
            blocks,
            (double)cfg->sample_rate_hz / (double)nfft,
            occupied_hz,
            best_spacing_hz,
            best_spacing_bins,
            center_freq_hz,
            best_hits,
            best_carriers,
            best_score,
            cfg->highres_spectrum_out != NULL ? cfg->highres_spectrum_out : "-",
            cfg->highres_spectrum_svg != NULL ? cfg->highres_spectrum_svg : "-",
            cfg->carrier_spacing_out != NULL ? cfg->carrier_spacing_out : "-",
            cfg->carrier_map_out != NULL ? cfg->carrier_map_out : "-");

    *out_center_bin = center_bin;
    *out_spacing_bins = best_spacing_bins;
    *out_center_freq_hz = center_freq_hz;
    *out_spacing_hz = best_spacing_hz;

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    if (spacing != NULL) {
        fclose(spacing);
    }
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    free(power);
    return rc;
}

static int write_window_sweep(const rbdvbt_config_t *cfg,
                              const complexf_t *samples,
                              size_t count,
                              uint32_t start,
                              uint32_t fft_size,
                              uint32_t gi_len,
                              uint32_t symbol_len,
                              double cfo_hz)
{
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    complexf_t *shifted = NULL;
    FILE *csv = NULL;
    FILE *svg = NULL;
    double *cp_scores = NULL;
    double *qpsk_scores = NULL;
    int32_t radius = (int32_t)cfg->window_sweep_radius;
    int32_t points = radius * 2 + 1;
    int32_t i;
    double max_cp = 0.0;
    double max_qpsk = 0.0;
    int32_t best_cp_delta = 0;
    int32_t best_qpsk_delta = 0;
    int rc = -1;

    if (cfg->window_sweep_out == NULL && cfg->window_sweep_svg == NULL) {
        return 0;
    }

    cp_scores = calloc((size_t)points, sizeof(*cp_scores));
    qpsk_scores = calloc((size_t)points, sizeof(*qpsk_scores));
    fft_in = fftwf_malloc(sizeof(*fft_in) * fft_size);
    fft_out = fftwf_malloc(sizeof(*fft_out) * fft_size);
    shifted = calloc(fft_size, sizeof(*shifted));
    if (cp_scores == NULL || qpsk_scores == NULL || fft_in == NULL || fft_out == NULL || shifted == NULL) {
        fprintf(stderr, "failed to allocate window sweep buffers\n");
        goto done;
    }

    plan = fftwf_plan_dft_1d((int)fft_size, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "failed to create FFTW plan for window sweep\n");
        goto done;
    }

    for (i = -radius; i <= radius; ++i) {
        int64_t candidate = (int64_t)start + i;
        size_t idx = (size_t)(i + radius);
        complexf_t corr = {0.0f, 0.0f};

        if (candidate < 0 || candidate >= (int64_t)symbol_len) {
            cp_scores[idx] = 0.0;
            qpsk_scores[idx] = 0.0;
            continue;
        }

        cp_scores[idx] = cp_score_at(samples,
                                     count,
                                     fft_size,
                                     gi_len,
                                     symbol_len,
                                     (uint32_t)candidate,
                                     cfg->probe_symbols > 80u ? 80u : cfg->probe_symbols,
                                     &corr);
        qpsk_scores[idx] = qpsk4_score_for_offset(cfg,
                                                  samples,
                                                  count,
                                                  (uint32_t)candidate,
                                                  fft_size,
                                                  gi_len,
                                                  symbol_len,
                                                  cfo_hz,
                                                  fft_in,
                                                  fft_out,
                                                  plan,
                                                  shifted);

        if (cp_scores[idx] > max_cp) {
            max_cp = cp_scores[idx];
            best_cp_delta = i;
        }
        if (qpsk_scores[idx] > max_qpsk) {
            max_qpsk = qpsk_scores[idx];
            best_qpsk_delta = i;
        }
    }

    if (cfg->window_sweep_out != NULL) {
        csv = fopen(cfg->window_sweep_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open window sweep output: %s\n", cfg->window_sweep_out);
            goto done;
        }

        fprintf(csv, "delta,offset,cp_score,qpsk4_score\n");
        for (i = -radius; i <= radius; ++i) {
            int64_t candidate = (int64_t)start + i;
            size_t idx = (size_t)(i + radius);
            fprintf(csv,
                    "%d,%lld,%.8f,%.8f\n",
                    i,
                    (long long)candidate,
                    cp_scores[idx],
                    qpsk_scores[idx]);
        }
        fclose(csv);
        csv = NULL;
    }

    if (cfg->window_sweep_svg != NULL) {
        svg = fopen(cfg->window_sweep_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open window sweep SVG: %s\n", cfg->window_sweep_svg);
            goto done;
        }

        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"900\" height=\"420\" viewBox=\"0 0 900 420\">\n");
        fprintf(svg, "<rect width=\"900\" height=\"420\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"40\" y1=\"360\" x2=\"860\" y2=\"360\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        fprintf(svg, "<line x1=\"450\" y1=\"36\" x2=\"450\" y2=\"370\" stroke=\"#333\" stroke-width=\"1\"/>\n");
        for (i = -radius; i <= radius; ++i) {
            size_t idx = (size_t)(i + radius);
            double x = 40.0 + ((double)(i + radius) / (double)(points - 1)) * 820.0;
            double cp_h = max_cp > 0.0 ? (cp_scores[idx] / max_cp) * 300.0 : 0.0;
            double q_h = max_qpsk > 0.0 ? (qpsk_scores[idx] / max_qpsk) * 300.0 : 0.0;
            fprintf(svg, "<line x1=\"%.2f\" y1=\"360\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#7aa7ff\" stroke-width=\"1\"/>\n", x, x, 360.0 - cp_h);
            fprintf(svg, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"1.5\" fill=\"#ffcc66\" fill-opacity=\"0.65\"/>\n", x, 360.0 - q_h);
        }
        fprintf(svg, "</svg>\n");
        fclose(svg);
        svg = NULL;
    }

    fprintf(stderr,
            "[window] radius=%d best_cp_delta=%d best_cp=%.5f best_qpsk_delta=%d best_qpsk4=%.5f csv=%s svg=%s\n",
            radius,
            best_cp_delta,
            max_cp,
            best_qpsk_delta,
            max_qpsk,
            cfg->window_sweep_out != NULL ? cfg->window_sweep_out : "-",
            cfg->window_sweep_svg != NULL ? cfg->window_sweep_svg : "-");

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    free(shifted);
    free(cp_scores);
    free(qpsk_scores);
    return rc;
}

static int write_logical_constellation(const rbdvbt_config_t *cfg,
                                       const complexf_t *samples,
                                       size_t count,
                                       uint32_t start,
                                       uint32_t useful_len,
                                       uint32_t gi_len,
                                       uint32_t symbol_len,
                                       double cfo_hz,
                                       uint32_t center_bin,
                                       uint32_t spacing_bins,
                                       double center_freq_hz,
                                       double spacing_hz)
{
    uint32_t nfft = cfg->highres_fft_size;
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    FILE *csv = NULL;
    FILE *svg = NULL;
    uint32_t symbols = cfg->probe_symbols > 80u ? 80u : cfg->probe_symbols;
    uint32_t used_symbols = 0;
    uint64_t point_index = 0;
    uint64_t svg_stride = 1;
    int rc = -1;
    uint32_t s;

    if (cfg->logical_constellation_out == NULL && cfg->logical_constellation_svg == NULL) {
        return 0;
    }

    if (spacing_bins == 0 || nfft < useful_len) {
        fprintf(stderr, "[logical] invalid grid for logical constellation\n");
        return -1;
    }

    fft_in = fftwf_malloc(sizeof(*fft_in) * nfft);
    fft_out = fftwf_malloc(sizeof(*fft_out) * nfft);
    if (fft_in == NULL || fft_out == NULL) {
        fprintf(stderr, "failed to allocate logical constellation FFT buffers\n");
        goto done;
    }

    plan = fftwf_plan_dft_1d((int)nfft, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "failed to create logical constellation FFTW plan\n");
        goto done;
    }

    if (cfg->logical_constellation_out != NULL) {
        csv = fopen(cfg->logical_constellation_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open logical constellation output: %s\n", cfg->logical_constellation_out);
            goto done;
        }
        fprintf(csv, "i,q,symbol,logical_carrier,freq_hz,role_hint,phase_intercept,phase_slope,pilot_count,prbs_shift,pilot_lock\n");
    }

    if (cfg->logical_constellation_svg != NULL) {
        uint64_t estimated = (uint64_t)symbols * 1705u;
        svg_stride = estimated / 30000u;
        if (svg_stride == 0) {
            svg_stride = 1;
        }
        svg = fopen(cfg->logical_constellation_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open logical constellation SVG: %s\n", cfg->logical_constellation_svg);
            goto done;
        }
        write_svg_header(svg);
    }

    for (s = 0; s < symbols; ++s) {
        size_t base = (size_t)start + (size_t)s * symbol_len + gi_len;
        int logical;

        int prbs_shift = 0;
        double best_prbs_lock = -1.0;
        double phase_intercept = 0.0;
        double phase_slope = 0.0;
        int pilot_count = 0;

        if (base + useful_len >= count) {
            break;
        }

        for (uint32_t k = 0; k < nfft; ++k) {
            fft_in[k][0] = 0.0f;
            fft_in[k][1] = 0.0f;
        }

        for (uint32_t k = 0; k < useful_len && k < nfft; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);

        for (int shift = 0; shift < 2047; ++shift) {
            double csum = 0.0;
            double ssum = 0.0;
            int count_pilots = 0;

            for (logical = -852; logical <= 852; ++logical) {
                int bin_i = (int)center_bin + logical * (int)spacing_bins;
                uint32_t raw_bin;
                complexf_t z;
                double ref_sign;
                double phase;

                if (logical == 0 || bin_i < 0 || bin_i >= (int)nfft) {
                    continue;
                }
                if (!is_continual_pilot_logical(logical) &&
                    !is_scattered_pilot_logical(logical, (int)(s % 4u))) {
                    continue;
                }

                raw_bin = ((uint32_t)bin_i + nfft / 2u) % nfft;
                z.re = fft_out[raw_bin][0];
                z.im = fft_out[raw_bin][1];
                if (c_abs2(z) <= 0.0f) {
                    continue;
                }

                ref_sign = dvbt_pilot_ref_sign_logical_shifted(logical, shift);
                z.re = (float)(z.re * ref_sign);
                z.im = (float)(z.im * ref_sign);
                phase = atan2(z.im, z.re);
                csum += cos(phase);
                ssum += sin(phase);
                count_pilots++;
            }

            if (count_pilots > 0) {
                double lock = sqrt(csum * csum + ssum * ssum) / (double)count_pilots;
                if (lock > best_prbs_lock) {
                    best_prbs_lock = lock;
                    prbs_shift = shift;
                }
            }
        }

        {
            double fit_n = 0.0;
            double fit_x = 0.0;
            double fit_y = 0.0;
            double fit_xx = 0.0;
            double fit_xy = 0.0;
            double prev_phase = 0.0;
            int have_prev = 0;

            for (logical = -852; logical <= 852; ++logical) {
                int bin_i = (int)center_bin + logical * (int)spacing_bins;
                uint32_t raw_bin;
                complexf_t z;
                double phase;
                double ref_sign;

                if (logical == 0 || bin_i < 0 || bin_i >= (int)nfft) {
                    continue;
                }
                if (!is_continual_pilot_logical(logical) &&
                    !is_scattered_pilot_logical(logical, (int)(s % 4u))) {
                    continue;
                }

                raw_bin = ((uint32_t)bin_i + nfft / 2u) % nfft;
                z.re = fft_out[raw_bin][0];
                z.im = fft_out[raw_bin][1];
                if (c_abs2(z) <= 0.0f) {
                    continue;
                }

                ref_sign = dvbt_pilot_ref_sign_logical_shifted(logical, prbs_shift);
                z.re = (float)(z.re * ref_sign);
                z.im = (float)(z.im * ref_sign);
                phase = atan2(z.im, z.re);
                if (have_prev) {
                    while (phase - prev_phase > M_PI) {
                        phase -= 2.0 * M_PI;
                    }
                    while (phase - prev_phase < -M_PI) {
                        phase += 2.0 * M_PI;
                    }
                }
                prev_phase = phase;
                have_prev = 1;

                fit_n += 1.0;
                fit_x += (double)logical;
                fit_y += phase;
                fit_xx += (double)logical * (double)logical;
                fit_xy += (double)logical * phase;
                pilot_count++;
            }

            double denom = fit_n * fit_xx - fit_x * fit_x;
            if (fit_n >= 2.0 && fabs(denom) > 1e-12) {
                phase_slope = (fit_n * fit_xy - fit_x * fit_y) / denom;
                phase_intercept = (fit_y - phase_slope * fit_x) / fit_n;
            }
        }

        for (logical = -852; logical <= 852; ++logical) {
            int bin_i = (int)center_bin + logical * (int)spacing_bins;
            uint32_t raw_bin;
            complexf_t z;
            double mag;
            double freq_hz = center_freq_hz + (double)logical * spacing_hz;
            const char *role = "data_or_unknown";

            if (logical == 0) {
                role = "dc";
                continue;
            }
            if (bin_i < 0 || bin_i >= (int)nfft) {
                continue;
            }
            if (is_continual_pilot_logical(logical)) {
                role = "continual_pilot";
            } else if (is_scattered_pilot_logical(logical, (int)(s % 4u))) {
                role = "scattered_pilot";
            }

            raw_bin = ((uint32_t)bin_i + nfft / 2u) % nfft;
            z.re = fft_out[raw_bin][0];
            z.im = fft_out[raw_bin][1];
            z = c_rotate(z, -(phase_intercept + phase_slope * (double)logical));
            mag = sqrt((double)c_abs2(z));
            if (mag <= 0.0) {
                continue;
            }
            z.re = (float)(z.re / mag);
            z.im = (float)(z.im / mag);

            if (csv != NULL) {
                fprintf(csv,
                        "%.7f,%.7f,%u,%d,%.6f,%s,%.9f,%.9f,%d,%d,%.9f\n",
                        z.re,
                        z.im,
                        s,
                        logical,
                        freq_hz,
                        role,
                        phase_intercept,
                        phase_slope,
                        pilot_count,
                        prbs_shift,
                        best_prbs_lock);
            }
            if (svg != NULL && (point_index % svg_stride) == 0) {
                const char *color = "#48d597";
                double opacity = 0.18;
                if (role[0] == 'c' || role[0] == 's') {
                    color = "#ffcc66";
                    opacity = 0.45;
                }
                write_svg_point(svg, z, color, opacity);
            }
            point_index++;
        }
        used_symbols++;
    }

    if (svg != NULL) {
        fprintf(svg, "</svg>\n");
    }

    fprintf(stderr,
            "[logical] symbols=%u carriers=1705 center=%.2fHz spacing=%.3fHz csv=%s svg=%s\n",
            used_symbols,
            center_freq_hz,
            spacing_hz,
            cfg->logical_constellation_out != NULL ? cfg->logical_constellation_out : "-",
            cfg->logical_constellation_svg != NULL ? cfg->logical_constellation_svg : "-");

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    return rc;
}

static double fine_timing_score_candidate(const rbdvbt_config_t *cfg,
                                          const complexf_t *samples,
                                          size_t count,
                                          uint32_t candidate_start,
                                          uint32_t useful_len,
                                          uint32_t gi_len,
                                          uint32_t symbol_len,
                                          double cfo_hz,
                                          uint32_t center_bin,
                                          uint32_t spacing_bins,
                                          fftwf_complex *fft_in,
                                          fftwf_complex *fft_out,
                                          fftwf_plan plan,
                                          double *out_pilot_lock)
{
    uint32_t nfft = cfg->highres_fft_size;
    uint32_t symbols = cfg->probe_symbols > 20u ? 20u : cfg->probe_symbols;
    complexf_t qpsk4 = {0.0f, 0.0f};
    double qpsk4_power = 0.0;
    double pilot_lock_sum = 0.0;
    uint32_t pilot_lock_count = 0;
    uint32_t used_symbols = 0;

    for (uint32_t s = 0; s < symbols; ++s) {
        size_t base = (size_t)candidate_start + (size_t)s * symbol_len + gi_len;
        double fit_n = 0.0;
        double fit_x = 0.0;
        double fit_y = 0.0;
        double fit_xx = 0.0;
        double fit_xy = 0.0;
        double prev_phase2 = 0.0;
        int have_prev = 0;
        double phase_intercept = 0.0;
        double phase_slope = 0.0;

        if (base + useful_len >= count) {
            break;
        }

        for (uint32_t k = 0; k < nfft; ++k) {
            fft_in[k][0] = 0.0f;
            fft_in[k][1] = 0.0f;
        }
        for (uint32_t k = 0; k < useful_len && k < nfft; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);

        for (int logical = -852; logical <= 852; ++logical) {
            int bin_i = (int)center_bin + logical * (int)spacing_bins;
            uint32_t raw_bin;
            complexf_t z;
            complexf_t z2;
            double phase2;

            if (logical == 0 || bin_i < 0 || bin_i >= (int)nfft) {
                continue;
            }
            if (!is_continual_pilot_logical(logical) &&
                !is_scattered_pilot_logical(logical, (int)(s % 4u))) {
                continue;
            }

            raw_bin = ((uint32_t)bin_i + nfft / 2u) % nfft;
            z.re = fft_out[raw_bin][0];
            z.im = fft_out[raw_bin][1];
            if (c_abs2(z) <= 0.0f) {
                continue;
            }

            z2 = c_mul(z, z);
            phase2 = atan2(z2.im, z2.re);
            if (have_prev) {
                while (phase2 - prev_phase2 > M_PI) {
                    phase2 -= 2.0 * M_PI;
                }
                while (phase2 - prev_phase2 < -M_PI) {
                    phase2 += 2.0 * M_PI;
                }
            }
            prev_phase2 = phase2;
            have_prev = 1;

            fit_n += 1.0;
            fit_x += (double)logical;
            fit_y += phase2;
            fit_xx += (double)logical * (double)logical;
            fit_xy += (double)logical * phase2;
        }

        if (fit_n >= 2.0) {
            double denom = fit_n * fit_xx - fit_x * fit_x;
            if (fabs(denom) > 1e-12) {
                double slope2 = (fit_n * fit_xy - fit_x * fit_y) / denom;
                double intercept2 = (fit_y - slope2 * fit_x) / fit_n;
                phase_slope = 0.5 * slope2;
                phase_intercept = 0.5 * intercept2;
            }
        }

        {
            double pc = 0.0;
            double ps = 0.0;
            uint32_t pn = 0;

            for (int logical = -852; logical <= 852; ++logical) {
                int bin_i = (int)center_bin + logical * (int)spacing_bins;
                uint32_t raw_bin;
                complexf_t z;
                double mag;

                if (logical == 0 || bin_i < 0 || bin_i >= (int)nfft) {
                    continue;
                }

                raw_bin = ((uint32_t)bin_i + nfft / 2u) % nfft;
                z.re = fft_out[raw_bin][0];
                z.im = fft_out[raw_bin][1];
                z = c_rotate(z, -(phase_intercept + phase_slope * (double)logical));
                mag = sqrt((double)c_abs2(z));
                if (mag <= 0.0) {
                    continue;
                }
                z.re = (float)(z.re / mag);
                z.im = (float)(z.im / mag);

                if (is_continual_pilot_logical(logical) ||
                    is_scattered_pilot_logical(logical, (int)(s % 4u))) {
                    pc += z.re;
                    ps += z.im;
                    pn++;
                    continue;
                }

                {
                    complexf_t z2 = c_mul(z, z);
                    complexf_t z4 = c_mul(z2, z2);
                    qpsk4 = c_add(qpsk4, z4);
                    qpsk4_power += 1.0;
                }
            }

            if (pn > 0) {
                pilot_lock_sum += sqrt(pc * pc + ps * ps) / (double)pn;
                pilot_lock_count++;
            }
        }

        used_symbols++;
    }

    *out_pilot_lock = pilot_lock_count > 0 ? pilot_lock_sum / (double)pilot_lock_count : 0.0;
    if (used_symbols == 0 || qpsk4_power <= 0.0) {
        return 0.0;
    }
    return c_abs(qpsk4) / qpsk4_power;
}

static int write_fine_timing_scan(const rbdvbt_config_t *cfg,
                                  const complexf_t *samples,
                                  size_t count,
                                  uint32_t cp_start,
                                  uint32_t useful_len,
                                  uint32_t gi_len,
                                  uint32_t symbol_len,
                                  double cfo_hz,
                                  uint32_t center_bin,
                                  uint32_t spacing_bins,
                                  uint32_t *out_best_start,
                                  double *out_best_metric)
{
    uint32_t nfft = cfg->highres_fft_size;
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    FILE *csv = NULL;
    FILE *svg = NULL;
    int32_t first = (int32_t)cp_start - (int32_t)gi_len;
    int32_t last = (int32_t)cp_start + (int32_t)gi_len;
    double best_metric = -1.0;
    double max_metric = 0.0;
    uint32_t best_start = cp_start;
    int rc = -1;

    if (cfg->fine_timing_out == NULL && cfg->fine_timing_svg == NULL) {
        *out_best_start = cp_start;
        *out_best_metric = 0.0;
        return 0;
    }
    if (spacing_bins == 0 || nfft < useful_len) {
        return 0;
    }
    if (first < 0) {
        first = 0;
    }
    if (last >= (int32_t)symbol_len) {
        last = (int32_t)symbol_len - 1;
    }

    fft_in = fftwf_malloc(sizeof(*fft_in) * nfft);
    fft_out = fftwf_malloc(sizeof(*fft_out) * nfft);
    if (fft_in == NULL || fft_out == NULL) {
        fprintf(stderr, "failed to allocate fine timing FFT buffers\n");
        goto done;
    }
    plan = fftwf_plan_dft_1d((int)nfft, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "failed to create fine timing FFTW plan\n");
        goto done;
    }

    if (cfg->fine_timing_out != NULL) {
        csv = fopen(cfg->fine_timing_out, "w");
        if (csv == NULL) {
            fprintf(stderr, "failed to open fine timing output: %s\n", cfg->fine_timing_out);
            goto done;
        }
        fprintf(csv, "offset,delta,qpsk4_score,pilot_lock,combined_metric\n");
    }

    for (int32_t cand = first; cand <= last; ++cand) {
        double pilot_lock = 0.0;
        double qpsk4 = fine_timing_score_candidate(cfg,
                                                   samples,
                                                   count,
                                                   (uint32_t)cand,
                                                   useful_len,
                                                   gi_len,
                                                   symbol_len,
                                                   cfo_hz,
                                                   center_bin,
                                                   spacing_bins,
                                                   fft_in,
                                                   fft_out,
                                                   plan,
                                                   &pilot_lock);
        double metric = qpsk4 * (0.5 + pilot_lock);

        if (csv != NULL) {
            fprintf(csv, "%d,%d,%.9f,%.9f,%.9f\n", cand, cand - (int32_t)cp_start, qpsk4, pilot_lock, metric);
        }
        if (metric > best_metric) {
            best_metric = metric;
            best_start = (uint32_t)cand;
        }
        if (metric > max_metric) {
            max_metric = metric;
        }
    }

    if (cfg->fine_timing_svg != NULL) {
        svg = fopen(cfg->fine_timing_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open fine timing SVG: %s\n", cfg->fine_timing_svg);
            goto done;
        }
        fprintf(svg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"900\" height=\"360\" viewBox=\"0 0 900 360\">\n");
        fprintf(svg, "<rect width=\"900\" height=\"360\" fill=\"#111\"/>\n");
        fprintf(svg, "<line x1=\"40\" y1=\"300\" x2=\"860\" y2=\"300\" stroke=\"#555\" stroke-width=\"1\"/>\n");
        if (cfg->fine_timing_out != NULL) {
            fflush(csv);
        }
        /* SVG is generated from a second cheap pass over the just-written CSV values would be clumsy here;
           leave the CSV as the authoritative fine-timing diagnostic for now. */
        fprintf(svg, "</svg>\n");
        fclose(svg);
        svg = NULL;
    }

    *out_best_start = best_start;
    *out_best_metric = best_metric;
    fprintf(stderr,
            "[finetiming] range=%d..%d best=%u delta=%d metric=%.6f csv=%s svg=%s\n",
            first,
            last,
            best_start,
            (int32_t)best_start - (int32_t)cp_start,
            best_metric,
            cfg->fine_timing_out != NULL ? cfg->fine_timing_out : "-",
            cfg->fine_timing_svg != NULL ? cfg->fine_timing_svg : "-");

    rc = 0;

done:
    if (csv != NULL) {
        fclose(csv);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    return rc;
}

static const char *dvbt_role_name(rbdvbt_dvbt_carrier_role_t role)
{
    switch (role) {
    case RBDVBT_DVBT_CARRIER_DATA:
        return "data";
    case RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT:
        return "continual_pilot";
    case RBDVBT_DVBT_CARRIER_SCATTERED_PILOT:
        return "scattered_pilot";
    case RBDVBT_DVBT_CARRIER_TPS:
        return "tps";
    case RBDVBT_DVBT_CARRIER_UNUSED:
        return "unused";
    }

    return "unknown";
}

static uint8_t qpsk_hard_decision(complexf_t z)
{
    uint8_t symbol = 0;

    if (z.re < 0.0f) {
        symbol |= 2u;
    }
    if (z.im < 0.0f) {
        symbol |= 1u;
    }

    return symbol;
}

static void dvbt2k_symbol_deinterleave_qpsk(uint32_t symbol_index,
                                            const uint8_t *rx_sio,
                                            uint8_t *out_sii)
{
    uint32_t i;

    if ((symbol_index & 1u) == 0u) {
        for (i = 0; i < RBDVBT_DVBT_2K_DATA_CELLS; ++i) {
            uint32_t h = 0;

            if (rbdvbt_dvbt_2k_symbol_interleaver(i, &h) == 0 && h < RBDVBT_DVBT_2K_DATA_CELLS) {
                out_sii[i] = rx_sio[h];
            }
        }
    } else {
        for (i = 0; i < RBDVBT_DVBT_2K_DATA_CELLS; ++i) {
            uint32_t h = 0;

            if (rbdvbt_dvbt_2k_symbol_interleaver(i, &h) == 0 && h < RBDVBT_DVBT_2K_DATA_CELLS) {
                out_sii[h] = rx_sio[i];
            }
        }
    }
}

static void dvbt_qpsk_bit_deinterleave_block(const uint8_t *symbols,
                                             uint8_t *dibits)
{
    uint8_t bi0[126];
    uint8_t bi1[126];
    uint32_t w;

    memset(bi0, 0, sizeof(bi0));
    memset(bi1, 0, sizeof(bi1));

    for (w = 0; w < 126u; ++w) {
        uint8_t sym = symbols[w] & 0x03u;

        bi0[w] = (uint8_t)((sym >> 1) & 0x01u);
        bi1[(w + 63u) % 126u] = (uint8_t)(sym & 0x01u);
    }

    for (w = 0; w < 126u; ++w) {
        dibits[w] = (uint8_t)((bi1[w] << 1) | bi0[w]);
    }
}

typedef struct {
    uint8_t dibit;
    float x_cost[2];
    float y_cost[2];
} qpsk_dibit_t;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t has_x;
    uint8_t has_y;
    float x_cost[2];
    float y_cost[2];
} viterbi_pair_t;

typedef struct {
    int valid;
    rbdvbt_fec_t fec;
    double metrics[128];
} live_viterbi_state_t;

static live_viterbi_state_t live_viterbi_state;

typedef struct {
    int valid;
    rbdvbt_fec_t fec;
    double metrics[128];
    uint8_t *history;
    size_t history_len;
    uint64_t steps;
    uint8_t out_byte;
    uint32_t out_bits;
} live_viterbi_stream_state_t;

static live_viterbi_stream_state_t live_viterbi_stream;

typedef struct {
    complexf_t *items;
    uint32_t *model_symbols;
    size_t count;
    size_t cap;
    size_t symbol_cap;
    uint64_t seq_start;
    uint32_t model_symbol_start;
    uint32_t symbol_count;
    int continuous;
} live_soft_dibit_fifo_t;

static live_soft_dibit_fifo_t live_soft_dibit_fifo;
#define LIVE_SOFT_FRAME_SYMBOLS 512u
#define LIVE_DECODE_WINDOW_SYMBOLS 256u
#define LIVE_VITERBI_CONSUME_SYMBOLS 64u
#define VITERBI_STATE_COUNT 64u

typedef struct live_decode_job {
    rbdvbt_fec_t fec;
    complexf_t *symbols;
    uint32_t *model_symbols;
    size_t symbol_cell_count;
    uint64_t seq_start;
    uint32_t model_symbol_start;
    uint32_t symbol_count;
    int continuous;
    const char *path;
    const char *ts_path;
    rbdvbt_status_context_t status;
    char symbol_rate[16];
    uint64_t epoch;
    struct live_decode_job *next;
} live_decode_job_t;

static pthread_mutex_t live_decode_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t live_decode_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t live_decode_space_cond = PTHREAD_COND_INITIALIZER;
static pthread_t live_decode_thread;
static int live_decode_thread_started;
static int live_decode_stop;
static live_decode_job_t *live_decode_head;
static live_decode_job_t *live_decode_tail;
static size_t live_decode_jobs;
static uint32_t live_decode_queued_symbols;
static uint32_t live_decode_processing_symbols;
static uint64_t live_decode_epoch;
static int live_decode_gap_pending;

#define LIVE_DECODE_MAX_QUEUED_SYMBOLS 1024u

typedef struct {
    int active;
    double t0;
    uint32_t chunks;
    double lock_sum;
    double lock_min;
    double snr_sum;
    double snr_min;
    uint32_t cont_bad;
    uint64_t max_delta_samples;
    uint64_t max_phase_delta_samples;
    uint32_t fifo_max_symbols;
    uint32_t fifo_drops;
    uint32_t fifo_drop_symbols;
    uint32_t low_pilot_drops;
    uint32_t packets;
    uint32_t sync_bad;
    uint32_t transport_errors;
    uint32_t cc_errors;
    uint32_t rs_uncorrectable;
} live_health_t;

static pthread_mutex_t live_health_mutex = PTHREAD_MUTEX_INITIALIZER;
static live_health_t live_health;

#define LIVE_HEALTH_PERIOD_SECONDS 5.0

static void live_health_reset_locked(double now)
{
    memset(&live_health, 0, sizeof(live_health));
    live_health.active = 1;
    live_health.t0 = now;
    live_health.lock_min = 1.0e9;
    live_health.snr_min = 1.0e9;
}

static void live_health_maybe_emit_locked(double now)
{
    double elapsed;

    if (!live_health.active) {
        live_health_reset_locked(now);
        return;
    }
    elapsed = now - live_health.t0;
    if (elapsed < LIVE_HEALTH_PERIOD_SECONDS) {
        return;
    }

    if (live_health.chunks > 0u || live_health.packets > 0u ||
        live_health.fifo_drops > 0u || live_health.cont_bad > 0u) {
        double lock_avg = live_health.chunks > 0u ?
            live_health.lock_sum / (double)live_health.chunks : 0.0;
        double snr_avg = live_health.chunks > 0u ?
            live_health.snr_sum / (double)live_health.chunks : 0.0;
        double lock_min = live_health.lock_min < 1.0e8 ? live_health.lock_min : 0.0;
        double snr_min = live_health.snr_min < 1.0e8 ? live_health.snr_min : 0.0;

        fprintf(stderr,
                "[health] %.1fs chunks=%u lock_min=%.5f lock_avg=%.5f snr_min=%.2fdB snr_avg=%.2fdB cont_bad=%u max_delta=%llu max_phase_delta=%llu fifo_max=%u fifo_drops=%u fifo_drop_symbols=%u low_pilot=%u packets=%u rs_uncorr=%u cc=%u tei=%u sync_bad=%u\n",
                elapsed,
                live_health.chunks,
                lock_min,
                lock_avg,
                snr_min,
                snr_avg,
                live_health.cont_bad,
                (unsigned long long)live_health.max_delta_samples,
                (unsigned long long)live_health.max_phase_delta_samples,
                live_health.fifo_max_symbols,
                live_health.fifo_drops,
                live_health.fifo_drop_symbols,
                live_health.low_pilot_drops,
                live_health.packets,
                live_health.rs_uncorrectable,
                live_health.cc_errors,
                live_health.transport_errors,
                live_health.sync_bad);
    }
    live_health_reset_locked(now);
}

static void live_health_note_frontend(double lock,
                                      double snr_db,
                                      int have_continuity_measurement,
                                      int frontend_continuous,
                                      uint64_t delta_samples,
                                      uint64_t phase_delta_samples)
{
    double now = monotonic_seconds();

    pthread_mutex_lock(&live_health_mutex);
    if (!live_health.active) {
        live_health_reset_locked(now);
    }
    live_health.chunks++;
    live_health.lock_sum += lock;
    live_health.snr_sum += snr_db;
    if (lock < live_health.lock_min) {
        live_health.lock_min = lock;
    }
    if (snr_db < live_health.snr_min) {
        live_health.snr_min = snr_db;
    }
    if (have_continuity_measurement && !frontend_continuous) {
        live_health.cont_bad++;
    }
    if (delta_samples > live_health.max_delta_samples) {
        live_health.max_delta_samples = delta_samples;
    }
    if (phase_delta_samples > live_health.max_phase_delta_samples) {
        live_health.max_phase_delta_samples = phase_delta_samples;
    }
    live_health_maybe_emit_locked(now);
    pthread_mutex_unlock(&live_health_mutex);
}

static void live_health_note_fifo(uint32_t queued_symbols,
                                  uint32_t processing_symbols,
                                  uint32_t dropped_symbols)
{
    double now = monotonic_seconds();
    uint32_t total_symbols = queued_symbols + processing_symbols;

    pthread_mutex_lock(&live_health_mutex);
    if (!live_health.active) {
        live_health_reset_locked(now);
    }
    if (total_symbols > live_health.fifo_max_symbols) {
        live_health.fifo_max_symbols = total_symbols;
    }
    if (dropped_symbols > 0u) {
        live_health.fifo_drops++;
        live_health.fifo_drop_symbols += dropped_symbols;
    }
    live_health_maybe_emit_locked(now);
    pthread_mutex_unlock(&live_health_mutex);
}

static void live_health_note_low_pilot(void)
{
    double now = monotonic_seconds();

    pthread_mutex_lock(&live_health_mutex);
    if (!live_health.active) {
        live_health_reset_locked(now);
    }
    live_health.low_pilot_drops++;
    live_health_maybe_emit_locked(now);
    pthread_mutex_unlock(&live_health_mutex);
}

void rbdvbt_live_health_note_outer(uint32_t packets,
                                   uint32_t sync_bad,
                                   uint32_t transport_errors,
                                   uint32_t cc_errors,
                                   uint32_t rs_uncorrectable)
{
    double now = monotonic_seconds();

    pthread_mutex_lock(&live_health_mutex);
    if (!live_health.active) {
        live_health_reset_locked(now);
    }
    live_health.packets += packets;
    live_health.sync_bad += sync_bad;
    live_health.transport_errors += transport_errors;
    live_health.cc_errors += cc_errors;
    live_health.rs_uncorrectable += rs_uncorrectable;
    live_health_maybe_emit_locked(now);
    pthread_mutex_unlock(&live_health_mutex);
}

static void live_decode_free_job(live_decode_job_t *job)
{
    if (job == NULL) {
        return;
    }
    free(job->symbols);
    free(job->model_symbols);
    free(job);
}

static size_t live_decode_drop_symbol_locked(void)
{
    live_decode_job_t *job = live_decode_head;
    size_t cells = RBDVBT_DVBT_2K_DATA_CELLS;

    if (job == NULL || job->symbol_count == 0u || job->symbol_cell_count < cells) {
        return 0u;
    }

    if (job->symbol_count == 1u) {
        live_decode_head = job->next;
        if (live_decode_head == NULL) {
            live_decode_tail = NULL;
        }
        live_decode_jobs--;
        if (live_decode_queued_symbols > 0u) {
            live_decode_queued_symbols--;
        }
        live_decode_free_job(job);
        return 1u;
    }

    memmove(job->symbols,
            &job->symbols[cells],
            (job->symbol_cell_count - cells) * sizeof(*job->symbols));
    if (job->model_symbols != NULL) {
        memmove(job->model_symbols,
                &job->model_symbols[1],
                ((size_t)job->symbol_count - 1u) * sizeof(*job->model_symbols));
    }
    job->symbol_cell_count -= cells;
    job->symbol_count--;
    job->seq_start++;
    job->model_symbol_start = job->model_symbols != NULL ?
        job->model_symbols[0] :
        job->model_symbol_start + 1u;
    job->continuous = 0;
    if (live_decode_queued_symbols > 0u) {
        live_decode_queued_symbols--;
    }
    return 1u;
}

static size_t live_decode_drop_symbols_locked(uint32_t symbols)
{
    size_t dropped = 0u;

    while (symbols > 0u && live_decode_head != NULL) {
        size_t n = live_decode_drop_symbol_locked();

        if (n == 0u) {
            break;
        }
        dropped += n;
        symbols--;
    }
    return dropped;
}

static uint32_t live_decode_invalidate_queued_locked(const char *reason)
{
    uint32_t dropped_symbols = live_decode_queued_symbols;
    uint32_t processing_symbols = live_decode_processing_symbols;
    size_t dropped_jobs = live_decode_jobs;

    while (live_decode_head != NULL) {
        live_decode_job_t *job = live_decode_head;
        live_decode_head = job->next;
        live_decode_free_job(job);
    }
    live_decode_tail = NULL;
    live_decode_jobs = 0u;
    live_decode_queued_symbols = 0u;
    live_decode_gap_pending = 0;
    live_decode_epoch++;
    gui_fifo_submit(live_decode_queued_symbols,
                    live_decode_processing_symbols,
                    LIVE_DECODE_MAX_QUEUED_SYMBOLS);
    pthread_cond_broadcast(&live_decode_space_cond);

    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        fprintf(stderr,
                "[fifo1] reset epoch=%llu reason=%s dropped_jobs=%zu dropped_symbols=%u processing_symbols=%u\n",
                (unsigned long long)live_decode_epoch,
                reason != NULL ? reason : "-",
                dropped_jobs,
                dropped_symbols,
                live_decode_processing_symbols);
    }
    return processing_symbols;
}

static uint32_t live_decode_invalidate_queued(const char *reason)
{
    uint32_t processing_symbols;

    pthread_mutex_lock(&live_decode_mutex);
    processing_symbols = live_decode_invalidate_queued_locked(reason);
    pthread_mutex_unlock(&live_decode_mutex);
    return processing_symbols;
}

static void live_decode_wait_idle(const char *reason)
{
    double t0 = monotonic_seconds();
    uint32_t processing_symbols;

    pthread_mutex_lock(&live_decode_mutex);
    while (live_decode_processing_symbols > 0u) {
        pthread_cond_wait(&live_decode_space_cond, &live_decode_mutex);
    }
    processing_symbols = live_decode_processing_symbols;
    pthread_mutex_unlock(&live_decode_mutex);

    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        double t1 = monotonic_seconds();
        fprintf(stderr,
                "[fifo1] idle epoch=%llu reason=%s processing_symbols=%u wait=%.3fs\n",
                (unsigned long long)live_decode_epoch,
                reason != NULL ? reason : "-",
                processing_symbols,
                t1 - t0);
    }
}
#define LIVE_METADATA_PILOT_LOCK_MIN 0.45
#define LIVE_METADATA_PILOT_LOCK_STRONG 0.55
#define LIVE_METADATA_SEARCH_RADIUS 96u
#define LIVE_METADATA_REFINE_SYMBOLS 4u
#define LIVE_METADATA_VERIFY_SYMBOLS 8u
#define LIVE_FRONTEND_CONTINUITY_MAX_SAMPLE_ERROR 16u
#define LIVE_GRDVBT_TRACK_RADIUS 8u

static void qpsk_axis_bit_costs(float value, float costs[2])
{
    const float target = 0.70710678f;
    float d0 = value - target;
    float d1 = value + target;

    costs[0] = d0 * d0;
    costs[1] = d1 * d1;
}

static void dvbt2k_symbol_deinterleave_complex(uint32_t symbol_index,
                                               const complexf_t *rx_sio,
                                               complexf_t *out_sii)
{
    uint32_t i;

    if ((symbol_index & 1u) == 0u) {
        for (i = 0; i < RBDVBT_DVBT_2K_DATA_CELLS; ++i) {
            uint32_t h = 0;

            if (rbdvbt_dvbt_2k_symbol_interleaver(i, &h) == 0 && h < RBDVBT_DVBT_2K_DATA_CELLS) {
                out_sii[i] = rx_sio[h];
            }
        }
    } else {
        for (i = 0; i < RBDVBT_DVBT_2K_DATA_CELLS; ++i) {
            uint32_t h = 0;

            if (rbdvbt_dvbt_2k_symbol_interleaver(i, &h) == 0 && h < RBDVBT_DVBT_2K_DATA_CELLS) {
                out_sii[h] = rx_sio[i];
            }
        }
    }
}

static void dvbt_qpsk_soft_bit_deinterleave_block(const complexf_t *symbols,
                                                  const uint8_t *dibits,
                                                  qpsk_dibit_t *soft_dibits)
{
    float x_costs[126][2];
    float y_costs[126][2];
    uint32_t w;

    memset(x_costs, 0, sizeof(x_costs));
    memset(y_costs, 0, sizeof(y_costs));

    for (w = 0; w < 126u; ++w) {
        qpsk_axis_bit_costs(symbols[w].re, x_costs[w]);
        qpsk_axis_bit_costs(symbols[w].im, y_costs[(w + 63u) % 126u]);
    }

    for (w = 0; w < 126u; ++w) {
        soft_dibits[w].dibit = (uint8_t)(dibits[w] & 0x03u);
        soft_dibits[w].x_cost[0] = x_costs[w][0];
        soft_dibits[w].x_cost[1] = x_costs[w][1];
        soft_dibits[w].y_cost[0] = y_costs[w][0];
        soft_dibits[w].y_cost[1] = y_costs[w][1];
    }
}

static int dvbt_qpsk_inner_deinterleave_symbols(const complexf_t *raw_symbols,
                                                size_t symbol_cell_count,
                                                uint32_t model_symbol_start,
                                                qpsk_dibit_t **out_dibits,
                                                size_t *out_dibit_count)
{
    qpsk_dibit_t *dibits;
    size_t full_symbols = symbol_cell_count / RBDVBT_DVBT_2K_DATA_CELLS;
    size_t out_count = full_symbols * RBDVBT_DVBT_2K_DATA_CELLS;

    if (out_dibits == NULL || out_dibit_count == NULL) {
        return -1;
    }
    *out_dibits = NULL;
    *out_dibit_count = 0u;
    if (out_count == 0u) {
        return 0;
    }

    dibits = malloc(out_count * sizeof(*dibits));
    if (dibits == NULL) {
        fprintf(stderr, "failed to allocate inner deinterleave dibits\n");
        return -1;
    }

    for (size_t sym = 0; sym < full_symbols; ++sym) {
        const complexf_t *raw = &raw_symbols[sym * RBDVBT_DVBT_2K_DATA_CELLS];
        complexf_t rx_sii_soft[RBDVBT_DVBT_2K_DATA_CELLS];
        uint8_t rx_sii[RBDVBT_DVBT_2K_DATA_CELLS];

        memset(rx_sii_soft, 0, sizeof(rx_sii_soft));
        memset(rx_sii, 0, sizeof(rx_sii));

        dvbt2k_symbol_deinterleave_complex(model_symbol_start + (uint32_t)sym,
                                           raw,
                                           rx_sii_soft);

        for (uint32_t i = 0; i < RBDVBT_DVBT_2K_DATA_CELLS; ++i) {
            rx_sii[i] = qpsk_hard_decision(rx_sii_soft[i]);
        }

        for (uint32_t block = 0; block < RBDVBT_DVBT_2K_DATA_CELLS / 126u; ++block) {
            uint8_t hard_dibits[126];
            qpsk_dibit_t soft_dibits[126];
            uint32_t base_index = block * 126u;

            dvbt_qpsk_bit_deinterleave_block(&rx_sii[base_index], hard_dibits);
            dvbt_qpsk_soft_bit_deinterleave_block(&rx_sii_soft[base_index],
                                                  hard_dibits,
                                                  soft_dibits);
            memcpy(&dibits[sym * RBDVBT_DVBT_2K_DATA_CELLS + base_index],
                   soft_dibits,
                   sizeof(soft_dibits));
        }
    }

    *out_dibits = dibits;
    *out_dibit_count = out_count;
    return 0;
}

static uint8_t dvbt_conv_parity_for_state(uint8_t state)
{
    uint8_t count = 0;
    uint8_t parity;

    if (state & 0x01u) {
        count++;
    }
    if (state & 0x02u) {
        count++;
    }
    if (state & 0x08u) {
        count++;
    }
    if (state & 0x10u) {
        count++;
    }
    if (state & 0x40u) {
        count++;
    }
    parity = (uint8_t)((count & 1u) << 1);

    count = 0;
    if (state & 0x01u) {
        count++;
    }
    if (state & 0x08u) {
        count++;
    }
    if (state & 0x10u) {
        count++;
    }
    if (state & 0x20u) {
        count++;
    }
    if (state & 0x40u) {
        count++;
    }
    parity |= (uint8_t)(count & 1u);

    return parity;
}

static void depuncture_pair_set(viterbi_pair_t *pair,
                                int has_x,
                                uint8_t x,
                                const float x_cost[2],
                                int has_y,
                                uint8_t y,
                                const float y_cost[2])
{
    pair->x = (uint8_t)(x & 1u);
    pair->y = (uint8_t)(y & 1u);
    pair->has_x = has_x ? 1u : 0u;
    pair->has_y = has_y ? 1u : 0u;
    pair->x_cost[0] = has_x && x_cost != NULL ? x_cost[0] : 0.0f;
    pair->x_cost[1] = has_x && x_cost != NULL ? x_cost[1] : 0.0f;
    pair->y_cost[0] = has_y && y_cost != NULL ? y_cost[0] : 0.0f;
    pair->y_cost[1] = has_y && y_cost != NULL ? y_cost[1] : 0.0f;
}

static uint32_t viterbi_traceback_bytes(rbdvbt_fec_t fec)
{
    switch (fec) {
    case RBDVBT_FEC_1_2:
        return 5u;
    case RBDVBT_FEC_2_3:
        return 9u;
    case RBDVBT_FEC_3_4:
        return 10u;
    case RBDVBT_FEC_5_6:
        return 15u;
    case RBDVBT_FEC_7_8:
        return 24u;
    case RBDVBT_FEC_AUTO:
        return 9u;
    }
    return 9u;
}

static void live_viterbi_stream_reset(void)
{
    live_viterbi_stream.valid = 0;
    live_viterbi_stream.fec = RBDVBT_FEC_AUTO;
    live_viterbi_stream.steps = 0u;
    live_viterbi_stream.out_byte = 0u;
    live_viterbi_stream.out_bits = 0u;
}

static size_t dvbt_depuncture_dibits(rbdvbt_fec_t fec,
                                     const qpsk_dibit_t *dibits,
                                     size_t dibit_count,
                                     viterbi_pair_t **out_pairs);

static int live_viterbi_stream_prepare(rbdvbt_fec_t fec)
{
    size_t traceback_bits = (size_t)viterbi_traceback_bytes(fec) * 8u;

    if (traceback_bits == 0u) {
        return -1;
    }
    if (live_viterbi_stream.history_len != traceback_bits) {
        uint8_t *history = realloc(live_viterbi_stream.history, traceback_bits * VITERBI_STATE_COUNT);

        if (history == NULL) {
            return -1;
        }
        live_viterbi_stream.history = history;
        live_viterbi_stream.history_len = traceback_bits;
    }

    for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
        live_viterbi_stream.metrics[s] = 0.0;
    }
    live_viterbi_stream.fec = fec;
    live_viterbi_stream.steps = 0u;
    live_viterbi_stream.out_byte = 0u;
    live_viterbi_stream.out_bits = 0u;
    live_viterbi_stream.valid = 1;
    return 0;
}

static int live_viterbi_stream_emit_bit(uint8_t bit,
                                        uint8_t **out,
                                        size_t *out_count,
                                        size_t *out_cap)
{
    live_viterbi_stream.out_byte = (uint8_t)((live_viterbi_stream.out_byte << 1) | (bit & 1u));
    live_viterbi_stream.out_bits++;
    if (live_viterbi_stream.out_bits < 8u) {
        return 0;
    }

    if (*out_count >= *out_cap) {
        size_t new_cap = *out_cap != 0u ? *out_cap * 2u : 4096u;
        uint8_t *new_out = realloc(*out, new_cap);

        if (new_out == NULL) {
            return -1;
        }
        *out = new_out;
        *out_cap = new_cap;
    }
    (*out)[(*out_count)++] = live_viterbi_stream.out_byte;
    live_viterbi_stream.out_byte = 0u;
    live_viterbi_stream.out_bits = 0u;
    return 0;
}

static int live_viterbi_stream_decode_bytes(rbdvbt_fec_t fec,
                                            const qpsk_dibit_t *dibits,
                                            size_t dibit_count,
                                            int continuous,
                                            uint8_t **out_bytes,
                                            size_t *out_byte_count,
                                            size_t *out_pair_count,
                                            double *out_best_metric)
{
    const double inf = 1.0e100;
    viterbi_pair_t *pairs = NULL;
    uint8_t *out = NULL;
    size_t out_count = 0u;
    size_t out_cap = 0u;
    size_t pair_count;
    size_t traceback_bits = (size_t)viterbi_traceback_bytes(fec) * 8u;
    double best_metric = 0.0;
    int rc = -1;

    *out_bytes = NULL;
    *out_byte_count = 0u;
    if (out_pair_count != NULL) {
        *out_pair_count = 0u;
    }
    if (out_best_metric != NULL) {
        *out_best_metric = 0.0;
    }

    if (!continuous || !live_viterbi_stream.valid || live_viterbi_stream.fec != fec) {
        if (live_viterbi_stream_prepare(fec) != 0) {
            fprintf(stderr, "failed to initialize live Viterbi stream\n");
            goto done;
        }
    }

    pair_count = dvbt_depuncture_dibits(fec, dibits, dibit_count, &pairs);
    if (pair_count == 0u || pairs == NULL) {
        fprintf(stderr, "failed to depuncture live Viterbi input\n");
        goto done;
    }
    if (out_pair_count != NULL) {
        *out_pair_count = pair_count;
    }

    for (size_t t = 0; t < pair_count; ++t) {
        double next_metrics[VITERBI_STATE_COUNT];
        double step_best = inf;
        int best_state = 0;
        uint8_t *history_row = &live_viterbi_stream.history[(live_viterbi_stream.steps % traceback_bits) * VITERBI_STATE_COUNT];

        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            next_metrics[s] = inf;
            history_row[s] = 0u;
        }

        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            if (live_viterbi_stream.metrics[s] >= inf) {
                continue;
            }
            for (int bit = 0; bit <= 1; ++bit) {
                uint8_t reg = (uint8_t)(((uint8_t)bit << 6) | (uint8_t)s);
                uint8_t next_state = (uint8_t)(reg >> 1);
                uint8_t expected = dvbt_conv_parity_for_state(reg);
                double branch = 0.0;
                double metric;

                if (pairs[t].has_x) {
                    branch += pairs[t].x_cost[expected & 1u];
                }
                if (pairs[t].has_y) {
                    branch += pairs[t].y_cost[(expected >> 1) & 1u];
                }

                metric = live_viterbi_stream.metrics[s] + branch;
                if (metric < next_metrics[next_state]) {
                    next_metrics[next_state] = metric;
                    history_row[next_state] = (uint8_t)s;
                }
            }
        }

        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            if (next_metrics[s] < step_best) {
                step_best = next_metrics[s];
                best_state = (int)s;
            }
        }
        if (step_best >= inf) {
            live_viterbi_stream_reset();
            goto done;
        }
        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            live_viterbi_stream.metrics[s] = next_metrics[s] - step_best;
        }
        best_metric += step_best;
        live_viterbi_stream.steps++;

        if (live_viterbi_stream.steps > traceback_bits) {
            uint8_t state = (uint8_t)best_state;

            for (size_t k = 0; k < traceback_bits; ++k) {
                uint64_t step = live_viterbi_stream.steps - 1u - k;
                const uint8_t *row = &live_viterbi_stream.history[(step % traceback_bits) * VITERBI_STATE_COUNT];

                state = row[state];
            }
            if (live_viterbi_stream_emit_bit((uint8_t)((state >> 5) & 1u),
                                             &out,
                                             &out_count,
                                             &out_cap) != 0) {
                goto done;
            }
        }
    }

    *out_bytes = out;
    *out_byte_count = out_count;
    if (out_best_metric != NULL) {
        *out_best_metric = best_metric;
    }
    out = NULL;
    rc = 0;

done:
    free(pairs);
    free(out);
    return rc;
}

static size_t dvbt_depuncture_dibits(rbdvbt_fec_t fec,
                                     const qpsk_dibit_t *dibits,
                                     size_t dibit_count,
                                     viterbi_pair_t **out_pairs)
{
    size_t group_in = 1;
    size_t group_out = 1;
    size_t groups;
    size_t pair_count;
    viterbi_pair_t *pairs;
    size_t i;
    size_t o = 0;

    switch (fec) {
    case RBDVBT_FEC_AUTO:
        *out_pairs = NULL;
        return 0;
    case RBDVBT_FEC_1_2:
        group_in = 1;
        group_out = 1;
        break;
    case RBDVBT_FEC_2_3:
        group_in = 3;
        group_out = 4;
        break;
    case RBDVBT_FEC_3_4:
        group_in = 2;
        group_out = 3;
        break;
    case RBDVBT_FEC_5_6:
        group_in = 3;
        group_out = 5;
        break;
    case RBDVBT_FEC_7_8:
        group_in = 4;
        group_out = 7;
        break;
    }

    groups = dibit_count / group_in;
    pair_count = groups * group_out;
    pairs = calloc(pair_count, sizeof(*pairs));
    if (pairs == NULL) {
        *out_pairs = NULL;
        return 0;
    }

    for (i = 0; i < groups; ++i) {
        const qpsk_dibit_t *t = &dibits[i * group_in];

        switch (fec) {
        case RBDVBT_FEC_AUTO:
            break;
        case RBDVBT_FEC_1_2:
            depuncture_pair_set(&pairs[o++], 1, t[0].dibit & 1u, t[0].x_cost, 1, (t[0].dibit >> 1) & 1u, t[0].y_cost);
            break;
        case RBDVBT_FEC_2_3:
            depuncture_pair_set(&pairs[o++], 1, t[0].dibit & 1u, t[0].x_cost, 1, (t[0].dibit >> 1) & 1u, t[0].y_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[1].dibit & 1u, t[1].x_cost);
            depuncture_pair_set(&pairs[o++], 1, (t[1].dibit >> 1) & 1u, t[1].y_cost, 1, t[2].dibit & 1u, t[2].x_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, (t[2].dibit >> 1) & 1u, t[2].y_cost);
            break;
        case RBDVBT_FEC_3_4:
            depuncture_pair_set(&pairs[o++], 1, t[0].dibit & 1u, t[0].x_cost, 1, (t[0].dibit >> 1) & 1u, t[0].y_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[1].dibit & 1u, t[1].x_cost);
            depuncture_pair_set(&pairs[o++], 1, (t[1].dibit >> 1) & 1u, t[1].y_cost, 0, 0, NULL);
            break;
        case RBDVBT_FEC_5_6:
            depuncture_pair_set(&pairs[o++], 1, t[0].dibit & 1u, t[0].x_cost, 1, (t[0].dibit >> 1) & 1u, t[0].y_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[1].dibit & 1u, t[1].x_cost);
            depuncture_pair_set(&pairs[o++], 1, (t[1].dibit >> 1) & 1u, t[1].y_cost, 0, 0, NULL);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[2].dibit & 1u, t[2].x_cost);
            depuncture_pair_set(&pairs[o++], 1, (t[2].dibit >> 1) & 1u, t[2].y_cost, 0, 0, NULL);
            break;
        case RBDVBT_FEC_7_8:
            depuncture_pair_set(&pairs[o++], 1, t[0].dibit & 1u, t[0].x_cost, 1, (t[0].dibit >> 1) & 1u, t[0].y_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[1].dibit & 1u, t[1].x_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, (t[1].dibit >> 1) & 1u, t[1].y_cost);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[2].dibit & 1u, t[2].x_cost);
            depuncture_pair_set(&pairs[o++], 1, (t[2].dibit >> 1) & 1u, t[2].y_cost, 0, 0, NULL);
            depuncture_pair_set(&pairs[o++], 0, 0, NULL, 1, t[3].dibit & 1u, t[3].x_cost);
            depuncture_pair_set(&pairs[o++], 1, (t[3].dibit >> 1) & 1u, t[3].y_cost, 0, 0, NULL);
            break;
        }
    }

    *out_pairs = pairs;
    return pair_count;
}

static int dvbt_viterbi_decode_pairs(const viterbi_pair_t *pairs,
                                     size_t pair_count,
                                     const double *initial_metrics,
                                     double *final_metrics,
                                     uint8_t **out_bits,
                                     size_t *out_bit_count,
                                     double *out_best_metric)
{
    const double inf = 1.0e100;
    double metrics[VITERBI_STATE_COUNT];
    double next_metrics[VITERBI_STATE_COUNT];
    uint8_t *prev_states = NULL;
    uint8_t *bits = NULL;
    int best_state = 0;
    double best_metric = inf;
    size_t t;

    prev_states = malloc(pair_count * VITERBI_STATE_COUNT);
    bits = malloc(pair_count);
    if (prev_states == NULL || bits == NULL) {
        free(prev_states);
        free(bits);
        return -1;
    }

    for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
        metrics[s] = initial_metrics != NULL ? initial_metrics[s] : 0.0;
    }

    for (t = 0; t < pair_count; ++t) {
        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            next_metrics[s] = inf;
            prev_states[t * VITERBI_STATE_COUNT + (size_t)s] = 0;
        }

        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            if (metrics[s] >= inf) {
                continue;
            }

            for (int bit = 0; bit <= 1; ++bit) {
                uint8_t reg = (uint8_t)(((uint8_t)bit << 6) | (uint8_t)s);
                uint8_t next_state = (uint8_t)(reg >> 1);
                uint8_t expected = dvbt_conv_parity_for_state(reg);
                double branch = 0.0;
                double metric;

                if (pairs[t].has_x) {
                    branch += pairs[t].x_cost[expected & 1u];
                }
                if (pairs[t].has_y) {
                    branch += pairs[t].y_cost[(expected >> 1) & 1u];
                }

                metric = metrics[s] + branch;
                if (metric < next_metrics[next_state]) {
                    next_metrics[next_state] = metric;
                    prev_states[t * VITERBI_STATE_COUNT + (size_t)next_state] = (uint8_t)s;
                }
            }
        }

        memcpy(metrics, next_metrics, sizeof(metrics));
    }

    for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
        if (metrics[s] < best_metric) {
            best_metric = metrics[s];
            best_state = (int)s;
        }
    }

    if (final_metrics != NULL) {
        for (uint32_t s = 0; s < VITERBI_STATE_COUNT; ++s) {
            final_metrics[s] = metrics[s] - best_metric;
        }
        for (uint32_t s = VITERBI_STATE_COUNT; s < 128u; ++s) {
            final_metrics[s] = inf;
        }
    }

    for (t = pair_count; t > 0; --t) {
        bits[t - 1u] = (uint8_t)((best_state >> 5) & 1u);
        best_state = prev_states[(t - 1u) * VITERBI_STATE_COUNT + (size_t)best_state];
    }

    free(prev_states);
    *out_bits = bits;
    *out_bit_count = pair_count;
    if (out_best_metric != NULL) {
        *out_best_metric = best_metric;
    }
    return 0;
}

static int decode_viterbi_bytes(rbdvbt_fec_t fec,
                                const qpsk_dibit_t *dibits,
                                size_t dibit_count,
                                const double *initial_metrics,
                                double *final_metrics,
                                uint8_t **out_bytes,
                                size_t *out_byte_count,
                                size_t *out_pair_count,
                                double *out_best_metric)
{
    viterbi_pair_t *pairs = NULL;
    uint8_t *bits = NULL;
    uint8_t *bytes = NULL;
    size_t pair_count;
    size_t bit_count = 0;
    size_t byte_count;
    double best_metric = 0.0;
    int rc = -1;

    *out_bytes = NULL;
    *out_byte_count = 0;
    if (out_pair_count != NULL) {
        *out_pair_count = 0;
    }
    if (out_best_metric != NULL) {
        *out_best_metric = 0.0;
    }

    pair_count = dvbt_depuncture_dibits(fec, dibits, dibit_count, &pairs);
    if (pair_count == 0 || pairs == NULL) {
        fprintf(stderr, "failed to depuncture Viterbi input\n");
        goto done;
    }

    if (dvbt_viterbi_decode_pairs(pairs,
                                  pair_count,
                                  initial_metrics,
                                  final_metrics,
                                  &bits,
                                  &bit_count,
                                  &best_metric) != 0) {
        fprintf(stderr, "failed to allocate Viterbi traceback buffers\n");
        goto done;
    }

    byte_count = bit_count / 8u;
    bytes = calloc(byte_count, sizeof(*bytes));
    if (bytes == NULL) {
        fprintf(stderr, "failed to allocate Viterbi byte output\n");
        goto done;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        uint8_t b = 0;

        for (uint32_t bit = 0; bit < 8u; ++bit) {
            b = (uint8_t)((b << 1) | (bits[i * 8u + bit] & 1u));
        }
        bytes[i] = b;
    }

    *out_bytes = bytes;
    *out_byte_count = byte_count;
    if (out_pair_count != NULL) {
        *out_pair_count = pair_count;
    }
    if (out_best_metric != NULL) {
        *out_best_metric = best_metric;
    }
    bytes = NULL;
    rc = 0;

done:
    free(pairs);
    free(bits);
    free(bytes);
    return rc;
}

static int write_selected_viterbi_output(rbdvbt_fec_t fec,
                                         const uint8_t *bytes,
                                         size_t byte_count,
                                         size_t dibit_count,
	                                         size_t pair_count,
	                                         double best_metric,
	                                         uint64_t seq_start,
	                                         uint32_t symbol_count,
	                                         int continuous,
	                                         const char *path,
	                                         const char *ts_path,
	                                         const rbdvbt_status_context_t *status)
{
	    FILE *f = NULL;
	    int rc = -1;

    if (path != NULL) {
        f = fopen(path, "wb");
        if (f == NULL) {
            fprintf(stderr, "failed to open Viterbi output: %s\n", path);
            goto done;
        }
        if (byte_count > 0 && fwrite(bytes, 1, byte_count, f) != byte_count) {
            fprintf(stderr, "failed to write Viterbi output: %s\n", path);
            goto done;
        }
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        fprintf(stderr,
                "[viterbi] fec=%s input_dibits=%zu mother_pairs=%zu decoded_bits=%zu decoded_bytes=%zu best_metric=%.2f output=%s\n",
                rbdvbt_fec_name(fec),
                dibit_count,
                pair_count,
                byte_count * 8u,
                byte_count,
                best_metric,
                path != NULL ? path : "-");
    }

	    if (ts_path != NULL) {
	        if (status != NULL && status->live_mode) {
	            (void)seq_start;
	            (void)symbol_count;
	            if (!continuous) {
	                rbdvbt_outer_reset_live_stream();
	            }
	            if (rbdvbt_outer_recover_ts(bytes, byte_count, ts_path, status) != 0) {
	                goto done;
	            }
        } else if (rbdvbt_outer_recover_ts(bytes, byte_count, ts_path, status) != 0) {
            goto done;
        }
    }

    rc = 0;

done:
    if (f != NULL) {
        fclose(f);
    }
    return rc;
}

static int write_viterbi_output(rbdvbt_fec_t fec,
                                const qpsk_dibit_t *dibits,
                                size_t dibit_count,
                                uint64_t seq_start,
                                uint32_t symbol_count,
                                int continuous,
                                const char *path,
                                const char *ts_path,
                                const rbdvbt_status_context_t *status)
{
    static const rbdvbt_fec_t fec_candidates[] = {
        RBDVBT_FEC_1_2,
        RBDVBT_FEC_2_3,
        RBDVBT_FEC_3_4,
        RBDVBT_FEC_5_6,
        RBDVBT_FEC_7_8
    };
    uint8_t *bytes = NULL;
    size_t byte_count = 0;
    size_t pair_count = 0;
    double best_metric = 0.0;
    double viterbi_t0 = 0.0;
    double viterbi_t1 = 0.0;
    double output_t1 = 0.0;
	
	    if (fec != RBDVBT_FEC_AUTO) {
	        double final_metrics[128];
		        const double *initial_metrics = NULL;
		
                if (status != NULL && status->live_mode) {
                    viterbi_t0 = monotonic_seconds();
                }
		        if (status != NULL && status->live_mode) {
		            if (live_viterbi_stream_decode_bytes(fec,
	                                                 dibits,
	                                                 dibit_count,
	                                                 continuous,
	                                                 &bytes,
	                                                 &byte_count,
	                                                 &pair_count,
	                                                 &best_metric) != 0) {
	                live_viterbi_stream_reset();
	                live_viterbi_state.valid = 0;
		                return -1;
		            }
                    viterbi_t1 = monotonic_seconds();
		            if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
	                fprintf(stderr,
	                        "[viterbi] live_stream=%s fec=%s traceback_bytes=%u emitted_bytes=%zu\n",
	                        continuous && live_viterbi_stream.valid ? "continued" : "cold",
	                        rbdvbt_fec_name(fec),
	                        viterbi_traceback_bytes(fec),
	                        byte_count);
	            }
	        } else {
	            if (decode_viterbi_bytes(fec,
	                                     dibits,
	                                     dibit_count,
	                                     initial_metrics,
	                                     final_metrics,
	                                     &bytes,
	                                     &byte_count,
	                                     &pair_count,
	                                     &best_metric) != 0) {
	                live_viterbi_state.valid = 0;
	                return -1;
	            }
	        }
		        {
	            int rc = write_selected_viterbi_output(fec,
                                                   bytes,
                                                   byte_count,
                                                   dibit_count,
                                                   pair_count,
	                                                   best_metric,
	                                                   seq_start,
	                                                   symbol_count,
	                                                   continuous,
	                                                   path,
		                                                   ts_path,
	                                                   status);
                if (status != NULL && status->live_mode) {
                    output_t1 = monotonic_seconds();
                    fprintf(stderr,
                            "[viterbi-time] seq=%llu symbols=%u dibits=%zu bytes=%zu viterbi=%.3fs output_outer=%.3fs total=%.3fs\n",
                            (unsigned long long)seq_start,
                            symbol_count,
                            dibit_count,
                            byte_count,
                            viterbi_t1 > viterbi_t0 ? viterbi_t1 - viterbi_t0 : 0.0,
                            output_t1 > viterbi_t1 ? output_t1 - viterbi_t1 : 0.0,
                            output_t1 > viterbi_t0 ? output_t1 - viterbi_t0 : 0.0);
                }
	            free(bytes);
            if (status != NULL && status->live_mode && rc == 0) {
                live_viterbi_state.valid = live_viterbi_stream.valid;
                live_viterbi_state.fec = fec;
            } else if (status != NULL && status->live_mode) {
                live_viterbi_stream_reset();
                live_viterbi_state.valid = 0;
            } else if (rc == 0) {
                memcpy(live_viterbi_state.metrics, final_metrics, sizeof(final_metrics));
                live_viterbi_state.fec = fec;
                live_viterbi_state.valid = 1;
            } else {
                live_viterbi_state.valid = 0;
            }
            return rc;
        }
    }

    {
        rbdvbt_fec_t selected_fec = RBDVBT_FEC_1_2;
        uint8_t *selected_bytes = NULL;
        size_t selected_byte_count = 0;
        size_t selected_pair_count = 0;
        double selected_metric = 0.0;
        uint32_t selected_score = 0;

        for (size_t i = 0; i < sizeof(fec_candidates) / sizeof(fec_candidates[0]); ++i) {
            uint8_t *candidate_bytes = NULL;
            size_t candidate_byte_count = 0;
            size_t candidate_pair_count = 0;
            double candidate_metric = 0.0;
            uint32_t candidate_score = 0;

            if (decode_viterbi_bytes(fec_candidates[i],
                                     dibits,
                                     dibit_count,
                                     NULL,
                                     NULL,
                                     &candidate_bytes,
                                     &candidate_byte_count,
                                     &candidate_pair_count,
                                     &candidate_metric) != 0) {
                continue;
            }

            if (ts_path != NULL) {
                rbdvbt_outer_metrics_t metrics;

                if (rbdvbt_outer_analyze_inner(candidate_bytes, candidate_byte_count, &metrics) == 0) {
                    candidate_score = metrics.score;
                    fprintf(stderr,
                            "[auto-fec] candidate=%s score=%u packets=%u pat=%u pmt=%u rs_uncorrectable=%u cc_errors=%u sync_bad=%u metric=%.2f\n",
                            rbdvbt_fec_name(fec_candidates[i]),
                            candidate_score,
                            metrics.packets,
                            metrics.pat_packets,
                            metrics.pmt_packets,
                            metrics.rs_uncorrectable,
                            metrics.cc_errors,
                            metrics.sync_bad,
                            candidate_metric);
                }
            } else if (candidate_pair_count > 0u) {
                double normalized = candidate_metric / (double)candidate_pair_count;

                candidate_score = normalized > 0.0 ? (uint32_t)(1000000.0 / (1.0 + normalized)) : 1000000u;
                fprintf(stderr,
                        "[auto-fec] candidate=%s score=%u normalized_metric=%.6f metric=%.2f\n",
                        rbdvbt_fec_name(fec_candidates[i]),
                        candidate_score,
                        normalized,
                        candidate_metric);
            }

            if (selected_bytes == NULL || candidate_score > selected_score) {
                free(selected_bytes);
                selected_bytes = candidate_bytes;
                selected_byte_count = candidate_byte_count;
                selected_pair_count = candidate_pair_count;
                selected_metric = candidate_metric;
                selected_fec = fec_candidates[i];
                selected_score = candidate_score;
                candidate_bytes = NULL;
            }

            free(candidate_bytes);
        }

        if (selected_bytes == NULL) {
            fprintf(stderr, "[auto-fec] failed to decode any FEC candidate\n");
            return -1;
        }

        fprintf(stderr,
                "[auto-fec] selected=%s score=%u\n",
                rbdvbt_fec_name(selected_fec),
                selected_score);

        {
            rbdvbt_status_context_t selected_status;
            const rbdvbt_status_context_t *selected_status_ptr = status;
            int rc;

            if (status != NULL) {
                selected_status = *status;
                selected_status.fec = rbdvbt_fec_name(selected_fec);
                selected_status_ptr = &selected_status;
            }

            rc = write_selected_viterbi_output(selected_fec,
                                               selected_bytes,
                                               selected_byte_count,
                                               dibit_count,
                                               selected_pair_count,
	                                               selected_metric,
	                                               seq_start,
	                                               symbol_count,
	                                               continuous,
	                                               path,
	                                               ts_path,
                                               selected_status_ptr);
            free(selected_bytes);
            return rc;
        }
    }
}

static void *live_decode_worker_main(void *arg)
{
    (void)arg;

    for (;;) {
        live_decode_job_t *job = NULL;

        pthread_mutex_lock(&live_decode_mutex);
        while (!live_decode_stop && live_decode_head == NULL) {
            pthread_cond_wait(&live_decode_cond, &live_decode_mutex);
        }
        if (live_decode_stop && live_decode_head == NULL) {
            pthread_mutex_unlock(&live_decode_mutex);
            break;
        }
        job = live_decode_head;
        live_decode_head = job->next;
        if (live_decode_head == NULL) {
            live_decode_tail = NULL;
        }
        live_decode_jobs--;
        if (live_decode_queued_symbols >= job->symbol_count) {
            live_decode_queued_symbols -= job->symbol_count;
        } else {
            live_decode_queued_symbols = 0u;
        }
        live_decode_processing_symbols += job->symbol_count;
        gui_fifo_submit(live_decode_queued_symbols,
                        live_decode_processing_symbols,
                        LIVE_DECODE_MAX_QUEUED_SYMBOLS);
        pthread_cond_signal(&live_decode_space_cond);
        pthread_mutex_unlock(&live_decode_mutex);

        double worker_t0 = monotonic_seconds();
        double worker_deint_time = 0.0;
        double worker_viterbi_outer_time = 0.0;
        uint32_t processed_symbols = 0u;
        int worker_failed = 0;
        int worker_stale = 0;

        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
		                    "[fifo1] worker inner_start seq=%llu symbols=%u cells=%zu model_symbol=%u frontend_continuous=%d queued=%zu\n",
	                    (unsigned long long)job->seq_start,
	                    job->symbol_count,
	                    job->symbol_cell_count,
	                    job->model_symbol_start,
                    job->continuous,
                    live_decode_jobs);
        }
        {
            size_t consume_cells = (size_t)LIVE_VITERBI_CONSUME_SYMBOLS * RBDVBT_DVBT_2K_DATA_CELLS;
            size_t offset = 0u;
            uint32_t symbol_offset = 0u;

            if (consume_cells == 0u) {
                consume_cells = job->symbol_cell_count;
            }
            while (offset < job->symbol_cell_count) {
                qpsk_dibit_t *viterbi_dibits = NULL;
                size_t viterbi_dibit_count = 0u;
                size_t count = job->symbol_cell_count - offset;
                uint32_t symbols = job->symbol_count - symbol_offset;
                int chunk_continuous = job->continuous || offset > 0u;

                pthread_mutex_lock(&live_decode_mutex);
                if (job->epoch != live_decode_epoch) {
                    worker_stale = 1;
                    pthread_mutex_unlock(&live_decode_mutex);
                    free(viterbi_dibits);
                    live_viterbi_stream_reset();
                    live_viterbi_state.valid = 0;
                    rbdvbt_outer_reset_live_stream();
                    break;
                }
                pthread_mutex_unlock(&live_decode_mutex);

                if (count > consume_cells) {
                    count = consume_cells;
                }
                symbols = (uint32_t)(count / RBDVBT_DVBT_2K_DATA_CELLS);
                if (symbols == 0u) {
                    symbols = 1u;
                }
	                uint32_t chunk_model_symbol = job->model_symbols != NULL && symbol_offset < job->symbol_count ?
	                    job->model_symbols[symbol_offset] :
	                    job->model_symbol_start + symbol_offset;
                double chunk_t0 = monotonic_seconds();

	                if (dvbt_qpsk_inner_deinterleave_symbols(&job->symbols[offset],
                                                         count,
                                                         chunk_model_symbol,
                                                         &viterbi_dibits,
	                                                         &viterbi_dibit_count) != 0) {
                    fprintf(stderr, "[fifo1] worker inner_deinterleave_failed\n");
                    live_viterbi_stream_reset();
                    live_viterbi_state.valid = 0;
	                    worker_failed = 1;
	                    break;
	                }
                double chunk_deint_t1 = monotonic_seconds();
                worker_deint_time += chunk_deint_t1 - chunk_t0;

                pthread_mutex_lock(&live_decode_mutex);
                if (job->epoch != live_decode_epoch) {
                    worker_stale = 1;
                    pthread_mutex_unlock(&live_decode_mutex);
                    free(viterbi_dibits);
                    live_viterbi_stream_reset();
                    live_viterbi_state.valid = 0;
                    rbdvbt_outer_reset_live_stream();
                    break;
                }
                pthread_mutex_unlock(&live_decode_mutex);

	                if (write_viterbi_output(job->fec,
                                         viterbi_dibits,
                                         viterbi_dibit_count,
                                         job->seq_start + symbol_offset,
                                         symbols,
                                         chunk_continuous,
                                         job->path,
                                         job->ts_path,
                                         &job->status) != 0) {
                    fprintf(stderr, "[fifo1] worker inner_failed\n");
                    live_viterbi_stream_reset();
                    live_viterbi_state.valid = 0;
                    free(viterbi_dibits);
	                    worker_failed = 1;
	                    break;
	                }
                pthread_mutex_lock(&live_decode_mutex);
                if (job->epoch != live_decode_epoch) {
                    worker_stale = 1;
                    pthread_mutex_unlock(&live_decode_mutex);
                    live_viterbi_stream_reset();
                    live_viterbi_state.valid = 0;
                    rbdvbt_outer_reset_live_stream();
                    break;
                }
                pthread_mutex_unlock(&live_decode_mutex);
                worker_viterbi_outer_time += monotonic_seconds() - chunk_deint_t1;
	                free(viterbi_dibits);
                offset += count;
                symbol_offset += symbols;
                processed_symbols += symbols;
            }
        }
        if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
            double worker_t1 = monotonic_seconds();
	            fprintf(stderr,
	                    "[fifo1] worker_done seq=%llu processed_symbols=%u job_symbols=%u deint=%.3fs viterbi_outer=%.3fs elapsed=%.3fs status=%s\n",
	                    (unsigned long long)job->seq_start,
	                    processed_symbols,
	                    job->symbol_count,
	                    worker_deint_time,
	                    worker_viterbi_outer_time,
	                    worker_t1 - worker_t0,
	                    worker_stale ? "stale" : (worker_failed ? "failed" : "ok"));
        }
        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr, "[fifo1] worker inner_done\n");
        }
        pthread_mutex_lock(&live_decode_mutex);
        if (live_decode_processing_symbols >= job->symbol_count) {
            live_decode_processing_symbols -= job->symbol_count;
        } else {
            live_decode_processing_symbols = 0u;
        }
        gui_fifo_submit(live_decode_queued_symbols,
                        live_decode_processing_symbols,
                        LIVE_DECODE_MAX_QUEUED_SYMBOLS);
        pthread_cond_broadcast(&live_decode_space_cond);
        pthread_mutex_unlock(&live_decode_mutex);
        live_decode_free_job(job);
    }

    return NULL;
}

static int live_decode_worker_start(void)
{
    if (live_decode_thread_started) {
        return 0;
    }
    live_decode_stop = 0;
    if (pthread_create(&live_decode_thread, NULL, live_decode_worker_main, NULL) != 0) {
        fprintf(stderr, "failed to start live decode worker\n");
        return -1;
    }
    live_decode_thread_started = 1;
    return 0;
}

static int live_decode_enqueue(rbdvbt_fec_t fec,
                               const complexf_t *symbols,
                               const uint32_t *model_symbols,
                               size_t symbol_cell_count,
                               uint64_t seq_start,
                               uint32_t model_symbol_start,
                               uint32_t symbol_count,
                               int continuous,
                               const char *path,
                               const char *ts_path,
                               const rbdvbt_status_context_t *status)
{
    live_decode_job_t *job;

    if (live_decode_worker_start() != 0) {
        return -1;
    }

    job = calloc(1, sizeof(*job));
    if (job == NULL) {
        fprintf(stderr, "failed to allocate live decode job\n");
        return -1;
    }
    job->symbols = malloc(symbol_cell_count * sizeof(*job->symbols));
    if (job->symbols == NULL) {
        fprintf(stderr, "failed to allocate live decode job symbols\n");
        free(job);
        return -1;
    }
    memcpy(job->symbols, symbols, symbol_cell_count * sizeof(*job->symbols));
    if (symbol_count > 0u) {
        job->model_symbols = malloc((size_t)symbol_count * sizeof(*job->model_symbols));
        if (job->model_symbols == NULL) {
            fprintf(stderr, "failed to allocate live decode job model symbols\n");
            free(job->symbols);
            free(job);
            return -1;
        }
        if (model_symbols != NULL) {
            memcpy(job->model_symbols, model_symbols, (size_t)symbol_count * sizeof(*job->model_symbols));
        } else {
            for (uint32_t i = 0; i < symbol_count; ++i) {
                job->model_symbols[i] = model_symbol_start + i;
            }
        }
    }
    job->symbol_cell_count = symbol_cell_count;
    job->seq_start = seq_start;
    job->model_symbol_start = model_symbol_start;
    job->symbol_count = symbol_count;
    job->continuous = continuous;
    job->fec = fec;
    job->path = path;
    job->ts_path = ts_path;
    job->status = *status;
    snprintf(job->symbol_rate, sizeof(job->symbol_rate), "%s", status->symbol_rate != NULL ? status->symbol_rate : "unknown");
    job->status.symbol_rate = job->symbol_rate;

    pthread_mutex_lock(&live_decode_mutex);
    if (!live_decode_stop &&
        live_decode_queued_symbols + symbol_count > LIVE_DECODE_MAX_QUEUED_SYMBOLS) {
        live_decode_gap_pending = 1;
        gui_fifo_submit(live_decode_queued_symbols,
                        live_decode_processing_symbols,
                        LIVE_DECODE_MAX_QUEUED_SYMBOLS);
        if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
            fprintf(stderr,
                    "[fifo1] backlog full dropping incoming_symbols=%u incoming_bits=%zu queued_symbols=%u processing_symbols=%u capacity_symbols=%u\n",
                    symbol_count,
                    symbol_cell_count * 2u,
                    live_decode_queued_symbols,
                    live_decode_processing_symbols,
                    LIVE_DECODE_MAX_QUEUED_SYMBOLS);
        }
        live_health_note_fifo(live_decode_queued_symbols,
                              live_decode_processing_symbols,
                              symbol_count);
        pthread_mutex_unlock(&live_decode_mutex);
        live_decode_free_job(job);
        return 0;
    }
    if (live_decode_stop) {
        pthread_mutex_unlock(&live_decode_mutex);
        live_decode_free_job(job);
        return -1;
    }
    job->epoch = live_decode_epoch;
    if (live_decode_gap_pending) {
        job->continuous = 0;
        live_decode_gap_pending = 0;
    }
    if (live_decode_tail != NULL) {
        live_decode_tail->next = job;
    } else {
        live_decode_head = job;
    }
    live_decode_tail = job;
    live_decode_jobs++;
    live_decode_queued_symbols += symbol_count;
    gui_fifo_submit(live_decode_queued_symbols,
                    live_decode_processing_symbols,
                    LIVE_DECODE_MAX_QUEUED_SYMBOLS);
    live_health_note_fifo(live_decode_queued_symbols,
                          live_decode_processing_symbols,
                          0u);
    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO) || live_decode_jobs > 2u) {
        fprintf(stderr,
		                "[fifo1] queued_inner seq=%llu symbols=%u cells=%zu model_symbol=%u frontend_continuous=%d queued=%zu queued_symbols=%u processing_symbols=%u\n",
		                (unsigned long long)seq_start,
		                symbol_count,
		                symbol_cell_count,
		                model_symbol_start,
                job->continuous,
                live_decode_jobs,
                live_decode_queued_symbols,
                live_decode_processing_symbols);
    }
    pthread_cond_signal(&live_decode_cond);
    pthread_mutex_unlock(&live_decode_mutex);
    return 0;
}

void rbdvbt_live_decoder_shutdown(void)
{
    if (live_decode_thread_started) {
        pthread_mutex_lock(&live_decode_mutex);
        live_decode_stop = 1;
        pthread_cond_broadcast(&live_decode_cond);
        pthread_cond_broadcast(&live_decode_space_cond);
        pthread_mutex_unlock(&live_decode_mutex);
        pthread_join(live_decode_thread, NULL);
        live_decode_thread_started = 0;
    }

    (void)live_decode_drop_symbols_locked(live_decode_queued_symbols);
    live_decode_queued_symbols = 0u;
    live_decode_processing_symbols = 0u;
    live_decode_gap_pending = 0;
    gui_fifo_submit(live_decode_queued_symbols,
                    live_decode_processing_symbols,
                    LIVE_DECODE_MAX_QUEUED_SYMBOLS);

}

static int live_soft_dibit_fifo_process(rbdvbt_fec_t fec,
                                        const complexf_t *symbols,
                                        const uint32_t *model_symbols,
                                        size_t symbol_cell_count,
                                        uint32_t symbol_count,
                                        uint64_t symbol_seq_start,
                                        uint32_t model_symbol_start,
                                        size_t preferred_window,
                                        int continuous_frontend,
                                        const char *path,
                                        const char *ts_path,
                                        const rbdvbt_status_context_t *status)
{
    int rc = 0;
    uint64_t append_seq = symbol_seq_start;

    if (!continuous_frontend) {
        live_soft_dibit_fifo.count = 0u;
        live_soft_dibit_fifo.symbol_count = 0u;
        live_soft_dibit_fifo.model_symbol_start = model_symbol_start;
        live_soft_dibit_fifo.continuous = 0;
        live_viterbi_stream_reset();
        live_viterbi_state.valid = 0;
    }

    if (!continuous_frontend && symbol_count > 0u) {
        preferred_window = (size_t)symbol_count * RBDVBT_DVBT_2K_DATA_CELLS;
    } else if (preferred_window == 0u) {
        preferred_window = (size_t)LIVE_DECODE_WINDOW_SYMBOLS * RBDVBT_DVBT_2K_DATA_CELLS;
    }
    if (preferred_window == 0u) {
        return 0;
    }

    if (symbol_cell_count > 0u) {
        size_t need = live_soft_dibit_fifo.count + symbol_cell_count;

        if (need > live_soft_dibit_fifo.cap) {
            size_t new_cap = live_soft_dibit_fifo.cap != 0u ? live_soft_dibit_fifo.cap : preferred_window * 2u;
            complexf_t *new_items;

            while (new_cap < need) {
                new_cap *= 2u;
            }
            new_items = realloc(live_soft_dibit_fifo.items, new_cap * sizeof(*new_items));
            if (new_items == NULL) {
                fprintf(stderr, "failed to grow live soft-dibit FIFO\n");
                live_soft_dibit_fifo.count = 0u;
                live_viterbi_stream_reset();
                live_viterbi_state.valid = 0;
                return -1;
            }
            live_soft_dibit_fifo.items = new_items;
            live_soft_dibit_fifo.cap = new_cap;
        }
        if ((size_t)live_soft_dibit_fifo.symbol_count + (size_t)symbol_count > live_soft_dibit_fifo.symbol_cap) {
            size_t need_symbols = (size_t)live_soft_dibit_fifo.symbol_count + (size_t)symbol_count;
            size_t new_symbol_cap = live_soft_dibit_fifo.symbol_cap != 0u ? live_soft_dibit_fifo.symbol_cap : 512u;
            uint32_t *new_model_symbols;

            while (new_symbol_cap < need_symbols) {
                new_symbol_cap *= 2u;
            }
            new_model_symbols = realloc(live_soft_dibit_fifo.model_symbols,
                                        new_symbol_cap * sizeof(*new_model_symbols));
            if (new_model_symbols == NULL) {
                fprintf(stderr, "failed to grow live soft-dibit model-symbol FIFO\n");
                live_soft_dibit_fifo.count = 0u;
                live_soft_dibit_fifo.symbol_count = 0u;
                live_viterbi_stream_reset();
                live_viterbi_state.valid = 0;
                return -1;
            }
            live_soft_dibit_fifo.model_symbols = new_model_symbols;
            live_soft_dibit_fifo.symbol_cap = new_symbol_cap;
        }

        memcpy(&live_soft_dibit_fifo.items[live_soft_dibit_fifo.count],
               symbols,
               symbol_cell_count * sizeof(*symbols));
        if (symbol_count > 0u) {
            if (model_symbols != NULL) {
                memcpy(&live_soft_dibit_fifo.model_symbols[live_soft_dibit_fifo.symbol_count],
                       model_symbols,
                       (size_t)symbol_count * sizeof(*model_symbols));
            } else {
                for (uint32_t i = 0; i < symbol_count; ++i) {
                    live_soft_dibit_fifo.model_symbols[live_soft_dibit_fifo.symbol_count + i] =
                        model_symbol_start + i;
                }
            }
        }
        if (live_soft_dibit_fifo.count == 0u) {
            live_soft_dibit_fifo.seq_start = append_seq;
            live_soft_dibit_fifo.model_symbol_start = model_symbol_start;
        }
        live_soft_dibit_fifo.count += symbol_cell_count;
        live_soft_dibit_fifo.symbol_count += symbol_count;
        live_soft_dibit_fifo.continuous = continuous_frontend;
        live_soft_next_symbol_seq = symbol_seq_start + symbol_count;
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        fprintf(stderr,
	                "[softfifo] appended_seq=%llu appended_symbols=%u frame_symbols=%u buffered_symbols=%u buffered_dibits=%zu window_symbols=%u frontend_continuous=%d\n",
	                (unsigned long long)append_seq,
                symbol_count,
                LIVE_SOFT_FRAME_SYMBOLS,
                live_soft_dibit_fifo.symbol_count,
                live_soft_dibit_fifo.count,
                (uint32_t)(preferred_window / RBDVBT_DVBT_2K_DATA_CELLS),
                continuous_frontend);
    }

    while (live_soft_dibit_fifo.count >= preferred_window &&
           live_soft_dibit_fifo.symbol_count >= (uint32_t)(preferred_window / RBDVBT_DVBT_2K_DATA_CELLS)) {
        uint32_t window_symbols = (uint32_t)(preferred_window / RBDVBT_DVBT_2K_DATA_CELLS);
        uint64_t window_seq = live_soft_dibit_fifo.seq_start;
        int window_continuous = live_soft_dibit_fifo.continuous;
        int window_rc = status != NULL && status->live_mode ?
            live_decode_enqueue(fec,
                                live_soft_dibit_fifo.items,
                                live_soft_dibit_fifo.model_symbols,
                                preferred_window,
                                window_seq,
                                live_soft_dibit_fifo.model_symbol_start,
                                window_symbols,
                                window_continuous,
                                path,
                                ts_path,
                                status) : -1;

        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[frontend-batch] fifo1_window seq=%llu symbols=%u cells=%zu frontend_continuous=%d buffered_symbols=%u\n",
                    (unsigned long long)window_seq,
                    window_symbols,
                    preferred_window,
                    window_continuous,
                    live_soft_dibit_fifo.symbol_count);
        }

        if (!(status != NULL && status->live_mode)) {
            qpsk_dibit_t *viterbi_dibits = NULL;
            size_t viterbi_dibit_count = 0u;

            if (dvbt_qpsk_inner_deinterleave_symbols(live_soft_dibit_fifo.items,
                                                     preferred_window,
                                                     live_soft_dibit_fifo.model_symbol_start,
                                                     &viterbi_dibits,
                                                     &viterbi_dibit_count) != 0) {
                window_rc = -1;
            } else {
                window_rc = write_viterbi_output(fec,
                                                 viterbi_dibits,
                                                 viterbi_dibit_count,
                                                 window_seq,
                                                 window_symbols,
                                                 window_continuous,
                                                 path,
                                                 ts_path,
                                                 status);
            }
            free(viterbi_dibits);
        }

        if (window_rc != 0) {
            rc = window_rc;
            if (!continuous_frontend) {
                live_soft_dibit_fifo.count = 0u;
                live_soft_dibit_fifo.symbol_count = 0u;
                live_viterbi_stream_reset();
                live_viterbi_state.valid = 0;
                break;
            }
        }

        live_soft_dibit_fifo.count -= preferred_window;
        live_soft_dibit_fifo.symbol_count -= window_symbols;
        live_soft_dibit_fifo.seq_start += window_symbols;
        live_soft_dibit_fifo.model_symbol_start =
            live_soft_dibit_fifo.symbol_count > 0u ?
            live_soft_dibit_fifo.model_symbols[window_symbols] :
            live_soft_dibit_fifo.model_symbol_start + window_symbols;
        if (live_soft_dibit_fifo.count > 0u) {
            memmove(live_soft_dibit_fifo.items,
                    &live_soft_dibit_fifo.items[preferred_window],
                    live_soft_dibit_fifo.count * sizeof(*live_soft_dibit_fifo.items));
        }
        if (live_soft_dibit_fifo.symbol_count > 0u) {
            memmove(live_soft_dibit_fifo.model_symbols,
                    &live_soft_dibit_fifo.model_symbols[window_symbols],
                    (size_t)live_soft_dibit_fifo.symbol_count * sizeof(*live_soft_dibit_fifo.model_symbols));
        }
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        fprintf(stderr,
                "[softfifo] after_consume seq_start=%llu buffered_symbols=%u buffered_dibits=%zu\n",
                (unsigned long long)live_soft_dibit_fifo.seq_start,
                live_soft_dibit_fifo.symbol_count,
                live_soft_dibit_fifo.count);
    }

    return rc;
}

static int estimate_dvbt2k_pilot_phase(const complexf_t *shifted,
                                       uint32_t symbol_index,
                                       int bin_shift,
                                       int conjugate,
                                       double *out_intercept,
                                       double *out_slope,
                                       double *out_lock,
                                       uint32_t *out_pilot_count)
{
    double fit_n = 0.0;
    double fit_x = 0.0;
    double fit_y = 0.0;
    double fit_xx = 0.0;
    double fit_xy = 0.0;
    double prev_phase = 0.0;
    double coh_re = 0.0;
    double coh_im = 0.0;
    int have_prev = 0;
    uint32_t pilot_count = 0;
    uint32_t logical_k;

    for (logical_k = 0; logical_k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++logical_k) {
        rbdvbt_dvbt_carrier_role_t role = rbdvbt_dvbt_2k_carrier_role(symbol_index, logical_k);
        int bin;
        int sign;
        complexf_t z;
        double phase;
        double x;

        if (role != RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT &&
            role != RBDVBT_DVBT_CARRIER_SCATTERED_PILOT) {
            continue;
        }

        bin = (int)rbdvbt_dvbt_2k_logical_to_fft_bin(logical_k) + bin_shift;
        if (bin < 0 || bin >= (int)RBDVBT_DVBT_2K_FFT_SIZE) {
            continue;
        }

        z = shifted[bin];
        if (conjugate) {
            z.im = -z.im;
        }
        if (c_abs2(z) <= 0.0f) {
            continue;
        }

        sign = rbdvbt_dvbt_2k_pilot_sign(logical_k);
        if (sign < 0) {
            z.re = -z.re;
            z.im = -z.im;
        }

        phase = atan2(z.im, z.re);
        if (have_prev) {
            while (phase - prev_phase > M_PI) {
                phase -= 2.0 * M_PI;
            }
            while (phase - prev_phase < -M_PI) {
                phase += 2.0 * M_PI;
            }
        }
        prev_phase = phase;
        have_prev = 1;

        x = (double)((int)logical_k - 852);
        fit_n += 1.0;
        fit_x += x;
        fit_y += phase;
        fit_xx += x * x;
        fit_xy += x * phase;
        pilot_count++;
    }

    *out_intercept = 0.0;
    *out_slope = 0.0;
    *out_lock = 0.0;
    *out_pilot_count = pilot_count;

    if (fit_n >= 2.0) {
        double denom = fit_n * fit_xx - fit_x * fit_x;

        if (fabs(denom) > 1e-12) {
            *out_slope = (fit_n * fit_xy - fit_x * fit_y) / denom;
            *out_intercept = (fit_y - *out_slope * fit_x) / fit_n;
        }
    }

    if (pilot_count > 0) {
        for (logical_k = 0; logical_k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++logical_k) {
            rbdvbt_dvbt_carrier_role_t role = rbdvbt_dvbt_2k_carrier_role(symbol_index, logical_k);
            int bin;
            int sign;
            complexf_t z;
            double x;
            double mag;

            if (role != RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT &&
                role != RBDVBT_DVBT_CARRIER_SCATTERED_PILOT) {
                continue;
            }

            bin = (int)rbdvbt_dvbt_2k_logical_to_fft_bin(logical_k) + bin_shift;
            if (bin < 0 || bin >= (int)RBDVBT_DVBT_2K_FFT_SIZE) {
                continue;
            }

            z = shifted[bin];
            if (conjugate) {
                z.im = -z.im;
            }
            if (c_abs2(z) <= 0.0f) {
                continue;
            }

            sign = rbdvbt_dvbt_2k_pilot_sign(logical_k);
            if (sign < 0) {
                z.re = -z.re;
                z.im = -z.im;
            }

            x = (double)((int)logical_k - 852);
            z = c_rotate(z, -(*out_intercept + *out_slope * x));
            mag = sqrt((double)c_abs2(z));
            if (mag <= 0.0) {
                continue;
            }
            coh_re += z.re / mag;
            coh_im += z.im / mag;
        }
        *out_lock = sqrt(coh_re * coh_re + coh_im * coh_im) / (double)pilot_count;
    }

    return pilot_count >= 2u ? 0 : -1;
}

static int write_dvbt2k_qpsk_constellation(const rbdvbt_config_t *cfg,
                                           const complexf_t *samples,
                                           size_t count,
                                           uint32_t start,
                                           uint32_t gi_len,
                                           uint32_t symbol_len,
                                           double cfo_hz)
{
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    complexf_t *shifted = NULL;
    FILE *out = NULL;
    FILE *svg = NULL;
    FILE *eq_out = NULL;
    FILE *eq_svg = NULL;
    FILE *demap_out = NULL;
    complexf_t *viterbi_symbols = NULL;
    uint32_t *viterbi_model_symbols = NULL;
    size_t viterbi_symbol_cell_count = 0;
    size_t viterbi_symbol_cell_cap = 0;
    uint32_t viterbi_model_symbol_count = 0;
    uint32_t max_symbols = cfg->probe_symbols;
    uint32_t total_symbols = cfg->probe_symbols;
    uint32_t skip_symbols = 0;
    uint32_t used_symbols = 0;
    uint64_t point_index = 0;
    uint64_t svg_stride = 1;
    double pilot_lock_sum = 0.0;
    double evm_error_sum = 0.0;
    uint64_t evm_count = 0;
    uint32_t pilot_lock_count = 0;
    uint64_t demap_dibits = 0;
    int best_bin_shift = 0;
    int best_conjugate = 0;
    int best_symbol_phase = 0;
    double best_alignment_lock = -1.0;
    uint64_t frame_symbol_seq_start = live_soft_next_symbol_seq;
    uint64_t first_symbol_sample_abs = 0u;
    uint64_t next_symbol_sample_abs = 0u;
    uint64_t expected_symbol_sample_abs = 0u;
    int frontend_continuous = 0;
    int rc = -1;
    double demod_t0 = 0.0;
    double demod_scan_t1 = 0.0;

    rbdvbt_dvbt_2k_model_init();

    if (cfg->live_mode) {
        size_t first_base = (size_t)start + gi_len;

        if (count > first_base + RBDVBT_DVBT_2K_FFT_SIZE) {
            uint32_t available_symbols = (uint32_t)((count - first_base - RBDVBT_DVBT_2K_FFT_SIZE) / symbol_len) + 1u;

            if (available_symbols > max_symbols) {
                if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
                    fprintf(stderr,
                            "[ofdm-live] expanding demap symbols from %u to %u to avoid leaving complete OFDM symbols behind\n",
                            max_symbols,
                            available_symbols);
                }
                max_symbols = available_symbols;
                total_symbols = available_symbols;
            }
        }
    }

    fft_in = fftwf_malloc(sizeof(*fft_in) * RBDVBT_DVBT_2K_FFT_SIZE);
    fft_out = fftwf_malloc(sizeof(*fft_out) * RBDVBT_DVBT_2K_FFT_SIZE);
    shifted = calloc(RBDVBT_DVBT_2K_FFT_SIZE, sizeof(*shifted));
    if (fft_in == NULL || fft_out == NULL || shifted == NULL) {
        fprintf(stderr, "failed to allocate DVB-T 2K constellation buffers\n");
        goto done;
    }

    plan = fftwf_plan_dft_1d((int)RBDVBT_DVBT_2K_FFT_SIZE,
                             fft_in,
                             fft_out,
                             FFTW_FORWARD,
                             FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "failed to create DVB-T 2K FFTW plan\n");
        goto done;
    }

    demod_t0 = monotonic_seconds();
    {
        uint32_t scan_symbols = max_symbols > 24u ? 24u : max_symbols;
        int bin_shift;
        int conjugate;
        int symbol_phase;
        int bin_first = -64;
        int bin_last = 64;
        int conjugate_first = 0;
        int conjugate_last = 1;
        int phase_first = 0;
        int phase_last = 3;

        if (cfg->live_mode) {
            scan_symbols = max_symbols > 4u ? 4u : max_symbols;
            if (live_frontend_cursor.valid &&
                live_frontend_cursor.sample_rate_hz == cfg->sample_rate_hz &&
                live_frontend_cursor.symbol_len == symbol_len) {
                bin_first = live_frontend_cursor.bin_shift;
                bin_last = bin_first;
                conjugate_first = live_frontend_cursor.conjugate;
                conjugate_last = conjugate_first;
            } else {
                bin_first = -16;
                bin_last = 16;
            }
        }

        for (conjugate = conjugate_first; conjugate <= conjugate_last; ++conjugate) {
            for (symbol_phase = phase_first; symbol_phase <= phase_last; ++symbol_phase) {
                for (bin_shift = bin_first; bin_shift <= bin_last; ++bin_shift) {
                    double lock_sum = 0.0;
                    uint32_t lock_count = 0;

                    for (uint32_t s = 0; s < scan_symbols; ++s) {
                        size_t base = (size_t)start + (size_t)s * symbol_len + gi_len;
                        double phase_intercept = 0.0;
                        double phase_slope = 0.0;
                        double pilot_lock = 0.0;
                        uint32_t pilot_count = 0;

                        if (base + RBDVBT_DVBT_2K_FFT_SIZE >= count) {
                            break;
                        }

                        for (uint32_t k = 0; k < RBDVBT_DVBT_2K_FFT_SIZE; ++k) {
                            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
                            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
                            fft_in[k][0] = v.re;
                            fft_in[k][1] = v.im;
                        }

                        fftwf_execute(plan);
                        fftshift_copy(fft_out, shifted, RBDVBT_DVBT_2K_FFT_SIZE);

                        if (estimate_dvbt2k_pilot_phase(shifted,
                                                        s + (uint32_t)symbol_phase,
                                                        bin_shift,
                                                        conjugate,
                                                        &phase_intercept,
                                                        &phase_slope,
                                                        &pilot_lock,
                                                        &pilot_count) == 0) {
                            lock_sum += pilot_lock;
                            lock_count++;
                        }
                    }

                    if (lock_count > 0) {
                        double lock = lock_sum / (double)lock_count;

                        if (lock > best_alignment_lock) {
                            best_alignment_lock = lock;
                            best_bin_shift = bin_shift;
                            best_conjugate = conjugate;
                            best_symbol_phase = symbol_phase;
                        }
                    }
                }
            }
        }

        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[pilot-scan] best_bin_shift=%d conjugate=%d symbol_phase_mod4=%d pilot_lock=%.5f scan_symbols=%u\n",
                    best_bin_shift,
                    best_conjugate,
                    best_symbol_phase,
                    best_alignment_lock,
                    scan_symbols);
        }
        demod_scan_t1 = monotonic_seconds();
    }

    if (cfg->live_mode && live_ofdm_prefix_count > 0u) {
        while (skip_symbols < total_symbols) {
            size_t base = (size_t)start + (size_t)skip_symbols * symbol_len + gi_len;

            if (base + RBDVBT_DVBT_2K_FFT_SIZE > live_ofdm_prefix_count) {
                break;
            }
            skip_symbols++;
        }
        total_symbols = max_symbols + skip_symbols;
        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[ofdm-live] demod_skip_symbols=%u demod_total_symbols=%u prefix=%zu\n",
                    skip_symbols,
                    total_symbols,
                    live_ofdm_prefix_count);
        }
    }

    if (cfg->constellation_out != NULL) {
        out = fopen(cfg->constellation_out, "w");
        if (out == NULL) {
            fprintf(stderr, "failed to open constellation output: %s\n", cfg->constellation_out);
            goto done;
        }
        fprintf(out,
                "i,q,symbol,logical_carrier,fft_bin,role,phase_intercept,phase_slope,pilot_count,pilot_lock,normalizer\n");
    }

    if (cfg->constellation_svg != NULL) {
        uint64_t estimated = (uint64_t)max_symbols * RBDVBT_DVBT_2K_DATA_CELLS;
        svg_stride = estimated / 25000u;
        if (svg_stride == 0) {
            svg_stride = 1;
        }
        svg = fopen(cfg->constellation_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open constellation SVG: %s\n", cfg->constellation_svg);
            goto done;
        }
        write_svg_header(svg);
    }

    if (cfg->equalized_out != NULL) {
        eq_out = fopen(cfg->equalized_out, "w");
        if (eq_out == NULL) {
            fprintf(stderr, "failed to open equalized output: %s\n", cfg->equalized_out);
            goto done;
        }
        fprintf(eq_out,
                "i,q,symbol,logical_carrier,fft_bin,role,phase_intercept,phase_slope,pilot_count,pilot_lock,normalizer\n");
    }

    if (cfg->equalized_svg != NULL) {
        eq_svg = fopen(cfg->equalized_svg, "w");
        if (eq_svg == NULL) {
            fprintf(stderr, "failed to open equalized SVG: %s\n", cfg->equalized_svg);
            goto done;
        }
        write_svg_header(eq_svg);
    }

    if (cfg->demap_out != NULL) {
        demap_out = fopen(cfg->demap_out, "w");
        if (demap_out == NULL) {
            fprintf(stderr, "failed to open demap output: %s\n", cfg->demap_out);
            goto done;
        }
        fprintf(demap_out,
                "symbol,dibit_index,dibit,bit0_lsb,bit1_msb,raw_qpsk_symbol,deinterleaved_qpsk_symbol\n");
    }

    if (cfg->viterbi_out != NULL || cfg->ts_out != NULL) {
        viterbi_symbol_cell_cap = (size_t)max_symbols * RBDVBT_DVBT_2K_DATA_CELLS;
        viterbi_symbols = malloc(viterbi_symbol_cell_cap * sizeof(*viterbi_symbols));
        viterbi_model_symbols = malloc((size_t)max_symbols * sizeof(*viterbi_model_symbols));
        if (viterbi_symbols == NULL || viterbi_model_symbols == NULL) {
            fprintf(stderr, "failed to allocate Viterbi symbol buffers\n");
            goto done;
        }
    }

    for (uint32_t s = 0; s < total_symbols; ++s) {
        size_t base = (size_t)start + (size_t)s * symbol_len + gi_len;
        uint32_t model_symbol = s + (uint32_t)best_symbol_phase;
        uint8_t rx_sio[RBDVBT_DVBT_2K_DATA_CELLS];
        uint8_t rx_sii[RBDVBT_DVBT_2K_DATA_CELLS];
        complexf_t rx_sio_soft[RBDVBT_DVBT_2K_DATA_CELLS];
        complexf_t rx_sii_soft[RBDVBT_DVBT_2K_DATA_CELLS];
        double phase_intercept = 0.0;
        double phase_slope = 0.0;
        double pilot_lock = 0.0;
        double power_sum = 0.0;
        double normalizer = 1.0;
        uint32_t pilot_count = 0;
        uint32_t data_count = 0;

        memset(rx_sio, 0, sizeof(rx_sio));
        memset(rx_sii, 0, sizeof(rx_sii));
        memset(rx_sio_soft, 0, sizeof(rx_sio_soft));
        memset(rx_sii_soft, 0, sizeof(rx_sii_soft));

        if (base + RBDVBT_DVBT_2K_FFT_SIZE >= count) {
            break;
        }

        for (uint32_t k = 0; k < RBDVBT_DVBT_2K_FFT_SIZE; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);
        fftshift_copy(fft_out, shifted, RBDVBT_DVBT_2K_FFT_SIZE);

        if (estimate_dvbt2k_pilot_phase(shifted,
                                        model_symbol,
                                        best_bin_shift,
                                        best_conjugate,
                                        &phase_intercept,
                                        &phase_slope,
                                        &pilot_lock,
                                        &pilot_count) != 0) {
            continue;
        }

        if (s < skip_symbols) {
            continue;
        }

        for (uint32_t di = 0; di < RBDVBT_DVBT_2K_DATA_CELLS; ++di) {
            uint32_t logical_k;
            int bin;
            double x;
            complexf_t z;

            if (rbdvbt_dvbt_2k_data_carrier(model_symbol, di, &logical_k) != 0) {
                continue;
            }
            bin = (int)rbdvbt_dvbt_2k_logical_to_fft_bin(logical_k) + best_bin_shift;
            if (bin < 0 || bin >= (int)RBDVBT_DVBT_2K_FFT_SIZE) {
                continue;
            }
            x = (double)((int)logical_k - 852);
            if (best_conjugate) {
                z = shifted[bin];
                z.im = -z.im;
            } else {
                z = shifted[bin];
            }
            z = c_rotate(z, -(phase_intercept + phase_slope * x));
            power_sum += c_abs2(z);
            data_count++;
        }

        if (data_count > 0 && power_sum > 0.0) {
            normalizer = sqrt(power_sum / (double)data_count);
        }

        for (uint32_t di = 0; di < RBDVBT_DVBT_2K_DATA_CELLS; ++di) {
            uint32_t logical_k;
            int bin;
            double x;
            complexf_t z;

            if (rbdvbt_dvbt_2k_data_carrier(model_symbol, di, &logical_k) != 0) {
                continue;
            }
            bin = (int)rbdvbt_dvbt_2k_logical_to_fft_bin(logical_k) + best_bin_shift;
            if (bin < 0 || bin >= (int)RBDVBT_DVBT_2K_FFT_SIZE) {
                continue;
            }
            x = (double)((int)logical_k - 852);
            if (best_conjugate) {
                z = shifted[bin];
                z.im = -z.im;
            } else {
                z = shifted[bin];
            }
            z = c_rotate(z, -(phase_intercept + phase_slope * x));
            if (normalizer > 0.0) {
                z.re = (float)(z.re / normalizer);
                z.im = (float)(z.im / normalizer);
            }

            if (!isfinite(z.re) || !isfinite(z.im)) {
                continue;
            }

            rx_sio[di] = qpsk_hard_decision(z);
            rx_sio_soft[di] = z;
            {
                float target = 0.70710678f;
                float er = z.re - (z.re >= 0.0f ? target : -target);
                float ei = z.im - (z.im >= 0.0f ? target : -target);

                evm_error_sum += (double)er * er + (double)ei * ei;
                evm_count++;
            }

            if (out != NULL) {
                fprintf(out,
                        "%.7f,%.7f,%u,%u,%u,%s,%.9f,%.12f,%u,%.7f,%.9f\n",
                        z.re,
                        z.im,
                        s,
                        logical_k,
                        (uint32_t)bin,
                        dvbt_role_name(RBDVBT_DVBT_CARRIER_DATA),
                        phase_intercept,
                        phase_slope,
                        pilot_count,
                        pilot_lock,
                        normalizer);
            }
            if (eq_out != NULL) {
                fprintf(eq_out,
                        "%.7f,%.7f,%u,%u,%u,%s,%.9f,%.12f,%u,%.7f,%.9f\n",
                        z.re,
                        z.im,
                        s,
                        logical_k,
                        (uint32_t)bin,
                        dvbt_role_name(RBDVBT_DVBT_CARRIER_DATA),
                        phase_intercept,
                        phase_slope,
                        pilot_count,
                        pilot_lock,
                        normalizer);
            }
            if (svg != NULL && (point_index % svg_stride) == 0) {
                write_svg_point(svg, z, "#48d597", 0.22);
            }
            if (eq_svg != NULL && (point_index % svg_stride) == 0) {
                write_svg_point(eq_svg, z, "#ffcc66", 0.24);
            }
            point_index++;
        }

        if (demap_out != NULL) {
            dvbt2k_symbol_deinterleave_qpsk(model_symbol, rx_sio, rx_sii);
            dvbt2k_symbol_deinterleave_complex(model_symbol, rx_sio_soft, rx_sii_soft);

            for (uint32_t block = 0; block < RBDVBT_DVBT_2K_DATA_CELLS / 126u; ++block) {
                uint8_t dibits[126];
                uint32_t base_index = block * 126u;

                dvbt_qpsk_bit_deinterleave_block(&rx_sii[base_index], dibits);

                for (uint32_t w = 0; w < 126u; ++w) {
                    uint32_t idx = base_index + w;
                    uint8_t dibit = dibits[w] & 0x03u;

                    if (demap_out != NULL) {
                        fprintf(demap_out,
                                "%u,%u,%u,%u,%u,%u,%u\n",
                                s,
                                idx,
                                dibit,
                                dibit & 0x01u,
                                (dibit >> 1) & 0x01u,
                                rx_sio[idx],
                                rx_sii[idx]);
                    }
                    demap_dibits++;
                }
            }
        }

        if (viterbi_symbols != NULL &&
            viterbi_symbol_cell_count + RBDVBT_DVBT_2K_DATA_CELLS <= viterbi_symbol_cell_cap) {
            memcpy(&viterbi_symbols[viterbi_symbol_cell_count],
                   rx_sio_soft,
                   RBDVBT_DVBT_2K_DATA_CELLS * sizeof(*viterbi_symbols));
            viterbi_symbol_cell_count += RBDVBT_DVBT_2K_DATA_CELLS;
            if (viterbi_model_symbols != NULL && viterbi_model_symbol_count < max_symbols) {
                viterbi_model_symbols[viterbi_model_symbol_count++] = model_symbol;
            }
            if (demap_out == NULL) {
                demap_dibits += RBDVBT_DVBT_2K_DATA_CELLS;
            }
        }

        pilot_lock_sum += pilot_lock;
        pilot_lock_count++;
        used_symbols++;
    }

    if (used_symbols == 0) {
        fprintf(stderr, "[dvbt2k] no complete pilot-locked OFDM symbols available\n");
        goto done;
    }

	if (cfg->live_mode) {
	    uint32_t first_used_symbol_phase = (uint32_t)((best_symbol_phase + (int)(skip_symbols & 0x03u)) & 0x03);
        int health_have_continuity = 0;
        uint64_t health_delta_samples = 0u;
        uint64_t health_phase_delta_samples = 0u;

	    first_symbol_sample_abs = live_resampled_chunk_start_abs +
	        (uint64_t)start +
	        (uint64_t)skip_symbols * (uint64_t)symbol_len;
        next_symbol_sample_abs = first_symbol_sample_abs + (uint64_t)used_symbols * (uint64_t)symbol_len;
        if (live_frontend_cursor.valid &&
            live_frontend_cursor.sample_rate_hz == cfg->sample_rate_hz &&
            live_frontend_cursor.symbol_len == symbol_len) {
            uint64_t expected = live_frontend_cursor.next_symbol_sample_abs;
            uint64_t delta = first_symbol_sample_abs > expected ?
                first_symbol_sample_abs - expected :
                expected - first_symbol_sample_abs;
            uint64_t phase_delta = symbol_phase_delta(first_symbol_sample_abs, expected, symbol_len);
            int64_t signed_delta = (int64_t)first_symbol_sample_abs - (int64_t)expected;
            int64_t rounded_delta_symbols = symbol_len != 0u ?
                (signed_delta >= 0 ?
                    (signed_delta + (int64_t)symbol_len / 2) / (int64_t)symbol_len :
                    -((-signed_delta + (int64_t)symbol_len / 2) / (int64_t)symbol_len)) :
	                0;

		    expected_symbol_sample_abs = expected;
            health_have_continuity = 1;
            health_delta_samples = delta;
            health_phase_delta_samples = phase_delta;
		    if (delta <= LIVE_FRONTEND_CONTINUITY_MAX_SAMPLE_ERROR &&
		        phase_delta <= LIVE_FRONTEND_CONTINUITY_MAX_SAMPLE_ERROR &&
		        best_bin_shift == live_frontend_cursor.bin_shift &&
	        best_conjugate == live_frontend_cursor.conjugate &&
	        pilot_lock_count > 0 &&
	        (pilot_lock_sum / (double)pilot_lock_count) >= 0.45) {
	        frontend_continuous = 1;
	        frame_symbol_seq_start = live_frontend_cursor.symbol_seq;
	    }
            if (cfg->live_mode && rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[frontend-cont] mode=%s first_abs=%llu expected_abs=%llu delta=%lld delta_symbols=%lld phase_delta=%llu start=%u skip=%u used=%u seq=%llu cursor_seq=%llu continuous=%d bin=%d/%d conj=%d/%d lock=%.5f\n",
                        live_frontend_sync_mode,
                        (unsigned long long)first_symbol_sample_abs,
                        (unsigned long long)expected,
                        (long long)signed_delta,
                        (long long)rounded_delta_symbols,
                        (unsigned long long)phase_delta,
                        start,
                        skip_symbols,
                        used_symbols,
                        (unsigned long long)frame_symbol_seq_start,
                        (unsigned long long)live_frontend_cursor.symbol_seq,
                        frontend_continuous,
                        best_bin_shift,
                        live_frontend_cursor.bin_shift,
                        best_conjugate,
                        live_frontend_cursor.conjugate,
                        pilot_lock_count > 0 ? pilot_lock_sum / (double)pilot_lock_count : 0.0);
            }
        }
        if (!frontend_continuous) {
            uint32_t processing_symbols;

            frame_symbol_seq_start = live_soft_next_symbol_seq;
            live_viterbi_stream_reset();
            live_viterbi_state.valid = 0;
            processing_symbols = live_decode_invalidate_queued("frontend-discontinuity");
            if (processing_symbols > 0u) {
                live_decode_wait_idle("frontend-discontinuity");
            }
            rbdvbt_outer_reset_live_stream();
        }
        live_symbol_continuity_ok = frontend_continuous;
        live_frontend_cursor.valid = 1;
        live_frontend_cursor.sample_rate_hz = cfg->sample_rate_hz;
        live_frontend_cursor.symbol_len = symbol_len;
        live_frontend_cursor.symbol_seq = frame_symbol_seq_start + used_symbols;
        live_frontend_cursor.next_symbol_sample_abs = next_symbol_sample_abs;
        live_frontend_cursor.last_symbol_sample_abs = first_symbol_sample_abs;
        live_frontend_cursor.cfo_hz = cfo_hz;
        live_frontend_cursor.bin_shift = best_bin_shift;
        live_frontend_cursor.conjugate = best_conjugate;
        live_frontend_cursor.symbol_phase_mod4 =
            (best_symbol_phase + (int)((skip_symbols + used_symbols) & 0x03u)) & 0x03;
        live_frontend_cursor.pilot_lock = pilot_lock_count > 0 ? pilot_lock_sum / (double)pilot_lock_count : 0.0;
	        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
	            fprintf(stderr,
		                    "[frontend-meta] seq=%llu symbols=%u first_abs=%llu next_abs=%llu expected_abs=%llu frontend_continuous=%d symbol_phase=%u local_symbol_phase=%d expected_symbol_phase=%d resamp_range=%llu..%llu\n",
	                    (unsigned long long)frame_symbol_seq_start,
	                    used_symbols,
	                    (unsigned long long)first_symbol_sample_abs,
	                    (unsigned long long)next_symbol_sample_abs,
	                    (unsigned long long)expected_symbol_sample_abs,
	                    frontend_continuous,
	                    first_used_symbol_phase,
	                    best_symbol_phase,
	                    live_frontend_cursor.symbol_phase_mod4 & 0x03,
	                    (unsigned long long)live_resampled_chunk_start_abs,
		                    (unsigned long long)(live_resampled_chunk_start_abs + live_resampled_chunk_count));
	        }
        live_health_note_frontend(live_frontend_cursor.pilot_lock,
                                  evm_count > 0 && evm_error_sum > 0.0 ? 10.0 * log10((double)evm_count / evm_error_sum) : 0.0,
                                  health_have_continuity,
                                  frontend_continuous,
                                  health_delta_samples,
                                  health_phase_delta_samples);
	    }

    if (svg != NULL) {
        fprintf(svg, "</svg>\n");
    }
    if (eq_svg != NULL) {
        fprintf(eq_svg, "</svg>\n");
    }

    if (cfg->viterbi_out != NULL || cfg->ts_out != NULL) {
        rbdvbt_status_context_t status;
        char status_symbol_rate[16];
        double avg_pilot_lock = pilot_lock_count > 0 ? pilot_lock_sum / (double)pilot_lock_count : 0.0;
        double snr_db = evm_count > 0 && evm_error_sum > 0.0 ?
            10.0 * log10((double)evm_count / evm_error_sum) :
            0.0;
        /* Provisional SSI: we do not have tuner RSSI yet, so derive a
         * conservative strength estimate from EVM/SNR instead of saturating
         * good-but-not-huge captures at 100%.
         */
        double ssi_float = (snr_db - 4.0) * (100.0 / 24.0);

        if (ssi_float < 0.0) {
            ssi_float = 0.0;
        }
        if (ssi_float > 100.0) {
            ssi_float = 100.0;
        }

        memset(&status, 0, sizeof(status));
        snprintf(status_symbol_rate, sizeof(status_symbol_rate), "%uks", (unsigned)(cfg->symbol_rate / 1000u));
        status.status_json_path = cfg->status_json;
        status.status_period_packets = cfg->status_period_packets;
        status.symbol_rate = status_symbol_rate;
        status.guard_interval = rbdvbt_guard_interval_name(cfg->guard_interval);
        status.fec = rbdvbt_fec_name(cfg->fec);
        status.modulation = "dvb-t";
        status.fft_mode = "2k";
        status.constellation = "QPSK";
        status.snr_db = snr_db;
        status.pilot_lock = avg_pilot_lock;
        status.cfo_hz = cfo_hz;
        status.bin_shift = best_bin_shift;
        status.symbol_phase = best_symbol_phase;
        status.input_samples = cfg->max_samples;
        status.lock_quality = (uint32_t)(avg_pilot_lock * 100.0 + 0.5);
        if (status.lock_quality > 100u) {
            status.lock_quality = 100u;
        }
        status.ssi = (uint32_t)(ssi_float + 0.5);
        status.wait_video_start = cfg->wait_video_start;
        status.live_mode = cfg->live_mode;
        status.gui_enabled = cfg->gui;

        if (cfg->live_mode) {
            live_status_hold.valid = 1;
            live_status_hold.pilot_lock = avg_pilot_lock;
            live_status_hold.snr_db = snr_db;
            live_status_hold.cfo_hz = cfo_hz;
            live_status_hold.bin_shift = best_bin_shift;
            live_status_hold.symbol_phase = best_symbol_phase;
        }

        if (cfg->gui) {
            gui_constellation_submit(viterbi_symbols,
                                     viterbi_symbol_cell_count,
                                     "QPSK",
                                     snr_db,
                                     avg_pilot_lock,
                                     cfo_hz);
        }

	        if (cfg->live_mode) {
	            if (avg_pilot_lock < LIVE_METADATA_PILOT_LOCK_MIN) {
                    live_health_note_low_pilot();
	                fprintf(stderr,
	                        "[dvbt2k] dropping low-pilot live chunk avg_pilot_lock=%.5f snr=%.2fdB; resetting inner continuity\n",
	                        avg_pilot_lock,
                        snr_db);
                live_symbol_continuity_ok = 0;
                live_soft_dibit_fifo.count = 0u;
                live_soft_dibit_fifo.symbol_count = 0u;
                live_viterbi_stream_reset();
                live_viterbi_state.valid = 0;
                if (live_decode_invalidate_queued("low-pilot") > 0u) {
                    live_decode_wait_idle("low-pilot");
                }
                rbdvbt_outer_reset_live_stream();
                live_frontend_cursor.valid = 0;
                rc = 0;
                goto done;
            }
            if (live_soft_dibit_fifo_process(cfg->fec,
                                             viterbi_symbols,
                                             viterbi_model_symbols,
                                             viterbi_symbol_cell_count,
                                             used_symbols,
                                             frame_symbol_seq_start,
                                             skip_symbols + (uint32_t)best_symbol_phase,
                                             (size_t)used_symbols * RBDVBT_DVBT_2K_DATA_CELLS,
                                             live_symbol_continuity_ok,
                                             cfg->viterbi_out,
                                             cfg->ts_out,
                                             &status) != 0) {
                goto done;
            }
        } else {
            qpsk_dibit_t *viterbi_dibits = NULL;
            size_t viterbi_dibit_count = 0u;

            if (dvbt_qpsk_inner_deinterleave_symbols(viterbi_symbols,
                                                     viterbi_symbol_cell_count,
                                                     skip_symbols + (uint32_t)best_symbol_phase,
                                                     &viterbi_dibits,
                                                     &viterbi_dibit_count) != 0 ||
                write_viterbi_output(cfg->fec,
                                     viterbi_dibits,
                                     viterbi_dibit_count,
                                     frame_symbol_seq_start,
                                     used_symbols,
                                     0,
                                     cfg->viterbi_out,
                                     cfg->ts_out,
                                     &status) != 0) {
                free(viterbi_dibits);
                goto done;
            }
            free(viterbi_dibits);
        }
    }

    fprintf(stderr,
            "[dvbt2k] fft=2048 gi=%u symbol=%u symbols=%u data_carriers=%u demap_dibits=%llu avg_pilot_lock=%.5f snr=%.2fdB cfo=%.2fHz bin_shift=%d conjugate=%d symbol_phase=%d constellation=%s svg=%s demap=%s viterbi=%s ts=%s fec=%s\n",
            gi_len,
            symbol_len,
            used_symbols,
            RBDVBT_DVBT_2K_DATA_CELLS,
            (unsigned long long)demap_dibits,
            pilot_lock_count > 0 ? pilot_lock_sum / (double)pilot_lock_count : 0.0,
            evm_count > 0 && evm_error_sum > 0.0 ? 10.0 * log10((double)evm_count / evm_error_sum) : 0.0,
            cfo_hz,
            best_bin_shift,
            best_conjugate,
            best_symbol_phase,
            cfg->constellation_out != NULL ? cfg->constellation_out : "-",
            cfg->constellation_svg != NULL ? cfg->constellation_svg : "-",
            cfg->demap_out != NULL ? cfg->demap_out : "-",
            cfg->viterbi_out != NULL ? cfg->viterbi_out : "-",
            cfg->ts_out != NULL ? cfg->ts_out : "-",
            rbdvbt_fec_name(cfg->fec));

    if (cfg->live_mode && rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        double demod_t1 = monotonic_seconds();
        fprintf(stderr,
                "[demod-time] pilot_scan=%.3fs symbols_fifo=%.3fs total=%.3fs\n",
                demod_scan_t1 > demod_t0 ? demod_scan_t1 - demod_t0 : 0.0,
                demod_t1 > demod_scan_t1 ? demod_t1 - demod_scan_t1 : 0.0,
                demod_t1 > demod_t0 ? demod_t1 - demod_t0 : 0.0);
    }

    rc = 0;

done:
    if (out != NULL) {
        fclose(out);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    if (eq_out != NULL) {
        fclose(eq_out);
    }
    if (eq_svg != NULL) {
        fclose(eq_svg);
    }
    if (demap_out != NULL) {
        fclose(demap_out);
    }
    free(viterbi_symbols);
    free(viterbi_model_symbols);
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    free(shifted);
    return rc;
}

static int write_constellation(const rbdvbt_config_t *cfg,
                               const complexf_t *samples,
                               size_t count,
                               uint32_t start,
                               uint32_t fft_size,
                               uint32_t gi_len,
                               uint32_t symbol_len,
                               double cfo_hz)
{
    fftwf_complex *fft_in = NULL;
    fftwf_complex *fft_out = NULL;
    fftwf_plan plan = NULL;
    complexf_t *shifted = NULL;
    double *bin_power = NULL;
    complexf_t *bin_qpsk4 = NULL;
    double *bin_qpsk4_power = NULL;
    double *qpsk_score = NULL;
    int *active = NULL;
    int *data_like = NULL;
    FILE *out = NULL;
    FILE *svg = NULL;
    FILE *eq_out = NULL;
    FILE *eq_svg = NULL;
    uint64_t point_index = 0;
    uint64_t eq_point_index = 0;
    uint64_t svg_stride = 1;
    uint64_t eq_svg_stride = 1;
    uint32_t symbols = 0;
    uint32_t max_symbols = cfg->probe_symbols;
    uint32_t s;
    double max_power = 0.0;
    double active_threshold;
    double mean_power = 0.0;
    double active_score_sum = 0.0;
    double score_threshold = 0.0;
    double data_power_sum = 0.0;
    double data_rms = 1.0;
    int active_bins = 0;
    int data_bins = 0;
    int rc = -1;

    fft_in = fftwf_malloc(sizeof(*fft_in) * fft_size);
    fft_out = fftwf_malloc(sizeof(*fft_out) * fft_size);
    shifted = calloc(fft_size, sizeof(*shifted));
    bin_power = calloc(fft_size, sizeof(*bin_power));
    bin_qpsk4 = calloc(fft_size, sizeof(*bin_qpsk4));
    bin_qpsk4_power = calloc(fft_size, sizeof(*bin_qpsk4_power));
    qpsk_score = calloc(fft_size, sizeof(*qpsk_score));
    active = calloc(fft_size, sizeof(*active));
    data_like = calloc(fft_size, sizeof(*data_like));
    if (fft_in == NULL || fft_out == NULL || shifted == NULL || bin_power == NULL ||
        bin_qpsk4 == NULL || bin_qpsk4_power == NULL || qpsk_score == NULL ||
        active == NULL || data_like == NULL) {
        fprintf(stderr, "failed to allocate FFT/probe buffers\n");
        goto done;
    }

    plan = fftwf_plan_dft_1d((int)fft_size, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "failed to create FFTW plan\n");
        goto done;
    }

    for (s = 0; s < max_symbols; ++s) {
        size_t base = (size_t)start + (size_t)s * symbol_len + gi_len;
        uint32_t k;

        if (base + fft_size >= count) {
            break;
        }

        for (k = 0; k < fft_size; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);
        fftshift_copy(fft_out, shifted, fft_size);

        for (k = 0; k < fft_size; ++k) {
            complexf_t z2 = c_mul(shifted[k], shifted[k]);
            complexf_t z4 = c_mul(z2, z2);
            bin_power[k] += c_abs2(shifted[k]);
            bin_qpsk4[k] = c_add(bin_qpsk4[k], z4);
            bin_qpsk4_power[k] += (double)c_abs2(shifted[k]) * (double)c_abs2(shifted[k]);
        }
        symbols++;
    }

    if (symbols == 0) {
        fprintf(stderr, "[probe] no complete OFDM symbols available after sync\n");
        goto done;
    }

    for (s = 0; s < fft_size; ++s) {
        bin_power[s] /= symbols;
        if (bin_qpsk4_power[s] > 0.0) {
            qpsk_score[s] = c_abs(bin_qpsk4[s]) / bin_qpsk4_power[s];
        }
        mean_power += bin_power[s];
        if (bin_power[s] > max_power) {
            max_power = bin_power[s];
        }
    }
    mean_power /= fft_size;

    active_threshold = mean_power * 2.0;
    if (active_threshold > max_power * 0.25) {
        active_threshold = max_power * 0.25;
    }
    for (s = 0; s < fft_size; ++s) {
        if (s != fft_size / 2u && bin_power[s] >= active_threshold) {
            active[s] = 1;
            active_bins++;
        }
    }

    if (active_bins < 8) {
        for (s = 0; s < fft_size; ++s) {
            active[s] = s != fft_size / 2u;
        }
        active_bins = (int)fft_size - 1;
        active_threshold = 0.0;
    }

    for (s = 0; s < fft_size; ++s) {
        if (active[s]) {
            active_score_sum += qpsk_score[s];
        }
    }
    if (active_bins > 0) {
        score_threshold = active_score_sum / (double)active_bins;
    }
    if (score_threshold < 0.02) {
        score_threshold = 0.02;
    }
    for (s = 0; s < fft_size; ++s) {
        if (active[s] && qpsk_score[s] >= score_threshold) {
            data_like[s] = 1;
            data_bins++;
        }
    }
    if (data_bins < 8) {
        for (s = 0; s < fft_size; ++s) {
            data_like[s] = active[s];
        }
        data_bins = active_bins;
        score_threshold = 0.0;
    }

    for (s = 0; s < fft_size; ++s) {
        if (data_like[s]) {
            data_power_sum += bin_power[s];
        }
    }
    if (data_bins > 0 && data_power_sum > 0.0) {
        data_rms = sqrt(data_power_sum / (double)data_bins);
    }

    if (write_spectrum_outputs(cfg, bin_power, active, data_like, fft_size) != 0 ||
        write_carrier_metrics(cfg, bin_power, qpsk_score, active, data_like, fft_size) != 0 ||
        write_mapping_scan(cfg, bin_power, qpsk_score, active, fft_size) != 0) {
        goto done;
    }

    out = fopen(cfg->constellation_out, "w");
    if (out == NULL) {
        fprintf(stderr, "failed to open constellation output: %s\n", cfg->constellation_out);
        goto done;
    }
    fprintf(out, "i,q,symbol,bin\n");

    if (cfg->constellation_svg != NULL) {
        uint64_t estimated_points = (uint64_t)symbols * (uint64_t)active_bins;
        svg_stride = estimated_points / 20000u;
        if (svg_stride == 0) {
            svg_stride = 1;
        }

        svg = fopen(cfg->constellation_svg, "w");
        if (svg == NULL) {
            fprintf(stderr, "failed to open constellation SVG: %s\n", cfg->constellation_svg);
            goto done;
        }

        write_svg_header(svg);
    }

    if (cfg->equalized_out != NULL) {
        eq_out = fopen(cfg->equalized_out, "w");
        if (eq_out == NULL) {
            fprintf(stderr, "failed to open equalized output: %s\n", cfg->equalized_out);
            goto done;
        }
        fprintf(eq_out, "i,q,symbol,bin,qpsk4_score,carrier_rms\n");
    }

    if (cfg->equalized_svg != NULL) {
        uint64_t estimated_points = (uint64_t)symbols * (uint64_t)data_bins;
        eq_svg_stride = estimated_points / 20000u;
        if (eq_svg_stride == 0) {
            eq_svg_stride = 1;
        }

        eq_svg = fopen(cfg->equalized_svg, "w");
        if (eq_svg == NULL) {
            fprintf(stderr, "failed to open equalized SVG: %s\n", cfg->equalized_svg);
            goto done;
        }

        write_svg_header(eq_svg);
    }

    for (s = 0; s < symbols; ++s) {
        size_t base = (size_t)start + (size_t)s * symbol_len + gi_len;
        complexf_t qpsk4 = {0.0f, 0.0f};
        double phase;
        uint32_t k;

        for (k = 0; k < fft_size; ++k) {
            double t = (double)(base + k) / (double)cfg->sample_rate_hz;
            complexf_t v = c_rotate(samples[base + k], -2.0 * M_PI * cfo_hz * t);
            fft_in[k][0] = v.re;
            fft_in[k][1] = v.im;
        }

        fftwf_execute(plan);
        fftshift_copy(fft_out, shifted, fft_size);

        for (k = 0; k < fft_size; ++k) {
            if (active[k]) {
                complexf_t z2 = c_mul(shifted[k], shifted[k]);
                complexf_t z4 = c_mul(z2, z2);
                qpsk4 = c_add(qpsk4, z4);
            }
        }

        phase = -0.25 * atan2(qpsk4.im, qpsk4.re) + (M_PI / 4.0);

        for (k = 0; k < fft_size; ++k) {
            if (active[k]) {
                complexf_t z = c_rotate(shifted[k], phase);
                double mag = sqrt((double)c_abs2(z));

                if (mag > 0.0) {
                    z.re = (float)(z.re / mag);
                    z.im = (float)(z.im / mag);
                    fprintf(out, "%.7f,%.7f,%u,%d\n",
                            z.re,
                            z.im,
                            s,
                            (int)k - (int)(fft_size / 2u));
                    if (svg != NULL && (point_index % svg_stride) == 0) {
                        write_svg_point(svg, z, "#48d597", 0.22);
                    }
                    point_index++;
                }
            }
        }

        for (k = 0; k < fft_size; ++k) {
            if (data_like[k]) {
                double phase_carrier = -0.25 * atan2(bin_qpsk4[k].im, bin_qpsk4[k].re) + (M_PI / 4.0);
                complexf_t z = c_rotate(shifted[k], phase_carrier);
                double carrier_rms = sqrt(bin_power[k]);
                double scale = carrier_rms > 0.0 ? data_rms / carrier_rms : 1.0;

                z.re = (float)(z.re * scale);
                z.im = (float)(z.im * scale);
                z.re = (float)(z.re / data_rms);
                z.im = (float)(z.im / data_rms);

                if (isfinite(z.re) && isfinite(z.im)) {
                    if (eq_out != NULL) {
                        fprintf(eq_out,
                                "%.7f,%.7f,%u,%d,%.7f,%.7f\n",
                                z.re,
                                z.im,
                                s,
                                (int)k - (int)(fft_size / 2u),
                                qpsk_score[k],
                                carrier_rms);
                    }
                    if (eq_svg != NULL && (eq_point_index % eq_svg_stride) == 0) {
                        write_svg_point(eq_svg, z, "#ffcc66", 0.24);
                    }
                    eq_point_index++;
                }
            }
        }
    }

    if (svg != NULL) {
        fprintf(svg, "</svg>\n");
    }
    if (eq_svg != NULL) {
        fprintf(eq_svg, "</svg>\n");
    }

    fprintf(stderr,
            "[probe] fft_size=%u gi_samples=%u symbol_samples=%u symbols=%u active_bins=%d data_bins=%d power_threshold=%.4g qpsk_threshold=%.4f cfo=%.2fHz constellation=%s svg=%s equalized=%s spectrum=%s metrics=%s\n",
            fft_size,
            gi_len,
            symbol_len,
            symbols,
            active_bins,
            data_bins,
            active_threshold,
            score_threshold,
            cfo_hz,
            cfg->constellation_out,
            cfg->constellation_svg != NULL ? cfg->constellation_svg : "-",
            cfg->equalized_out != NULL ? cfg->equalized_out : "-",
            cfg->spectrum_out != NULL ? cfg->spectrum_out : "-",
            cfg->carrier_metrics_out != NULL ? cfg->carrier_metrics_out : "-");

    rc = 0;

done:
    if (out != NULL) {
        fclose(out);
    }
    if (svg != NULL) {
        fclose(svg);
    }
    if (eq_out != NULL) {
        fclose(eq_out);
    }
    if (eq_svg != NULL) {
        fclose(eq_svg);
    }
    if (plan != NULL) {
        fftwf_destroy_plan(plan);
    }
    if (fft_in != NULL) {
        fftwf_free(fft_in);
    }
    if (fft_out != NULL) {
        fftwf_free(fft_out);
    }
    free(shifted);
    free(bin_power);
    free(bin_qpsk4);
    free(bin_qpsk4_power);
    free(qpsk_score);
    free(active);
    free(data_like);
    return rc;
}

int rbdvbt_run_constellation_probe(const rbdvbt_config_t *cfg)
{
    rbdvbt_config_t effective_cfg = *cfg;
    complexf_t *samples = NULL;
    size_t count = 0;
    rbdvbt_iq_stats_t stats;
    uint32_t fft_size = effective_cfg.fft_size;
    uint32_t gi_len = 0;
    uint32_t symbol_len = 0;
    uint32_t sync_symbols;
    uint32_t start;
    int used_live_sync_hint = 0;
    complexf_t corr;
    double score;
    double cfo_hz;
    uint32_t logical_center_bin = 0;
    uint32_t logical_spacing_bins = 0;
    double logical_center_hz = 0.0;
    double logical_spacing_hz = 0.0;
    rbdvbt_status_context_t status;
    char status_symbol_rate[16];
    int rc = -1;
    double t_start = monotonic_seconds();
    double t_read = t_start;
    double t_resample = t_start;
    double t_prep = t_start;
    double t_aux = t_start;
    double t_sync = t_start;
    double t_demod = t_start;

    if (effective_cfg.guard_interval != RBDVBT_GI_AUTO) {
        gi_len = guard_samples(fft_size, effective_cfg.guard_interval);
        symbol_len = fft_size + gi_len;
    }

    if (effective_cfg.guard_interval != RBDVBT_GI_AUTO && (gi_len == 0 || symbol_len <= fft_size)) {
        fprintf(stderr, "invalid guard interval for fft-size %u\n", fft_size);
        return -1;
    }

    if (gui_constellation_start(effective_cfg.gui) != 0) {
        return -1;
    }
    if (gui_fifo_start(effective_cfg.gui) != 0) {
        return -1;
    }

    if (read_probe_samples(&effective_cfg, &samples, &count, &stats) != 0) {
        return -1;
    }
    t_read = monotonic_seconds();
    if (count == 0u) {
        free(samples);
        return RBDVBT_PROBE_EOF;
    }

    init_status_context_from_config(&effective_cfg, &status, status_symbol_rate, sizeof(status_symbol_rate));
    rbdvbt_status_publish_idle(&status, "processing", (uint64_t)count);
    if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
        rbdvbt_iq_stats_print(stderr, &stats);
    }
    remove_dc(samples, count);

    if (effective_cfg.resample_x4) {
        complexf_t *resampled = NULL;
        size_t resampled_count = 0;

        if (effective_cfg.sample_rate_hz > UINT32_MAX / 4u) {
            fprintf(stderr, "[resample] --sample-rate too high for --resample-x4\n");
            goto done;
        }
        if (resample_x4_sinc(samples, count, &resampled, &resampled_count) != 0) {
            goto done;
        }

        free(samples);
        samples = resampled;
        count = resampled_count;
        effective_cfg.sample_rate_hz *= 4u;

        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[resample] x4 sinc input_rate=%u output_rate=%u input_samples=%zu output_samples=%zu\n",
                    cfg->sample_rate_hz,
                    effective_cfg.sample_rate_hz,
                    stats.samples,
                    count);
        }
        init_status_context_from_config(&effective_cfg, &status, status_symbol_rate, sizeof(status_symbol_rate));
        rbdvbt_status_publish_idle(&status, "resample", (uint64_t)stats.samples);
    }

    if (effective_cfg.resample_to_dvbt_rate) {
        complexf_t *resampled = NULL;
        size_t resampled_count = 0;
        double target_rate = ((double)effective_cfg.symbol_rate * 8.0 / 7.0) * (double)effective_cfg.dvbt_ir;
        double ratio = target_rate / (double)effective_cfg.sample_rate_hz;
        uint32_t target_rate_hz;

        if (target_rate <= 0.0 || target_rate > (double)UINT32_MAX) {
            fprintf(stderr, "[resample] invalid DVB-T target sample rate %.3f\n", target_rate);
            goto done;
        }

        target_rate_hz = (uint32_t)lrint(target_rate);
        if (effective_cfg.live_mode) {
            if (resample_sinc_ratio_live(samples,
                                         count,
                                         ratio,
                                         effective_cfg.sample_rate_hz,
                                         target_rate_hz,
                                         &resampled,
                                         &resampled_count) != 0) {
                goto done;
            }
        } else if (resample_sinc_ratio(samples, count, ratio, &resampled, &resampled_count) != 0) {
            goto done;
        }

        free(samples);
        samples = resampled;
        count = resampled_count;
        effective_cfg.sample_rate_hz = target_rate_hz;

        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[resample] dvbt-rate sinc input_rate=%u output_rate=%u ratio=%.9f ir=%u input_samples=%zu output_samples=%zu\n",
                    cfg->sample_rate_hz,
                    effective_cfg.sample_rate_hz,
                    ratio,
                    effective_cfg.dvbt_ir,
                    stats.samples,
                    count);
        }
        init_status_context_from_config(&effective_cfg, &status, status_symbol_rate, sizeof(status_symbol_rate));
        rbdvbt_status_publish_idle(&status, "resample", (uint64_t)stats.samples);
    }
    t_resample = monotonic_seconds();

    if (effective_cfg.guard_interval == RBDVBT_GI_AUTO) {
        effective_cfg.guard_interval = select_guard_interval_auto(&effective_cfg, samples, count, fft_size);
        gi_len = guard_samples(fft_size, effective_cfg.guard_interval);
        symbol_len = fft_size + gi_len;
        init_status_context_from_config(&effective_cfg, &status, status_symbol_rate, sizeof(status_symbol_rate));
        rbdvbt_status_publish_idle(&status, "auto-gi", (uint64_t)stats.samples);
    } else if (gi_len == 0 || symbol_len <= fft_size) {
        gi_len = guard_samples(fft_size, effective_cfg.guard_interval);
        symbol_len = fft_size + gi_len;
    }

    if (gi_len == 0 || symbol_len <= fft_size) {
        fprintf(stderr, "invalid guard interval for fft-size %u\n", fft_size);
        goto done;
    }

    if (live_ofdm_prepend_tail(&effective_cfg, &samples, &count, symbol_len) != 0) {
        goto done;
    }

    if (count < (size_t)symbol_len * 4u) {
        fprintf(stderr, "[probe] not enough samples for OFDM acquisition\n");
        rc = RBDVBT_PROBE_EOF;
        goto done;
    }
    t_prep = monotonic_seconds();

    if (write_highres_spectrum(&effective_cfg,
                               samples,
                               count,
                               &logical_center_bin,
                               &logical_spacing_bins,
                               &logical_center_hz,
                               &logical_spacing_hz) != 0) {
        goto done;
    }

    if (write_acquisition_scan(&effective_cfg, samples, count) != 0) {
        goto done;
    }
    t_aux = monotonic_seconds();

    sync_symbols = effective_cfg.probe_symbols;
    if ((size_t)sync_symbols * symbol_len > count) {
        sync_symbols = (uint32_t)(count / symbol_len);
    }
    if (sync_symbols > 200u) {
        sync_symbols = 200u;
    }

    live_symbol_continuity_ok = 0;

    if (effective_cfg.live_mode &&
        live_frontend_cursor.valid &&
        live_frontend_cursor.sample_rate_hz == effective_cfg.sample_rate_hz &&
        live_frontend_cursor.symbol_len == symbol_len) {
        uint64_t range_start = live_resampled_chunk_start_abs;
        uint64_t range_end = live_resampled_chunk_start_abs + live_resampled_chunk_count;
        uint64_t expected_abs = live_frontend_cursor.next_symbol_sample_abs;
        uint64_t selected_abs = expected_abs;
        uint64_t symbol_need = (uint64_t)gi_len + (uint64_t)fft_size;

        while (selected_abs < range_start) {
            selected_abs += (uint64_t)symbol_len;
        }
        if (live_ofdm_prefix_count >= (size_t)symbol_len &&
            selected_abs >= range_start + (uint64_t)symbol_len) {
            selected_abs -= (uint64_t)symbol_len;
        }

        if (selected_abs >= range_start &&
            selected_abs + symbol_need <= range_end &&
            selected_abs - range_start <= (uint64_t)UINT32_MAX) {
            uint32_t predicted_local = (uint32_t)(selected_abs - range_start);
            int64_t predicted_symbol_delta;
            predicted_symbol_delta = ((int64_t)selected_abs - (int64_t)expected_abs) / (int64_t)symbol_len;
            start = find_symbol_start_near_local_grdvbt_ml(samples,
                                                           count,
                                                           fft_size,
                                                           gi_len,
                                                           symbol_len,
                                                           sync_symbols,
                                                           predicted_local,
                                                           LIVE_GRDVBT_TRACK_RADIUS,
                                                           &score,
                                                           &corr);
            used_live_sync_hint = 1;
            live_symbol_continuity_ok = 1;
            if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
                fprintf(stderr,
                        "[sync] grdvbt-track expected_abs=%llu selected_abs=%llu predicted_local=%u local=%u symbol_delta=%lld radius=%u score=%.4f\n",
                        (unsigned long long)expected_abs,
                        (unsigned long long)selected_abs,
                        predicted_local,
                        start,
                        (long long)predicted_symbol_delta,
                        LIVE_GRDVBT_TRACK_RADIUS,
                        score);
            }
        }
    }

    if (!used_live_sync_hint) {
        start = find_symbol_start_grdvbt_ml(samples,
                                            count,
                                            fft_size,
                                            gi_len,
                                            symbol_len,
                                            sync_symbols,
                                            &score,
                                            &corr);

        if (write_cp_timing(&effective_cfg,
                            samples,
                            count,
                            fft_size,
                            gi_len,
                            symbol_len,
                            sync_symbols,
                            &start,
                            &score,
                            &corr) != 0) {
                goto done;
        }
    }

    cfo_hz = -atan2(corr.im, corr.re) * (double)effective_cfg.sample_rate_hz /
             (2.0 * M_PI * (double)fft_size);

    fprintf(stderr,
            "[sync] start=%u score=%.4f corr_phase=%.4f cfo=%.2fHz sync_symbols=%u mode=%s\n",
            start,
            score,
            atan2(corr.im, corr.re),
            cfo_hz,
            sync_symbols,
            used_live_sync_hint ? "track" : "acquire");
    live_frontend_sync_mode = used_live_sync_hint ? "track" : "acquire";
    t_sync = monotonic_seconds();

    if (effective_cfg.fine_timing_out != NULL || effective_cfg.fine_timing_svg != NULL) {
        uint32_t fine_start = start;
        double fine_metric = 0.0;

        if (write_fine_timing_scan(&effective_cfg,
                                   samples,
                                   count,
                                   start,
                                   fft_size,
                                   gi_len,
                                   symbol_len,
                                   cfo_hz,
                                   logical_center_bin,
                                   logical_spacing_bins,
                                   &fine_start,
                                   &fine_metric) != 0) {
            goto done;
        }
        if (fine_start != start) {
            start = fine_start;
            score = cp_score_at(samples,
                                count,
                                fft_size,
                                gi_len,
                                symbol_len,
                                start,
                                sync_symbols,
                                &corr);
            cfo_hz = -atan2(corr.im, corr.re) * (double)effective_cfg.sample_rate_hz /
                     (2.0 * M_PI * (double)fft_size);
            if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
                fprintf(stderr,
                        "[sync] fine-selected start=%u cp_score=%.4f cfo=%.2fHz fine_metric=%.6f\n",
                        start,
                        score,
                        cfo_hz,
                        fine_metric);
            }
        }
    }

    if (write_window_sweep(&effective_cfg, samples, count, start, fft_size, gi_len, symbol_len, cfo_hz) != 0) {
        goto done;
    }

    if (write_logical_constellation(&effective_cfg,
                                    samples,
                                    count,
                                    start,
                                    fft_size,
                                    gi_len,
                                    symbol_len,
                                    cfo_hz,
                                    logical_center_bin,
                                    logical_spacing_bins,
                                    logical_center_hz,
                                    logical_spacing_hz) != 0) {
        goto done;
    }

    if (fft_size == RBDVBT_DVBT_2K_FFT_SIZE) {
        rbdvbt_status_publish_idle(&status, "demod", (uint64_t)stats.samples);
        rc = write_dvbt2k_qpsk_constellation(&effective_cfg, samples, count, start, gi_len, symbol_len, cfo_hz);
    } else {
        rbdvbt_status_publish_idle(&status, "demod", (uint64_t)stats.samples);
        rc = write_constellation(&effective_cfg, samples, count, start, fft_size, gi_len, symbol_len, cfo_hz);
    }
    t_demod = monotonic_seconds();

    if (effective_cfg.live_mode && rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        fprintf(stderr,
                "[frontend-time] read=%.3fs resample=%.3fs prep=%.3fs aux=%.3fs sync=%.3fs demod_fifo=%.3fs total=%.3fs samples=%zu\n",
                t_read - t_start,
                t_resample - t_read,
                t_prep - t_resample,
                t_aux - t_prep,
                t_sync - t_aux,
                t_demod - t_sync,
                t_demod - t_start,
                count);
    }

    if (effective_cfg.live_mode && rc == 0) {
        live_sync_hint.valid = 1;
        live_sync_hint.sample_rate_hz = effective_cfg.sample_rate_hz;
        live_sync_hint.symbol_rate = effective_cfg.symbol_rate;
        live_sync_hint.guard_interval = effective_cfg.guard_interval;
        live_sync_hint.fft_size = fft_size;
        live_sync_hint.symbol_len = symbol_len;
        live_sync_hint.chunk_output_samples = (uint32_t)(count % UINT32_MAX);
        live_sync_hint.start = start;
        live_sync_hint.cfo_hz = cfo_hz;
        live_sync_hint.cp_score = score;
    } else if (effective_cfg.live_mode && rc != RBDVBT_PROBE_EOF) {
        live_sync_hint.valid = 0;
    }

done:
    free(samples);
    return rc;
}
