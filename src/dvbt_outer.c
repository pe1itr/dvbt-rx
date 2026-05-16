#include "dvbt_outer.h"

#include "config.h"
#include "pipeline.h"
#include "udp_ts_output.h"

#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifdef RBDVBT_HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#define RS_DATA_LEN 188u
#define RS_PARITY_LEN 16u
#define RS_BLOCK_LEN 204u
#define OUTER_I 12u
#define OUTER_M 17u
#define OUTER_TRANSIENT ((OUTER_I - 1u) * OUTER_M * OUTER_I)
#define SCRAMBLE_SEQ_LEN 1503u
#define VIDEO_GATE_CONFIG_PACKETS 16u
#define FIFO3_CAPACITY_PACKETS 64u
#define TS_WRITE_BURST_PACKETS 7u
#define TS_WRITE_BURST_BYTES (TS_WRITE_BURST_PACKETS * RS_DATA_LEN)

#define VIDEO_NAL_IDR 0x01u
#define VIDEO_NAL_SPS 0x02u
#define VIDEO_NAL_PPS 0x04u
#define VIDEO_NAL_VPS 0x08u
#define LIVE_GRDVBT_SOFT_FAIL_LIMIT 4u
#define LIVE_GRDVBT_HARD_FAIL_LIMIT 2u
#define LIVE_GRDVBT_HARD_FAIL_BLOCKS 32u
#define LIVE_GRDVBT_LOW_PILOT_RESET_LIMIT 3u
#define LIVE_GRDVBT_PILOT_LOCK_MIN 0.45
#define STATUS_JSON_RETRY_SECONDS 30

typedef enum {
    RX_LOCK_SEARCH,
    RX_LOCK_LOCKING,
    RX_LOCK_LOCKED,
    RX_LOCK_DEGRADED,
    RX_LOCK_RELOCK
} rx_lock_state_t;

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_tables_ready;
static uint32_t next_status_update_seq;

typedef struct {
    pthread_mutex_t mutex;
    char path[1024];
    int failed;
    time_t next_retry_unix;
    uint32_t suppressed;
} status_json_output_state_t;

static status_json_output_state_t status_json_output = {
    PTHREAD_MUTEX_INITIALIZER,
    {0},
    0,
    0,
    0
};

typedef struct {
    uint8_t *storage;
    uint8_t *rows[OUTER_I];
    uint32_t pos[OUTER_I];
    uint32_t len[OUTER_I];
} outer_deinterleaver_t;

typedef struct {
    uint32_t deint_phase;
    uint32_t rs_phase;
    uint32_t block_phase;
    uint32_t score;
    uint32_t rs_ok;
    uint32_t sync_ok;
    uint32_t blocks;
} outer_candidate_t;

typedef struct {
    uint32_t sync_scan_blocks;
    uint32_t rs_probe_blocks;
    size_t min_pending_bytes;
    uint32_t min_rs_ok;
    uint32_t min_sync_ok;
    uint32_t min_sync_only_ok;
    uint32_t soft_fail_limit;
    uint32_t hard_fail_limit;
    uint32_t low_pilot_reset_limit;
} outer_scan_policy_t;

typedef struct {
    int valid;
    uint32_t deint_phase;
    uint32_t rs_phase;
    uint32_t block_phase;
    uint64_t stream_byte_offset;
    uint64_t consumed_deint_bytes;
    uint32_t lock_jobs;
    uint32_t fail_jobs;
} live_outer_alignment_state_t;

static live_outer_alignment_state_t live_outer_alignment;

typedef struct {
    int valid;
    uint8_t *bytes;
    size_t count;
    size_t cap;
    uint32_t block_phase;
    int derand_locked;
    uint32_t derand_phase;
    uint64_t stream_byte_offset;
    uint32_t lock_jobs;
    uint32_t fail_jobs;
} live_outer_stream_state_t;

static live_outer_stream_state_t live_outer_stream;

typedef struct {
    uint8_t *bytes;
    size_t count;
    size_t cap;
    int synchronized;
    uint32_t phase8;
    uint32_t fail_packets;
    uint64_t stream_byte_offset;
} live_mpeg_sync_state_t;

static live_mpeg_sync_state_t live_mpeg_sync;

typedef struct {
    int valid;
    int deinterleaver_ready;
    outer_deinterleaver_t deinterleaver;
    uint8_t *pending;
    size_t pending_count;
    size_t pending_cap;
    uint32_t branch;
    uint32_t block_phase;
    uint8_t rs_block[RS_BLOCK_LEN];
    uint32_t rs_count;
    uint64_t input_bytes;
    uint64_t deint_bytes;
    uint64_t rs_blocks;
    uint32_t lock_jobs;
    uint32_t fail_jobs;
    uint32_t hard_fail_jobs;
    uint32_t low_pilot_jobs;
    int probation_lock;
} live_grdvbt_outer_state_t;

static live_grdvbt_outer_state_t live_grdvbt_outer;

typedef struct {
    uint32_t packets;
    uint32_t sync_bad;
    uint32_t transport_errors;
    uint32_t pat_packets;
    uint32_t pmt_packets;
    uint32_t sdt_packets;
    uint32_t pmt_pid;
    uint32_t service_id;
    uint32_t program_id;
    char service_name[64];
    char service_provider[64];
    uint32_t pcr_pid;
    uint32_t video_pid;
    uint32_t audio_pid;
    uint8_t video_stream_type;
    uint8_t audio_stream_type;
    uint8_t cc_seen[8192];
    uint8_t cc_value[8192];
    uint32_t cc_errors;
} ts_validator_t;

typedef struct {
    int valid;
    ts_validator_t validator;
    time_t updated_unix;
    uint32_t rs_bad;
    uint32_t rs_ok;
    uint32_t rs_corrected;
    uint32_t rs_corrected_bytes;
    uint32_t rs_uncorrectable;
    uint32_t written_packets;
} live_status_snapshot_t;

typedef struct {
    int valid;
    uint32_t session_id;
    uint32_t restarts;
    time_t started_unix;
    uint64_t packets;
    uint64_t written_packets;
    uint64_t sync_bad;
    uint64_t transport_errors;
    uint64_t cc_errors;
    uint64_t pat_packets;
    uint64_t pmt_packets;
    uint64_t sdt_packets;
    uint64_t rs_bad;
    uint64_t rs_ok;
    uint64_t rs_corrected;
    uint64_t rs_corrected_bytes;
    uint64_t rs_uncorrectable;
    uint32_t last_packets;
    uint32_t last_written_packets;
    uint32_t last_sync_bad;
    uint32_t last_transport_errors;
    uint32_t last_cc_errors;
    uint32_t last_pat_packets;
    uint32_t last_pmt_packets;
    uint32_t last_sdt_packets;
    uint32_t last_rs_bad;
    uint32_t last_rs_ok;
    uint32_t last_rs_corrected;
    uint32_t last_rs_corrected_bytes;
    uint32_t last_rs_uncorrectable;
} live_ts_session_t;

#define STATUS_LIVE_HOLD_SECONDS 30

static live_status_snapshot_t live_status_snapshot;
static live_ts_session_t live_ts_session;

static void live_ts_session_start(time_t now)
{
    uint32_t next_id = live_ts_session.valid ? live_ts_session.session_id + 1u : 1u;
    uint32_t restarts = live_ts_session.valid ? live_ts_session.restarts + 1u : 0u;

    memset(&live_ts_session, 0, sizeof(live_ts_session));
    if (next_id == 0u) {
        next_id = 1u;
    }
    live_ts_session.valid = 1;
    live_ts_session.session_id = next_id;
    live_ts_session.restarts = restarts;
    live_ts_session.started_unix = now;
}

static uint32_t counter_delta_u32(uint32_t current, uint32_t *last)
{
    uint32_t delta;

    if (current < *last) {
        *last = 0u;
    }
    delta = current - *last;
    *last = current;
    return delta;
}

static void live_ts_session_note(const ts_validator_t *v,
                                 uint32_t rs_bad,
                                 uint32_t rs_ok,
                                 uint32_t rs_corrected,
                                 uint32_t rs_corrected_bytes,
                                 uint32_t rs_uncorrectable,
                                 uint32_t written_packets,
                                 time_t now)
{
    if (v == NULL) {
        return;
    }
    if (!live_ts_session.valid) {
        live_ts_session_start(now);
    }

    live_ts_session.packets += counter_delta_u32(v->packets, &live_ts_session.last_packets);
    live_ts_session.written_packets += counter_delta_u32(written_packets, &live_ts_session.last_written_packets);
    live_ts_session.sync_bad += counter_delta_u32(v->sync_bad, &live_ts_session.last_sync_bad);
    live_ts_session.transport_errors += counter_delta_u32(v->transport_errors, &live_ts_session.last_transport_errors);
    live_ts_session.cc_errors += counter_delta_u32(v->cc_errors, &live_ts_session.last_cc_errors);
    live_ts_session.pat_packets += counter_delta_u32(v->pat_packets, &live_ts_session.last_pat_packets);
    live_ts_session.pmt_packets += counter_delta_u32(v->pmt_packets, &live_ts_session.last_pmt_packets);
    live_ts_session.sdt_packets += counter_delta_u32(v->sdt_packets, &live_ts_session.last_sdt_packets);
    live_ts_session.rs_bad += counter_delta_u32(rs_bad, &live_ts_session.last_rs_bad);
    live_ts_session.rs_ok += counter_delta_u32(rs_ok, &live_ts_session.last_rs_ok);
    live_ts_session.rs_corrected += counter_delta_u32(rs_corrected, &live_ts_session.last_rs_corrected);
    live_ts_session.rs_corrected_bytes += counter_delta_u32(rs_corrected_bytes, &live_ts_session.last_rs_corrected_bytes);
    live_ts_session.rs_uncorrectable += counter_delta_u32(rs_uncorrectable, &live_ts_session.last_rs_uncorrectable);
}

static void live_outer_alignment_note_result(uint32_t rs_ok,
                                             uint32_t rs_uncorrectable,
                                             uint32_t written,
                                             size_t deint_count,
                                             uint32_t processed_blocks,
                                             const ts_validator_t *validator)
{
    int clear = 0;

    if (!live_outer_alignment.valid) {
        return;
    }

    if (written == 0u || rs_ok == 0u) {
        clear = 1;
    } else if (rs_uncorrectable >= rs_ok) {
        clear = 1;
    } else if (validator != NULL &&
               validator->packets > 0u &&
               (validator->transport_errors + validator->cc_errors + validator->sync_bad) > validator->packets / 4u) {
        clear = 1;
    }

    if (clear) {
        live_outer_alignment.fail_jobs++;
        if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
            fprintf(stderr,
                    "[outer-state] relock deint_phase=%u rs_phase=%u block_phase=%u fail_jobs=%u rs_ok=%u rs_uncorrectable=%u written=%u\n",
                    live_outer_alignment.deint_phase,
                    live_outer_alignment.rs_phase,
                    live_outer_alignment.block_phase,
                    live_outer_alignment.fail_jobs,
                    rs_ok,
                    rs_uncorrectable,
                    written);
        }
        live_outer_alignment.valid = 0;
    } else {
        uint32_t advance = (uint32_t)(deint_count % RS_BLOCK_LEN);

        live_outer_alignment.fail_jobs = 0u;
        live_outer_alignment.lock_jobs++;
        live_outer_alignment.stream_byte_offset += deint_count;
        live_outer_alignment.consumed_deint_bytes += deint_count;
        live_outer_alignment.rs_phase = (live_outer_alignment.rs_phase + RS_BLOCK_LEN - advance) % RS_BLOCK_LEN;
        live_outer_alignment.block_phase = (live_outer_alignment.block_phase + processed_blocks) & 7u;
        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[outer-state] advanced stream_byte_offset=%llu consumed_deint_bytes=%llu next_rs_phase=%u next_block_phase=%u processed_blocks=%u deint_count=%zu\n",
                    (unsigned long long)live_outer_alignment.stream_byte_offset,
                    (unsigned long long)live_outer_alignment.consumed_deint_bytes,
                    live_outer_alignment.rs_phase,
                    live_outer_alignment.block_phase,
                    processed_blocks,
                    deint_count);
        }
    }
}

typedef struct {
    FILE *file;
    int is_udp;
    rbdvbt_udp_ts_output_t *udp;
    char label[128];
} ts_output_sink_t;

typedef struct {
    int stdout_enabled;
    int waiting_for_video_start;
    uint8_t pat[RS_DATA_LEN];
    uint8_t pmt[RS_DATA_LEN];
    uint8_t video_config[VIDEO_GATE_CONFIG_PACKETS][RS_DATA_LEN];
    uint8_t write_buf[TS_WRITE_BURST_BYTES];
    int have_pat;
    int have_pmt;
    int have_video_vps;
    int have_video_sps;
    int have_video_pps;
    uint32_t video_config_count;
    uint32_t write_buf_packets;
} ts_output_gate_t;

static ts_output_gate_t live_ts_gate;
static int live_ts_gate_ready;
static int live_ts_file_ready;

#ifdef RBDVBT_HAVE_X11
#define GUI_STATUS_X 530u
#define GUI_STATUS_Y 370u
#define GUI_STATUS_W 640u
#define GUI_STATUS_H 270u
#define GUI_STATUS_FEC_MISS_LIMIT 6u
#define GUI_STATUS_TS_MISS_LIMIT 12u

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    int enabled;
    int started;
    int stop;
    int updated;
    char service_name[64];
    char service_provider[64];
    char symbol_rate[24];
    char fec[24];
    char guard[24];
    char fft_mode[16];
    char constellation[16];
    double pilot_lock;
    double snr_db;
    double cfo_hz;
    uint32_t packets;
    uint32_t written_packets;
    uint32_t rs_ok;
    uint32_t rs_uncorrectable;
    uint32_t cc_errors;
    uint32_t transport_errors;
    int lamp_ofdm;
    int lamp_fec;
    int lamp_ts_info;
    uint32_t fec_miss_count;
    uint32_t ts_miss_count;
} gui_status_state_t;

static gui_status_state_t gui_status = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    0,
    0,
    0,
    0,
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    0.0,
    0.0,
    0.0,
    0u,
    0u,
    0u,
    0u,
    0u,
    0u,
    0,
    0,
    0,
    0u,
    0
};

