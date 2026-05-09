#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rbdvbt_log_level_t global_log_level = RBDVBT_LOG_INFO;

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t)value;
    return 0;
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    *out = (uint64_t)value;
    return 0;
}

static int parse_symbol_rate(const char *text, rbdvbt_symbol_rate_t *out)
{
    uint32_t hz = 0;

    if (strcmp(text, "125k") == 0 || strcmp(text, "125ks") == 0) {
        *out = RBDVBT_SR_125K;
    } else if (strcmp(text, "250k") == 0 || strcmp(text, "250ks") == 0) {
        *out = RBDVBT_SR_250K;
    } else if (strcmp(text, "333k") == 0 || strcmp(text, "333ks") == 0) {
        *out = RBDVBT_SR_333K;
    } else if (strcmp(text, "500k") == 0 || strcmp(text, "500ks") == 0) {
        *out = RBDVBT_SR_500K;
    } else if (parse_u32(text, &hz) == 0) {
        if (hz == 125000u) {
            *out = RBDVBT_SR_125K;
        } else if (hz == 250000u) {
            *out = RBDVBT_SR_250K;
        } else if (hz == 333000u || hz == 333333u) {
            *out = RBDVBT_SR_333K;
        } else if (hz == 500000u) {
            *out = RBDVBT_SR_500K;
        } else {
            return -1;
        }
    } else {
        return -1;
    }

    return 0;
}

static int parse_guard_interval(const char *text, rbdvbt_guard_interval_t *out)
{
    if (strcmp(text, "auto") == 0) {
        *out = RBDVBT_GI_AUTO;
    } else if (strcmp(text, "1/8") == 0) {
        *out = RBDVBT_GI_1_8;
    } else if (strcmp(text, "1/16") == 0) {
        *out = RBDVBT_GI_1_16;
    } else if (strcmp(text, "1/32") == 0) {
        *out = RBDVBT_GI_1_32;
    } else {
        return -1;
    }

    return 0;
}

static int parse_fec(const char *text, rbdvbt_fec_t *out)
{
    if (strcmp(text, "auto") == 0) {
        *out = RBDVBT_FEC_AUTO;
    } else if (strcmp(text, "1/2") == 0 || strcmp(text, "12") == 0) {
        *out = RBDVBT_FEC_1_2;
    } else if (strcmp(text, "2/3") == 0 || strcmp(text, "23") == 0) {
        *out = RBDVBT_FEC_2_3;
    } else if (strcmp(text, "3/4") == 0 || strcmp(text, "34") == 0) {
        *out = RBDVBT_FEC_3_4;
    } else if (strcmp(text, "5/6") == 0 || strcmp(text, "56") == 0) {
        *out = RBDVBT_FEC_5_6;
    } else if (strcmp(text, "7/8") == 0 || strcmp(text, "78") == 0) {
        *out = RBDVBT_FEC_7_8;
    } else {
        return -1;
    }

    return 0;
}

static int parse_input_format(const char *text, rbdvbt_input_format_t *out)
{
    if (strcmp(text, "u8") == 0) {
        *out = RBDVBT_INPUT_U8;
    } else if (strcmp(text, "s16") == 0 || strcmp(text, "cs16") == 0) {
        *out = RBDVBT_INPUT_S16;
    } else {
        return -1;
    }

    return 0;
}

static int parse_log_level(const char *text, rbdvbt_log_level_t *out)
{
    if (strcmp(text, "quiet") == 0 || strcmp(text, "none") == 0 ||
        strcmp(text, "silent") == 0 || strcmp(text, "off") == 0) {
        *out = RBDVBT_LOG_QUIET;
    } else if (strcmp(text, "error") == 0 || strcmp(text, "err") == 0) {
        *out = RBDVBT_LOG_ERROR;
    } else if (strcmp(text, "warn") == 0 || strcmp(text, "warning") == 0) {
        *out = RBDVBT_LOG_WARN;
    } else if (strcmp(text, "info") == 0) {
        *out = RBDVBT_LOG_INFO;
    } else if (strcmp(text, "debug") == 0) {
        *out = RBDVBT_LOG_DEBUG;
    } else if (strcmp(text, "trace") == 0) {
        *out = RBDVBT_LOG_TRACE;
    } else {
        return -1;
    }

    return 0;
}

