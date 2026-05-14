#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float re;
    float im;
} complexf_t;

typedef struct {
    double time_s;
    double snr_db;
    double doppler_hz;
    double path_delay_us;
    double path_ratio_db;
    double notch_depth_db;
} scenario_row_t;

typedef struct {
    const char *in_path;
    const char *out_path;
    const char *scenario_path;
    const char *report_path;
    uint32_t sample_rate_hz;
    double target_snr_db;
    double path_ratio_offset_db;
    double max_echo_amp;
    uint32_t seed;
} config_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --in clean.iq --out impaired.iq --scenario metadata.csv --sample-rate HZ\n"
            "       [--target-snr-db DB] [--path-ratio-offset-db DB] [--max-echo-amp A]\n"
            "       [--seed N] [--report FILE.csv]\n",
            argv0);
}

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

static int parse_args(int argc, char **argv, config_t *cfg)
{
    int i;

    memset(cfg, 0, sizeof(*cfg));
    cfg->target_snr_db = 7.5;
    cfg->max_echo_amp = 4.0;
    cfg->seed = 1u;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--in") == 0 && i + 1 < argc) {
            cfg->in_path = argv[++i];
        } else if (strcmp(arg, "--out") == 0 && i + 1 < argc) {
            cfg->out_path = argv[++i];
        } else if (strcmp(arg, "--scenario") == 0 && i + 1 < argc) {
            cfg->scenario_path = argv[++i];
        } else if (strcmp(arg, "--sample-rate") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->sample_rate_hz) != 0 || cfg->sample_rate_hz == 0u) {
                fprintf(stderr, "invalid --sample-rate value\n");
                return -1;
            }
        } else if (strcmp(arg, "--target-snr-db") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->target_snr_db) != 0) {
                fprintf(stderr, "invalid --target-snr-db value\n");
                return -1;
            }
        } else if (strcmp(arg, "--path-ratio-offset-db") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->path_ratio_offset_db) != 0) {
                fprintf(stderr, "invalid --path-ratio-offset-db value\n");
                return -1;
            }
        } else if (strcmp(arg, "--max-echo-amp") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &cfg->max_echo_amp) != 0 || cfg->max_echo_amp <= 0.0) {
                fprintf(stderr, "invalid --max-echo-amp value\n");
                return -1;
            }
        } else if (strcmp(arg, "--seed") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->seed) != 0) {
                fprintf(stderr, "invalid --seed value\n");
                return -1;
            }
        } else if (strcmp(arg, "--report") == 0 && i + 1 < argc) {
            cfg->report_path = argv[++i];
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", arg);
            return -1;
        }
    }

    if (cfg->in_path == NULL || cfg->out_path == NULL ||
        cfg->scenario_path == NULL || cfg->sample_rate_hz == 0u) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

static int read_file_s16_iq(const char *path, complexf_t **out_samples, size_t *out_count)
{
    FILE *f = NULL;
    long size;
    unsigned char *raw = NULL;
    complexf_t *samples = NULL;
    size_t count;
    size_t n;
    int rc = -1;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "failed to open IQ input %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "failed to size IQ input %s\n", path);
        goto done;
    }
    if ((size % 4) != 0) {
        fprintf(stderr, "IQ input size is not a whole number of s16 IQ samples\n");
        goto done;
    }
    raw = malloc((size_t)size);
    if (raw == NULL) {
        fprintf(stderr, "failed to allocate IQ input buffer\n");
        goto done;
    }
    if (fread(raw, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "failed to read IQ input %s\n", path);
        goto done;
    }

    count = (size_t)size / 4u;
    samples = malloc(count * sizeof(*samples));
    if (samples == NULL) {
        fprintf(stderr, "failed to allocate IQ sample buffer\n");
        goto done;
    }
    for (n = 0; n < count; ++n) {
        const unsigned char *p = &raw[n * 4u];
        int16_t i = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        int16_t q = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        samples[n].re = (float)i / 32768.0f;
        samples[n].im = (float)q / 32768.0f;
    }

    *out_samples = samples;
    *out_count = count;
    samples = NULL;
    rc = 0;

done:
    free(samples);
    free(raw);
    if (f != NULL) {
        fclose(f);
    }
    return rc;
}