static unsigned long gui_alloc_color(Display *display, int screen, const char *name, unsigned long fallback)
{
    XColor exact;
    XColor color;

    if (XAllocNamedColor(display, DefaultColormap(display, screen), name, &color, &exact)) {
        return color.pixel;
    }
    return fallback;
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

static void gui_draw_lamp(Display *display,
                          Drawable drawable,
                          GC gc,
                          unsigned long on_color,
                          unsigned long off_color,
                          int on,
                          int x,
                          int y,
                          const char *label)
{
    XSetForeground(display, gc, on ? on_color : off_color);
    XFillArc(display, drawable, gc, x, y - 11, 18, 18, 0, 360 * 64);
    XSetForeground(display, gc, off_color);
    XDrawArc(display, drawable, gc, x, y - 11, 18, 18, 0, 360 * 64);
    XDrawString(display, drawable, gc, x + 26, y + 3, label, (int)strlen(label));
}

static void *gui_status_thread_main(void *arg)
{
    Display *display;
    int screen;
    Window window;
    GC gc;
    Pixmap pixmap;
    unsigned int width = GUI_STATUS_W;
    unsigned int height = GUI_STATUS_H;
    unsigned long black;
    unsigned long white;
    unsigned long green;
    unsigned long red;
    unsigned long yellow;
    unsigned long gray;
    Atom wm_delete;

    (void)arg;

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "[gui] unable to open X11 display; status GUI disabled\n");
        pthread_mutex_lock(&gui_status.mutex);
        gui_status.enabled = 0;
        gui_status.started = 0;
        pthread_mutex_unlock(&gui_status.mutex);
        return NULL;
    }

    screen = DefaultScreen(display);
    black = BlackPixel(display, screen);
    white = WhitePixel(display, screen);
    green = gui_alloc_color(display, screen, "lime green", white);
    red = gui_alloc_color(display, screen, "red3", white);
    yellow = gui_alloc_color(display, screen, "gold", white);
    gray = gui_alloc_color(display, screen, "gray55", white);

    window = XCreateSimpleWindow(display,
                                 RootWindow(display, screen),
                                 GUI_STATUS_X,
                                 GUI_STATUS_Y,
                                 width,
                                 height,
                                 1,
                                 white,
                                 black);
    gui_set_window_geometry(display, window, GUI_STATUS_X, GUI_STATUS_Y, width, height);
    XStoreName(display, window, "rbdvbt_rx service status");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask);
    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    XMapWindow(display, window);
    XMoveWindow(display, window, GUI_STATUS_X, GUI_STATUS_Y);
    gc = XCreateGC(display, window, 0, NULL);
    pixmap = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen));

    while (1) {
        struct timespec deadline;
        char service_name[64];
        char service_provider[64];
        char symbol_rate[24];
        char fec[24];
        char guard[24];
        char fft_mode[16];
        char constellation[16];
        double pilot_lock;
        double snr_db;
        double cfo_hz;
        uint32_t packets;
        uint32_t written_packets;
        uint32_t rs_ok;
        uint32_t rs_uncorrectable;
        uint32_t cc_errors;
        uint32_t transport_errors;
        int lamp_ofdm;
        int lamp_fec;
        int lamp_ts_info;

        {
            struct timeval now;

            gettimeofday(&now, NULL);
            deadline.tv_sec = now.tv_sec;
            deadline.tv_nsec = (long)now.tv_usec * 1000L + 200000000L;
        }
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&gui_status.mutex);
        while (!gui_status.stop && !gui_status.updated) {
            if (pthread_cond_timedwait(&gui_status.cond,
                                       &gui_status.mutex,
                                       &deadline) != 0) {
                break;
            }
        }
        if (gui_status.stop) {
            pthread_mutex_unlock(&gui_status.mutex);
            break;
        }
        snprintf(service_name, sizeof(service_name), "%s", gui_status.service_name);
        snprintf(service_provider, sizeof(service_provider), "%s", gui_status.service_provider);
        snprintf(symbol_rate, sizeof(symbol_rate), "%s", gui_status.symbol_rate);
        snprintf(fec, sizeof(fec), "%s", gui_status.fec);
        snprintf(guard, sizeof(guard), "%s", gui_status.guard);
        snprintf(fft_mode, sizeof(fft_mode), "%s", gui_status.fft_mode);
        snprintf(constellation, sizeof(constellation), "%s", gui_status.constellation);
        pilot_lock = gui_status.pilot_lock;
        snr_db = gui_status.snr_db;
        cfo_hz = gui_status.cfo_hz;
        packets = gui_status.packets;
        written_packets = gui_status.written_packets;
        rs_ok = gui_status.rs_ok;
        rs_uncorrectable = gui_status.rs_uncorrectable;
        cc_errors = gui_status.cc_errors;
        transport_errors = gui_status.transport_errors;
        lamp_ofdm = gui_status.lamp_ofdm;
        lamp_fec = gui_status.lamp_fec;
        lamp_ts_info = gui_status.lamp_ts_info;
        gui_status.updated = 0;
        pthread_mutex_unlock(&gui_status.mutex);

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);
            if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == wm_delete) {
                pthread_mutex_lock(&gui_status.mutex);
                gui_status.enabled = 0;
                gui_status.stop = 1;
                pthread_mutex_unlock(&gui_status.mutex);
                break;
            }
            if (event.type == ConfigureNotify) {
                XConfigureEvent *cfg = &event.xconfigure;

                if (cfg->width > 120 && cfg->height > 120 &&
                    ((unsigned int)cfg->width != width || (unsigned int)cfg->height != height)) {
                    width = (unsigned int)cfg->width;
                    height = (unsigned int)cfg->height;
                    XFreePixmap(display, pixmap);
                    pixmap = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen));
                }
            }
        }
        if (gui_status.stop) {
            break;
        }

        XSetForeground(display, gc, black);
        XFillRectangle(display, pixmap, gc, 0, 0, width, height);

        XSetForeground(display, gc, yellow);
        XDrawString(display, pixmap, gc, 12, 22, "Service status", 14);

        gui_draw_lamp(display, pixmap, gc, green, red, lamp_ofdm, 18, 52, "OFDM lock");
        gui_draw_lamp(display, pixmap, gc, green, red, lamp_fec, 168, 52, "FEC lock");
        gui_draw_lamp(display, pixmap, gc, green, red, lamp_ts_info, 318, 52, "TS info");

        {
            char line[160];

            XSetForeground(display, gc, white);
            snprintf(line, sizeof(line), "Service : %s", service_name[0] != '\0' ? service_name : "-");
            XDrawString(display, pixmap, gc, 12, 88, line, (int)strlen(line));
            snprintf(line, sizeof(line), "Provider: %s", service_provider[0] != '\0' ? service_provider : "-");
            XDrawString(display, pixmap, gc, 12, 110, line, (int)strlen(line));
            snprintf(line,
                     sizeof(line),
                     "Settings: SR=%s  FEC=%s  GI=%s  FFT=%s  %s",
                     symbol_rate[0] != '\0' ? symbol_rate : "-",
                     fec[0] != '\0' ? fec : "-",
                     guard[0] != '\0' ? guard : "-",
                     fft_mode[0] != '\0' ? fft_mode : "-",
                     constellation[0] != '\0' ? constellation : "-");
            XDrawString(display, pixmap, gc, 12, 140, line, (int)strlen(line));
            snprintf(line,
                     sizeof(line),
                     "Signal: pilot=%.3f  SNR=%.1fdB  CFO=%.1fHz",
                     pilot_lock,
                     snr_db,
                     cfo_hz);
            XDrawString(display, pixmap, gc, 12, 168, line, (int)strlen(line));
            snprintf(line,
                     sizeof(line),
                     "TS/FEC: packets=%u written=%u rs_ok=%u rs_uncorr=%u cc=%u tei=%u",
                     packets,
                     written_packets,
                     rs_ok,
                     rs_uncorrectable,
                     cc_errors,
                     transport_errors);
            XDrawString(display, pixmap, gc, 12, 196, line, (int)strlen(line));
        }

        XSetForeground(display, gc, gray);
        XDrawString(display, pixmap, gc, 12, 238, "Green means the latest receiver status sees that lock/info.", 58);

        XCopyArea(display, pixmap, window, gc, 0, 0, width, height, 0, 0);
        XFlush(display);
    }

    XFreePixmap(display, pixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    pthread_mutex_lock(&gui_status.mutex);
    gui_status.started = 0;
    gui_status.enabled = 0;
    pthread_mutex_unlock(&gui_status.mutex);
    return NULL;
}

static int gui_status_start(int enabled)
{
    if (!enabled) {
        return 0;
    }

    pthread_mutex_lock(&gui_status.mutex);
    if (gui_status.started || gui_status.enabled) {
        pthread_mutex_unlock(&gui_status.mutex);
        return 0;
    }
    gui_status.updated = 1;
    gui_status.stop = 0;
    gui_status.enabled = 1;
    gui_status.started = 1;
    if (pthread_create(&gui_status.thread, NULL, gui_status_thread_main, NULL) != 0) {
        gui_status.enabled = 0;
        gui_status.started = 0;
        pthread_mutex_unlock(&gui_status.mutex);
        fprintf(stderr, "[gui] failed to start status GUI thread\n");
        return -1;
    }
    pthread_detach(gui_status.thread);
    pthread_mutex_unlock(&gui_status.mutex);
    return 0;
}

static void gui_status_submit(const rbdvbt_status_context_t *status,
                              const ts_validator_t *v,
                              uint32_t rs_ok,
                              uint32_t rs_uncorrectable,
                              uint32_t written_packets,
                              int lamp_ofdm,
                              int lamp_fec,
                              int lamp_ts_info)
{
    if (status == NULL || !status->gui_enabled) {
        return;
    }
    if (gui_status_start(1) != 0) {
        return;
    }

    pthread_mutex_lock(&gui_status.mutex);
    if (!gui_status.enabled) {
        pthread_mutex_unlock(&gui_status.mutex);
        return;
    }
    if (v->service_name[0] != '\0') {
        snprintf(gui_status.service_name, sizeof(gui_status.service_name), "%s", v->service_name);
    }
    if (v->service_provider[0] != '\0') {
        snprintf(gui_status.service_provider, sizeof(gui_status.service_provider), "%s", v->service_provider);
    }
    snprintf(gui_status.symbol_rate,
             sizeof(gui_status.symbol_rate),
             "%s",
             status->symbol_rate != NULL ? status->symbol_rate : "unknown");
    snprintf(gui_status.fec, sizeof(gui_status.fec), "%s", status->fec != NULL ? status->fec : "unknown");
    snprintf(gui_status.guard,
             sizeof(gui_status.guard),
             "%s",
             status->guard_interval != NULL ? status->guard_interval : "unknown");
    snprintf(gui_status.fft_mode,
             sizeof(gui_status.fft_mode),
             "%s",
             status->fft_mode != NULL ? status->fft_mode : "2k");
    snprintf(gui_status.constellation,
             sizeof(gui_status.constellation),
             "%s",
             status->constellation != NULL ? status->constellation : "QPSK");
    gui_status.pilot_lock = status->pilot_lock;
    gui_status.snr_db = status->snr_db;
    gui_status.cfo_hz = status->cfo_hz;
    gui_status.packets = v->packets;
    gui_status.written_packets = written_packets;
    gui_status.rs_ok = rs_ok;
    gui_status.rs_uncorrectable = rs_uncorrectable;
    gui_status.cc_errors = v->cc_errors;
    gui_status.transport_errors = v->transport_errors;
    gui_status.lamp_ofdm = lamp_ofdm;
    if (lamp_fec) {
        gui_status.lamp_fec = 1;
        gui_status.fec_miss_count = 0u;
    } else if (gui_status.fec_miss_count < GUI_STATUS_FEC_MISS_LIMIT) {
        gui_status.fec_miss_count++;
    } else {
        gui_status.lamp_fec = 0;
    }
    if (lamp_ts_info) {
        gui_status.lamp_ts_info = 1;
        gui_status.ts_miss_count = 0u;
    } else if (gui_status.ts_miss_count < GUI_STATUS_TS_MISS_LIMIT) {
        gui_status.ts_miss_count++;
    } else {
        gui_status.lamp_ts_info = 0;
    }
    gui_status.updated = 1;
    pthread_cond_signal(&gui_status.cond);
    pthread_mutex_unlock(&gui_status.mutex);
}
#else
static void gui_status_submit(const rbdvbt_status_context_t *status,
                              const ts_validator_t *v,
                              uint32_t rs_ok,
                              uint32_t rs_uncorrectable,
                              uint32_t written_packets,
                              int lamp_ofdm,
                              int lamp_fec,
                              int lamp_ts_info)
{
    (void)status;
    (void)v;
    (void)rs_ok;
    (void)rs_uncorrectable;
    (void)written_packets;
    (void)lamp_ofdm;
    (void)lamp_fec;
    (void)lamp_ts_info;
}
#endif

static const char *lock_state_name(rx_lock_state_t state)
{
    switch (state) {
    case RX_LOCK_SEARCH:
        return "SEARCH";
    case RX_LOCK_LOCKING:
        return "LOCKING";
    case RX_LOCK_LOCKED:
        return "LOCKED";
    case RX_LOCK_DEGRADED:
        return "DEGRADED";
    case RX_LOCK_RELOCK:
        return "RELOCK";
    }
    return "SEARCH";
}

static rx_lock_state_t status_display_lock_state(rx_lock_state_t state, int held_locked)
{
    if (held_locked &&
        (state == RX_LOCK_SEARCH ||
         state == RX_LOCK_RELOCK ||
         state == RX_LOCK_LOCKING)) {
        return RX_LOCK_LOCKED;
    }
    return state;
}

static void gf_tables_init(void)
{
    uint16_t x = 1;

    if (gf_tables_ready) {
        return;
    }

    for (uint32_t i = 0; i < 255u; ++i) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if ((x & 0x100u) != 0u) {
            x = (uint16_t)((x & 0xffu) ^ 0x1du);
        }
    }
    for (uint32_t i = 255u; i < 512u; ++i) {
        gf_exp[i] = gf_exp[i - 255u];
    }

    gf_tables_ready = 1;
}

static uint8_t gf_mul_fast(uint8_t a, uint8_t b)
{
    if (a == 0u || b == 0u) {
        return 0;
    }
    return gf_exp[(uint32_t)gf_log[a] + gf_log[b]];
}

static uint8_t gf_div(uint8_t a, uint8_t b)
{
    int32_t e;

    if (a == 0u) {
        return 0;
    }
    if (b == 0u) {
        return 0;
    }

    e = (int32_t)gf_log[a] - (int32_t)gf_log[b];
    if (e < 0) {
        e += 255;
    }
    return gf_exp[(uint32_t)e];
}

static uint8_t gf_inv(uint8_t a)
{
    if (a == 0u) {
        return 0;
    }
    return gf_exp[(255u - gf_log[a]) % 255u];
}

static uint8_t gf_pow_alpha(int32_t exponent)
{
    while (exponent < 0) {
        exponent += 255;
    }
    return gf_exp[(uint32_t)exponent % 255u];
}

static uint8_t gf_pow_elem(uint8_t a, uint32_t exponent)
{
    if (exponent == 0u) {
        return 1;
    }
    if (a == 0u) {
        return 0;
    }
    return gf_exp[((uint32_t)gf_log[a] * exponent) % 255u];
}

static uint8_t gf_poly_eval(const uint8_t *poly, uint32_t degree, uint8_t x)
{
    uint8_t y = 0;

    for (int32_t i = (int32_t)degree; i >= 0; --i) {
        y = (uint8_t)(gf_mul_fast(y, x) ^ poly[i]);
    }

    return y;
}

static void rs_syndromes_204(const uint8_t *block, uint8_t *syndrome)
{
    gf_tables_init();

    for (uint32_t r = 0; r < RS_PARITY_LEN; ++r) {
        uint8_t x = gf_pow_alpha((int32_t)r);
        uint8_t y = 0;

        for (uint32_t i = 0; i < RS_BLOCK_LEN; ++i) {
            y = (uint8_t)(gf_mul_fast(y, x) ^ block[i]);
        }
        syndrome[r] = y;
    }
}

static int rs_has_syndrome(const uint8_t *syndrome)
{
    for (uint32_t i = 0; i < RS_PARITY_LEN; ++i) {
        if (syndrome[i] != 0u) {
            return 1;
        }
    }
    return 0;
}

static int rs_block_is_clean_204(const uint8_t *block)
{
    uint8_t syndrome[RS_PARITY_LEN];

    rs_syndromes_204(block, syndrome);
    return !rs_has_syndrome(syndrome);
}

static int rs_berlekamp_massey(const uint8_t *syndrome,
                               uint8_t *locator,
                               uint32_t *out_degree)
{
    uint8_t c[RS_PARITY_LEN + 1u];
    uint8_t b[RS_PARITY_LEN + 1u];
    uint32_t l = 0;
    uint32_t m = 1;
    uint8_t bb = 1;

    memset(c, 0, sizeof(c));
    memset(b, 0, sizeof(b));
    c[0] = 1;
    b[0] = 1;

    for (uint32_t n = 0; n < RS_PARITY_LEN; ++n) {
        uint8_t d = syndrome[n];

        for (uint32_t i = 1; i <= l; ++i) {
            d ^= gf_mul_fast(c[i], syndrome[n - i]);
        }

        if (d == 0u) {
            m++;
            continue;
        }

        {
            uint8_t t[RS_PARITY_LEN + 1u];
            uint8_t coef = gf_div(d, bb);

            memcpy(t, c, sizeof(t));
            for (uint32_t i = 0; i + m <= RS_PARITY_LEN; ++i) {
                if (b[i] != 0u) {
                    c[i + m] ^= gf_mul_fast(coef, b[i]);
                }
            }

            if (2u * l <= n) {
                l = n + 1u - l;
                memcpy(b, t, sizeof(b));
                bb = d;
                m = 1;
            } else {
                m++;
            }
        }
    }

    if (l > RS_PARITY_LEN / 2u) {
        return -1;
    }

    memset(locator, 0, RS_PARITY_LEN + 1u);
    memcpy(locator, c, RS_PARITY_LEN + 1u);
    *out_degree = l;
    return 0;
}

static int rs_correct_204(uint8_t *block)
{
    uint8_t syndrome[RS_PARITY_LEN];
    uint8_t locator[RS_PARITY_LEN + 1u];
    uint8_t omega[RS_PARITY_LEN];
    uint32_t degree = 0;
    uint32_t positions[RS_PARITY_LEN / 2u];
    uint32_t position_count = 0;

    rs_syndromes_204(block, syndrome);
    if (!rs_has_syndrome(syndrome)) {
        return 0;
    }

    if (rs_berlekamp_massey(syndrome, locator, &degree) != 0) {
        return -1;
    }
    if (degree == 0u || degree > RS_PARITY_LEN / 2u) {
        return -1;
    }

    for (uint32_t k = 0; k < RS_BLOCK_LEN; ++k) {
        uint8_t x = gf_pow_alpha(203 - (int32_t)k);
        uint8_t inv_x = gf_inv(x);

        if (gf_poly_eval(locator, degree, inv_x) == 0u) {
            if (position_count >= RS_PARITY_LEN / 2u) {
                return -1;
            }
            positions[position_count++] = k;
        }
    }

    if (position_count != degree) {
        return -1;
    }

    memset(omega, 0, sizeof(omega));
    for (uint32_t i = 0; i < RS_PARITY_LEN; ++i) {
        if (syndrome[i] == 0u) {
            continue;
        }
        for (uint32_t j = 0; j <= degree; ++j) {
            if (i + j < RS_PARITY_LEN && locator[j] != 0u) {
                omega[i + j] ^= gf_mul_fast(syndrome[i], locator[j]);
            }
        }
    }

    for (uint32_t p = 0; p < position_count; ++p) {
        uint32_t k = positions[p];
        uint8_t x = gf_pow_alpha(203 - (int32_t)k);
        uint8_t inv_x = gf_inv(x);
        uint8_t numerator = gf_poly_eval(omega, RS_PARITY_LEN - 1u, inv_x);
        uint8_t denominator = 0;
        uint8_t magnitude;

        for (uint32_t i = 1; i <= degree; i += 2u) {
            denominator ^= gf_mul_fast(locator[i], gf_pow_elem(inv_x, i - 1u));
        }
        if (denominator == 0u) {
            return -1;
        }

        magnitude = gf_mul_fast(x, gf_div(numerator, denominator));
        block[k] ^= magnitude;
    }

    rs_syndromes_204(block, syndrome);
    if (rs_has_syndrome(syndrome)) {
        return -1;
    }

    return (int)position_count;
}

static int outer_deinterleaver_init(outer_deinterleaver_t *d)
{
    uint32_t total = 0;

    memset(d, 0, sizeof(*d));
    for (uint32_t i = 0; i < OUTER_I; ++i) {
        d->len[i] = (OUTER_I - 1u - i) * OUTER_M;
        total += d->len[i] > 0u ? d->len[i] : 1u;
    }

    d->storage = calloc(total, sizeof(*d->storage));
    if (d->storage == NULL) {
        return -1;
    }

    total = 0;
    for (uint32_t i = 0; i < OUTER_I; ++i) {
        uint32_t len = d->len[i] > 0u ? d->len[i] : 1u;

        d->rows[i] = &d->storage[total];
        total += len;
    }

    return 0;
}

