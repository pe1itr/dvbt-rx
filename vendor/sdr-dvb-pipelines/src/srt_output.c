#define _POSIX_C_SOURCE 200809L
#include "srt_output.h"
#include <glib-unix.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "process_control.h"
#include <unistd.h>

#define TS_SIZE 188
#define QUEUE_LIMIT (1024 * 1024)
#define QUEUE_AGE_US 1000000

typedef struct {
    guint8 bytes[TS_SIZE];
    gint64 time;
    unsigned offset;
} Packet;

typedef struct {
    GQueue queue;
    guint8 sync[TS_SIZE * 3];
    unsigned used;
    gboolean synchronized;
    int video_pid, video_cc;
    gint64 last_video, retry_at, kill_at;
    GPid child;
    int input;
} Relay;

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping = 1; }

gboolean srt_output_validate_url(const char *url, GError **error) {
    /* GUri requires GLib 2.66; Ubuntu 20.04 ships 2.64. Keep the accepted
     * caller URL grammar explicit and use the older URI unescape API. */
    GMatchInfo *match = NULL;
    GRegex *pattern = g_regex_new("\\Asrt://([A-Za-z0-9._-]+|\\[[0-9A-Fa-f:.]+\\]):([0-9]{1,5})/?(?:\\?([^#\\s]*))?\\z", 0, 0, NULL);
    gboolean valid = url && g_regex_match(pattern, url, 0, &match);
    if (valid) {
        char *host = g_match_info_fetch(match, 1);
        char *port = g_match_info_fetch(match, 2);
        char *query = g_match_info_fetch(match, 3);
        /* Older GLib returns NULL when the trailing optional group did not
         * participate; a plain srt://host:port is the normal repeater URL. */
        if (!query) query = g_strdup("");
        long number = strtol(port, NULL, 10);
        valid = number > 0 && number <= 65535;
        if (host[0] == '[') {
            host[strlen(host)-1] = 0;
            valid &= g_hostname_is_ip_address(host+1);
        }
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char **fields = g_strsplit(query, "&", -1);
        for (unsigned i = 0; fields[i] && *query; ++i) {
            char *equals = strchr(fields[i], '=');
            if (!equals) { valid = FALSE; break; }
            *equals = 0;
            char *value = g_uri_unescape_string(equals+1, NULL);
            const char *key = fields[i];
            /* FFmpeg uses the first occurrence: reject duplicate options. */
            if (!value || g_hash_table_contains(seen, key)) valid = FALSE;
            else if (!strcmp(key, "mode")) valid &= !strcmp(value, "caller");
            else if (strcmp(key, "latency") && strcmp(key, "streamid") &&
                     strcmp(key, "passphrase") && strcmp(key, "pbkeylen")) valid = FALSE;
            g_hash_table_add(seen, g_strdup(key));
            g_free(value);
        }
        g_strfreev(fields); g_hash_table_unref(seen);
        g_free(host); g_free(port); g_free(query);
    }
    if (match) g_match_info_free(match);
    g_regex_unref(pattern);
    if (!valid) g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
        "SRT output requires srt://HOST:PORT; optional parameters: mode=caller, latency, streamid, passphrase, pbkeylen");
    return valid;
}

static void clear_queue(Relay *r) { g_queue_clear_full(&r->queue, g_free); }

static void accept_packet(Relay *r, const guint8 *p, gint64 now) {
    unsigned pid = ((p[1] & 31) << 8) | p[2];
    unsigned control = (p[3] >> 4) & 3;
    unsigned at = control & 2 ? 5u + p[4] : 4u;
    /* Only advancing, unscrambled video payload counts as reception. PSI,
     * null packets, adaptation-only packets and TEI cannot hold SRT open. */
    if (!(p[1] & 0x80) && !(p[3] & 0xc0) && (control & 1) && at < TS_SIZE && pid != 8191) {
        if ((p[1] & 0x40) && at + 9 <= TS_SIZE && !p[at] && !p[at+1] &&
            p[at+2] == 1 && (p[at+3] & 0xf0) == 0xe0 && (p[at+6] & 0xc0) == 0x80) {
            if (r->video_pid != (int)pid) r->video_cc = -1;
            r->video_pid = pid;
        }
        if (r->video_pid == (int)pid && r->video_cc != (p[3] & 15)) {
            r->video_cc = p[3] & 15;
            r->last_video = now;
        }
    }
    Packet *packet = g_new0(Packet, 1);
    memcpy(packet->bytes, p, TS_SIZE); packet->time = now;
    g_queue_push_tail(&r->queue, packet);
    /* Bound both memory and latency while FFmpeg probes/connects or the
     * network/encoder stalls. SDR reception must never wait for the server. */
    while (r->queue.length > QUEUE_LIMIT / TS_SIZE ||
           (r->queue.head && now - ((Packet *)r->queue.head->data)->time > QUEUE_AGE_US))
        g_free(g_queue_pop_head(&r->queue));
}

