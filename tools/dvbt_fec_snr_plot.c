#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKET_BITS (188u * 8u)
#define STATE_COUNT 128u
#define QPSK_AMP 0.7071067811865476
#define RBDVBT_PI 3.14159265358979323846

typedef struct {
    double channel_snr_db;
    double esn0_db;
    double ebn0_db;
    double noise_bandwidth_hz;
    double guard_fraction;
    uint32_t symbol_rate_hz;
    char guard[8];
    char symbol_rate[8];
    char fec[8];
    uint32_t offered_packets;
    uint32_t correct_packets;
    uint32_t missed_or_bad_packets;
    double packet_error_percent;
} result_t;

typedef struct {
    const char *csv_path;
    const char *svg_path;
    uint32_t packets;
    double snr_min;
    double snr_max;
    double snr_step;
    double noise_bandwidth_hz;
    double guard_fraction;
    const char *guard_name;
    uint64_t seed;
} config_t;

typedef struct {
    uint64_t state;
    int have_spare;
    double spare;
} rng_t;

static uint8_t pred0[STATE_COUNT];
static uint8_t pred1[STATE_COUNT];
static uint8_t exp_x[STATE_COUNT];
static uint8_t exp_y[STATE_COUNT];

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--csv plots/fec_packet_error_vs_snr.csv]\n"
            "       [--svg plots/fec_packet_error_vs_snr.svg]\n"
            "       [--packets N] [--snr-min DB] [--snr-max DB] [--snr-step DB]\n"
            "       [--noise-bandwidth HZ]\n"
            "       [--seed N]\n",
            argv0);
}

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 100000000ul) {
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

static int parse_double(const char *text, double *out)
{
    char *end = NULL;
    double value;

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value)) {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_guard(const char *text, double *guard_fraction, const char **guard_name)
{
    if (strcmp(text, "1/32") == 0 || strcmp(text, "32") == 0) {
        *guard_fraction = 1.0 / 32.0;
        *guard_name = "1/32";
    } else if (strcmp(text, "1/16") == 0 || strcmp(text, "16") == 0) {
        *guard_fraction = 1.0 / 16.0;
        *guard_name = "1/16";
    } else if (strcmp(text, "1/8") == 0 || strcmp(text, "8") == 0) {
        *guard_fraction = 1.0 / 8.0;
        *guard_name = "1/8";
    } else if (strcmp(text, "1/4") == 0 || strcmp(text, "4") == 0) {
        *guard_fraction = 1.0 / 4.0;
        *guard_name = "1/4";
    } else {
        return -1;
    }
    return 0;
}

static int parse_args(int argc, char **argv, config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->csv_path = "plots/fec_packet_error_vs_snr.csv";
    cfg->svg_path = "plots/fec_packet_error_vs_snr_by_sr.svg";
    cfg->packets = 240u;
    cfg->snr_min = 0.0;
    cfg->snr_max = 8.0;
    cfg->snr_step = 0.2;
    cfg->noise_bandwidth_hz = 333000.0;
    cfg->guard_fraction = 1.0 / 32.0;
    cfg->guard_name = "1/32";
    cfg->seed = 20260508ull;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            usage(argv[0]);
            return -1;
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            cfg->csv_path = argv[++i];
        } else if (strcmp(argv[i], "--svg") == 0 && i + 1 < argc) {
            cfg->svg_path = argv[++i];
        } else if (strcmp(argv[i], "--packets") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->packets) != 0 || cfg->packets == 0u) {
                fprintf(stderr, "invalid --packets value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--snr-min") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->snr_min) != 0) {
                fprintf(stderr, "invalid --snr-min value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--snr-max") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->snr_max) != 0) {
                fprintf(stderr, "invalid --snr-max value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--snr-step") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->snr_step) != 0 || cfg->snr_step <= 0.0) {
                fprintf(stderr, "invalid --snr-step value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--noise-bandwidth") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->noise_bandwidth_hz) != 0 ||
                cfg->noise_bandwidth_hz <= 0.0) {
                fprintf(stderr, "invalid --noise-bandwidth value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--guard") == 0 && i + 1 < argc) {
            if (parse_guard(argv[++i], &cfg->guard_fraction, &cfg->guard_name) != 0) {
                fprintf(stderr, "invalid --guard value; expected 1/4, 1/8, 1/16, or 1/32\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &cfg->seed) != 0) {
                fprintf(stderr, "invalid --seed value\n");
                return -1;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->snr_max < cfg->snr_min) {
        fprintf(stderr, "--snr-max must be >= --snr-min\n");
        return -1;
    }
    return 0;
}

static uint64_t rng_next_u64(rng_t *rng)
{
    uint64_t x = rng->state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->state = x;
    return x * 2685821657736338717ull;
}

static double rng_uniform_open(rng_t *rng)
{
    uint64_t x = rng_next_u64(rng);

    return ((double)(x >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

static double rng_normal(rng_t *rng)
{
    double u1;
    double u2;
    double r;
    double theta;

    if (rng->have_spare) {
        rng->have_spare = 0;
        return rng->spare;
    }

    u1 = rng_uniform_open(rng);
    u2 = rng_uniform_open(rng);
    r = sqrt(-2.0 * log(u1));
    theta = 2.0 * RBDVBT_PI * u2;
    rng->spare = r * sin(theta);
    rng->have_spare = 1;
    return r * cos(theta);
}

static uint8_t conv_parity_for_state(uint8_t state)
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

static void init_trellis(void)
{
    for (uint32_t ns = 0; ns < STATE_COUNT; ++ns) {
        uint8_t parity = conv_parity_for_state((uint8_t)ns);

        pred0[ns] = (uint8_t)(((ns & 0x3fu) << 1) | 0u);
        pred1[ns] = (uint8_t)(((ns & 0x3fu) << 1) | 1u);
        exp_x[ns] = (uint8_t)(parity & 1u);
        exp_y[ns] = (uint8_t)((parity >> 1) & 1u);
    }
}

static void generate_bits(rng_t *rng, uint8_t *bits, size_t bit_count)
{
    uint64_t bucket = 0;
    unsigned left = 0;

    for (size_t i = 0; i < bit_count; ++i) {
        if (left == 0u) {
            bucket = rng_next_u64(rng);
            left = 64u;
        }
        bits[i] = (uint8_t)(bucket & 1u);
        bucket >>= 1;
        left--;
    }
}

static void encode_mother(const uint8_t *bits, size_t bit_count, uint8_t *x, uint8_t *y)
{
    uint8_t state = 0;

    for (size_t i = 0; i < bit_count; ++i) {
        uint8_t parity;

        state = (uint8_t)(((uint8_t)state >> 1) | (bits[i] ? 0x40u : 0u));
        parity = conv_parity_for_state(state);
        x[i] = (uint8_t)(parity & 1u);
        y[i] = (uint8_t)((parity >> 1) & 1u);
    }
}

static int fec_has_x(const char *fec, size_t bit_index)
{
    if (strcmp(fec, "2/3") == 0) {
        size_t m = bit_index & 3u;

        return !(m == 1u || m == 3u);
    }
    if (strcmp(fec, "3/4") == 0) {
        return (bit_index % 3u) != 1u;
    }
    return 1;
}

static int fec_has_y(const char *fec, size_t bit_index)
{
    if (strcmp(fec, "3/4") == 0) {
        return (bit_index % 3u) != 2u;
    }
    return 1;
}

static void viterbi_decode_awgn(const char *fec,
                                const uint8_t *x,
                                const uint8_t *y,
                                size_t bit_count,
                                double esn0_db,
                                uint64_t seed,
                                uint8_t *decoded)
{
    const double inf = 1.0e90;
    double metrics[STATE_COUNT];
    double next_metrics[STATE_COUNT];
    uint8_t *prev_states = malloc(bit_count * STATE_COUNT);
    rng_t rng;
    double esn0_linear = pow(10.0, esn0_db / 10.0);
    double sigma = sqrt(1.0 / (2.0 * esn0_linear));
    int best_state = 0;
    double best_metric = inf;

    if (prev_states == NULL) {
        fprintf(stderr, "failed to allocate Viterbi traceback\n");
        exit(1);
    }

    rng.state = seed != 0u ? seed : 1u;
    rng.have_spare = 0;
    rng.spare = 0.0;

    for (uint32_t s = 0; s < STATE_COUNT; ++s) {
        metrics[s] = 0.0;
    }

    for (size_t t = 0; t < bit_count; ++t) {
        int has_x = fec_has_x(fec, t);
        int has_y = fec_has_y(fec, t);
        double rx_x = 0.0;
        double rx_y = 0.0;
        double x_cost[2] = {0.0, 0.0};
        double y_cost[2] = {0.0, 0.0};

        if (has_x) {
            double tx_x = x[t] == 0u ? QPSK_AMP : -QPSK_AMP;

            rx_x = tx_x + sigma * rng_normal(&rng);
            x_cost[0] = (rx_x - QPSK_AMP) * (rx_x - QPSK_AMP);
            x_cost[1] = (rx_x + QPSK_AMP) * (rx_x + QPSK_AMP);
        }

        if (has_y) {
            double tx_y = y[t] == 0u ? QPSK_AMP : -QPSK_AMP;

            rx_y = tx_y + sigma * rng_normal(&rng);
            y_cost[0] = (rx_y - QPSK_AMP) * (rx_y - QPSK_AMP);
            y_cost[1] = (rx_y + QPSK_AMP) * (rx_y + QPSK_AMP);
        }

        for (uint32_t ns = 0; ns < STATE_COUNT; ++ns) {
            double branch = 0.0;
            double m0 = metrics[pred0[ns]];
            double m1 = metrics[pred1[ns]];
            uint8_t prev = pred0[ns];

            if (has_x) {
                branch += x_cost[exp_x[ns]];
            }
            if (has_y) {
                branch += y_cost[exp_y[ns]];
            }
            if (m1 < m0) {
                m0 = m1;
                prev = pred1[ns];
            }
            next_metrics[ns] = m0 + branch;
            prev_states[t * STATE_COUNT + ns] = prev;
        }

        memcpy(metrics, next_metrics, sizeof(metrics));
    }

    for (uint32_t s = 0; s < STATE_COUNT; ++s) {
        if (metrics[s] < best_metric) {
            best_metric = metrics[s];
            best_state = (int)s;
        }
    }

    for (size_t t = bit_count; t > 0u; --t) {
        decoded[t - 1u] = (uint8_t)((best_state >> 6) & 1u);
        best_state = prev_states[(t - 1u) * STATE_COUNT + (size_t)best_state];
    }

    free(prev_states);
}

static result_t simulate_one(const char *symbol_rate,
                             uint32_t symbol_rate_hz,
                             const char *fec,
                             double channel_snr_db,
                             double noise_bandwidth_hz,
                             double guard_fraction,
                             const char *guard_name,
                             uint32_t packets,
                             uint64_t seed)
{
    size_t bit_count = (size_t)packets * PACKET_BITS;
    uint8_t *bits = malloc(bit_count);
    uint8_t *x = malloc(bit_count);
    uint8_t *y = malloc(bit_count);
    uint8_t *decoded = malloc(bit_count);
    rng_t rng;
    result_t r;
    uint32_t correct = 0;
    double guard_penalty_db = 10.0 * log10(1.0 + guard_fraction);
    double esn0_db = channel_snr_db +
        10.0 * log10(noise_bandwidth_hz / (double)symbol_rate_hz) -
        guard_penalty_db;
    double ebn0_db = esn0_db - 10.0 * log10(2.0);

    if (bits == NULL || x == NULL || y == NULL || decoded == NULL) {
        fprintf(stderr, "failed to allocate simulation buffers\n");
        exit(1);
    }

    rng.state = seed != 0u ? seed : 1u;
    rng.have_spare = 0;
    rng.spare = 0.0;
    generate_bits(&rng, bits, bit_count);
    encode_mother(bits, bit_count, x, y);
    viterbi_decode_awgn(fec, x, y, bit_count, esn0_db, seed ^ 0x9e3779b97f4a7c15ull, decoded);

    for (uint32_t p = 0; p < packets; ++p) {
        size_t off = (size_t)p * PACKET_BITS;
        int ok = 1;

        for (uint32_t b = 0; b < PACKET_BITS; ++b) {
            if (decoded[off + b] != bits[off + b]) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            correct++;
        }
    }

    memset(&r, 0, sizeof(r));
    r.channel_snr_db = channel_snr_db;
    r.esn0_db = esn0_db;
    r.ebn0_db = ebn0_db;
    r.noise_bandwidth_hz = noise_bandwidth_hz;
    r.guard_fraction = guard_fraction;
    r.symbol_rate_hz = symbol_rate_hz;
    snprintf(r.guard, sizeof(r.guard), "%s", guard_name);
    snprintf(r.symbol_rate, sizeof(r.symbol_rate), "%s", symbol_rate);
    snprintf(r.fec, sizeof(r.fec), "%s", fec);
    r.offered_packets = packets;
    r.correct_packets = correct;
    r.missed_or_bad_packets = packets - correct;
    r.packet_error_percent = 100.0 * (double)r.missed_or_bad_packets / (double)packets;

    free(bits);
    free(x);
    free(y);
    free(decoded);
    return r;
}

static int write_csv(const char *path, const result_t *rows, size_t count)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        fprintf(stderr, "failed to open CSV output: %s\n", path);
        return -1;
    }
    fprintf(f, "channel_snr_db,noise_bandwidth_hz,guard,guard_fraction,symbol_rate,symbol_rate_hz,esn0_db,ebn0_db,fec,offered_packets,correct_packets,missed_or_bad_packets,packet_error_percent\n");
    for (size_t i = 0; i < count; ++i) {
        fprintf(f,
                "%.2f,%.0f,%s,%.6f,%s,%u,%.2f,%.2f,%s,%u,%u,%u,%.2f\n",
                rows[i].channel_snr_db,
                rows[i].noise_bandwidth_hz,
                rows[i].guard,
                rows[i].guard_fraction,
                rows[i].symbol_rate,
                rows[i].symbol_rate_hz,
                rows[i].esn0_db,
                rows[i].ebn0_db,
                rows[i].fec,
                rows[i].offered_packets,
                rows[i].correct_packets,
                rows[i].missed_or_bad_packets,
                rows[i].packet_error_percent);
    }
    fclose(f);
    return 0;
}

static double sx(double snr, double snr_min, double snr_max, double left, double plot_w)
{
    return left + (snr - snr_min) * plot_w / (snr_max - snr_min);
}

static double sy(double err, double top, double plot_h)
{
    return top + (100.0 - err) * plot_h / 100.0;
}

static int write_svg(const char *path,
                     const result_t *rows,
                     size_t count,
                     double snr_min,
                     double snr_max,
                     const char *guard_name)
{
    const double width = 980.0;
    const double height = 620.0;
    const double left = 82.0;
    const double right = 34.0;
    const double top = 50.0;
    const double bottom = 78.0;
    const double plot_w = width - left - right;
    const double plot_h = height - top - bottom;
    FILE *f = fopen(path, "w");
    const char *symbol_rates[] = {"333k", "250k", "150k"};
    const char *fecs[] = {"1/2", "2/3", "3/4"};
    const char *colors[] = {"#1769aa", "#c2410c", "#2f855a"};

    if (f == NULL) {
        fprintf(stderr, "failed to open SVG output: %s\n", path);
        return -1;
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"0 0 %.0f %.0f\">\n", width, height, width, height);
    fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
    fprintf(f, "<text x=\"%.0f\" y=\"25\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"18\" font-weight=\"700\">DVB-T QPSK packet error vs fixed-bandwidth C/N, GI %s</text>\n", width / 2.0, guard_name);
    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"%.0f\" fill=\"#fbfbfb\" stroke=\"#222\" stroke-width=\"1\"/>\n", left, top, plot_w, plot_h);

    for (int ytick = 0; ytick <= 100; ytick += 10) {
        double y = sy((double)ytick, top, plot_h);

        fprintf(f, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#e1e1e1\" stroke-width=\"1\"/>\n", left, y, left + plot_w, y);
        fprintf(f, "<text x=\"%.2f\" y=\"%.2f\" text-anchor=\"end\" font-family=\"sans-serif\" font-size=\"12\">%d%%</text>\n", left - 10.0, y + 4.0, ytick);
    }
    for (int xtick = (int)ceil(snr_min); xtick <= (int)floor(snr_max); xtick += 2) {
        double x = sx((double)xtick, snr_min, snr_max, left, plot_w);

        fprintf(f, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#ececec\" stroke-width=\"1\"/>\n", x, top, x, top + plot_h);
        fprintf(f, "<text x=\"%.2f\" y=\"%.2f\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"12\">%d</text>\n", x, top + plot_h + 24.0, xtick);
    }

    fprintf(f, "<text x=\"%.0f\" y=\"%.0f\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"14\">channel C/N in fixed noise bandwidth (dB)</text>\n", left + plot_w / 2.0, height - 24.0);
    fprintf(f, "<text x=\"23\" y=\"%.0f\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"14\" transform=\"rotate(-90 23 %.0f)\">packet error percentage</text>\n", top + plot_h / 2.0, top + plot_h / 2.0);

    for (size_t sr_i = 0; sr_i < 3u; ++sr_i) {
        for (size_t fec_i = 0; fec_i < 3u; ++fec_i) {
            const char *dash = "";

            if (sr_i == 1u) {
                dash = " stroke-dasharray=\"7 5\"";
            } else if (sr_i == 2u) {
                dash = " stroke-dasharray=\"2 5\"";
            }

            fprintf(f, "<polyline points=\"");
            for (size_t i = 0; i < count; ++i) {
                if (strcmp(rows[i].symbol_rate, symbol_rates[sr_i]) == 0 &&
                    strcmp(rows[i].fec, fecs[fec_i]) == 0) {
                    fprintf(f,
                            "%.2f,%.2f ",
                            sx(rows[i].channel_snr_db, snr_min, snr_max, left, plot_w),
                            sy(rows[i].packet_error_percent, top, plot_h));
                }
            }
            fprintf(f, "\" fill=\"none\" stroke=\"%s\" stroke-width=\"3\"%s/>\n", colors[fec_i], dash);
            for (size_t i = 0; i < count; ++i) {
                if (strcmp(rows[i].symbol_rate, symbol_rates[sr_i]) == 0 &&
                    strcmp(rows[i].fec, fecs[fec_i]) == 0) {
                    fprintf(f,
                            "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"3.2\" fill=\"%s\" fill-opacity=\"%.2f\"/>\n",
                            sx(rows[i].channel_snr_db, snr_min, snr_max, left, plot_w),
                            sy(rows[i].packet_error_percent, top, plot_h),
                            colors[fec_i],
                            sr_i == 0u ? 0.9 : 0.55);
                }
            }
        }
    }

    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"214\" height=\"190\" fill=\"#fff\" stroke=\"#ccc\"/>\n", left + plot_w - 226.0, top + 4.0);
    for (size_t fec_i = 0; fec_i < 3u; ++fec_i) {
        double y = top + 22.0 + (double)fec_i * 25.0;

        fprintf(f, "<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"%s\" stroke-width=\"3\"/><text x=\"%.0f\" y=\"%.0f\" font-family=\"sans-serif\" font-size=\"13\">FEC %s</text>\n", left + plot_w - 214.0, y, left + plot_w - 182.0, y, colors[fec_i], left + plot_w - 172.0, y + 4.0, fecs[fec_i]);
    }
    fprintf(f, "<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"#333\" stroke-width=\"3\"/><text x=\"%.0f\" y=\"%.0f\" font-family=\"sans-serif\" font-size=\"13\">SR 333k</text>\n", left + plot_w - 214.0, top + 104.0, left + plot_w - 182.0, top + 104.0, left + plot_w - 172.0, top + 108.0);
    fprintf(f, "<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"#333\" stroke-width=\"3\" stroke-dasharray=\"7 5\"/><text x=\"%.0f\" y=\"%.0f\" font-family=\"sans-serif\" font-size=\"13\">SR 250k</text>\n", left + plot_w - 214.0, top + 129.0, left + plot_w - 182.0, top + 129.0, left + plot_w - 172.0, top + 133.0);
    fprintf(f, "<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"#333\" stroke-width=\"3\" stroke-dasharray=\"2 5\"/><text x=\"%.0f\" y=\"%.0f\" font-family=\"sans-serif\" font-size=\"13\">SR 150k</text>\n", left + plot_w - 214.0, top + 154.0, left + plot_w - 182.0, top + 154.0, left + plot_w - 172.0, top + 158.0);
    fprintf(f, "<text x=\"82\" y=\"596\" font-family=\"sans-serif\" font-size=\"11\" fill=\"#555\">Constant transmit power and N0. Es/N0 = C/N + 10log10(B/Rs) - 10log10(1+GI); CSV includes Es/N0 and Eb/N0.</text>\n");
    fprintf(f, "</svg>\n");
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    config_t cfg;
    result_t *rows;
    size_t snr_count;
    size_t row_count = 0;
    const char *symbol_rates[] = {"333k", "250k", "150k"};
    const uint32_t symbol_rate_hz[] = {333000u, 250000u, 150000u};
    const char *fecs[] = {"1/2", "2/3", "3/4"};

    if (parse_args(argc, argv, &cfg) != 0) {
        return 2;
    }

    init_trellis();
    snr_count = (size_t)floor((cfg.snr_max - cfg.snr_min) / cfg.snr_step + 1.0000001);
    rows = calloc(snr_count * 9u, sizeof(*rows));
    if (rows == NULL) {
        fprintf(stderr, "failed to allocate results\n");
        return 1;
    }

    for (size_t sr_i = 0; sr_i < 3u; ++sr_i) {
        for (size_t fec_i = 0; fec_i < 3u; ++fec_i) {
            for (size_t s = 0; s < snr_count; ++s) {
                double channel_snr_db = cfg.snr_min + (double)s * cfg.snr_step;

                rows[row_count] = simulate_one(symbol_rates[sr_i],
                                               symbol_rate_hz[sr_i],
                                               fecs[fec_i],
                                               channel_snr_db,
                                               cfg.noise_bandwidth_hz,
                                               cfg.guard_fraction,
                                               cfg.guard_name,
                                               cfg.packets,
                                               cfg.seed + (uint64_t)fec_i * 1000003ull + (uint64_t)s * 9176ull);
                fprintf(stderr,
                        "[snr] gi=%s sr=%s fec=%s cn=%.2f esn0=%.2f ebn0=%.2f packets=%u correct=%u error=%.2f%%\n",
                        rows[row_count].guard,
                        rows[row_count].symbol_rate,
                        rows[row_count].fec,
                        rows[row_count].channel_snr_db,
                        rows[row_count].esn0_db,
                        rows[row_count].ebn0_db,
                        rows[row_count].offered_packets,
                        rows[row_count].correct_packets,
                        rows[row_count].packet_error_percent);
                row_count++;
            }
        }
    }

    if (write_csv(cfg.csv_path, rows, row_count) != 0 ||
        write_svg(cfg.svg_path, rows, row_count, cfg.snr_min, cfg.snr_max, cfg.guard_name) != 0) {
        free(rows);
        return 1;
    }

    fprintf(stderr, "wrote %s\n", cfg.csv_path);
    fprintf(stderr, "wrote %s\n", cfg.svg_path);
    free(rows);
    return 0;
}