int rbdvbt_parse_args(int argc, char **argv, rbdvbt_config_t *cfg)
{
    int i;

    memset(cfg, 0, sizeof(*cfg));
    cfg->input_format = RBDVBT_INPUT_S16;
    cfg->fft_size = 2048;
    cfg->probe_symbols = 200;
    cfg->window_sweep_radius = 128;
    cfg->highres_fft_size = 32768;
    cfg->dvbt_ir = 1;
    cfg->fec = RBDVBT_FEC_1_2;
    cfg->status_period_packets = 100;
    cfg->log_level = RBDVBT_LOG_INFO;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            rbdvbt_print_usage(argv[0]);
            return -1;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            cfg->show_version = 1;
        } else if (strcmp(arg, "--stdin") == 0) {
            cfg->use_stdin = 1;
        } else if (strcmp(arg, "--probe-constellation") == 0) {
            cfg->probe_constellation = 1;
        } else if (strcmp(arg, "--live") == 0) {
            cfg->live_mode = 1;
            cfg->probe_constellation = 1;
        } else if (strcmp(arg, "--gui") == 0) {
            cfg->gui = 1;
            cfg->probe_constellation = 1;
        } else if (strcmp(arg, "--resample-x4") == 0) {
            cfg->resample_x4 = 1;
        } else if (strcmp(arg, "--resample-to-dvbt-rate") == 0) {
            cfg->resample_to_dvbt_rate = 1;
        } else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            cfg->verbose = 1;
            cfg->log_level = RBDVBT_LOG_DEBUG;
        } else if ((strcmp(arg, "--loglevel") == 0 || strcmp(arg, "--log-level") == 0) && i + 1 < argc) {
            if (parse_log_level(argv[++i], &cfg->log_level) != 0) {
                fprintf(stderr, "invalid --loglevel value; expected quiet, error, warn, info, debug, or trace\n");
                return -1;
            }
        } else if (strcmp(arg, "--sample-rate") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->sample_rate_hz) != 0) {
                fprintf(stderr, "invalid --sample-rate value\n");
                return -1;
            }
        } else if ((strcmp(arg, "--sr") == 0 || strcmp(arg, "--symbol-rate") == 0) && i + 1 < argc) {
            if (parse_symbol_rate(argv[++i], &cfg->symbol_rate) != 0) {
                fprintf(stderr, "invalid --sr value; expected 125k, 250k, 333k, 500k, or Hz 125000, 250000, 333000, 333333, 500000\n");
                return -1;
            }
        } else if (strcmp(arg, "--gi") == 0 && i + 1 < argc) {
            if (parse_guard_interval(argv[++i], &cfg->guard_interval) != 0) {
                fprintf(stderr, "invalid --gi value; expected auto, 1/8, 1/16, or 1/32\n");
                return -1;
            }
        } else if (strcmp(arg, "--fec") == 0 && i + 1 < argc) {
            if (parse_fec(argv[++i], &cfg->fec) != 0) {
                fprintf(stderr, "invalid --fec value; expected auto, 1/2, 2/3, 3/4, 5/6, or 7/8\n");
                return -1;
            }
        } else if (strcmp(arg, "--input-format") == 0 && i + 1 < argc) {
            if (parse_input_format(argv[++i], &cfg->input_format) != 0) {
                fprintf(stderr, "invalid --input-format value; expected u8 or s16\n");
                return -1;
            }
        } else if (strcmp(arg, "--max-samples") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &cfg->max_samples) != 0) {
                fprintf(stderr, "invalid --max-samples value\n");
                return -1;
            }
        } else if (strcmp(arg, "--fft-size") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->fft_size) != 0 || cfg->fft_size == 0) {
                fprintf(stderr, "invalid --fft-size value\n");
                return -1;
            }
        } else if (strcmp(arg, "--dvbt-ir") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->dvbt_ir) != 0 ||
                !(cfg->dvbt_ir == 1 || cfg->dvbt_ir == 2 || cfg->dvbt_ir == 4 || cfg->dvbt_ir == 8)) {
                fprintf(stderr, "invalid --dvbt-ir value; expected 1, 2, 4, or 8\n");
                return -1;
            }
        } else if (strcmp(arg, "--probe-symbols") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->probe_symbols) != 0 || cfg->probe_symbols == 0) {
                fprintf(stderr, "invalid --probe-symbols value\n");
                return -1;
            }
        } else if ((strcmp(arg, "--live-symbols") == 0 ||
                    strcmp(arg, "--frontend-symbols") == 0) && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->live_frontend_symbols) != 0 ||
                cfg->live_frontend_symbols == 0) {
                fprintf(stderr, "invalid --live-symbols value\n");
                return -1;
            }
        } else if (strcmp(arg, "--constellation-out") == 0 && i + 1 < argc) {
            cfg->constellation_out = argv[++i];
        } else if (strcmp(arg, "--constellation-svg") == 0 && i + 1 < argc) {
            cfg->constellation_svg = argv[++i];
        } else if (strcmp(arg, "--demap-out") == 0 && i + 1 < argc) {
            cfg->demap_out = argv[++i];
        } else if (strcmp(arg, "--viterbi-out") == 0 && i + 1 < argc) {
            cfg->viterbi_out = argv[++i];
        } else if (strcmp(arg, "--ts-out") == 0 && i + 1 < argc) {
            cfg->ts_out = argv[++i];
        } else if (strcmp(arg, "--stdout-ts") == 0) {
            cfg->ts_out = "-";
        } else if (strcmp(arg, "--wait-video-start") == 0) {
            cfg->wait_video_start = 1;
        } else if (strcmp(arg, "--status-json") == 0 && i + 1 < argc) {
            cfg->status_json = argv[++i];
        } else if (strcmp(arg, "--status-period-packets") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->status_period_packets) != 0 || cfg->status_period_packets == 0) {
                fprintf(stderr, "invalid --status-period-packets value\n");
                return -1;
            }
        } else if (strcmp(arg, "--spectrum-out") == 0 && i + 1 < argc) {
            cfg->spectrum_out = argv[++i];
        } else if (strcmp(arg, "--spectrum-svg") == 0 && i + 1 < argc) {
            cfg->spectrum_svg = argv[++i];
        } else if (strcmp(arg, "--carrier-metrics-out") == 0 && i + 1 < argc) {
            cfg->carrier_metrics_out = argv[++i];
        } else if (strcmp(arg, "--equalized-out") == 0 && i + 1 < argc) {
            cfg->equalized_out = argv[++i];
        } else if (strcmp(arg, "--equalized-svg") == 0 && i + 1 < argc) {
            cfg->equalized_svg = argv[++i];
        } else if (strcmp(arg, "--window-sweep-out") == 0 && i + 1 < argc) {
            cfg->window_sweep_out = argv[++i];
        } else if (strcmp(arg, "--window-sweep-svg") == 0 && i + 1 < argc) {
            cfg->window_sweep_svg = argv[++i];
        } else if (strcmp(arg, "--window-sweep-radius") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->window_sweep_radius) != 0) {
                fprintf(stderr, "invalid --window-sweep-radius value\n");
                return -1;
            }
        } else if (strcmp(arg, "--acquisition-scan-out") == 0 && i + 1 < argc) {
            cfg->acquisition_scan_out = argv[++i];
        } else if (strcmp(arg, "--acquisition-scan-svg") == 0 && i + 1 < argc) {
            cfg->acquisition_scan_svg = argv[++i];
        } else if (strcmp(arg, "--mapping-scan-out") == 0 && i + 1 < argc) {
            cfg->mapping_scan_out = argv[++i];
        } else if (strcmp(arg, "--mapping-scan-svg") == 0 && i + 1 < argc) {
            cfg->mapping_scan_svg = argv[++i];
        } else if (strcmp(arg, "--highres-spectrum-out") == 0 && i + 1 < argc) {
            cfg->highres_spectrum_out = argv[++i];
        } else if (strcmp(arg, "--highres-spectrum-svg") == 0 && i + 1 < argc) {
            cfg->highres_spectrum_svg = argv[++i];
        } else if (strcmp(arg, "--carrier-spacing-out") == 0 && i + 1 < argc) {
            cfg->carrier_spacing_out = argv[++i];
        } else if (strcmp(arg, "--carrier-map-out") == 0 && i + 1 < argc) {
            cfg->carrier_map_out = argv[++i];
        } else if (strcmp(arg, "--carrier-map-svg") == 0 && i + 1 < argc) {
            cfg->carrier_map_svg = argv[++i];
        } else if (strcmp(arg, "--logical-constellation-out") == 0 && i + 1 < argc) {
            cfg->logical_constellation_out = argv[++i];
        } else if (strcmp(arg, "--logical-constellation-svg") == 0 && i + 1 < argc) {
            cfg->logical_constellation_svg = argv[++i];
        } else if (strcmp(arg, "--cp-timing-out") == 0 && i + 1 < argc) {
            cfg->cp_timing_out = argv[++i];
        } else if (strcmp(arg, "--cp-timing-svg") == 0 && i + 1 < argc) {
            cfg->cp_timing_svg = argv[++i];
        } else if (strcmp(arg, "--fine-timing-out") == 0 && i + 1 < argc) {
            cfg->fine_timing_out = argv[++i];
        } else if (strcmp(arg, "--fine-timing-svg") == 0 && i + 1 < argc) {
            cfg->fine_timing_svg = argv[++i];
        } else if (strcmp(arg, "--highres-fft-size") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->highres_fft_size) != 0 || cfg->highres_fft_size == 0) {
                fprintf(stderr, "invalid --highres-fft-size value\n");
                return -1;
            }
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", arg);
            rbdvbt_print_usage(argv[0]);
            return -1;
        }
    }

    if (cfg->show_version) {
        return 0;
    }
    if (!cfg->use_stdin) {
        fprintf(stderr, "missing required --stdin\n");
        return -1;
    }
    if (cfg->sample_rate_hz == 0) {
        fprintf(stderr, "missing required --sample-rate\n");
        return -1;
    }
    if (cfg->symbol_rate == 0) {
        fprintf(stderr, "missing required --sr\n");
        return -1;
    }
    if (cfg->resample_x4 && cfg->resample_to_dvbt_rate) {
        fprintf(stderr, "--resample-x4 and --resample-to-dvbt-rate are mutually exclusive\n");
        return -1;
    }

    rbdvbt_log_set_level(cfg->log_level);
    return 0;
}