static void feed(Relay *r, const guint8 *data, size_t size, gint64 now) {
    for (size_t i = 0; i < size; ++i) {
        r->sync[r->used++] = data[i];
        if (r->synchronized && r->used == TS_SIZE) {
            if (r->sync[0] == 0x47) { accept_packet(r, r->sync, now); r->used = 0; }
            else r->synchronized = FALSE;
        }
        if (!r->synchronized && r->used == sizeof(r->sync)) {
            if (r->sync[0] == 0x47 && r->sync[TS_SIZE] == 0x47 && r->sync[2*TS_SIZE] == 0x47) {
                for (unsigned j = 0; j < 3; ++j) accept_packet(r, r->sync + j*TS_SIZE, now);
                r->synchronized = TRUE; r->used = 0;
            } else {
                memmove(r->sync, r->sync+1, --r->used);
            }
        }
    }
}

static void child_setup(gpointer unused) {
    (void)unused;
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    /* Inherit the supervisor's pipeline process group for forced shutdown. */
}

gboolean srt_output_valid_preset(const char *preset) {
    const char *names[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); ++i)
        if (!g_strcmp0(preset, names[i])) return TRUE;
    return FALSE;
}

static gboolean launch(Relay *r, const char *ffmpeg, const char *url, int fps, int bitrate,
                       const char *preset, int video_threads, int filter_threads, GError **error) {
    char fps_text[16], bitrate_text[32], buffer_text[32], gop[16];
    char threads_text[16], filter_threads_text[16];
    g_snprintf(threads_text, sizeof(threads_text), "%d", video_threads);
    g_snprintf(filter_threads_text, sizeof(filter_threads_text), "%d", filter_threads);
    g_snprintf(fps_text, sizeof(fps_text), "%d", fps);
    g_snprintf(bitrate_text, sizeof(bitrate_text), "%dk", bitrate);
    g_snprintf(buffer_text, sizeof(buffer_text), "%dk", bitrate * 2);
    g_snprintf(gop, sizeof(gop), "%d", fps);
    /* dar includes anamorphic SAR (e.g. 720x576); produce square pixels and
     * letter/pillarbox rather than stretching SD or 4:3 pictures. */
    char *filter = g_strdup_printf("bwdif=mode=send_frame:deint=interlaced,"
        "scale=w='trunc(min(1920,1080*dar)/2)*2':h='trunc(min(1080,1920/dar)/2)*2',"
        "setsar=1,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black,fps=%d", fps);
    char *destination = g_strdup_printf("%s%cmode=caller&transtype=live&pkt_size=1316&"
        "connect_timeout=3000&timeout=1000000&linger=0", url, strchr(url, '?') ? '&' : '?');
    char *args[] = {(char *)ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostdin",
        "-filter_threads", filter_threads_text, "-threads", threads_text,
        "-fflags", "+discardcorrupt", "-analyzeduration", "1000000", "-probesize", "1000000",
        "-f", "mpegts", "-i", "pipe:0", "-map", "0:v:0", "-map", "0:a:0?",
        "-vf", filter, "-c:v", "libx264", "-preset", (char *)preset, "-tune", "zerolatency",
        "-threads:v", threads_text,
        "-pix_fmt", "yuv420p", "-r", fps_text, "-g", gop, "-keyint_min", gop,
        "-sc_threshold", "0", "-b:v", bitrate_text, "-maxrate", bitrate_text, "-bufsize", buffer_text,
        "-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-ac", "2",
        "-sn", "-dn", "-f", "mpegts", "-mpegts_flags", "+resend_headers",
        "-muxdelay", "0", "-flush_packets", "1", destination, NULL};
    gboolean ok = g_spawn_async_with_pipes(NULL, args, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
        child_setup, NULL, &r->child, &r->input, NULL, NULL, error);
    g_free(filter); g_free(destination);
    if (ok && !g_unix_set_fd_nonblocking(r->input, TRUE, error)) {
        close(r->input); r->input = -1;
        process_stop(r->child, FALSE, TRUE); waitpid(r->child, NULL, 0); g_spawn_close_pid(r->child); r->child = 0;
        return FALSE;
    }
    return ok;
}

static void stop_child(Relay *r, gint64 now) {
    if (r->input >= 0) { close(r->input); r->input = -1; }
    if (r->child && !r->kill_at) {
        process_stop(r->child, FALSE, FALSE); r->kill_at = now + 500000;
    }
}

