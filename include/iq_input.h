#ifndef RBDVBT_IQ_INPUT_H
#define RBDVBT_IQ_INPUT_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    float i;
    float q;
} rbdvbt_complex_t;

typedef struct {
    uint64_t samples;
    double sum_i;
    double sum_q;
    double sum_power;
    float peak_abs_i;
    float peak_abs_q;
    uint64_t clipped_i;
    uint64_t clipped_q;
} rbdvbt_iq_stats_t;

size_t rbdvbt_read_iq(FILE *stream,
                      rbdvbt_input_format_t format,
                      rbdvbt_complex_t *out,
                      size_t max_samples);

void rbdvbt_iq_stats_init(rbdvbt_iq_stats_t *stats);
void rbdvbt_iq_stats_update(rbdvbt_iq_stats_t *stats,
                            const rbdvbt_complex_t *samples,
                            size_t count);
void rbdvbt_iq_stats_print(FILE *stream, const rbdvbt_iq_stats_t *stats);
void rbdvbt_iq_stats_log(rbdvbt_log_level_t level, const rbdvbt_iq_stats_t *stats);

#endif