static uint8_t outer_deinterleaver_step(outer_deinterleaver_t *d,
                                        uint32_t branch,
                                        uint8_t in)
{
    uint32_t len = d->len[branch];
    uint32_t pos;
    uint8_t out;

    if (len == 0u) {
        return in;
    }

    pos = d->pos[branch];
    out = d->rows[branch][pos];
    d->rows[branch][pos] = in;
    d->pos[branch] = (pos + 1u) % len;
    return out;
}

static void outer_deinterleaver_free(outer_deinterleaver_t *d)
{
    free(d->storage);
    memset(d, 0, sizeof(*d));
}

static uint8_t *outer_deinterleave_phase(const uint8_t *inner,
                                         size_t inner_count,
                                         uint32_t phase,
                                         size_t *out_count)
{
    outer_deinterleaver_t d;
    uint8_t *out;
    size_t n = 0;

    if (phase >= OUTER_I || inner_count <= phase) {
        return NULL;
    }
    if (outer_deinterleaver_init(&d) != 0) {
        return NULL;
    }

    out = malloc(inner_count - phase);
    if (out == NULL) {
        outer_deinterleaver_free(&d);
        return NULL;
    }

    for (size_t i = phase; i < inner_count; ++i) {
        uint32_t branch = (uint32_t)(n % OUTER_I);

        out[n++] = outer_deinterleaver_step(&d, branch, inner[i]);
    }

    outer_deinterleaver_free(&d);
    *out_count = n;
    return out;
}

static void build_scrambler_table(uint8_t *table)
{
    uint32_t reg = 0x4a80u;

    for (uint32_t i = 0; i < SCRAMBLE_SEQ_LEN; ++i) {
        uint8_t b = 0;

        for (uint32_t j = 0; j < 8u; ++j) {
            uint32_t out = reg & 1u;

            reg >>= 1;
            out ^= reg & 1u;
            if (out != 0u) {
                reg |= 0x4000u;
            }
            b = (uint8_t)((b << 1) | (out & 1u));
        }
        table[i] = b;
    }
}

static int sync_matches(uint8_t sync, uint32_t block_phase)
{
    return sync == (block_phase == 0u ? 0xb8u : 0x47u);
}

static outer_scan_policy_t outer_scan_policy_default(void)
{
    outer_scan_policy_t p;

    p.sync_scan_blocks = 256u;
    p.rs_probe_blocks = 16u;
    p.min_pending_bytes = 0u;
    p.min_rs_ok = 1u;
    p.min_sync_ok = 0u;
    p.min_sync_only_ok = 96u;
    p.soft_fail_limit = LIVE_GRDVBT_SOFT_FAIL_LIMIT;
    p.hard_fail_limit = LIVE_GRDVBT_HARD_FAIL_LIMIT;
    p.low_pilot_reset_limit = LIVE_GRDVBT_LOW_PILOT_RESET_LIMIT;
    return p;
}

static uint32_t status_symbol_rate_hz(const rbdvbt_status_context_t *status)
{
    if (status == NULL || status->symbol_rate == NULL) {
        return 0u;
    }
    if (strstr(status->symbol_rate, "150") != NULL) {
        return 150000u;
    }
    if (strstr(status->symbol_rate, "250") != NULL) {
        return 250000u;
    }
    if (strstr(status->symbol_rate, "333") != NULL) {
        return 333000u;
    }
    if (strstr(status->symbol_rate, "500") != NULL) {
        return 500000u;
    }
    return 0u;
}

static outer_scan_policy_t outer_scan_policy_for_status(const rbdvbt_status_context_t *status)
{
    outer_scan_policy_t p = outer_scan_policy_default();
    uint32_t sr_hz = status_symbol_rate_hz(status);
    double observe_s = 0.0;

    if (sr_hz == 0u) {
        return p;
    }

    if (sr_hz <= 150000u) {
        observe_s = 1.5;
        p.rs_probe_blocks = 48u;
        p.sync_scan_blocks = 192u;
        p.min_rs_ok = 2u;
        p.min_sync_ok = 24u;
        p.min_sync_only_ok = 96u;
        p.soft_fail_limit = 6u;
        p.hard_fail_limit = 3u;
        p.low_pilot_reset_limit = 4u;
    } else if (sr_hz <= 250000u) {
        observe_s = 1.1;
        p.rs_probe_blocks = 64u;
        p.sync_scan_blocks = 224u;
        p.min_rs_ok = 2u;
        p.min_sync_ok = 32u;
        p.min_sync_only_ok = 96u;
        p.soft_fail_limit = 5u;
        p.hard_fail_limit = 3u;
        p.low_pilot_reset_limit = 3u;
    } else {
        observe_s = 0.60;
        p.rs_probe_blocks = 64u;
        p.sync_scan_blocks = 256u;
        p.min_rs_ok = 2u;
        p.min_sync_ok = 32u;
        p.min_sync_only_ok = 128u;
        p.soft_fail_limit = 4u;
        p.hard_fail_limit = 2u;
        p.low_pilot_reset_limit = 3u;
    }

    /* QPSK 2K has 1512 data cells per OFDM symbol. With the reduced-bandwidth
     * rate convention, inner bytes/s ~= SR * 1512 / 14784.
     */
    p.min_pending_bytes = (size_t)((double)sr_hz * (1512.0 / 14784.0) * observe_s);
    if (p.min_pending_bytes < RS_BLOCK_LEN * 32u) {
        p.min_pending_bytes = RS_BLOCK_LEN * 32u;
    }
    return p;
}

static outer_candidate_t score_candidate_with_policy(const uint8_t *deint,
                                                     size_t deint_count,
                                                     uint32_t deint_phase,
                                                     uint32_t rs_phase,
                                                     uint32_t block_phase,
                                                     const outer_scan_policy_t *policy)
{
    outer_candidate_t c;
    size_t offset = OUTER_TRANSIENT + rs_phase;
    uint32_t max_blocks = 0;
    outer_scan_policy_t default_policy;

    if (policy == NULL) {
        default_policy = outer_scan_policy_default();
        policy = &default_policy;
    }

    memset(&c, 0, sizeof(c));
    c.deint_phase = deint_phase;
    c.rs_phase = rs_phase;
    c.block_phase = block_phase;

    if (offset >= deint_count) {
        return c;
    }

    max_blocks = (uint32_t)((deint_count - offset) / RS_BLOCK_LEN);
    if (max_blocks > policy->sync_scan_blocks) {
        max_blocks = policy->sync_scan_blocks;
    }

    for (uint32_t b = 0; b < max_blocks; ++b) {
        const uint8_t *block = &deint[offset + (size_t)b * RS_BLOCK_LEN];
        uint32_t phase = (block_phase + b) & 7u;

        c.blocks++;
        if (sync_matches(block[0], phase)) {
            c.sync_ok++;
        }
        if (b >= policy->rs_probe_blocks) {
            continue;
        }
        if (sync_matches(block[0], phase) && rs_block_is_clean_204(block)) {
            c.rs_ok++;
        }
    }

    c.score = c.rs_ok * 1000u + c.sync_ok;
    return c;
}

static outer_candidate_t score_candidate(const uint8_t *deint,
                                         size_t deint_count,
                                         uint32_t deint_phase,
                                         uint32_t rs_phase,
                                         uint32_t block_phase)
{
    return score_candidate_with_policy(deint,
                                       deint_count,
                                       deint_phase,
                                       rs_phase,
                                       block_phase,
                                       NULL);
}

static int scan_outer_alignment_with_policy(const uint8_t *inner,
                                            size_t inner_count,
                                            outer_candidate_t *best,
                                            const outer_scan_policy_t *policy)
{
    if (best == NULL) {
        return -1;
    }
    memset(best, 0, sizeof(*best));

    for (uint32_t deint_phase = 0; deint_phase < OUTER_I; ++deint_phase) {
        size_t deint_count = 0;
        uint8_t *deint = outer_deinterleave_phase(inner, inner_count, deint_phase, &deint_count);

        if (deint == NULL) {
            continue;
        }

        for (uint32_t rs_phase = 0; rs_phase < RS_BLOCK_LEN; ++rs_phase) {
            for (uint32_t block_phase = 0; block_phase < 8u; ++block_phase) {
                outer_candidate_t c = score_candidate_with_policy(deint,
                                                                  deint_count,
                                                                  deint_phase,
                                                                  rs_phase,
                                                                  block_phase,
                                                                  policy);

                if (c.score > best->score) {
                    *best = c;
                }
            }
        }

        free(deint);
    }

    return best->blocks != 0u ? 0 : -1;
}

static int scan_outer_alignment(const uint8_t *inner,
                                size_t inner_count,
                                outer_candidate_t *best)
{
    return scan_outer_alignment_with_policy(inner, inner_count, best, NULL);
}

static int outer_candidate_is_acquirable(const outer_candidate_t *best,
                                         const outer_scan_policy_t *policy,
                                         int *probation)
{
    if (probation != NULL) {
        *probation = 0;
    }
    if (best == NULL || policy == NULL) {
        return 0;
    }
    if (best->rs_ok >= policy->min_rs_ok && best->sync_ok >= policy->min_sync_ok) {
        return 1;
    }
    if (best->sync_ok >= policy->min_sync_only_ok) {
        if (probation != NULL) {
            *probation = 1;
        }
        return 1;
    }
    return 0;
}

static outer_candidate_t live_outer_alignment_candidate(const uint8_t *deint,
                                                        size_t deint_count)
{
    if (live_outer_alignment.valid) {
        return score_candidate(deint,
                               deint_count,
                               live_outer_alignment.deint_phase,
                               live_outer_alignment.rs_phase,
                               live_outer_alignment.block_phase);
    }

    {
        outer_candidate_t c;

        memset(&c, 0, sizeof(c));
        return c;
    }
}

static void live_outer_alignment_store(const outer_candidate_t *best)
{
    if (best == NULL || best->rs_ok == 0u) {
        return;
    }

    live_outer_alignment.valid = 1;
    live_outer_alignment.deint_phase = best->deint_phase;
    live_outer_alignment.rs_phase = best->rs_phase;
    live_outer_alignment.block_phase = best->block_phase;
    live_outer_alignment.stream_byte_offset = 0u;
    live_outer_alignment.consumed_deint_bytes = 0u;
    live_outer_alignment.lock_jobs++;
    live_outer_alignment.fail_jobs = 0u;
}

static void live_outer_stream_reset(void)
{
    live_outer_stream.valid = 0;
    live_outer_stream.count = 0;
    live_outer_stream.block_phase = 0;
    live_outer_stream.derand_locked = 0;
    live_outer_stream.derand_phase = 0;
    live_outer_stream.stream_byte_offset = 0u;
}

static void live_grdvbt_outer_reset(void)
{
    uint8_t *pending = live_grdvbt_outer.pending;
    size_t pending_cap = live_grdvbt_outer.pending_cap;

    if (live_grdvbt_outer.deinterleaver_ready) {
        outer_deinterleaver_free(&live_grdvbt_outer.deinterleaver);
    }
    memset(&live_grdvbt_outer, 0, sizeof(live_grdvbt_outer));
    live_grdvbt_outer.pending = pending;
    live_grdvbt_outer.pending_cap = pending_cap;
    live_grdvbt_outer.pending_count = 0u;
}

static void live_mpeg_sync_reset(void)
{
    live_mpeg_sync.count = 0;
    live_mpeg_sync.synchronized = 0;
    live_mpeg_sync.phase8 = 0u;
    live_mpeg_sync.fail_packets = 0u;
    live_mpeg_sync.stream_byte_offset = 0u;
}

void rbdvbt_outer_reset_live_stream(void)
{
    live_mpeg_sync_reset();
    live_outer_stream_reset();
    live_grdvbt_outer_reset();
    live_outer_alignment.valid = 0;
    memset(&live_status_snapshot, 0, sizeof(live_status_snapshot));
    live_ts_session_start(time(NULL));
}

static int live_mpeg_sync_reserve(size_t extra)
{
    size_t need = live_mpeg_sync.count + extra;
    size_t cap = live_mpeg_sync.cap;
    uint8_t *p;

    if (need <= cap) {
        return 0;
    }
    if (cap == 0u) {
        cap = RS_BLOCK_LEN * 64u;
    }
    while (cap < need) {
        if (cap > ((size_t)-1) / 2u) {
            return -1;
        }
        cap *= 2u;
    }
    p = realloc(live_mpeg_sync.bytes, cap);
    if (p == NULL) {
        return -1;
    }
    live_mpeg_sync.bytes = p;
    live_mpeg_sync.cap = cap;
    return 0;
}

static int live_mpeg_sync_append(const uint8_t *bytes, size_t count)
{
    if (count == 0u) {
        return 0;
    }
    if (live_mpeg_sync_reserve(count) != 0) {
        return -1;
    }
    memcpy(live_mpeg_sync.bytes + live_mpeg_sync.count, bytes, count);
    live_mpeg_sync.count += count;
    return 0;
}

static int live_mpeg_sync_search(void)
{
    const uint32_t scan_packets = 8u;
    const uint32_t want_syncs = 6u;
    size_t need = (size_t)RS_BLOCK_LEN * scan_packets;

	while (live_mpeg_sync.count >= need) {
	    for (uint32_t offset = 0; offset < RS_BLOCK_LEN; ++offset) {
	        uint32_t sync47 = 0u;
	        uint32_t syncb8 = 0u;
	        uint32_t phase8 = 0u;
	        int have_phase = 0;

            for (uint32_t j = 0; j < scan_packets; ++j) {
                uint8_t b = live_mpeg_sync.bytes[offset + (size_t)j * RS_BLOCK_LEN];

                if (b == 0x47u) {
                    sync47++;
                } else if (b == 0xb8u) {
                    syncb8++;
                    phase8 = (8u - (j & 7u)) & 7u;
                    have_phase = 1;
                }
            }
            if (have_phase && sync47 >= want_syncs && syncb8 == 1u) {
                if (offset > 0u) {
                    memmove(live_mpeg_sync.bytes,
                            live_mpeg_sync.bytes + offset,
                            live_mpeg_sync.count - offset);
                    live_mpeg_sync.count -= offset;
                    live_mpeg_sync.stream_byte_offset += offset;
                }
                live_mpeg_sync.synchronized = 1;
                live_mpeg_sync.phase8 = phase8;
                live_mpeg_sync.fail_packets = 0u;
                if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                    fprintf(stderr,
                            "[mpeg-sync] locked offset=%u phase8=%u buffered=%zu sync47=%u syncb8=%u\n",
                            offset,
                            live_mpeg_sync.phase8,
                            live_mpeg_sync.count,
                            sync47,
                            syncb8);
                }
                return 1;
            }
        }

        memmove(live_mpeg_sync.bytes,
                live_mpeg_sync.bytes + RS_BLOCK_LEN,
                live_mpeg_sync.count - RS_BLOCK_LEN);
        live_mpeg_sync.count -= RS_BLOCK_LEN;
        live_mpeg_sync.stream_byte_offset += RS_BLOCK_LEN;
    }

    return 0;
}

static int live_mpeg_sync_pop_packet(uint8_t *packet)
{
    uint8_t expected;

    if (!live_mpeg_sync.synchronized) {
        if (!live_mpeg_sync_search()) {
            return 0;
        }
    }
    if (live_mpeg_sync.count < RS_BLOCK_LEN) {
        return 0;
    }

    expected = live_mpeg_sync.phase8 == 0u ? 0xb8u : 0x47u;
    if (live_mpeg_sync.bytes[0] != expected) {
        if (live_mpeg_sync.fail_packets < UINT32_MAX) {
            live_mpeg_sync.fail_packets++;
        }
        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[mpeg-sync] weak expected=0x%02x got=0x%02x buffered=%zu fail_packets=%u; keeping cadence\n",
                    expected,
                    live_mpeg_sync.bytes[0],
                    live_mpeg_sync.count,
                    live_mpeg_sync.fail_packets);
        }
    } else {
        live_mpeg_sync.fail_packets = 0u;
    }

    memcpy(packet, live_mpeg_sync.bytes, RS_BLOCK_LEN);
    memmove(live_mpeg_sync.bytes,
            live_mpeg_sync.bytes + RS_BLOCK_LEN,
            live_mpeg_sync.count - RS_BLOCK_LEN);
    live_mpeg_sync.count -= RS_BLOCK_LEN;
    live_mpeg_sync.stream_byte_offset += RS_BLOCK_LEN;
    live_mpeg_sync.phase8 = (live_mpeg_sync.phase8 + 1u) & 7u;
    return 1;
}

static int live_outer_stream_reserve(size_t extra)
{
    size_t need = live_outer_stream.count + extra;
    size_t cap = live_outer_stream.cap;
    uint8_t *p;

    if (need <= cap) {
        return 0;
    }
    if (cap == 0u) {
        cap = OUTER_TRANSIENT + RS_BLOCK_LEN * 32u;
    }
    while (cap < need) {
        if (cap > ((size_t)-1) / 2u) {
            return -1;
        }
        cap *= 2u;
    }
    p = realloc(live_outer_stream.bytes, cap);
    if (p == NULL) {
        return -1;
    }
    live_outer_stream.bytes = p;
    live_outer_stream.cap = cap;
    return 0;
}

static int live_outer_stream_append(const uint8_t *bytes, size_t count)
{
    if (count == 0u) {
        return 0;
    }
    if (live_outer_stream_reserve(count) != 0) {
        return -1;
    }
    memcpy(live_outer_stream.bytes + live_outer_stream.count, bytes, count);
    live_outer_stream.count += count;
    return 0;
}

static int live_outer_stream_append_rs_packet(const uint8_t *packet, uint32_t phase8)
{
    if (!live_outer_stream.valid) {
        live_outer_stream.block_phase = phase8 & 7u;
    }
    live_outer_stream.valid = 1;
    return live_outer_stream_append(packet, RS_BLOCK_LEN);
}

