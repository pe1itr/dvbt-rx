#ifndef RBDVBT_DVBT_OUTER_H
#define RBDVBT_DVBT_OUTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *status_json_path;
    uint32_t status_period_packets;
    const char *symbol_rate;
    const char *guard_interval;
    const char *fec;
    const char *modulation;
    const char *fft_mode;
    const char *constellation;
    double snr_db;
    double pilot_lock;
    uint64_t input_samples;
    uint32_t lock_quality;
    uint32_t ssi;
} rbdvbt_status_context_t;

void rbdvbt_status_publish_idle(const rbdvbt_status_context_t *status,
                                const char *stage,
                                uint64_t input_samples);

int rbdvbt_outer_recover_ts(const uint8_t *inner,
                            size_t inner_count,
                            const char *ts_path,
                            const rbdvbt_status_context_t *status);

#endif
