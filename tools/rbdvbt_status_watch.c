#define _POSIX_C_SOURCE 199309L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop_requested;
static int terminal_screen_active;

typedef struct {
    char symbol_rate[32];
    char modulation[32];
    char fft[32];
    char constellation[32];
    char fec[32];
    char guard[32];
    char stage[32];
    char service_name[64];
    char service_provider[64];
    unsigned locked;
    unsigned status_update;
    unsigned updated_unix;
    unsigned input_samples;
    unsigned lock_quality;
    double pilot_lock;
    unsigned ssi;
    unsigned sqi;
    double snr;
    unsigned afc_enabled;
    unsigned afc_advised;
    int afc_delta_bins;
    unsigned afc_trend_count;
    unsigned pe;
    unsigned service_id;
    unsigned program_id;
    unsigned packets;
    unsigned written_packets;
    unsigned sync_bad;
    unsigned transport_errors;
    unsigned cc_errors;
    unsigned pat_packets;
    unsigned pmt_packets;
    unsigned sdt_packets;
    unsigned pmt_pid;
    unsigned pcr_pid;
    unsigned video_pid;
    unsigned audio_pid;
    unsigned rs_bad;
    unsigned rs_corrected;
    unsigned rs_corrected_bytes;
    unsigned rs_uncorrectable;
    unsigned lamp_symbol_rate;
    unsigned lamp_guard;
    unsigned lamp_fec;
    unsigned lamp_ofdm_sync;
    unsigned lamp_inner_fec;
    unsigned lamp_rs_lock;
    unsigned lamp_rs_clean;
    unsigned lamp_ts_sync;
    unsigned lamp_pat;
    unsigned lamp_pmt;
    unsigned lamp_sdt;
    unsigned lamp_service;
    unsigned lamp_av;
    unsigned lamp_output;
    int have_snr;
    int have_pilot_lock;
} status_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s STATUS.json [--interval-ms N] [--once] [--no-clear]\n",
            argv0);
}

static void handle_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static void terminal_enter_status_screen(void)
{
    if (terminal_screen_active) {
        return;
    }

    printf("\033[?1049h\033[?25l\033[2J\033[H");
    fflush(stdout);
    terminal_screen_active = 1;
}

static void terminal_leave_status_screen(void)
{
    if (!terminal_screen_active) {
        return;
    }

    printf("\033[0m\033[?25h\033[?1049l");
    fflush(stdout);
    terminal_screen_active = 0;
}