void rbdvbt_print_usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --stdin --input-format s16 --sample-rate HZ --sr 125k|250k|333k|500k|125000|250000|333000|333333|500000 --gi auto|1/8|1/16|1/32 [--fec auto|1/2|2/3|3/4|5/6|7/8 --live --probe-constellation --resample-to-dvbt-rate --dvbt-ir 1 --constellation-out qpsk.csv --demap-out dibits.csv --viterbi-out inner.bin --ts-out recovered.ts|- --wait-video-start\n"
            "       --gui --live-symbols N --loglevel quiet|error|warn|info|debug|trace --version\n"
            "       use --ts-out - or --stdout-ts to write MPEG-TS packets to stdout; use --status-json status.json for receiver status]\n",
            argv0);
}

int rbdvbt_log_enabled(rbdvbt_log_level_t level)
{
    return level <= global_log_level;
}

void rbdvbt_log_set_level(rbdvbt_log_level_t level)
{
    global_log_level = level;
}

const char *rbdvbt_log_level_name(rbdvbt_log_level_t level)
{
    switch (level) {
    case RBDVBT_LOG_QUIET:
        return "quiet";
    case RBDVBT_LOG_ERROR:
        return "error";
    case RBDVBT_LOG_WARN:
        return "warn";
    case RBDVBT_LOG_INFO:
        return "info";
    case RBDVBT_LOG_DEBUG:
        return "debug";
    case RBDVBT_LOG_TRACE:
        return "trace";
    }

    return "unknown";
}