static int write_file_s16_iq(const char *path, const complexf_t *samples, size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t n;

    if (f == NULL) {
        fprintf(stderr, "failed to open IQ output %s\n", path);
        return -1;
    }
    for (n = 0; n < count; ++n) {
        double ri = samples[n].re;
        double rq = samples[n].im;
        int32_t i;
        int32_t q;
        unsigned char raw[4];

        if (ri > 0.999969) {
            ri = 0.999969;
        } else if (ri < -1.0) {
            ri = -1.0;
        }
        if (rq > 0.999969) {
            rq = 0.999969;
        } else if (rq < -1.0) {
            rq = -1.0;
        }
        i = (int32_t)lrint(ri * 32768.0);
        q = (int32_t)lrint(rq * 32768.0);
        raw[0] = (unsigned char)(i & 0xff);
        raw[1] = (unsigned char)((i >> 8) & 0xff);
        raw[2] = (unsigned char)(q & 0xff);
        raw[3] = (unsigned char)((q >> 8) & 0xff);
        if (fwrite(raw, 1, sizeof(raw), f) != sizeof(raw)) {
            fprintf(stderr, "failed to write IQ output %s\n", path);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int read_scenario_csv(const char *path, scenario_row_t **out_rows, size_t *out_count)
{
    FILE *f = NULL;
    char line[1024];
    scenario_row_t *rows = NULL;
    size_t count = 0;
    size_t cap = 0;
    int rc = -1;

    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "failed to open scenario %s\n", path);
        return -1;
    }
    if (fgets(line, sizeof(line), f) == NULL) {
        fprintf(stderr, "empty scenario %s\n", path);
        goto done;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        scenario_row_t row;
        char *field;
        int col = 0;
        int have = 0;

        memset(&row, 0, sizeof(row));
        field = strtok(line, ",\r\n");
        while (field != NULL) {
            double value = strtod(field, NULL);

            if (col == 0) {
                row.time_s = value;
                have |= 1;
            } else if (col == 3) {
                row.snr_db = value;
                have |= 2;
            } else if (col == 4) {
                row.doppler_hz = value;
                have |= 4;
            } else if (col == 6) {
                row.path_delay_us = value;
                have |= 8;
            } else if (col == 8) {
                row.path_ratio_db = value;
                have |= 16;
            } else if (col == 9) {
                row.notch_depth_db = value;
                have |= 32;
            }
            col++;
            field = strtok(NULL, ",\r\n");
        }
        if (have != 63) {
            continue;
        }
        if (count == cap) {
            size_t new_cap = cap == 0 ? 256u : cap * 2u;
            scenario_row_t *new_rows = realloc(rows, new_cap * sizeof(*new_rows));
            if (new_rows == NULL) {
                fprintf(stderr, "failed to allocate scenario rows\n");
                goto done;
            }
            rows = new_rows;
            cap = new_cap;
        }
        rows[count++] = row;
    }
    if (count == 0u) {
        fprintf(stderr, "scenario has no usable rows: %s\n", path);
        goto done;
    }
    *out_rows = rows;
    *out_count = count;
    rows = NULL;
    rc = 0;

done:
    free(rows);
    if (f != NULL) {
        fclose(f);
    }
    return rc;
}

static double rand_uniform(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return ((double)(*state) + 1.0) / 4294967297.0;
}

static double rand_normal(uint32_t *state)
{
    double u1 = rand_uniform(state);
    double u2 = rand_uniform(state);

    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static complexf_t lerp_sample(const complexf_t *samples, size_t count, double pos)
{
    size_t idx;
    double frac;
    complexf_t z = {0.0f, 0.0f};

    if (pos < 0.0 || count == 0u) {
        return z;
    }
    idx = (size_t)pos;
    if (idx + 1u >= count) {
        if (idx < count) {
            return samples[idx];
        }
        return z;
    }
    frac = pos - (double)idx;
    z.re = (float)((1.0 - frac) * samples[idx].re + frac * samples[idx + 1u].re);
    z.im = (float)((1.0 - frac) * samples[idx].im + frac * samples[idx + 1u].im);
    return z;
}

static scenario_row_t scenario_at(const scenario_row_t *rows,
                                  size_t count,
                                  double t_norm,
                                  size_t *cursor)
{
    double first_t = rows[0].time_s;
    double last_t = rows[count - 1u].time_s;
    double t = first_t + t_norm * (last_t - first_t);
    scenario_row_t out;
    double frac;

    while (*cursor + 2u < count && rows[*cursor + 1u].time_s < t) {
        (*cursor)++;
    }
    if (*cursor + 1u >= count || rows[*cursor + 1u].time_s <= rows[*cursor].time_s) {
        return rows[*cursor];
    }
    frac = (t - rows[*cursor].time_s) / (rows[*cursor + 1u].time_s - rows[*cursor].time_s);
    out.time_s = t;
    out.snr_db = rows[*cursor].snr_db + frac * (rows[*cursor + 1u].snr_db - rows[*cursor].snr_db);
    out.doppler_hz = rows[*cursor].doppler_hz + frac * (rows[*cursor + 1u].doppler_hz - rows[*cursor].doppler_hz);
    out.path_delay_us = rows[*cursor].path_delay_us + frac * (rows[*cursor + 1u].path_delay_us - rows[*cursor].path_delay_us);
    out.path_ratio_db = rows[*cursor].path_ratio_db + frac * (rows[*cursor + 1u].path_ratio_db - rows[*cursor].path_ratio_db);
    out.notch_depth_db = rows[*cursor].notch_depth_db + frac * (rows[*cursor + 1u].notch_depth_db - rows[*cursor].notch_depth_db);
    return out;
}

static double mean_power(const complexf_t *samples, size_t count)
{
    double sum = 0.0;
    size_t n;

    for (n = 0; n < count; ++n) {
        sum += (double)samples[n].re * samples[n].re + (double)samples[n].im * samples[n].im;
    }
    return count > 0u ? sum / (double)count : 0.0;
}

static void normalize_peak(complexf_t *samples, size_t count, double target_peak)
{
    double peak = 0.0;
    size_t n;

    for (n = 0; n < count; ++n) {
        double ai = fabs(samples[n].re);
        double aq = fabs(samples[n].im);
        if (ai > peak) {
            peak = ai;
        }
        if (aq > peak) {
            peak = aq;
        }
    }
    if (peak > target_peak && peak > 0.0) {
        double scale = target_peak / peak;
        for (n = 0; n < count; ++n) {
            samples[n].re = (float)(samples[n].re * scale);
            samples[n].im = (float)(samples[n].im * scale);
        }
    }
}

int main(int argc, char **argv)
{
    config_t cfg;
    complexf_t *input = NULL;
    complexf_t *output = NULL;
    scenario_row_t *rows = NULL;
    size_t input_count = 0;
    size_t row_count = 0;
    size_t cursor = 0;
    uint32_t rng;
    double phase = 0.0;
    double signal_power;
    double noise_power;
    double noise_sigma;
    double min_snr = 1e30;
    double max_snr = -1e30;
    double max_notch = 0.0;
    double max_delay = 0.0;
    size_t n;
    FILE *report = NULL;
    int rc = 1;

    if (parse_args(argc, argv, &cfg) != 0) {
        return 1;
    }
    if (read_file_s16_iq(cfg.in_path, &input, &input_count) != 0 ||
        read_scenario_csv(cfg.scenario_path, &rows, &row_count) != 0) {
        goto done;
    }
    output = calloc(input_count, sizeof(*output));
    if (output == NULL) {
        fprintf(stderr, "failed to allocate output samples\n");
        goto done;
    }

    for (n = 0; n < input_count; ++n) {
        double t_norm = input_count > 1u ? (double)n / (double)(input_count - 1u) : 0.0;
        scenario_row_t row = scenario_at(rows, row_count, t_norm, &cursor);
        double delay_samples = row.path_delay_us * 1e-6 * (double)cfg.sample_rate_hz;
        double echo_amp = pow(10.0, (row.path_ratio_db + cfg.path_ratio_offset_db) / 20.0);
        complexf_t direct = input[n];
        complexf_t echo = lerp_sample(input, input_count, (double)n - delay_samples);
        double c;
        double s;

        if (echo_amp > cfg.max_echo_amp) {
            echo_amp = cfg.max_echo_amp;
        }
        phase += 2.0 * M_PI * row.doppler_hz / (double)cfg.sample_rate_hz;
        if (phase > M_PI || phase < -M_PI) {
            phase = fmod(phase, 2.0 * M_PI);
        }
        c = cos(phase);
        s = sin(phase);

        output[n].re = (float)(direct.re + echo_amp * (echo.re * c - echo.im * s));
        output[n].im = (float)(direct.im + echo_amp * (echo.re * s + echo.im * c));

        if (row.snr_db < min_snr) {
            min_snr = row.snr_db;
        }
        if (row.snr_db > max_snr) {
            max_snr = row.snr_db;
        }
        if (row.notch_depth_db > max_notch) {
            max_notch = row.notch_depth_db;
        }
        if (row.path_delay_us > max_delay) {
            max_delay = row.path_delay_us;
        }
    }

    normalize_peak(output, input_count, 0.70);
    signal_power = mean_power(output, input_count);
    noise_power = signal_power / pow(10.0, cfg.target_snr_db / 10.0);
    noise_sigma = sqrt(noise_power / 2.0);
    rng = cfg.seed == 0u ? 1u : cfg.seed;

    for (n = 0; n < input_count; ++n) {
        output[n].re = (float)(output[n].re + noise_sigma * rand_normal(&rng));
        output[n].im = (float)(output[n].im + noise_sigma * rand_normal(&rng));
    }
    normalize_peak(output, input_count, 0.98);

    if (cfg.report_path != NULL) {
        report = fopen(cfg.report_path, "w");
        if (report == NULL) {
            fprintf(stderr, "failed to open report %s\n", cfg.report_path);
            goto done;
        }
        fprintf(report,
                "scenario,input_samples,target_snr_db,metadata_snr_min_db,metadata_snr_max_db,max_notch_db,max_delay_us,path_ratio_offset_db,max_echo_amp\n");
        fprintf(report,
                "%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                cfg.scenario_path,
                input_count,
                cfg.target_snr_db,
                min_snr,
                max_snr,
                max_notch,
                max_delay,
                cfg.path_ratio_offset_db,
                cfg.max_echo_amp);
    }

    if (write_file_s16_iq(cfg.out_path, output, input_count) != 0) {
        goto done;
    }
    fprintf(stderr,
            "[iq_airscatter_channel] in=%s out=%s samples=%zu target_snr=%.2fdB metadata_snr=%.2f..%.2fdB max_notch=%.2fdB max_delay=%.3fus\n",
            cfg.in_path,
            cfg.out_path,
            input_count,
            cfg.target_snr_db,
            min_snr,
            max_snr,
            max_notch,
            max_delay);
    rc = 0;

done:
    if (report != NULL) {
        fclose(report);
    }
    free(rows);
    free(output);
    free(input);
    return rc;
}
