#include "iq_input.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static float abs_float(float value)
{
    return value < 0.0f ? -value : value;
}

static int16_t read_le_s16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

size_t rbdvbt_read_iq(FILE *stream,
                      rbdvbt_input_format_t format,
                      rbdvbt_complex_t *out,
                      size_t max_samples)
{
    unsigned char raw[8192];
    size_t bytes_per_sample = format == RBDVBT_INPUT_U8 ? 2u : 4u;
    size_t wanted = max_samples * bytes_per_sample;
    size_t got;
    size_t count;
    size_t n;

    if (wanted > sizeof(raw)) {
        wanted = sizeof(raw) - (sizeof(raw) % bytes_per_sample);
    }

    got = fread(raw, 1, wanted, stream);
    count = got / bytes_per_sample;

    for (n = 0; n < count; ++n) {
        if (format == RBDVBT_INPUT_U8) {
            int i = (int)raw[2 * n] - 127;
            int q = (int)raw[2 * n + 1] - 127;
            out[n].i = (float)i / 128.0f;
            out[n].q = (float)q / 128.0f;
        } else {
            const unsigned char *p = &raw[4 * n];
            int16_t i = read_le_s16(p);
            int16_t q = read_le_s16(p + 2);
            out[n].i = (float)i / 32768.0f;
            out[n].q = (float)q / 32768.0f;
        }
    }

    return count;
}

void rbdvbt_iq_stats_init(rbdvbt_iq_stats_t *stats)
{
    stats->samples = 0;
    stats->sum_i = 0.0;
    stats->sum_q = 0.0;
    stats->sum_power = 0.0;
    stats->peak_abs_i = 0.0f;
    stats->peak_abs_q = 0.0f;
    stats->clipped_i = 0;
    stats->clipped_q = 0;
}

void rbdvbt_iq_stats_update(rbdvbt_iq_stats_t *stats,
                            const rbdvbt_complex_t *samples,
                            size_t count)
{
    size_t n;

    for (n = 0; n < count; ++n) {
        float ai = abs_float(samples[n].i);
        float aq = abs_float(samples[n].q);

        stats->samples++;
        stats->sum_i += samples[n].i;
        stats->sum_q += samples[n].q;
        stats->sum_power += (double)samples[n].i * samples[n].i +
                            (double)samples[n].q * samples[n].q;

        if (ai > stats->peak_abs_i) {
            stats->peak_abs_i = ai;
        }
        if (aq > stats->peak_abs_q) {
            stats->peak_abs_q = aq;
        }
        if (ai >= 0.999f) {
            stats->clipped_i++;
        }
        if (aq >= 0.999f) {
            stats->clipped_q++;
        }
    }
}

void rbdvbt_iq_stats_print(FILE *stream, const rbdvbt_iq_stats_t *stats)
{
    double mean_i = 0.0;
    double mean_q = 0.0;
    double rms = 0.0;

    if (stats->samples != 0) {
        mean_i = stats->sum_i / (double)stats->samples;
        mean_q = stats->sum_q / (double)stats->samples;
        rms = sqrt(stats->sum_power / (double)stats->samples);
    }

    fprintf(stream,
            "[input] samples=%llu mean_i=%.7f mean_q=%.7f rms=%.7f peak_i=%.7f peak_q=%.7f clipped_i=%llu clipped_q=%llu\n",
            (unsigned long long)stats->samples,
            mean_i,
            mean_q,
            rms,
            stats->peak_abs_i,
            stats->peak_abs_q,
            (unsigned long long)stats->clipped_i,
            (unsigned long long)stats->clipped_q);
}