const char *rbdvbt_input_format_name(rbdvbt_input_format_t fmt)
{
    return fmt == RBDVBT_INPUT_U8 ? "u8" : "s16";
}

const char *rbdvbt_symbol_rate_name(rbdvbt_symbol_rate_t sr)
{
    switch (sr) {
    case RBDVBT_SR_125K:
        return "125k";
    case RBDVBT_SR_250K:
        return "250k";
    case RBDVBT_SR_333K:
        return "333k";
    case RBDVBT_SR_500K:
        return "500k";
    }

    return "unknown";
}

const char *rbdvbt_guard_interval_name(rbdvbt_guard_interval_t gi)
{
    switch (gi) {
    case RBDVBT_GI_AUTO:
        return "auto";
    case RBDVBT_GI_1_8:
        return "1/8";
    case RBDVBT_GI_1_16:
        return "1/16";
    case RBDVBT_GI_1_32:
        return "1/32";
    }

    return "unknown";
}

const char *rbdvbt_fec_name(rbdvbt_fec_t fec)
{
    switch (fec) {
    case RBDVBT_FEC_AUTO:
        return "auto";
    case RBDVBT_FEC_1_2:
        return "1/2";
    case RBDVBT_FEC_2_3:
        return "2/3";
    case RBDVBT_FEC_3_4:
        return "3/4";
    case RBDVBT_FEC_5_6:
        return "5/6";
    case RBDVBT_FEC_7_8:
        return "7/8";
    }

    return "unknown";
}

double rbdvbt_guard_interval_fraction(rbdvbt_guard_interval_t gi)
{
    switch (gi) {
    case RBDVBT_GI_AUTO:
        return 0.0;
    case RBDVBT_GI_1_8:
        return 1.0 / 8.0;
    case RBDVBT_GI_1_16:
        return 1.0 / 16.0;
    case RBDVBT_GI_1_32:
        return 1.0 / 32.0;
    }

    return 0.0;
}