static int parse_unsigned_arg(const char *text, unsigned *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 1000000ul) {
        return -1;
    }
    *out = (unsigned)value;
    return 0;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    size_t got;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = calloc((size_t)size + 1u, 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1u, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static const char *find_key_value(const char *json, const char *key)
{
    char pattern[96];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (p == NULL) {
        return NULL;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return NULL;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static void json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *p = find_key_value(json, key);
    size_t n = 0;

    if (out_len == 0u) {
        return;
    }
    out[0] = '\0';
    if (p == NULL || *p != '"') {
        return;
    }
    p++;
    while (*p != '\0' && *p != '"' && n + 1u < out_len) {
        out[n++] = *p++;
    }
    out[n] = '\0';
}

static unsigned json_get_uint(const char *json, const char *key)
{
    const char *p = find_key_value(json, key);
    unsigned value = 0;

    if (p != NULL) {
        (void)sscanf(p, "%u", &value);
    }
    return value;
}

static int json_get_int(const char *json, const char *key)
{
    const char *p = find_key_value(json, key);
    int value = 0;

    if (p != NULL) {
        (void)sscanf(p, "%d", &value);
    }
    return value;
}

static double json_get_double(const char *json, const char *key, int *present)
{
    const char *p = find_key_value(json, key);
    double value = 0.0;

    *present = 0;
    if (p == NULL || strncmp(p, "null", 4u) == 0) {
        return 0.0;
    }
    if (sscanf(p, "%lf", &value) == 1) {
        *present = 1;
    }
    return value;
}

static unsigned json_get_bool(const char *json, const char *key)
{
    const char *p = find_key_value(json, key);

    return p != NULL && strncmp(p, "true", 4u) == 0 ? 1u : 0u;
}

static void status_from_json(const char *json, status_t *s)
{
    memset(s, 0, sizeof(*s));
    json_get_string(json, "symbol_rate", s->symbol_rate, sizeof(s->symbol_rate));
    json_get_string(json, "modulation", s->modulation, sizeof(s->modulation));
    json_get_string(json, "fft", s->fft, sizeof(s->fft));
    json_get_string(json, "constellation", s->constellation, sizeof(s->constellation));
    json_get_string(json, "fec", s->fec, sizeof(s->fec));
    json_get_string(json, "guard", s->guard, sizeof(s->guard));
    json_get_string(json, "stage", s->stage, sizeof(s->stage));
    json_get_string(json, "service_name", s->service_name, sizeof(s->service_name));
    json_get_string(json, "service_provider", s->service_provider, sizeof(s->service_provider));
    s->locked = strstr(json, "\"locked\": true") != NULL ? 1u : 0u;
    s->status_update = json_get_uint(json, "status_update");
    s->updated_unix = json_get_uint(json, "updated_unix");
    s->input_samples = json_get_uint(json, "input_samples");
    s->lock_quality = json_get_uint(json, "lock_quality");
    s->pilot_lock = json_get_double(json, "pilot_lock", &s->have_pilot_lock);
    s->ssi = json_get_uint(json, "ssi");
    s->sqi = json_get_uint(json, "sqi");
    s->snr = json_get_double(json, "snr", &s->have_snr);
    s->afc_enabled = json_get_bool(json, "afc_enabled");
    s->afc_advised = json_get_bool(json, "afc_advised");
    s->afc_delta_bins = json_get_int(json, "afc_delta_bins");
    s->afc_trend_count = json_get_uint(json, "afc_trend_count");
    s->pe = json_get_uint(json, "pe");
    s->service_id = json_get_uint(json, "service_id");
    s->program_id = json_get_uint(json, "program_id");
    s->packets = json_get_uint(json, "packets");
    s->written_packets = json_get_uint(json, "written_packets");
    s->sync_bad = json_get_uint(json, "sync_bad");
    s->transport_errors = json_get_uint(json, "transport_errors");
    s->cc_errors = json_get_uint(json, "cc_errors");
    s->pat_packets = json_get_uint(json, "pat_packets");
    s->pmt_packets = json_get_uint(json, "pmt_packets");
    s->sdt_packets = json_get_uint(json, "sdt_packets");
    s->pmt_pid = json_get_uint(json, "pmt_pid");
    s->pcr_pid = json_get_uint(json, "pcr_pid");
    s->video_pid = json_get_uint(json, "video_pid");
    s->audio_pid = json_get_uint(json, "audio_pid");
    s->rs_bad = json_get_uint(json, "rs_bad");
    s->rs_corrected = json_get_uint(json, "rs_corrected");
    s->rs_corrected_bytes = json_get_uint(json, "rs_corrected_bytes");
    s->rs_uncorrectable = json_get_uint(json, "rs_uncorrectable");
    s->lamp_symbol_rate = json_get_bool(json, "lamp_symbol_rate");
    s->lamp_guard = json_get_bool(json, "lamp_guard");
    s->lamp_fec = json_get_bool(json, "lamp_fec");
    s->lamp_ofdm_sync = json_get_bool(json, "lamp_ofdm_sync");
    s->lamp_inner_fec = json_get_bool(json, "lamp_inner_fec");
    s->lamp_rs_lock = json_get_bool(json, "lamp_rs_lock");
    s->lamp_rs_clean = json_get_bool(json, "lamp_rs_clean");
    s->lamp_ts_sync = json_get_bool(json, "lamp_ts_sync");
    s->lamp_pat = json_get_bool(json, "lamp_pat");
    s->lamp_pmt = json_get_bool(json, "lamp_pmt");
    s->lamp_sdt = json_get_bool(json, "lamp_sdt");
    s->lamp_service = json_get_bool(json, "lamp_service");
    s->lamp_av = json_get_bool(json, "lamp_av");
    s->lamp_output = json_get_bool(json, "lamp_output");
}

static const char *text_or_dash(const char *text)
{
    return text[0] != '\0' ? text : "-";
}

static void bar(char *out, size_t out_len, unsigned value)
{
    const unsigned width = 20;
    unsigned fill = value > 100u ? width : (value * width + 50u) / 100u;
    size_t pos = 0;

    if (out_len == 0u) {
        return;
    }
    if (pos + 1u < out_len) {
        out[pos++] = '[';
    }
    for (unsigned i = 0; i < width && pos + 1u < out_len; ++i) {
        out[pos++] = i < fill ? '#' : '.';
    }
    if (pos + 1u < out_len) {
        out[pos++] = ']';
    }
    out[pos] = '\0';
}

static void lamp(const char *label, unsigned on)
{
    printf("  %s%-13s\033[0m", on ? "\033[32m[OK] " : "\033[31m[--] ", label);
}

static void print_status(const char *path, const status_t *s, int clear_screen)
{
    char ssi_bar[32];
    char sqi_bar[32];
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char stamp[32] = "-";
    char updated_stamp[32] = "-";

    if (tm_now != NULL) {
        strftime(stamp, sizeof(stamp), "%H:%M:%S", tm_now);
    }
    if (s->updated_unix != 0u) {
        time_t updated = (time_t)s->updated_unix;
        struct tm *tm_updated = localtime(&updated);

        if (tm_updated != NULL) {
            strftime(updated_stamp, sizeof(updated_stamp), "%H:%M:%S", tm_updated);
        }
    }

    bar(ssi_bar, sizeof(ssi_bar), s->ssi);
    bar(sqi_bar, sizeof(sqi_bar), s->sqi);

    if (clear_screen) {
        printf("\033[H\033[2J");
    }

    printf("DVB-T receiver status  %s\n", stamp);
    printf("source: %s\n\n", path);
    printf("JSON\n");
    printf("  update: %-7u file time: %s  stage: %-8s input_samples: %u\n\n",
           s->status_update,
           updated_stamp,
           text_or_dash(s->stage),
           s->input_samples);

    printf("Pipeline lamps\n");
    lamp("symbol-rate", s->lamp_symbol_rate);
    lamp("guard", s->lamp_guard);
    lamp("FEC", s->lamp_fec);
    lamp("OFDM/pilot", s->lamp_ofdm_sync);
    printf("\n");
    lamp("inner-FEC", s->lamp_inner_fec);
    lamp("RS lock", s->lamp_rs_lock);
    lamp("RS clean", s->lamp_rs_clean);
    lamp("TS sync", s->lamp_ts_sync);
    printf("\n");
    lamp("PAT", s->lamp_pat);
    lamp("PMT", s->lamp_pmt);
    lamp("SDT", s->lamp_sdt);
    lamp("service", s->lamp_service);
    printf("\n");
    lamp("A/V PID", s->lamp_av);
    lamp("output", s->lamp_output);
    printf("\n\n");

    printf("Signal\n");
    printf("  lock: %-3s  quality: %3u%%",
           s->locked ? "yes" : "no",
           s->lock_quality);
    if (s->have_pilot_lock) {
        printf("  pilot_lock: %.5f\n", s->pilot_lock);
    } else {
        printf("  pilot_lock: -\n");
    }
    printf("  modulation: %-8s  fft: %-3s  constellation: %-5s\n",
           text_or_dash(s->modulation),
           text_or_dash(s->fft),
           text_or_dash(s->constellation));
    printf("  symbol_rate: %-6s  FEC: %-3s  guard: %-5s\n",
           text_or_dash(s->symbol_rate),
           text_or_dash(s->fec),
           text_or_dash(s->guard));
    if (s->have_snr) {
        printf("  SNR: %6.2f dB\n", s->snr);
    } else {
        printf("  SNR:      - dB\n");
    }
    printf("  SSI: %3u %s\n", s->ssi, ssi_bar);
    printf("  SQI: %3u %s\n", s->sqi, sqi_bar);
    printf("  AFC: %-3s advised: %-3s delta_bins: %+d trend: %u\n",
           s->afc_enabled ? "on" : "off",
           s->afc_advised ? "yes" : "no",
           s->afc_delta_bins,
           s->afc_trend_count);
    printf("  PE:  %u\n\n", s->pe);

    printf("Transport stream\n");
    printf("  packets: %-7u written: %-7u sync_bad: %-3u TEI: %-3u CC errors: %u\n",
           s->packets,
           s->written_packets,
           s->sync_bad,
           s->transport_errors,
           s->cc_errors);
    printf("  PAT packets: %-4u PMT packets: %-4u SDT packets: %-4u PMT PID: 0x%04x PCR PID: 0x%04x\n",
           s->pat_packets,
           s->pmt_packets,
           s->sdt_packets,
           s->pmt_pid,
           s->pcr_pid);
    printf("  service_id: %-5u program_id: %-5u video PID: 0x%04x audio PID: 0x%04x\n\n",
           s->service_id,
           s->program_id,
           s->video_pid,
           s->audio_pid);
    printf("  service_name: %-24s provider: %s\n\n",
           text_or_dash(s->service_name),
           text_or_dash(s->service_provider));

    printf("Reed-Solomon\n");
    printf("  corrected codewords: %-5u corrected bytes: %-5u bad: %-3u uncorrectable: %u\n",
           s->rs_corrected,
           s->rs_corrected_bytes,
           s->rs_bad,
           s->rs_uncorrectable);
    fflush(stdout);
}

static void sleep_ms(unsigned interval_ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(interval_ms / 1000u);
    ts.tv_nsec = (long)(interval_ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    unsigned interval_ms = 500;
    int once = 0;
    int clear_screen = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            if (parse_unsigned_arg(argv[++i], &interval_ms) != 0 || interval_ms == 0u) {
                fprintf(stderr, "invalid --interval-ms value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--no-clear") == 0) {
            clear_screen = 0;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (path == NULL) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (clear_screen && !once) {
        terminal_enter_status_screen();
    }

    while (!stop_requested) {
        char *json = read_file(path);

        if (json == NULL || json[0] == '\0') {
            if (clear_screen) {
                printf("\033[H\033[2J");
            }
            printf("Waiting for status file: %s\n", path);
            printf("\nPress Ctrl-C to exit.\n");
            fflush(stdout);
            free(json);
        } else {
            status_t status;

            status_from_json(json, &status);
            print_status(path, &status, clear_screen);
            free(json);
        }

        if (once) {
            break;
        }
        sleep_ms(interval_ms);
    }

    terminal_leave_status_screen();
    return 0;
}