int srt_output_main(int argc, char **argv) {
    char *url = NULL, *ffmpeg = NULL, *preset = NULL;
    int loss_ms = 2000, retry_ms = 2000, fps = 25, bitrate = 4000, video_threads = 0, filter_threads = 0;
    GOptionEntry entries[] = {
        {"url", 0, 0, G_OPTION_ARG_STRING, &url, "SRT caller destination", "URL"},
        {"ffmpeg", 0, 0, G_OPTION_ARG_FILENAME, &ffmpeg, "FFmpeg executable", "PATH"},
        {"loss-ms", 0, 0, G_OPTION_ARG_INT, &loss_ms, "Disconnect after no video payload", "MS"},
        {"retry-ms", 0, 0, G_OPTION_ARG_INT, &retry_ms, "Retry failed output", "MS"},
        {"fps", 0, 0, G_OPTION_ARG_INT, &fps, "Full HD output frame rate", "FPS"},
        {"preset", 0, 0, G_OPTION_ARG_STRING, &preset, "x264 speed/quality preset (default veryfast)", "PRESET"},
        {"video-threads", 0, 0, G_OPTION_ARG_INT, &video_threads, "Video decoder/encoder threads; 0=auto", "N"},
        {"filter-threads", 0, 0, G_OPTION_ARG_INT, &filter_threads, "Filter threads; 0=auto", "N"},
        {"bitrate-kbps", 0, 0, G_OPTION_ARG_INT, &bitrate, "H.264 target bitrate", "KBPS"}, {NULL}
    };
    GOptionContext *context = g_option_context_new("- gated Full HD SRT output; MPEG-TS on stdin");
    g_option_context_add_main_entries(context, entries, NULL);
    GError *error = NULL; int result = 2;
    if (!g_option_context_parse(context, &argc, &argv, &error)) goto done;
    if (!preset) preset = g_strdup("veryfast");
    if (argc != 1 || !url || loss_ms < 100 || loss_ms > 60000 || retry_ms < 100 || retry_ms > 60000 ||
        fps < 1 || fps > 60 || bitrate < 100 || bitrate > 50000 || !srt_output_valid_preset(preset) ||
        video_threads < 0 || video_threads > 64 || filter_threads < 0 || filter_threads > 64) {
        g_set_error_literal(&error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Invalid SRT output arguments; see --help");
        goto done;
    }
    if (!srt_output_validate_url(url, &error)) goto done;
    if (!ffmpeg) ffmpeg = g_strdup("ffmpeg");
    char *found = g_find_program_in_path(ffmpeg);
    if (!found) {
        g_set_error(&error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "FFmpeg not found: %s", ffmpeg); goto done;
    }
    g_free(found);
    stopping = 0;
    signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal); signal(SIGPIPE, SIG_IGN);
    Relay r = {.queue = G_QUEUE_INIT, .input = -1, .video_pid = -1, .video_cc = -1};
    result = 0;
    g_printerr("[srt] Waiting for video; output 1920x1080p%d H.264/AAC, loss timeout %d ms\n", fps, loss_ms);
    while (!stopping || r.child) {
        gint64 now = g_get_monotonic_time();
        gboolean active = r.last_video && now - r.last_video < (gint64)loss_ms * 1000;
        if (!active && r.last_video) {
            g_printerr("[srt] Video lost; disconnecting and waiting for reception\n");
            r.last_video = 0; r.video_pid = r.video_cc = -1;
            clear_queue(&r); stop_child(&r, now); r.retry_at = 0;
        }
        if (stopping) stop_child(&r, now);
        if (r.child) {
            if (r.kill_at && now >= r.kill_at) process_stop(r.child, FALSE, TRUE);
            int status; GPid reaped = waitpid(r.child, &status, WNOHANG);
            if (reaped == r.child) {
                if (!r.kill_at) {
                    g_printerr("[srt] FFmpeg stopped (%s %d); retrying while video is present\n",
                        WIFEXITED(status) ? "exit" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
                    r.retry_at = now + (gint64)retry_ms * 1000;
                }
                if (r.input >= 0) close(r.input);
                r.input = -1; g_spawn_close_pid(r.child); r.child = 0; r.kill_at = 0;
                clear_queue(&r);
            }
        }
        if (stopping && !r.child) break;
        if (!stopping && active && !r.child && now >= r.retry_at) {
            g_printerr("[srt] Video present; starting Full HD encoder / SRT caller\n");
            if (!launch(&r, ffmpeg, url, fps, bitrate, preset, video_threads, filter_threads, &error)) { result = 1; break; }
        }
        struct pollfd fds[2] = {{.fd = stopping ? -1 : STDIN_FILENO, .events = POLLIN},
            {.fd = r.input, .events = g_queue_is_empty(&r.queue) ? 0 : POLLOUT}};
        int rc = poll(fds, 2, 20);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) { result = 1; stopping = 1; continue; }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            guint8 bytes[65536]; ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (n > 0) feed(&r, bytes, n, g_get_monotonic_time());
            else if (!n || (errno != EINTR && errno != EAGAIN)) {
                if (n < 0) result = 1;
                stopping = 1;
            }
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            stop_child(&r, now); r.retry_at = now + (gint64)retry_ms * 1000;
        } else if (fds[1].revents & POLLOUT) {
            /* Bounded writes per turn, preserving partial packet writes. */
            for (unsigned i = 0; i < 350 && r.queue.head; ++i) {
                Packet *p = r.queue.head->data;
                ssize_t n = write(r.input, p->bytes + p->offset, TS_SIZE - p->offset);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n <= 0) { stop_child(&r, now); r.retry_at = now + (gint64)retry_ms * 1000; break; }
                p->offset += n;
                if (p->offset == TS_SIZE) g_free(g_queue_pop_head(&r.queue));
            }
        }
    }
    clear_queue(&r);
done:
    if (error) { g_printerr("[srt] %s\n", error->message); g_clear_error(&error); }
    g_free(url); g_free(ffmpeg); g_free(preset); g_option_context_free(context);
    return result;
}
