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
    double cfo_hz;
    int32_t bin_shift;
    int32_t symbol_phase;
    uint64_t input_samples;
    uint32_t lock_quality;
    uint32_t ssi;
    int wait_video_start;
    int live_mode;
    int afc_enabled;
    int afc_advised;
    int32_t afc_delta_bins;
    uint32_t afc_trend_count;
    int gui_enabled;
} rbdvbt_status_context_t;

typedef struct {
    uint32_t packets;
    uint32_t sync_bad;
    uint32_t transport_errors;
    uint32_t cc_errors;
    uint32_t pat_packets;
    uint32_t pmt_packets;
    uint32_t sdt_packets;
    uint32_t rs_bad;
    uint32_t rs_corrected;
    uint32_t rs_corrected_bytes;
    uint32_t rs_uncorrectable;
    uint32_t score;
} rbdvbt_outer_metrics_t;

void rbdvbt_status_publish_idle(const rbdvbt_status_context_t *status,
                                const char *stage,
                                uint64_t input_samples);

void rbdvbt_outer_reset_live_stream(void);
void rbdvbt_live_health_note_outer(uint32_t packets,
                                   uint32_t sync_bad,
                                   uint32_t transport_errors,
                                   uint32_t cc_errors,
                                   uint32_t rs_bad,
                                   uint32_t rs_corrected,
                                   uint32_t rs_uncorrectable,
                                   uint32_t written_packets,
                                   size_t outer_acquire_pending,
                                   const char *outer_state);

int rbdvbt_outer_analyze_inner(const uint8_t *inner,
                               size_t inner_count,
                               rbdvbt_outer_metrics_t *metrics);

int rbdvbt_outer_recover_ts(const uint8_t *inner,
                            size_t inner_count,
                            const char *ts_path,
                            const rbdvbt_status_context_t *status);

#endif