static int live_outer_stream_pop_rs(uint8_t *block)
{
    if (!live_outer_stream.valid ||
        live_outer_stream.count < OUTER_TRANSIENT + RS_BLOCK_LEN) {
        return 0;
    }

    for (uint32_t i = 0; i < RS_BLOCK_LEN; ++i) {
        uint32_t delay = (OUTER_M * (OUTER_I - 1u) +
                          (OUTER_M * OUTER_I) -
                          (OUTER_M * i) % (OUTER_M * OUTER_I)) %
                         (OUTER_M * OUTER_I);
        size_t src = OUTER_TRANSIENT + i - (size_t)delay * OUTER_I;

        block[i] = live_outer_stream.bytes[src];
    }

    memmove(live_outer_stream.bytes,
            live_outer_stream.bytes + RS_BLOCK_LEN,
            live_outer_stream.count - RS_BLOCK_LEN);
    live_outer_stream.count -= RS_BLOCK_LEN;
    live_outer_stream.stream_byte_offset += RS_BLOCK_LEN;
    return 1;
}

__attribute__((unused)) static int live_outer_stream_acquire(const uint8_t *inner,
                                                             size_t inner_count,
                                                             outer_candidate_t *best)
{
    size_t skip;

    if (scan_outer_alignment(inner, inner_count, best) != 0 || best->rs_ok == 0u) {
        return -1;
    }

    skip = (size_t)best->deint_phase + best->rs_phase;
    if (skip >= inner_count) {
        return -1;
    }

    live_outer_stream_reset();
    live_outer_stream.valid = 1;
    live_outer_stream.block_phase = best->block_phase;
    live_outer_stream.lock_jobs++;
    live_outer_stream.fail_jobs = 0u;
    live_outer_alignment_store(best);

    if (live_outer_stream_append(inner + skip, inner_count - skip) != 0) {
        live_outer_stream_reset();
        return -1;
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        fprintf(stderr,
                "[outer-state] acquired leandvb_stream deint_phase=%u rs_phase=%u block_phase=%u skip=%zu buffered=%zu rs_probe_ok=%u sync_ok=%u\n",
                best->deint_phase,
                best->rs_phase,
                best->block_phase,
                skip,
                live_outer_stream.count,
                best->rs_ok,
                best->sync_ok);
    }

    return 0;
}

static void descramble_packet(const uint8_t *rs_data,
                              uint32_t block_phase,
                              const uint8_t *scramble,
                              uint8_t *ts)
{
    ts[0] = 0x47u;
    for (uint32_t i = 1; i < RS_DATA_LEN; ++i) {
        uint32_t sc = block_phase * RS_DATA_LEN + i - 1u;

        ts[i] = (uint8_t)(rs_data[i] ^ scramble[sc]);
    }
}

static uint16_t ts_read16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void ts_validator_init(ts_validator_t *v)
{
    memset(v, 0, sizeof(*v));
    v->pmt_pid = 0x1fffu;
    v->service_id = 0;
    v->program_id = 0;
    v->service_name[0] = '\0';
    v->service_provider[0] = '\0';
    v->pcr_pid = 0x1fffu;
    v->video_pid = 0x1fffu;
    v->audio_pid = 0x1fffu;
}

static void ts_validator_parse_pat(ts_validator_t *v, const uint8_t *section, size_t len)
{
    uint16_t section_length;
    size_t end;

    if (len < 8u || section[0] != 0x00u) {
        return;
    }

    section_length = (uint16_t)(ts_read16(&section[1]) & 0x0fffu);
    if ((size_t)section_length + 3u > len || section_length < 9u) {
        return;
    }

    end = 3u + section_length - 4u;
    for (size_t pos = 8u; pos + 4u <= end; pos += 4u) {
        uint16_t program_number = ts_read16(&section[pos]);
        uint16_t pid = (uint16_t)(ts_read16(&section[pos + 2u]) & 0x1fffu);

        if (program_number != 0u) {
            v->service_id = program_number;
            v->program_id = program_number;
            v->pmt_pid = pid;
            return;
        }
    }
}

static void copy_dvb_text(char *out, size_t out_len, const uint8_t *src, size_t src_len)
{
    size_t n;

    if (out_len == 0u) {
        return;
    }
    n = src_len < out_len - 1u ? src_len : out_len - 1u;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = src[i];

        out[i] = (c >= 32u && c <= 126u) ? (char)c : '?';
    }
    out[n] = '\0';
}

static void ts_validator_parse_pmt(ts_validator_t *v, const uint8_t *section, size_t len)
{
    uint16_t section_length;
    uint16_t program_info_length;
    size_t end;
    size_t pos;

    if (len < 12u || section[0] != 0x02u) {
        return;
    }

    section_length = (uint16_t)(ts_read16(&section[1]) & 0x0fffu);
    if ((size_t)section_length + 3u > len || section_length < 13u) {
        return;
    }

    v->pcr_pid = (uint16_t)(ts_read16(&section[8]) & 0x1fffu);
    program_info_length = (uint16_t)(ts_read16(&section[10]) & 0x0fffu);
    pos = 12u + program_info_length;
    end = 3u + section_length - 4u;

    while (pos + 5u <= end) {
        uint8_t stream_type = section[pos];
        uint16_t elementary_pid = (uint16_t)(ts_read16(&section[pos + 1u]) & 0x1fffu);
        uint16_t es_info_length = (uint16_t)(ts_read16(&section[pos + 3u]) & 0x0fffu);

        if ((stream_type == 0x1bu || stream_type == 0x24u) && v->video_pid == 0x1fffu) {
            v->video_pid = elementary_pid;
            v->video_stream_type = stream_type;
        } else if (stream_type == 0x0fu && v->audio_pid == 0x1fffu) {
            v->audio_pid = elementary_pid;
            v->audio_stream_type = stream_type;
        }

        pos += 5u + es_info_length;
    }
}

static void ts_validator_parse_sdt(ts_validator_t *v, const uint8_t *section, size_t len)
{
    uint16_t section_length;
    size_t end;
    size_t pos = 11u;

    if (len < 14u || section[0] != 0x42u) {
        return;
    }

    section_length = (uint16_t)(ts_read16(&section[1]) & 0x0fffu);
    if ((size_t)section_length + 3u > len || section_length < 12u) {
        return;
    }

    end = 3u + section_length - 4u;
    while (pos + 5u <= end) {
        uint16_t service_id = ts_read16(&section[pos]);
        uint16_t descriptor_loop_length = (uint16_t)(ts_read16(&section[pos + 3u]) & 0x0fffu);
        size_t desc_pos = pos + 5u;
        size_t desc_end = desc_pos + descriptor_loop_length;

        if (desc_end > end) {
            return;
        }

        while (desc_pos + 2u <= desc_end) {
            uint8_t tag = section[desc_pos];
            uint8_t desc_len = section[desc_pos + 1u];
            const uint8_t *desc = &section[desc_pos + 2u];

            if (desc_pos + 2u + desc_len > desc_end) {
                break;
            }
            if (tag == 0x48u && desc_len >= 3u &&
                (v->service_id == 0u || v->service_id == service_id)) {
                uint8_t provider_len = desc[1];
                size_t service_name_len_pos = 2u + provider_len;

                if (service_name_len_pos < desc_len) {
                    uint8_t service_name_len = desc[service_name_len_pos];
                    const uint8_t *provider = &desc[2];
                    const uint8_t *name = &desc[service_name_len_pos + 1u];

                    if (2u + provider_len <= desc_len &&
                        service_name_len_pos + 1u + service_name_len <= desc_len) {
                        copy_dvb_text(v->service_provider, sizeof(v->service_provider), provider, provider_len);
                        copy_dvb_text(v->service_name, sizeof(v->service_name), name, service_name_len);
                        if (v->service_id == 0u) {
                            v->service_id = service_id;
                            v->program_id = service_id;
                        }
                    }
                }
            }

            desc_pos += 2u + desc_len;
        }

        pos = desc_end;
    }
}

static void ts_validator_parse_psi(ts_validator_t *v,
                                   const uint8_t *packet,
                                   uint16_t pid,
                                   size_t payload_start,
                                   int payload_unit_start)
{
    size_t pos = payload_start;
    size_t section_len;

    if (!payload_unit_start || pos >= RS_DATA_LEN) {
        return;
    }

    pos += packet[pos] + 1u;
    if (pos + 3u > RS_DATA_LEN) {
        return;
    }

    section_len = (size_t)(ts_read16(&packet[pos + 1u]) & 0x0fffu) + 3u;
    if (pos + section_len > RS_DATA_LEN) {
        return;
    }

    if (pid == 0x0000u) {
        v->pat_packets++;
        ts_validator_parse_pat(v, &packet[pos], section_len);
    } else if (pid == v->pmt_pid) {
        v->pmt_packets++;
        ts_validator_parse_pmt(v, &packet[pos], section_len);
    } else if (pid == 0x0011u) {
        v->sdt_packets++;
        ts_validator_parse_sdt(v, &packet[pos], section_len);
    }
}

static void ts_validator_observe(ts_validator_t *v, const uint8_t *packet)
{
    uint16_t pid;
    uint8_t afc;
    uint8_t cc;
    int payload_unit_start;
    int has_payload;
    size_t payload_start = 4u;

    v->packets++;
    if (packet[0] != 0x47u) {
        v->sync_bad++;
        return;
    }

    if ((packet[1] & 0x80u) != 0u) {
        v->transport_errors++;
    }

    payload_unit_start = (packet[1] & 0x40u) != 0u;
    pid = (uint16_t)(((uint16_t)(packet[1] & 0x1fu) << 8) | packet[2]);
    afc = (uint8_t)((packet[3] >> 4) & 0x03u);
    cc = (uint8_t)(packet[3] & 0x0fu);
    has_payload = afc == 1u || afc == 3u;

    if (pid != 0x1fffu) {
        if (!v->cc_seen[pid]) {
            v->cc_seen[pid] = 1u;
            v->cc_value[pid] = cc;
        } else if (has_payload) {
            uint8_t expected = (uint8_t)((v->cc_value[pid] + 1u) & 0x0fu);

            if (cc != expected) {
                v->cc_errors++;
            }
            v->cc_value[pid] = cc;
        } else {
            v->cc_value[pid] = cc;
        }
    }

    if (afc == 2u || afc == 3u) {
        uint8_t adaptation_length;

        if (payload_start >= RS_DATA_LEN) {
            return;
        }
        adaptation_length = packet[payload_start++];
        if (payload_start + adaptation_length > RS_DATA_LEN) {
            return;
        }
        payload_start += adaptation_length;
    }

    if (has_payload && payload_start < RS_DATA_LEN) {
        ts_validator_parse_psi(v, packet, pid, payload_start, payload_unit_start);
    }
}

static uint16_t ts_packet_pid(const uint8_t *packet)
{
    return (uint16_t)(((uint16_t)(packet[1] & 0x1fu) << 8) | packet[2]);
}

static int ts_packet_payload_start(const uint8_t *packet, size_t *payload_start)
{
    uint8_t afc = (uint8_t)((packet[3] >> 4) & 0x03u);
    size_t pos = 4u;

    if (afc == 0u || afc == 2u) {
        return 0;
    }
    if (afc == 3u) {
        uint8_t adaptation_length;

        if (pos >= RS_DATA_LEN) {
            return 0;
        }
        adaptation_length = packet[pos++];
        if (pos + adaptation_length > RS_DATA_LEN) {
            return 0;
        }
        pos += adaptation_length;
    }
    if (pos >= RS_DATA_LEN) {
        return 0;
    }
    *payload_start = pos;
    return 1;
}

static uint32_t video_payload_nal_flags(const uint8_t *payload, size_t len, uint8_t stream_type)
{
    size_t i = 0;
    uint32_t flags = 0;

    while (i + 4u < len) {
        size_t start = 0;

        if (payload[i] == 0x00u && payload[i + 1u] == 0x00u && payload[i + 2u] == 0x01u) {
            start = i + 3u;
        } else if (i + 5u < len &&
                   payload[i] == 0x00u &&
                   payload[i + 1u] == 0x00u &&
                   payload[i + 2u] == 0x00u &&
                   payload[i + 3u] == 0x01u) {
            start = i + 4u;
        }

        if (start != 0u && start < len) {
            if (stream_type == 0x24u) {
                if (start + 1u < len) {
                    uint8_t nal_type = (uint8_t)((payload[start] >> 1) & 0x3fu);

                    if (nal_type == 19u || nal_type == 20u || nal_type == 21u) {
                        flags |= VIDEO_NAL_IDR;
                    } else if (nal_type == 32u) {
                        flags |= VIDEO_NAL_VPS;
                    } else if (nal_type == 33u) {
                        flags |= VIDEO_NAL_SPS;
                    } else if (nal_type == 34u) {
                        flags |= VIDEO_NAL_PPS;
                    }
                }
            } else {
                uint8_t nal_type = (uint8_t)(payload[start] & 0x1fu);

                if (nal_type == 5u) {
                    flags |= VIDEO_NAL_IDR;
                } else if (nal_type == 7u) {
                    flags |= VIDEO_NAL_SPS;
                } else if (nal_type == 8u) {
                    flags |= VIDEO_NAL_PPS;
                }
            }
            i = start + 1u;
        } else {
            i++;
        }
    }

    return flags;
}

static uint32_t ts_packet_video_nal_flags(const uint8_t *packet,
                                          uint32_t video_pid,
                                          uint8_t stream_type)
{
    size_t payload_start = 0;
    const uint8_t *payload;
    size_t len;

    if (video_pid == 0x1fffu || ts_packet_pid(packet) != video_pid) {
        return 0;
    }
    if (!ts_packet_payload_start(packet, &payload_start)) {
        return 0;
    }

    payload = &packet[payload_start];
    len = RS_DATA_LEN - payload_start;
    if (len >= 9u && payload[0] == 0x00u && payload[1] == 0x00u && payload[2] == 0x01u) {
        size_t pes_header_len = (size_t)payload[8];

        if (9u + pes_header_len < len) {
            payload += 9u + pes_header_len;
            len -= 9u + pes_header_len;
        }
    }

    return video_payload_nal_flags(payload, len, stream_type);
}

static void ts_output_gate_init(ts_output_gate_t *gate, int wait_video_start)
{
    memset(gate, 0, sizeof(*gate));
    gate->waiting_for_video_start = wait_video_start ? 1 : 0;
}

static void ts_output_gate_relock(ts_output_gate_t *gate, int wait_video_start)
{
    gate->stdout_enabled = 0;
    gate->waiting_for_video_start = wait_video_start ? 1 : 0;
    gate->have_pat = 0;
    gate->have_pmt = 0;
    gate->have_video_vps = 0;
    gate->have_video_sps = 0;
    gate->have_video_pps = 0;
    gate->video_config_count = 0;
}

static int ts_output_is_udp_path(const char *ts_path)
{
    return ts_path != NULL && strncmp(ts_path, "udp://", 6u) == 0;
}

static int ts_output_parse_udp_path(const char *ts_path, char *host, size_t host_len, uint16_t *port)
{
    const char *spec;
    const char *colon;
    char *endp;
    unsigned long parsed_port;
    size_t len;

    if (ts_path == NULL || host == NULL || host_len == 0u || port == NULL ||
        !ts_output_is_udp_path(ts_path)) {
        return -1;
    }

    spec = ts_path + 6u;
    colon = strrchr(spec, ':');
    if (colon == NULL || colon == spec || colon[1] == '\0') {
        return -1;
    }
    len = (size_t)(colon - spec);
    if (len >= host_len) {
        return -1;
    }
    memcpy(host, spec, len);
    host[len] = '\0';

    parsed_port = strtoul(colon + 1u, &endp, 10);
    if (*endp != '\0' || parsed_port == 0ul || parsed_port > 65535ul) {
        return -1;
    }
    *port = (uint16_t)parsed_port;
    return 0;
}

static int ts_output_sink_open_udp(ts_output_sink_t *sink, const char *ts_path)
{
    char host[96];
    uint16_t port;

    if (ts_output_parse_udp_path(ts_path, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "invalid UDP TS output, expected udp://HOST:PORT: %s\n", ts_path);
        return -1;
    }

    if (rbdvbt_udp_ts_output_open(&sink->udp, host, port) != 0) {
        return -1;
    }

    sink->is_udp = 1;
    snprintf(sink->label, sizeof(sink->label), "%s", ts_path);
    return 0;
}

static int ts_output_sink_open(ts_output_sink_t *sink, const char *ts_path, int live_mode)
{
    memset(sink, 0, sizeof(*sink));
    snprintf(sink->label, sizeof(sink->label), "%s", ts_path != NULL ? ts_path : "-");

    if (ts_output_is_udp_path(ts_path)) {
        return ts_output_sink_open_udp(sink, ts_path);
    }
    if (strcmp(ts_path, "-") == 0) {
        sink->file = stdout;
        return 0;
    }

    sink->file = fopen(ts_path, live_mode && live_ts_file_ready ? "ab" : "wb");
    if (sink->file != NULL && live_mode) {
        live_ts_file_ready = 1;
    }
    return sink->file != NULL ? 0 : -1;
}

static int ts_output_sink_write(ts_output_sink_t *sink, const uint8_t *bytes, size_t byte_count)
{
    if (sink->is_udp) {
        return rbdvbt_udp_ts_output_write(sink->udp, bytes, byte_count);
    }
    return fwrite(bytes, 1, byte_count, sink->file) == byte_count ? 0 : -1;
}

static void ts_output_sink_close(ts_output_sink_t *sink)
{
    if (sink == NULL) {
        return;
    }
    if (sink->is_udp && sink->udp != NULL) {
        rbdvbt_udp_ts_output_close(sink->udp);
        sink->udp = NULL;
    } else if (sink->file != NULL && sink->file != stdout) {
        fclose(sink->file);
    }
    sink->file = NULL;
}

static int ts_output_gate_flush(ts_output_gate_t *gate, ts_output_sink_t *sink, const char *ts_path)
{
    size_t bytes;

    if (gate->write_buf_packets == 0u) {
        return 0;
    }

    bytes = (size_t)gate->write_buf_packets * RS_DATA_LEN;
    if (ts_output_sink_write(sink, gate->write_buf, bytes) != 0) {
        fprintf(stderr, "failed to write TS output: %s\n", ts_path);
        return -1;
    }
    gate->write_buf_packets = 0u;
    return 0;
}

static int ts_output_gate_write_packet(ts_output_gate_t *gate,
                                       ts_output_sink_t *sink,
                                       const char *ts_path,
                                       const uint8_t *packet)
{
    memcpy(&gate->write_buf[(size_t)gate->write_buf_packets * RS_DATA_LEN],
           packet,
           RS_DATA_LEN);
    gate->write_buf_packets++;
    if (gate->write_buf_packets == TS_WRITE_BURST_PACKETS) {
        return ts_output_gate_flush(gate, sink, ts_path);
    }
    return 0;
}

static void ts_output_gate_note_video(ts_output_gate_t *gate,
                                      const uint8_t *packet,
                                      uint32_t nal_flags)
{
    if ((nal_flags & VIDEO_NAL_VPS) != 0u) {
        gate->have_video_vps = 1;
    }
    if ((nal_flags & VIDEO_NAL_SPS) != 0u) {
        gate->have_video_sps = 1;
    }
    if ((nal_flags & VIDEO_NAL_PPS) != 0u) {
        gate->have_video_pps = 1;
    }
    if ((nal_flags & VIDEO_NAL_IDR) == 0u &&
        (nal_flags & (VIDEO_NAL_VPS | VIDEO_NAL_SPS | VIDEO_NAL_PPS)) != 0u) {
        uint32_t slot = gate->video_config_count;

        if (slot >= VIDEO_GATE_CONFIG_PACKETS) {
            memmove(&gate->video_config[0],
                    &gate->video_config[1],
                    (VIDEO_GATE_CONFIG_PACKETS - 1u) * RS_DATA_LEN);
            slot = VIDEO_GATE_CONFIG_PACKETS - 1u;
        } else {
            gate->video_config_count++;
        }
        memcpy(gate->video_config[slot], packet, RS_DATA_LEN);
    }
}

static int ts_output_gate_try_write(ts_output_gate_t *gate,
                                    const ts_validator_t *v,
                                    ts_output_sink_t *sink,
                                    const char *ts_path,
                                    const uint8_t *packet,
                                    int pilot_lock_ok,
                                    uint32_t *written_packets)
{
    uint16_t pid = ts_packet_pid(packet);
    uint32_t nal_flags = ts_packet_video_nal_flags(packet, v->video_pid, v->video_stream_type);
    int psi_packet = 0;
    int hevc_video = v->video_stream_type == 0x24u;

    if (pid == 0x0000u) {
        memcpy(gate->pat, packet, RS_DATA_LEN);
        gate->have_pat = 1;
        psi_packet = 1;
    } else if (pid == v->pmt_pid && v->pmt_pid != 0x1fffu) {
        memcpy(gate->pmt, packet, RS_DATA_LEN);
        gate->have_pmt = 1;
        psi_packet = 1;
    }

    if (packet[0] != 0x47u || (packet[1] & 0x80u) != 0u) {
        return 0;
    }
    if (gate->waiting_for_video_start && nal_flags != 0u) {
        ts_output_gate_note_video(gate, packet, nal_flags);
    }

    if (!gate->stdout_enabled) {
        if (!pilot_lock_ok ||
            !gate->have_pat ||
            !gate->have_pmt ||
            v->pat_packets == 0u ||
            v->pmt_packets == 0u ||
            v->video_pid == 0x1fffu) {
            return 0;
        }
        if (gate->waiting_for_video_start) {
            if ((hevc_video && !gate->have_video_vps) ||
                !gate->have_video_sps ||
                !gate->have_video_pps ||
                (nal_flags & VIDEO_NAL_IDR) == 0u) {
                if (v->packets < 32u) {
                    return 0;
                }
            }
            gate->waiting_for_video_start = 0;
        }
        gate->stdout_enabled = 1;
        if (gate->have_pat) {
            if (ts_output_gate_write_packet(gate, sink, ts_path, gate->pat) != 0) {
                return -1;
            }
            (*written_packets)++;
        }
        if (gate->have_pmt) {
            if (ts_output_gate_write_packet(gate, sink, ts_path, gate->pmt) != 0) {
                return -1;
            }
            (*written_packets)++;
        }
        for (uint32_t i = 0; i < gate->video_config_count; ++i) {
            if (ts_output_gate_write_packet(gate, sink, ts_path, gate->video_config[i]) != 0) {
                return -1;
            }
            (*written_packets)++;
        }
    }

    if (psi_packet && gate->stdout_enabled) {
        return 0;
    }
    if (ts_output_gate_write_packet(gate, sink, ts_path, packet) != 0) {
        return -1;
    }
    (*written_packets)++;
    return 0;
}

static int block4_consume_fifo3_packet(const rbdvbt_ts_packet_t *item,
                                       ts_output_gate_t *gate,
                                       ts_validator_t *validator,
                                       ts_output_sink_t *sink,
                                       const char *ts_path,
                                       int live_mode,
                                       int pilot_lock_ok,
                                       rx_lock_state_t *lock_state,
                                       const rbdvbt_status_context_t *status,
                                       uint32_t *written)
{
    uint32_t prev_cc_errors;

    prev_cc_errors = validator->cc_errors;
    ts_validator_observe(validator, item->bytes);

    if (item->bytes[0] != 0x47u || (item->bytes[1] & 0x80u) != 0u) {
        *lock_state = gate->stdout_enabled ? RX_LOCK_DEGRADED : RX_LOCK_LOCKING;
        if (!live_mode || !gate->stdout_enabled) {
            ts_output_gate_relock(gate, status != NULL ? status->wait_video_start : 0);
        }
        return 0;
    }
    if (validator->cc_errors != prev_cc_errors) {
        *lock_state = gate->stdout_enabled ? RX_LOCK_DEGRADED : RX_LOCK_LOCKING;
    }

    if (gate->stdout_enabled) {
        *lock_state = RX_LOCK_LOCKED;
    } else if (validator->pat_packets > 0u &&
               validator->pmt_packets > 0u &&
               validator->video_pid != 0x1fffu) {
        *lock_state = gate->waiting_for_video_start ? RX_LOCK_LOCKING : RX_LOCK_LOCKED;
    } else {
        *lock_state = RX_LOCK_LOCKING;
    }

    if (ts_output_gate_try_write(gate,
                                 validator,
                                 sink,
                                 ts_path,
                                 item->bytes,
                                 pilot_lock_ok,
                                 written) != 0) {
        return -1;
    }
    if (gate->stdout_enabled && *lock_state != RX_LOCK_DEGRADED) {
        *lock_state = RX_LOCK_LOCKED;
    }
    return 0;
}

static void ts_pid_format(uint32_t pid, char *out, size_t out_len)
{
    if (pid == 0x1fffu) {
        snprintf(out, out_len, "-");
    } else {
        snprintf(out, out_len, "0x%04x", pid);
    }
}

static void ts_validator_report(const ts_validator_t *v)
{
    char pmt_pid[16];
    char pcr_pid[16];
    char video_pid[16];
    char audio_pid[16];

    ts_pid_format(v->pmt_pid, pmt_pid, sizeof(pmt_pid));
    ts_pid_format(v->pcr_pid, pcr_pid, sizeof(pcr_pid));
    ts_pid_format(v->video_pid, video_pid, sizeof(video_pid));
    ts_pid_format(v->audio_pid, audio_pid, sizeof(audio_pid));

    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO) ||
        (rbdvbt_log_enabled(RBDVBT_LOG_ERROR) &&
         (v->sync_bad != 0u ||
          v->transport_errors != 0u ||
          v->cc_errors != 0u))) {
        fprintf(stderr,
                "[ts] packets=%u sync_bad=%u transport_errors=%u cc_errors=%u pat_packets=%u pmt_packets=%u sdt_packets=%u pmt_pid=%s pcr_pid=%s video_pid=%s video_type=0x%02x audio_pid=%s audio_type=0x%02x service=\"%s\" provider=\"%s\"\n",
                v->packets,
                v->sync_bad,
                v->transport_errors,
                v->cc_errors,
                v->pat_packets,
                v->pmt_packets,
                v->sdt_packets,
                pmt_pid,
                pcr_pid,
                video_pid,
                v->video_stream_type,
                audio_pid,
                v->audio_stream_type,
                v->service_name,
                v->service_provider);
    }
}

static uint32_t status_sqi(const ts_validator_t *v, uint32_t rs_uncorrectable)
{
    uint32_t penalties = rs_uncorrectable + v->transport_errors + v->cc_errors + v->sync_bad;

    if (v->packets == 0u) {
        return 0;
    }
    if (penalties == 0u) {
        return 100;
    }
    if (penalties >= v->packets) {
        return 0;
    }
    return (uint32_t)(100u - (penalties * 100u) / v->packets);
}

static int status_validator_has_program(const ts_validator_t *v)
{
    return v != NULL &&
        v->packets > 0u &&
        v->pat_packets > 0u &&
        v->pmt_packets > 0u;
}

static int status_validator_has_useful_data(const ts_validator_t *v,
                                            uint32_t rs_ok,
                                            uint32_t written_packets)
{
    return v != NULL &&
        (v->packets > 0u ||
         v->pat_packets > 0u ||
         v->pmt_packets > 0u ||
         v->sdt_packets > 0u ||
         v->service_id != 0u ||
         v->service_name[0] != '\0' ||
         v->video_pid != 0x1fffu ||
         v->audio_pid != 0x1fffu ||
         rs_ok > 0u ||
         written_packets > 0u);
}

static void live_status_snapshot_update(const ts_validator_t *v,
                                        time_t updated_unix,
                                        uint32_t rs_bad,
                                        uint32_t rs_ok,
                                        uint32_t rs_corrected,
                                        uint32_t rs_corrected_bytes,
                                        uint32_t rs_uncorrectable,
                                        uint32_t written_packets)
{
    if (!status_validator_has_useful_data(v, rs_ok, written_packets)) {
        return;
    }
    if (status_validator_has_program(&live_status_snapshot.validator) &&
        !status_validator_has_program(v)) {
        return;
    }
    live_status_snapshot.valid = 1;
    live_status_snapshot.validator = *v;
    live_status_snapshot.updated_unix = updated_unix;
    live_status_snapshot.rs_bad = rs_bad;
    live_status_snapshot.rs_ok = rs_ok;
    live_status_snapshot.rs_corrected = rs_corrected;
    live_status_snapshot.rs_corrected_bytes = rs_corrected_bytes;
    live_status_snapshot.rs_uncorrectable = rs_uncorrectable;
    live_status_snapshot.written_packets = written_packets;
}

static int status_json_write_allowed(const char *path, time_t now)
{
    int allowed = 1;

    if (path == NULL) {
        return 0;
    }

    pthread_mutex_lock(&status_json_output.mutex);
    if (strncmp(status_json_output.path, path, sizeof(status_json_output.path)) != 0) {
        snprintf(status_json_output.path, sizeof(status_json_output.path), "%s", path);
        status_json_output.failed = 0;
        status_json_output.next_retry_unix = 0;
        status_json_output.suppressed = 0;
    }
    if (status_json_output.failed && now < status_json_output.next_retry_unix) {
        status_json_output.suppressed++;
        allowed = 0;
    }
    pthread_mutex_unlock(&status_json_output.mutex);

    return allowed;
}

static void status_json_note_success(const char *path)
{
    uint32_t suppressed = 0;
    int recovered = 0;

    if (path == NULL) {
        return;
    }

    pthread_mutex_lock(&status_json_output.mutex);
    if (strncmp(status_json_output.path, path, sizeof(status_json_output.path)) == 0 &&
        status_json_output.failed) {
        recovered = 1;
        suppressed = status_json_output.suppressed;
        status_json_output.failed = 0;
        status_json_output.next_retry_unix = 0;
        status_json_output.suppressed = 0;
    }
    pthread_mutex_unlock(&status_json_output.mutex);

    if (recovered) {
        fprintf(stderr,
                "status JSON output recovered: %s suppressed_writes=%u\n",
                path,
                suppressed);
    }
}

static void status_json_note_failure(const char *path,
                                     const char *operation,
                                     int errnum,
                                     time_t now)
{
    uint32_t suppressed = 0;

    if (path == NULL) {
        return;
    }

    pthread_mutex_lock(&status_json_output.mutex);
    snprintf(status_json_output.path, sizeof(status_json_output.path), "%s", path);
    suppressed = status_json_output.suppressed;
    status_json_output.failed = 1;
    status_json_output.next_retry_unix = now + STATUS_JSON_RETRY_SECONDS;
    status_json_output.suppressed = 0;
    pthread_mutex_unlock(&status_json_output.mutex);

    if (errnum != 0) {
        fprintf(stderr,
                "failed to %s status JSON output: %s: %s; suppressing status JSON writes for %ds",
                operation,
                path,
                strerror(errnum),
                STATUS_JSON_RETRY_SECONDS);
    } else {
        fprintf(stderr,
                "failed to %s status JSON output: %s; suppressing status JSON writes for %ds",
                operation,
                path,
                STATUS_JSON_RETRY_SECONDS);
    }
    if (suppressed > 0u) {
        fprintf(stderr, " after %u suppressed writes", suppressed);
    }
    fprintf(stderr, "\n");
}

static void status_write_json(const rbdvbt_status_context_t *status,
                              const ts_validator_t *v,
                              uint32_t rs_bad,
                              uint32_t rs_ok,
                              uint32_t rs_corrected,
                              uint32_t rs_corrected_bytes,
                              uint32_t rs_uncorrectable,
                              uint32_t written_packets,
                              rx_lock_state_t lock_state,
                              int stdout_enabled,
                              int waiting_for_video_start,
                              const char *stage,
                              uint64_t input_samples)
{
    FILE *f;
    char tmp_path[1024];
    uint32_t sqi;
    uint32_t pe;
    uint32_t locked;
    uint32_t lock_quality;
    uint32_t ssi;
    const ts_validator_t *display_v;
    uint32_t display_rs_bad;
    uint32_t display_rs_ok;
    uint32_t display_rs_corrected;
    uint32_t display_rs_corrected_bytes;
    uint32_t display_rs_uncorrectable;
    uint32_t display_written_packets;
    int lamp_symbol_rate;
    int lamp_guard;
    int lamp_fec;
    int lamp_ofdm_sync;
    int lamp_inner_fec;
    int lamp_rs_lock;
    int lamp_rs_clean;
    int lamp_ts_sync;
    int lamp_pat;
    int lamp_pmt;
    int lamp_sdt;
    int lamp_service;
    int lamp_av;
    int lamp_output;
    uint32_t update_seq;
    time_t updated_unix;
    int current_locked;
    int held_locked;

    if (status == NULL) {
        return;
    }

    updated_unix = time(NULL);

    if (status->live_mode) {
        live_ts_session_note(v,
                             rs_bad,
                             rs_ok,
                             rs_corrected,
                             rs_corrected_bytes,
                             rs_uncorrectable,
                             written_packets,
                             updated_unix);
        live_status_snapshot_update(v,
                                    updated_unix,
                                    rs_bad,
                                    rs_ok,
                                    rs_corrected,
                                    rs_corrected_bytes,
                                    rs_uncorrectable,
                                    written_packets);
    }

    display_v = v;
    display_rs_bad = rs_bad;
    display_rs_ok = rs_ok;
    display_rs_corrected = rs_corrected;
    display_rs_corrected_bytes = rs_corrected_bytes;
    display_rs_uncorrectable = rs_uncorrectable;
    display_written_packets = written_packets;
    if (status->live_mode &&
        live_status_snapshot.valid &&
        !status_validator_has_useful_data(v, rs_ok, written_packets)) {
        display_v = &live_status_snapshot.validator;
        display_rs_bad = live_status_snapshot.rs_bad;
        display_rs_ok = live_status_snapshot.rs_ok;
        display_rs_corrected = live_status_snapshot.rs_corrected;
        display_rs_corrected_bytes = live_status_snapshot.rs_corrected_bytes;
        display_rs_uncorrectable = live_status_snapshot.rs_uncorrectable;
        display_written_packets = live_status_snapshot.written_packets;
    }

    current_locked = status_validator_has_program(v) &&
        (lock_state == RX_LOCK_LOCKED ||
         (stdout_enabled && lock_state == RX_LOCK_DEGRADED));
    held_locked = status->live_mode &&
        live_status_snapshot.valid &&
        status_validator_has_program(&live_status_snapshot.validator) &&
        updated_unix >= live_status_snapshot.updated_unix &&
        updated_unix - live_status_snapshot.updated_unix <= STATUS_LIVE_HOLD_SECONDS;
    pe = display_rs_uncorrectable + display_v->transport_errors + display_v->cc_errors + display_v->sync_bad;
    locked = current_locked || held_locked;
    sqi = status_sqi(display_v, display_rs_uncorrectable);
    lock_quality = locked ? 100u : status->lock_quality;
    if (lock_quality > 100u) {
        lock_quality = 100u;
    }
    ssi = locked ? status->ssi : 0u;
    lamp_symbol_rate = status->symbol_rate != NULL && strcmp(status->symbol_rate, "unknown") != 0;
    lamp_guard = status->guard_interval != NULL && strcmp(status->guard_interval, "unknown") != 0;
    lamp_fec = status->fec != NULL && strcmp(status->fec, "unknown") != 0 && strcmp(status->fec, "auto") != 0;
    lamp_ofdm_sync = isfinite(status->pilot_lock) && status->pilot_lock >= 0.45;
    lamp_inner_fec = (display_rs_ok + display_rs_bad + display_rs_uncorrectable) > 0u;
    lamp_rs_lock = display_rs_ok > 0u;
    lamp_rs_clean = display_rs_ok > 0u && display_rs_uncorrectable == 0u;
    lamp_ts_sync = display_v->packets > 0u && display_v->sync_bad == 0u;
    lamp_pat = display_v->pat_packets > 0u;
    lamp_pmt = display_v->pmt_packets > 0u;
    lamp_sdt = display_v->sdt_packets > 0u;
    lamp_service = display_v->service_id != 0u || display_v->service_name[0] != '\0';
    lamp_av = display_v->video_pid != 0x1fffu || display_v->audio_pid != 0x1fffu;
    lamp_output = display_written_packets > 0u || stdout_enabled;
    update_seq = next_status_update_seq++;

    gui_status_submit(status,
                      display_v,
                      display_rs_ok,
                      display_rs_uncorrectable,
                      display_written_packets,
                      lamp_ofdm_sync,
                      lamp_rs_lock,
                      (lamp_pat && lamp_pmt) || lamp_sdt || lamp_service);

    if (status->status_json_path == NULL) {
        return;
    }

    if (!status_json_write_allowed(status->status_json_path, updated_unix)) {
        return;
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", status->status_json_path) >= (int)sizeof(tmp_path)) {
        status_json_note_failure(status->status_json_path, "prepare", 0, updated_unix);
        return;
    }

    f = fopen(tmp_path, "w");
    if (f == NULL) {
        status_json_note_failure(status->status_json_path, "open", errno, updated_unix);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"status_update\": %u,\n", update_seq);
    fprintf(f, "  \"updated_unix\": %lld,\n", (long long)updated_unix);
    fprintf(f, "  \"stage\": \"%s\",\n", stage != NULL ? stage : "unknown");
    fprintf(f, "  \"lock_state\": \"%s\",\n",
            lock_state_name(status_display_lock_state(lock_state, held_locked)));
    fprintf(f, "  \"input_samples\": %llu,\n", (unsigned long long)input_samples);
    fprintf(f, "  \"symbol_rate\": \"%s\",\n", status->symbol_rate != NULL ? status->symbol_rate : "unknown");
    fprintf(f, "  \"modulation\": \"%s\",\n", status->modulation != NULL ? status->modulation : "dvb-t");
    fprintf(f, "  \"fft\": \"%s\",\n", status->fft_mode != NULL ? status->fft_mode : "2k");
    fprintf(f, "  \"constellation\": \"%s\",\n", status->constellation != NULL ? status->constellation : "QPSK");
    fprintf(f, "  \"fec\": \"%s\",\n", status->fec != NULL ? status->fec : "unknown");
    fprintf(f, "  \"guard\": \"%s\",\n", status->guard_interval != NULL ? status->guard_interval : "unknown");
    fprintf(f, "  \"locked\": %s,\n", locked ? "true" : "false");
    fprintf(f, "  \"lock_quality\": %u,\n", lock_quality);
    if (isfinite(status->pilot_lock)) {
        fprintf(f, "  \"pilot_lock\": %.5f,\n", status->pilot_lock);
    } else {
        fprintf(f, "  \"pilot_lock\": null,\n");
    }
    fprintf(f, "  \"ssi\": %u,\n", ssi);
    if (isfinite(status->snr_db)) {
        fprintf(f, "  \"sqi\": %u,\n  \"snr\": %.2f,\n  \"snr_db\": %.2f,\n", sqi, status->snr_db, status->snr_db);
    } else {
        fprintf(f, "  \"sqi\": %u,\n  \"snr\": null,\n  \"snr_db\": null,\n", sqi);
    }
    if (isfinite(status->cfo_hz)) {
        fprintf(f, "  \"cfo_hz\": %.3f,\n", status->cfo_hz);
    } else {
        fprintf(f, "  \"cfo_hz\": null,\n");
    }
    fprintf(f, "  \"bin_shift\": %d,\n", status->bin_shift);
    fprintf(f, "  \"afc_enabled\": %s,\n", status->afc_enabled ? "true" : "false");
    fprintf(f, "  \"afc_advised\": %s,\n", status->afc_advised ? "true" : "false");
    fprintf(f, "  \"afc_delta_bins\": %d,\n", status->afc_delta_bins);
    fprintf(f, "  \"afc_trend_count\": %u,\n", status->afc_trend_count);
    fprintf(f, "  \"symbol_phase\": %d,\n", status->symbol_phase);
    fprintf(f, "  \"fifo1_queued_symbols\": %u,\n", status->fifo1_queued_symbols);
    fprintf(f, "  \"fifo1_processing_symbols\": %u,\n", status->fifo1_processing_symbols);
    fprintf(f, "  \"fifo1_capacity_symbols\": %u,\n", status->fifo1_capacity_symbols);
    fprintf(f, "  \"fifo1_load_percent\": %u,\n", status->fifo1_load_percent);
    fprintf(f, "  \"pe\": %u,\n", pe);
    fprintf(f, "  \"service_id\": %u,\n", display_v->service_id);
    fprintf(f, "  \"program_id\": %u,\n", display_v->program_id);
    fprintf(f, "  \"service_name\": \"");
    for (const char *p = display_v->service_name; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
        }
        fputc(*p, f);
    }
    fprintf(f, "\",\n");
    fprintf(f, "  \"service_provider\": \"");
    for (const char *p = display_v->service_provider; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
        }
        fputc(*p, f);
    }
    fprintf(f, "\",\n");
    fprintf(f, "  \"packets\": %u,\n", display_v->packets);
    fprintf(f, "  \"written_packets\": %u,\n", display_written_packets);
    fprintf(f, "  \"sync_bad\": %u,\n", display_v->sync_bad);
    fprintf(f, "  \"transport_errors\": %u,\n", display_v->transport_errors);
    fprintf(f, "  \"cc_errors\": %u,\n", display_v->cc_errors);
    fprintf(f, "  \"pat_packets\": %u,\n", display_v->pat_packets);
    fprintf(f, "  \"pmt_packets\": %u,\n", display_v->pmt_packets);
    fprintf(f, "  \"sdt_packets\": %u,\n", display_v->sdt_packets);
    fprintf(f, "  \"pmt_pid\": %u,\n", display_v->pmt_pid == 0x1fffu ? 8191u : display_v->pmt_pid);
    fprintf(f, "  \"pcr_pid\": %u,\n", display_v->pcr_pid == 0x1fffu ? 8191u : display_v->pcr_pid);
    fprintf(f, "  \"video_pid\": %u,\n", display_v->video_pid == 0x1fffu ? 8191u : display_v->video_pid);
    fprintf(f, "  \"audio_pid\": %u,\n", display_v->audio_pid == 0x1fffu ? 8191u : display_v->audio_pid);
    fprintf(f, "  \"ts_session_id\": %u,\n", status->live_mode && live_ts_session.valid ? live_ts_session.session_id : 0u);
    fprintf(f, "  \"ts_session_restarts\": %u,\n", status->live_mode && live_ts_session.valid ? live_ts_session.restarts : 0u);
    fprintf(f, "  \"ts_session_start_unix\": %lld,\n",
            (long long)(status->live_mode && live_ts_session.valid ? live_ts_session.started_unix : 0));
    fprintf(f, "  \"ts_session_packets\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.packets : 0u));
    fprintf(f, "  \"ts_session_written_packets\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.written_packets : 0u));
    fprintf(f, "  \"ts_session_sync_bad\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.sync_bad : 0u));
    fprintf(f, "  \"ts_session_transport_errors\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.transport_errors : 0u));
    fprintf(f, "  \"ts_session_cc_errors\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.cc_errors : 0u));
    fprintf(f, "  \"ts_session_pat_packets\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.pat_packets : 0u));
    fprintf(f, "  \"ts_session_pmt_packets\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.pmt_packets : 0u));
    fprintf(f, "  \"ts_session_sdt_packets\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.sdt_packets : 0u));
    fprintf(f, "  \"ts_session_rs_ok\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.rs_ok : 0u));
    fprintf(f, "  \"ts_session_rs_bad\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.rs_bad : 0u));
    fprintf(f, "  \"ts_session_rs_corrected\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.rs_corrected : 0u));
    fprintf(f, "  \"ts_session_rs_corrected_bytes\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.rs_corrected_bytes : 0u));
    fprintf(f, "  \"ts_session_rs_uncorrectable\": %llu,\n",
            (unsigned long long)(status->live_mode ? live_ts_session.rs_uncorrectable : 0u));
    fprintf(f, "  \"stdout_enabled\": %s,\n", stdout_enabled ? "true" : "false");
    fprintf(f, "  \"waiting_for_video_start\": %s,\n", waiting_for_video_start ? "true" : "false");
    fprintf(f, "  \"lamp_symbol_rate\": %s,\n", lamp_symbol_rate ? "true" : "false");
    fprintf(f, "  \"lamp_guard\": %s,\n", lamp_guard ? "true" : "false");
    fprintf(f, "  \"lamp_fec\": %s,\n", lamp_fec ? "true" : "false");
    fprintf(f, "  \"lamp_ofdm_sync\": %s,\n", lamp_ofdm_sync ? "true" : "false");
    fprintf(f, "  \"lamp_inner_fec\": %s,\n", lamp_inner_fec ? "true" : "false");
    fprintf(f, "  \"lamp_rs_lock\": %s,\n", lamp_rs_lock ? "true" : "false");
    fprintf(f, "  \"lamp_rs_clean\": %s,\n", lamp_rs_clean ? "true" : "false");
    fprintf(f, "  \"lamp_ts_sync\": %s,\n", lamp_ts_sync ? "true" : "false");
    fprintf(f, "  \"lamp_pat\": %s,\n", lamp_pat ? "true" : "false");
    fprintf(f, "  \"lamp_pmt\": %s,\n", lamp_pmt ? "true" : "false");
    fprintf(f, "  \"lamp_sdt\": %s,\n", lamp_sdt ? "true" : "false");
    fprintf(f, "  \"lamp_service\": %s,\n", lamp_service ? "true" : "false");
    fprintf(f, "  \"lamp_av\": %s,\n", lamp_av ? "true" : "false");
    fprintf(f, "  \"lamp_output\": %s,\n", lamp_output ? "true" : "false");
    fprintf(f, "  \"rs_ok\": %u,\n", display_rs_ok);
    fprintf(f, "  \"rs_bad\": %u,\n", display_rs_bad);
    fprintf(f, "  \"rs_corrected\": %u,\n", display_rs_corrected);
    fprintf(f, "  \"rs_corrected_bytes\": %u,\n", display_rs_corrected_bytes);
    fprintf(f, "  \"rs_uncorrectable\": %u\n", display_rs_uncorrectable);
    fprintf(f, "}\n");
    if (fclose(f) != 0) {
        status_json_note_failure(status->status_json_path, "close", errno, updated_unix);
        return;
    }
#ifdef _WIN32
    remove(status->status_json_path);
#endif
    if (rename(tmp_path, status->status_json_path) != 0) {
        status_json_note_failure(status->status_json_path, "publish", errno, updated_unix);
        remove(tmp_path);
        return;
    }
    status_json_note_success(status->status_json_path);
}

void rbdvbt_status_publish_idle(const rbdvbt_status_context_t *status,
                                const char *stage,
                                uint64_t input_samples)
{
    ts_validator_t validator;

    ts_validator_init(&validator);
    status_write_json(status,
                      &validator,
                      0,
                      0,
                      0,
                      0,
                      0,
                      0,
                      RX_LOCK_SEARCH,
                      0,
                      status != NULL ? status->wait_video_start : 0,
                      stage,
                      input_samples);
}

static int live_grdvbt_outer_push_deint_byte(uint8_t deint_byte,
                                             const uint8_t *scramble,
                                             rbdvbt_fifo_t *fifo3,
                                             ts_output_gate_t *gate,
                                             ts_validator_t *validator,
                                             ts_output_sink_t *sink,
                                             const char *ts_path,
                                             const rbdvbt_status_context_t *status,
                                             uint32_t *written,
                                             uint32_t *rs_bad,
                                             uint32_t *rs_ok,
                                             uint32_t *rs_corrected,
                                             uint32_t *rs_corrected_bytes,
                                             uint32_t *rs_uncorrectable,
                                             uint32_t *processed_blocks,
                                             rx_lock_state_t *lock_state)
{
    live_grdvbt_outer.rs_block[live_grdvbt_outer.rs_count++] = deint_byte;
    if (live_grdvbt_outer.rs_count < RS_BLOCK_LEN) {
        return 0;
    }

    {
        uint32_t phase = live_grdvbt_outer.block_phase;
        uint8_t corrected[RS_BLOCK_LEN];
        uint8_t ts[RS_DATA_LEN];
        int correction_result;
        int pilot_lock_ok = status == NULL ||
            !isfinite(status->pilot_lock) ||
            status->pilot_lock >= LIVE_GRDVBT_PILOT_LOCK_MIN;
        rbdvbt_ts_packet_t fifo3_item;
        int fifo_rc;

        live_grdvbt_outer.rs_count = 0u;
        (*processed_blocks)++;
        live_grdvbt_outer.rs_blocks++;
        live_grdvbt_outer.block_phase = (live_grdvbt_outer.block_phase + 1u) & 7u;

        memcpy(corrected, live_grdvbt_outer.rs_block, sizeof(corrected));
        correction_result = rs_correct_204(corrected);
        if (correction_result < 0) {
            (*rs_bad)++;
            (*rs_uncorrectable)++;
            *lock_state = gate->stdout_enabled ? RX_LOCK_DEGRADED : RX_LOCK_RELOCK;
            return 0;
        }

        (*rs_ok)++;
        if (correction_result > 0) {
            (*rs_corrected)++;
            *rs_corrected_bytes += (uint32_t)correction_result;
        }
        descramble_packet(corrected, phase, scramble, ts);

        memset(&fifo3_item, 0, sizeof(fifo3_item));
        fifo3_item.generation_id = 0u;
        fifo3_item.packet_index = *rs_ok - 1u;
        memcpy(fifo3_item.bytes, ts, sizeof(fifo3_item.bytes));
        fifo3_item.rs_corrected_errors = correction_result > 0 ? (uint32_t)correction_result : 0u;

        fifo_rc = rbdvbt_fifo_try_push(fifo3, &fifo3_item);
        if (fifo_rc == 1) {
            rbdvbt_pipeline_event_t event;

            memset(&event, 0, sizeof(event));
            event.type = RBDVBT_PIPELINE_EVENT_FIFO_OVERRUN;
            event.source_block = "block3_outer_fec";
            event.fifo_name = fifo3->name;
            event.item_index = fifo3_item.packet_index;
            event.value = (uint32_t)rbdvbt_fifo_count(fifo3);
            rbdvbt_pipeline_publish_event(&event);
            return -1;
        }
        if (fifo_rc != 0) {
            return -1;
        }

        while (rbdvbt_fifo_try_pop(fifo3, &fifo3_item) == 0) {
            if (block4_consume_fifo3_packet(&fifo3_item,
                                            gate,
                                            validator,
                                            sink,
                                            ts_path,
                                            1,
                                            pilot_lock_ok,
                                            lock_state,
                                            status,
                                            written) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int live_grdvbt_outer_append_pending(const uint8_t *bytes, size_t count)
{
    size_t need;
    uint8_t *p;

    if (count == 0u) {
        return 0;
    }
    need = live_grdvbt_outer.pending_count + count;
    if (need > live_grdvbt_outer.pending_cap) {
        size_t cap = live_grdvbt_outer.pending_cap != 0u ? live_grdvbt_outer.pending_cap : RS_BLOCK_LEN * 1024u;

        while (cap < need) {
            if (cap > ((size_t)-1) / 2u) {
                return -1;
            }
            cap *= 2u;
        }
        p = realloc(live_grdvbt_outer.pending, cap);
        if (p == NULL) {
            return -1;
        }
        live_grdvbt_outer.pending = p;
        live_grdvbt_outer.pending_cap = cap;
    }
    memcpy(live_grdvbt_outer.pending + live_grdvbt_outer.pending_count, bytes, count);
    live_grdvbt_outer.pending_count += count;
    return 0;
}

static int live_grdvbt_outer_process_bytes(const uint8_t *inner,
                                           size_t inner_count,
                                           size_t start,
                                           size_t drop_deint,
                                           const uint8_t *scramble,
                                           rbdvbt_fifo_t *fifo3,
                                           ts_output_gate_t *gate,
                                           ts_validator_t *validator,
                                           ts_output_sink_t *sink,
                                           const char *ts_path,
                                           const rbdvbt_status_context_t *status,
                                           uint32_t *written,
                                           uint32_t *rs_bad,
                                           uint32_t *rs_ok,
                                           uint32_t *rs_corrected,
                                           uint32_t *rs_corrected_bytes,
                                           uint32_t *rs_uncorrectable,
                                           uint32_t *processed_blocks,
                                           rx_lock_state_t *lock_state)
{
    for (size_t i = start; i < inner_count; ++i) {
        uint8_t deint_byte = outer_deinterleaver_step(&live_grdvbt_outer.deinterleaver,
                                                      live_grdvbt_outer.branch,
                                                      inner[i]);

        live_grdvbt_outer.input_bytes++;
        live_grdvbt_outer.branch = (live_grdvbt_outer.branch + 1u) % OUTER_I;
        live_grdvbt_outer.deint_bytes++;
        if (drop_deint > 0u) {
            drop_deint--;
            continue;
        }
        if (live_grdvbt_outer_push_deint_byte(deint_byte,
                                              scramble,
                                              fifo3,
                                              gate,
                                              validator,
                                              sink,
                                              ts_path,
                                              status,
                                              written,
                                              rs_bad,
                                              rs_ok,
                                              rs_corrected,
                                              rs_corrected_bytes,
                                              rs_uncorrectable,
                                              processed_blocks,
                                              lock_state) != 0) {
            return -1;
        }
    }

    return 0;
}

static int live_grdvbt_outer_process(const uint8_t *inner,
                                     size_t inner_count,
                                     const uint8_t *scramble,
                                     rbdvbt_fifo_t *fifo3,
                                     ts_output_gate_t *gate,
                                     ts_validator_t *validator,
                                     ts_output_sink_t *sink,
                                     const char *ts_path,
                                     const rbdvbt_status_context_t *status,
                                     uint32_t *written,
                                     uint32_t *rs_bad,
                                     uint32_t *rs_ok,
                                     uint32_t *rs_corrected,
                                     uint32_t *rs_corrected_bytes,
                                     uint32_t *rs_uncorrectable,
                                     uint32_t *processed_blocks,
                                     rx_lock_state_t *lock_state)
{
    size_t start = 0u;
    size_t drop_deint = 0u;

    if (!live_grdvbt_outer.valid) {
        outer_candidate_t best;
        outer_scan_policy_t policy = outer_scan_policy_for_status(status);
        int probation_lock = 0;

        if (live_grdvbt_outer_append_pending(inner, inner_count) != 0) {
            fprintf(stderr, "[outer] failed to append live gr-dvbt pending bytes\n");
            return -1;
        }
        if (live_grdvbt_outer.pending_count < policy.min_pending_bytes) {
            if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
                fprintf(stderr,
                        "[outer-state] grdvbt acquire accumulating pending=%zu need=%zu sr=%s\n",
                        live_grdvbt_outer.pending_count,
                        policy.min_pending_bytes,
                        status != NULL && status->symbol_rate != NULL ? status->symbol_rate : "unknown");
            }
            return 0;
        }
        if (scan_outer_alignment_with_policy(live_grdvbt_outer.pending,
                                             live_grdvbt_outer.pending_count,
                                             &best,
                                             &policy) != 0 ||
            !outer_candidate_is_acquirable(&best, &policy, &probation_lock)) {
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] grdvbt acquire pending=%zu failed rs_ok=%u/%u sync_ok=%u/%u sync_only=%u sr=%s\n",
                        live_grdvbt_outer.pending_count,
                        best.rs_ok,
                        policy.min_rs_ok,
                        best.sync_ok,
                        policy.min_sync_ok,
                        policy.min_sync_only_ok,
                        status != NULL && status->symbol_rate != NULL ? status->symbol_rate : "unknown");
            }
            return 0;
        }

        {
            uint8_t *pending = live_grdvbt_outer.pending;
            size_t pending_count = live_grdvbt_outer.pending_count;
            size_t pending_cap = live_grdvbt_outer.pending_cap;

            live_grdvbt_outer_reset();
            live_grdvbt_outer.pending = pending;
            live_grdvbt_outer.pending_count = pending_count;
            live_grdvbt_outer.pending_cap = pending_cap;
        }
        if (outer_deinterleaver_init(&live_grdvbt_outer.deinterleaver) != 0) {
            fprintf(stderr, "[outer] failed to initialize live gr-dvbt deinterleaver\n");
            return -1;
        }
        live_grdvbt_outer.deinterleaver_ready = 1;
        live_grdvbt_outer.valid = 1;
        live_grdvbt_outer.branch = 0u;
        live_grdvbt_outer.block_phase = best.block_phase;
        live_grdvbt_outer.lock_jobs++;
        live_grdvbt_outer.probation_lock = probation_lock;
        start = best.deint_phase;
        drop_deint = OUTER_TRANSIENT + best.rs_phase;
        if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
            fprintf(stderr,
                    "[outer-state] acquired grdvbt_stream deint_phase=%u rs_phase=%u block_phase=%u drop_deint=%zu rs_probe_ok=%u sync_ok=%u probation=%d\n",
                    best.deint_phase,
                    best.rs_phase,
                    best.block_phase,
                    drop_deint,
                    best.rs_ok,
                    best.sync_ok,
                    probation_lock);
        }
        if (live_grdvbt_outer_process_bytes(live_grdvbt_outer.pending,
                                            live_grdvbt_outer.pending_count,
                                            start,
                                            drop_deint,
                                            scramble,
                                            fifo3,
                                            gate,
                                            validator,
                                            sink,
                                            ts_path,
                                            status,
                                            written,
                                            rs_bad,
                                            rs_ok,
                                            rs_corrected,
                                            rs_corrected_bytes,
                                            rs_uncorrectable,
                                            processed_blocks,
                                            lock_state) != 0) {
            return -1;
        }
        live_grdvbt_outer.pending_count = 0u;
        return 0;
    }

    return live_grdvbt_outer_process_bytes(inner,
                                           inner_count,
                                           0u,
                                           0u,
                                           scramble,
                                           fifo3,
                                           gate,
                                           validator,
                                           sink,
                                           ts_path,
                                           status,
                                           written,
                                           rs_bad,
                                           rs_ok,
                                           rs_corrected,
                                           rs_corrected_bytes,
                                           rs_uncorrectable,
                                           processed_blocks,
                                           lock_state);
}

static uint32_t outer_quality_score(const ts_validator_t *v,
                                    uint32_t rs_uncorrectable,
                                    uint32_t rs_corrected,
                                    uint32_t written_packets)
{
    uint32_t errors = rs_uncorrectable + v->sync_bad + v->transport_errors + v->cc_errors;
    uint32_t score = 0;

    if (written_packets == 0u) {
        return 0;
    }

    score += written_packets;
    score += v->pat_packets * 10000u;
    score += v->pmt_packets * 10000u;
    score += v->sdt_packets * 1000u;
    score += rs_corrected * 10u;
    if (v->video_pid != 0x1fffu) {
        score += 5000u;
    }
    if (v->audio_pid != 0x1fffu) {
        score += 1000u;
    }

    if (errors * 100u >= score) {
        return 0;
    }
    return score - errors * 100u;
}

int rbdvbt_outer_analyze_inner(const uint8_t *inner,
                               size_t inner_count,
                               rbdvbt_outer_metrics_t *metrics)
{
    uint8_t scramble[SCRAMBLE_SEQ_LEN];
    outer_candidate_t best;
    uint8_t *best_deint = NULL;
    size_t best_deint_count = 0;
    uint32_t written = 0;
    uint32_t rs_bad = 0;
    uint32_t rs_corrected = 0;
    uint32_t rs_corrected_bytes = 0;
    uint32_t rs_uncorrectable = 0;
    ts_validator_t validator;
    int rc = -1;

    if (metrics == NULL) {
        return -1;
    }
    memset(metrics, 0, sizeof(*metrics));
    memset(&best, 0, sizeof(best));
    ts_validator_init(&validator);
    build_scrambler_table(scramble);

    for (uint32_t deint_phase = 0; deint_phase < OUTER_I; ++deint_phase) {
        size_t deint_count = 0;
        uint8_t *deint = outer_deinterleave_phase(inner, inner_count, deint_phase, &deint_count);

        if (deint == NULL) {
            continue;
        }

        for (uint32_t rs_phase = 0; rs_phase < RS_BLOCK_LEN; ++rs_phase) {
            for (uint32_t block_phase = 0; block_phase < 8u; ++block_phase) {
                outer_candidate_t c = score_candidate(deint,
                                                      deint_count,
                                                      deint_phase,
                                                      rs_phase,
                                                      block_phase);

                if (c.score > best.score) {
                    best = c;
                }
            }
        }

        free(deint);
    }

    if (best.blocks == 0u) {
        goto done;
    }

    best_deint = outer_deinterleave_phase(inner, inner_count, best.deint_phase, &best_deint_count);
    if (best_deint == NULL) {
        goto done;
    }

    {
        size_t offset = OUTER_TRANSIENT + best.rs_phase;
        uint32_t blocks = (uint32_t)((best_deint_count - offset) / RS_BLOCK_LEN);

        for (uint32_t b = 0; b < blocks; ++b) {
            const uint8_t *block = &best_deint[offset + (size_t)b * RS_BLOCK_LEN];
            uint32_t phase = (best.block_phase + b) & 7u;
            uint8_t corrected[RS_BLOCK_LEN];
            uint8_t ts[RS_DATA_LEN];
            int correction_result;

            memcpy(corrected, block, sizeof(corrected));
            correction_result = rs_correct_204(corrected);
            if (correction_result < 0) {
                rs_bad++;
                rs_uncorrectable++;
            } else {
                if (correction_result > 0) {
                    rs_corrected++;
                    rs_corrected_bytes += (uint32_t)correction_result;
                }
            }
            descramble_packet(corrected, phase, scramble, ts);
            ts_validator_observe(&validator, ts);
            written++;
        }
    }

    metrics->packets = validator.packets;
    metrics->sync_bad = validator.sync_bad;
    metrics->transport_errors = validator.transport_errors;
    metrics->cc_errors = validator.cc_errors;
    metrics->pat_packets = validator.pat_packets;
    metrics->pmt_packets = validator.pmt_packets;
    metrics->sdt_packets = validator.sdt_packets;
    metrics->rs_bad = rs_bad;
    metrics->rs_corrected = rs_corrected;
    metrics->rs_corrected_bytes = rs_corrected_bytes;
    metrics->rs_uncorrectable = rs_uncorrectable;
    metrics->score = outer_quality_score(&validator, rs_uncorrectable, rs_corrected, written);
    rc = 0;

done:
    free(best_deint);
    return rc;
}

int rbdvbt_outer_recover_ts(const uint8_t *inner,
                            size_t inner_count,
                            const char *ts_path,
                            const rbdvbt_status_context_t *status)
{
    uint8_t scramble[SCRAMBLE_SEQ_LEN];
    outer_candidate_t best;
    uint8_t *best_deint = NULL;
    size_t best_deint_count = 0;
    ts_output_sink_t sink;
    int sink_ready = 0;
    uint32_t written = 0;
    uint32_t rs_bad = 0;
    uint32_t rs_ok = 0;
    uint32_t rs_corrected = 0;
    uint32_t rs_corrected_bytes = 0;
    uint32_t rs_uncorrectable = 0;
    ts_validator_t validator;
    ts_output_gate_t local_gate;
    ts_output_gate_t *gate;
    rbdvbt_fifo_t fifo3;
    rx_lock_state_t lock_state = RX_LOCK_SEARCH;
    int live_mode = status != NULL && status->live_mode;
    int live_mpeg_sync_enabled = 0;
    int fifo3_ready = 0;
    uint32_t processed_blocks = 0;
    outer_scan_policy_t live_policy = outer_scan_policy_for_status(status);
    int rc = -1;

    memset(&best, 0, sizeof(best));
    memset(&fifo3, 0, sizeof(fifo3));
    memset(&sink, 0, sizeof(sink));
    sink.udp = NULL;
    ts_validator_init(&validator);
    if (rbdvbt_fifo_init(&fifo3, "FIFO3_TS_PACKETS", sizeof(rbdvbt_ts_packet_t), FIFO3_CAPACITY_PACKETS) != 0) {
        fprintf(stderr, "[outer] failed to initialize FIFO3 TS packet queue\n");
        goto done;
    }
    fifo3_ready = 1;
    if (live_mode) {
        if (!live_ts_gate_ready) {
            ts_output_gate_init(&live_ts_gate, status != NULL ? status->wait_video_start : 0);
            live_ts_gate_ready = 1;
        }
        gate = &live_ts_gate;
    } else {
        ts_output_gate_init(&local_gate, status != NULL ? status->wait_video_start : 0);
        gate = &local_gate;
    }
    build_scrambler_table(scramble);

    if (live_mode &&
        status != NULL &&
        isfinite(status->pilot_lock) &&
        status->pilot_lock < LIVE_GRDVBT_PILOT_LOCK_MIN) {
        if (!live_grdvbt_outer.valid) {
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] skip low-pilot chunk pilot_lock=%.5f; keep pending acquisition bytes=%zu\n",
                        status->pilot_lock,
                        live_grdvbt_outer.pending_count);
            }
        } else {
            live_grdvbt_outer.low_pilot_jobs++;
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] skip low-pilot chunk pilot_lock=%.5f low_pilot_jobs=%u/%u; keeping outer cadence\n",
                        status->pilot_lock,
                        live_grdvbt_outer.low_pilot_jobs,
                        live_policy.low_pilot_reset_limit);
            }
            if (live_grdvbt_outer.low_pilot_jobs >= live_policy.low_pilot_reset_limit) {
                if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                    fprintf(stderr,
                            "[outer-state] relock grdvbt_stream consecutive_low_pilot=%u; resetting outer cadence\n",
                            live_grdvbt_outer.low_pilot_jobs);
                }
                rbdvbt_outer_reset_live_stream();
            }
        }
        status_write_json(status,
                          &validator,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          gate->stdout_enabled ? RX_LOCK_DEGRADED : RX_LOCK_RELOCK,
                          gate->stdout_enabled,
                          gate->waiting_for_video_start,
                          "pilot-drop",
                          status->input_samples);
        rbdvbt_live_health_note_outer(0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      live_grdvbt_outer.pending_count,
                                      "PILOT_DROP");
        rc = 0;
        goto done;
    }

    status_write_json(status,
                      &validator,
                      0,
                      0,
                      0,
                      0,
                      0,
                      0,
                      RX_LOCK_SEARCH,
                      gate->stdout_enabled,
	                      gate->waiting_for_video_start,
	                      "outer",
	                      status != NULL ? status->input_samples : 0u);

    if (live_mode && !live_mpeg_sync_enabled) {
        if (ts_output_sink_open(&sink, ts_path, live_mode) != 0) {
            fprintf(stderr, "failed to open TS output: %s\n", ts_path);
            goto done;
        }
        sink_ready = 1;

        if (live_grdvbt_outer_process(inner,
                                      inner_count,
                                      scramble,
                                      &fifo3,
                                      gate,
                                      &validator,
                                      &sink,
                                      ts_path,
                                      status,
                                      &written,
                                      &rs_bad,
                                      &rs_ok,
                                      &rs_corrected,
                                      &rs_corrected_bytes,
                                      &rs_uncorrectable,
                                      &processed_blocks,
                                      &lock_state) != 0) {
            lock_state = RX_LOCK_RELOCK;
            rc = -1;
            goto done;
        }

        if (rbdvbt_log_enabled(RBDVBT_LOG_INFO) ||
            (rbdvbt_log_enabled(RBDVBT_LOG_ERROR) &&
             (rs_uncorrectable != 0u || written == 0u))) {
            fprintf(stderr,
                    "[outer] grdvbt_stream input_bytes=%llu deint_bytes=%llu rs_buffer=%u branch=%u block_phase=%u processed_blocks=%u written_packets=%u rs_bad=%u rs_corrected=%u rs_corrected_bytes=%u rs_uncorrectable=%u output=%s\n",
                    (unsigned long long)live_grdvbt_outer.input_bytes,
                    (unsigned long long)live_grdvbt_outer.deint_bytes,
                    live_grdvbt_outer.rs_count,
                    live_grdvbt_outer.branch,
                    live_grdvbt_outer.block_phase,
                    processed_blocks,
                    written,
                    rs_bad,
                    rs_corrected,
                    rs_corrected_bytes,
                    rs_uncorrectable,
                    ts_path);
        }
        ts_validator_report(&validator);

        if (processed_blocks > 0u &&
            (rs_ok == 0u || rs_uncorrectable >= rs_ok)) {
            int hard_cadence_fail = processed_blocks >= LIVE_GRDVBT_HARD_FAIL_BLOCKS &&
                rs_ok == 0u &&
                written == 0u;

            live_grdvbt_outer.fail_jobs++;
            if (hard_cadence_fail) {
                live_grdvbt_outer.hard_fail_jobs++;
            } else {
                live_grdvbt_outer.hard_fail_jobs = 0u;
            }
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] degraded grdvbt_stream fail_jobs=%u/%u hard_fail_jobs=%u/%u rs_ok=%u rs_uncorrectable=%u written=%u rs_buffer=%u\n",
                        live_grdvbt_outer.fail_jobs,
                        live_policy.soft_fail_limit,
                        live_grdvbt_outer.hard_fail_jobs,
                        live_policy.hard_fail_limit,
                        rs_ok,
                        rs_uncorrectable,
                        written,
                        live_grdvbt_outer.rs_count);
            }
            lock_state = gate->stdout_enabled ? RX_LOCK_DEGRADED : RX_LOCK_RELOCK;
            if ((hard_cadence_fail && live_grdvbt_outer.hard_fail_jobs >= live_policy.hard_fail_limit) ||
                live_grdvbt_outer.fail_jobs >= live_policy.soft_fail_limit) {
                if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                    if (hard_cadence_fail && live_grdvbt_outer.hard_fail_jobs >= live_policy.hard_fail_limit) {
                        fprintf(stderr,
                                "[outer-state] relock grdvbt_stream hard_cadence_fail jobs=%u blocks=%u rs_ok=%u written=%u; resetting outer cadence\n",
                                live_grdvbt_outer.hard_fail_jobs,
                                processed_blocks,
                                rs_ok,
                                written);
                    } else {
                        fprintf(stderr,
                                "[outer-state] relock grdvbt_stream consecutive_fail_jobs=%u; resetting outer cadence\n",
                                live_grdvbt_outer.fail_jobs);
                    }
                }
                live_grdvbt_outer_reset();
                live_outer_alignment.valid = 0;
                lock_state = RX_LOCK_RELOCK;
            }
        } else if (processed_blocks > 0u) {
            live_grdvbt_outer.fail_jobs = 0u;
            live_grdvbt_outer.hard_fail_jobs = 0u;
            live_grdvbt_outer.low_pilot_jobs = 0u;
            live_grdvbt_outer.probation_lock = 0;
            live_grdvbt_outer.lock_jobs++;
            if (written > 0u && validator.packets > 0u) {
                lock_state = RX_LOCK_LOCKED;
            }
        }

        {
            const char *health_outer_state = lock_state_name(lock_state);

            if (!live_grdvbt_outer.valid && live_grdvbt_outer.pending_count > 0u) {
                health_outer_state = "ACQUIRE";
            }
            rbdvbt_live_health_note_outer(validator.packets,
                                          validator.sync_bad,
                                          validator.transport_errors,
                                          validator.cc_errors,
                                          rs_bad,
                                          rs_corrected,
                                          rs_uncorrectable,
                                          written,
                                          live_grdvbt_outer.pending_count,
                                          health_outer_state);
        }

        status_write_json(status,
                          &validator,
                          rs_bad,
                          rs_ok,
                          rs_corrected,
                          rs_corrected_bytes,
                          rs_uncorrectable,
                          written,
                          lock_state,
                          gate->stdout_enabled,
                          gate->waiting_for_video_start,
                          "grdvbt_stream",
                          status != NULL ? status->input_samples : 0u);
        rc = 0;
        goto done;
    }

	    if (live_mode && live_mpeg_sync_enabled) {
	        if (live_mpeg_sync_append(inner, inner_count) != 0) {
	            fprintf(stderr, "[outer] failed to append FIFO2 bytes to MPEG sync buffer\n");
	            goto done;
	        } else if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
	            fprintf(stderr,
	                    "[mpeg-sync] appended bytes=%zu buffered=%zu synchronized=%d phase8=%u\n",
	                    inner_count,
	                    live_mpeg_sync.count,
	                    live_mpeg_sync.synchronized,
	                    live_mpeg_sync.phase8);
	        }
	        for (;;) {
	            uint8_t packet[RS_BLOCK_LEN];
	            uint32_t packet_phase;
	            int sync_rc = live_mpeg_sync_pop_packet(packet);

	            if (sync_rc < 0) {
	                lock_state = RX_LOCK_RELOCK;
	                break;
	            }
	            if (sync_rc == 0) {
	                break;
	            }
	            packet_phase = (live_mpeg_sync.phase8 + 7u) & 7u;
	            if (live_outer_stream_append_rs_packet(packet, packet_phase) != 0) {
	                fprintf(stderr, "[outer] failed to append MPEG-synced packet to outer stream\n");
	                goto done;
	            }
	        }

	        if (ts_output_sink_open(&sink, ts_path, live_mode) != 0) {
            fprintf(stderr, "failed to open TS output: %s\n", ts_path);
            goto done;
        }
        sink_ready = 1;

        for (;;) {
            uint8_t block[RS_BLOCK_LEN];
            uint32_t phase = live_outer_stream.block_phase;
            uint8_t corrected[RS_BLOCK_LEN];
            uint8_t ts[RS_DATA_LEN];
            int correction_result;
            int pilot_lock_ok = status == NULL ||
                !isfinite(status->pilot_lock) ||
                status->pilot_lock >= LIVE_GRDVBT_PILOT_LOCK_MIN;
            rbdvbt_ts_packet_t fifo3_item;
            int fifo_rc;
            int have_block = live_outer_stream_pop_rs(block);

            if (have_block < 0) {
                goto done;
            }
            if (have_block == 0) {
                break;
            }

            processed_blocks++;
            live_outer_stream.block_phase = (live_outer_stream.block_phase + 1u) & 7u;
            memcpy(corrected, block, sizeof(corrected));
            correction_result = rs_correct_204(corrected);
            if (correction_result < 0) {
                rs_bad++;
                rs_uncorrectable++;
                lock_state = gate->stdout_enabled ? RX_LOCK_DEGRADED : RX_LOCK_RELOCK;
                continue;
            }

	            rs_ok++;
	            best.rs_ok++;
	            if (correction_result > 0) {
	                rs_corrected++;
	                rs_corrected_bytes += (uint32_t)correction_result;
	            }

	            if (corrected[0] == 0xb8u) {
	                phase = 0u;
	                live_outer_stream.derand_locked = 1;
	                live_outer_stream.derand_phase = 1u;
	                live_outer_stream.block_phase = 1u;
	            } else if (corrected[0] == 0x47u && live_outer_stream.derand_locked) {
	                phase = live_outer_stream.derand_phase;
	                if (phase == 0u) {
	                    live_outer_stream.derand_locked = 0;
	                    continue;
	                }
	                live_outer_stream.derand_phase = (phase + 1u) & 7u;
	                live_outer_stream.block_phase = live_outer_stream.derand_phase;
	            } else {
	                live_outer_stream.derand_locked = 0;
	                continue;
	            }

	            if (sync_matches(corrected[0], phase)) {
	                best.sync_ok++;
	            }
	            descramble_packet(corrected, phase, scramble, ts);

            memset(&fifo3_item, 0, sizeof(fifo3_item));
            fifo3_item.generation_id = 0u;
            fifo3_item.packet_index = rs_ok - 1u;
            memcpy(fifo3_item.bytes, ts, sizeof(fifo3_item.bytes));
            fifo3_item.rs_corrected_errors = correction_result > 0 ? (uint32_t)correction_result : 0u;

            fifo_rc = rbdvbt_fifo_try_push(&fifo3, &fifo3_item);
            if (fifo_rc == 1) {
                rbdvbt_pipeline_event_t event;

                memset(&event, 0, sizeof(event));
                event.type = RBDVBT_PIPELINE_EVENT_FIFO_OVERRUN;
                event.source_block = "block3_outer_fec";
                event.fifo_name = fifo3.name;
                event.item_index = fifo3_item.packet_index;
                event.value = (uint32_t)rbdvbt_fifo_count(&fifo3);
                rbdvbt_pipeline_publish_event(&event);
                goto done;
            }
            if (fifo_rc != 0) {
                goto done;
            }

            while (rbdvbt_fifo_try_pop(&fifo3, &fifo3_item) == 0) {
                if (block4_consume_fifo3_packet(&fifo3_item,
                                                gate,
                                                &validator,
                                                &sink,
                                                ts_path,
                                                live_mode,
                                                pilot_lock_ok,
                                                &lock_state,
                                                status,
                                                &written) != 0) {
                    goto done;
                }
            }
        }

        best.blocks = processed_blocks;
        if (rbdvbt_log_enabled(RBDVBT_LOG_INFO) ||
            (rbdvbt_log_enabled(RBDVBT_LOG_ERROR) &&
             (rs_uncorrectable != 0u || written == 0u))) {
            fprintf(stderr,
                    "[outer] leandvb_stream buffered=%zu processed_blocks=%u block_phase=%u written_packets=%u rs_bad=%u rs_corrected=%u rs_corrected_bytes=%u rs_uncorrectable=%u output=%s\n",
                    live_outer_stream.count,
                    processed_blocks,
                    live_outer_stream.block_phase,
                    written,
                    rs_bad,
                    rs_corrected,
                    rs_corrected_bytes,
                    rs_uncorrectable,
                    ts_path);
        }
        ts_validator_report(&validator);

        if (processed_blocks > 0u &&
            (rs_ok == 0u || rs_uncorrectable >= rs_ok)) {
            live_outer_stream.fail_jobs++;
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] relock leandvb_stream fail_jobs=%u rs_ok=%u rs_uncorrectable=%u written=%u buffered=%zu\n",
                        live_outer_stream.fail_jobs,
                        rs_ok,
                        rs_uncorrectable,
                        written,
                        live_outer_stream.count);
            }
            live_mpeg_sync_reset();
            live_outer_stream_reset();
            live_outer_alignment.valid = 0;
            lock_state = RX_LOCK_RELOCK;
        } else if (processed_blocks > 0u) {
            live_outer_stream.fail_jobs = 0u;
            live_outer_stream.lock_jobs++;
            live_outer_alignment.valid = 1;
            live_outer_alignment.block_phase = live_outer_stream.block_phase;
            live_outer_alignment.stream_byte_offset = live_outer_stream.stream_byte_offset;
            live_outer_alignment.consumed_deint_bytes = live_outer_stream.stream_byte_offset;
            if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
                fprintf(stderr,
                        "[outer-state] advanced leandvb_stream stream_byte_offset=%llu next_block_phase=%u processed_blocks=%u buffered=%zu\n",
                        (unsigned long long)live_outer_stream.stream_byte_offset,
                        live_outer_stream.block_phase,
                        processed_blocks,
                        live_outer_stream.count);
            }
        }

        status_write_json(status,
                          &validator,
                          rs_bad,
                          rs_ok,
                          rs_corrected,
                          rs_corrected_bytes,
                          rs_uncorrectable,
                          written,
                          lock_state,
                          gate->stdout_enabled,
                          gate->waiting_for_video_start,
                          lock_state_name(lock_state),
                          status != NULL ? status->input_samples : 0u);
        rc = 0;
        goto done;
    }

    if (live_mode && live_outer_alignment.valid) {
        size_t deint_count = 0;
        uint8_t *deint = outer_deinterleave_phase(inner,
                                                  inner_count,
                                                  live_outer_alignment.deint_phase,
                                                  &deint_count);

        if (deint != NULL) {
            best = live_outer_alignment_candidate(deint, deint_count);
            free(deint);
        }
        if (best.blocks == 0u || best.rs_ok == 0u) {
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] fixed alignment failed deint_phase=%u rs_phase=%u block_phase=%u stream_byte_offset=%llu consumed_deint_bytes=%llu blocks=%u rs_ok=%u sync_ok=%u; rescanning\n",
                        live_outer_alignment.deint_phase,
                        live_outer_alignment.rs_phase,
                        live_outer_alignment.block_phase,
                        (unsigned long long)live_outer_alignment.stream_byte_offset,
                        (unsigned long long)live_outer_alignment.consumed_deint_bytes,
                        best.blocks,
                        best.rs_ok,
                        best.sync_ok);
            }
            live_outer_alignment.valid = 0;
            memset(&best, 0, sizeof(best));
        } else if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr,
                    "[outer-state] using fixed alignment deint_phase=%u rs_phase=%u block_phase=%u stream_byte_offset=%llu consumed_deint_bytes=%llu rs_probe_ok=%u sync_ok=%u\n",
                    best.deint_phase,
                    best.rs_phase,
                    best.block_phase,
                    (unsigned long long)live_outer_alignment.stream_byte_offset,
                    (unsigned long long)live_outer_alignment.consumed_deint_bytes,
                    best.rs_ok,
                    best.sync_ok);
        }
    }

    if (!live_mode || !live_outer_alignment.valid) {
        if (scan_outer_alignment(inner, inner_count, &best) != 0) {
            fprintf(stderr, "[outer] failed to find byte/RS alignment\n");
            goto done;
        }
        if (live_mode && best.rs_ok > 0u) {
            live_outer_alignment_store(&best);
            if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                fprintf(stderr,
                        "[outer-state] acquired deint_phase=%u rs_phase=%u block_phase=%u stream_byte_offset=%llu consumed_deint_bytes=%llu rs_probe_ok=%u sync_ok=%u\n",
                        best.deint_phase,
                        best.rs_phase,
                        best.block_phase,
                        (unsigned long long)live_outer_alignment.stream_byte_offset,
                        (unsigned long long)live_outer_alignment.consumed_deint_bytes,
                        best.rs_ok,
                        best.sync_ok);
            }
        }
    }

    if (best.blocks == 0u) {
        fprintf(stderr, "[outer] failed to find byte/RS alignment\n");
        goto done;
    }
    if (best.rs_ok == 0u) {
        fprintf(stderr,
                "[outer] no RS-clean alignment found deint_phase=%u rs_phase=%u block_phase=%u sync_ok=%u; relock needed\n",
                best.deint_phase,
                best.rs_phase,
                best.block_phase,
                best.sync_ok);
        lock_state = RX_LOCK_RELOCK;
        status_write_json(status,
                          &validator,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          lock_state,
                          gate->stdout_enabled,
                          gate->waiting_for_video_start,
                          "relock",
                          status != NULL ? status->input_samples : 0u);
        goto done;
    }
    best.rs_ok = 0;

    best_deint = outer_deinterleave_phase(inner, inner_count, best.deint_phase, &best_deint_count);
    if (best_deint == NULL) {
        fprintf(stderr, "[outer] failed to rebuild selected byte deinterleaver phase\n");
        goto done;
    }

    if (ts_output_sink_open(&sink, ts_path, live_mode) != 0) {
        fprintf(stderr, "failed to open TS output: %s\n", ts_path);
        goto done;
    }
    sink_ready = 1;

    {
        size_t offset = OUTER_TRANSIENT + best.rs_phase;
        uint32_t blocks = (uint32_t)((best_deint_count - offset) / RS_BLOCK_LEN);

        processed_blocks = blocks;
        for (uint32_t b = 0; b < blocks; ++b) {
            const uint8_t *block = &best_deint[offset + (size_t)b * RS_BLOCK_LEN];
            uint32_t phase = (best.block_phase + b) & 7u;
            uint8_t corrected[RS_BLOCK_LEN];
            uint8_t ts[RS_DATA_LEN];
            int correction_result;
            int pilot_lock_ok = status == NULL ||
                !isfinite(status->pilot_lock) ||
                status->pilot_lock >= LIVE_GRDVBT_PILOT_LOCK_MIN;
            rbdvbt_ts_packet_t fifo3_item;
            int fifo_rc;

            memcpy(corrected, block, sizeof(corrected));
            correction_result = rs_correct_204(corrected);
            if (correction_result < 0) {
                rs_bad++;
                rs_uncorrectable++;
                if (live_mode && gate->stdout_enabled) {
                    lock_state = RX_LOCK_DEGRADED;
                } else {
                    lock_state = rs_ok == 0u ? RX_LOCK_SEARCH : RX_LOCK_RELOCK;
                    ts_output_gate_relock(gate, status != NULL ? status->wait_video_start : 0);
                }
                continue;
            } else {
                best.rs_ok++;
                rs_ok++;
                if (correction_result > 0) {
                    rs_corrected++;
                    rs_corrected_bytes += (uint32_t)correction_result;
                }
            }
            descramble_packet(corrected, phase, scramble, ts);

            memset(&fifo3_item, 0, sizeof(fifo3_item));
            fifo3_item.generation_id = 0u;
            fifo3_item.packet_index = rs_ok - 1u;
            memcpy(fifo3_item.bytes, ts, sizeof(fifo3_item.bytes));
            fifo3_item.rs_corrected_errors = correction_result > 0 ? (uint32_t)correction_result : 0u;

            fifo_rc = rbdvbt_fifo_try_push(&fifo3, &fifo3_item);
            if (fifo_rc == 1) {
                rbdvbt_pipeline_event_t event;

                memset(&event, 0, sizeof(event));
                event.type = RBDVBT_PIPELINE_EVENT_FIFO_OVERRUN;
                event.source_block = "block3_outer_fec";
                event.fifo_name = fifo3.name;
                event.item_index = fifo3_item.packet_index;
                event.value = (uint32_t)rbdvbt_fifo_count(&fifo3);
                rbdvbt_pipeline_publish_event(&event);
                goto done;
            }
            if (fifo_rc != 0) {
                goto done;
            }

            while (rbdvbt_fifo_try_pop(&fifo3, &fifo3_item) == 0) {
                if (block4_consume_fifo3_packet(&fifo3_item,
                                                gate,
                                                &validator,
                                                &sink,
                                                ts_path,
                                                live_mode,
                                                pilot_lock_ok,
                                                &lock_state,
                                                status,
                                                &written) != 0) {
                    goto done;
                }
            }
            if (status != NULL &&
                status->status_json_path != NULL &&
                status->status_period_packets != 0u &&
                written > 0u &&
                (written % status->status_period_packets) == 0u) {
                status_write_json(status,
                                  &validator,
                                  rs_bad,
                                  rs_ok,
                                  rs_corrected,
                                  rs_corrected_bytes,
                                  rs_uncorrectable,
                                  written,
                                  lock_state,
                                  gate->stdout_enabled,
                                  gate->waiting_for_video_start,
                                  "ts",
                                  status->input_samples);
            }
        }
    }

    if (rbdvbt_log_enabled(RBDVBT_LOG_INFO) ||
        (rbdvbt_log_enabled(RBDVBT_LOG_ERROR) &&
         (rs_uncorrectable != 0u || written == 0u))) {
        fprintf(stderr,
                "[outer] deint_phase=%u rs_phase=%u block_phase=%u scan_blocks=%u scan_rs_ok=%u scan_sync_ok=%u written_packets=%u rs_bad=%u rs_corrected=%u rs_corrected_bytes=%u rs_uncorrectable=%u output=%s\n",
                best.deint_phase,
                best.rs_phase,
                best.block_phase,
                best.blocks,
                best.rs_ok,
                best.sync_ok,
                written,
                rs_bad,
                rs_corrected,
                rs_corrected_bytes,
                rs_uncorrectable,
                ts_path);
    }
    ts_validator_report(&validator);
    if (written == 0u || validator.packets == 0u) {
        lock_state = RX_LOCK_RELOCK;
    } else if (rs_uncorrectable >= rs_ok) {
        lock_state = (live_mode && gate->stdout_enabled) ? RX_LOCK_DEGRADED : RX_LOCK_RELOCK;
    }
    if (live_mode) {
        live_outer_alignment_note_result(rs_ok,
                                         rs_uncorrectable,
                                         written,
                                         best_deint_count,
                                         processed_blocks,
                                         &validator);
    }
    status_write_json(status,
                      &validator,
                      rs_bad,
                      rs_ok,
                      rs_corrected,
                      rs_corrected_bytes,
                      rs_uncorrectable,
                      written,
                      lock_state,
                      gate->stdout_enabled,
                      gate->waiting_for_video_start,
                      "done",
                      status != NULL ? status->input_samples : 0u);

    if (lock_state == RX_LOCK_RELOCK) {
        rc = -1;
    } else {
        rc = 0;
    }

done:
    if (sink_ready && gate != NULL && gate->write_buf_packets > 0u) {
        if (ts_output_gate_flush(gate, &sink, ts_path) != 0) {
            rc = -1;
        }
    }
    if (fifo3_ready) {
        rbdvbt_fifo_free(&fifo3);
    }
    if (sink_ready) {
        ts_output_sink_close(&sink);
    }
    free(best_deint);
    return rc;
}
