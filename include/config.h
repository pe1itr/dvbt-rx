#ifndef RBDVBT_CONFIG_H
#define RBDVBT_CONFIG_H

#include <stdint.h>

typedef enum {
    RBDVBT_INPUT_U8,
    RBDVBT_INPUT_S16
} rbdvbt_input_format_t;

typedef enum {
    RBDVBT_SR_125K = 125000,
    RBDVBT_SR_250K = 250000,
    RBDVBT_SR_333K = 333000,
    RBDVBT_SR_500K = 500000
} rbdvbt_symbol_rate_t;

typedef enum {
    RBDVBT_GI_AUTO = -1,
    RBDVBT_GI_1_8,
    RBDVBT_GI_1_16,
    RBDVBT_GI_1_32
} rbdvbt_guard_interval_t;

typedef enum {
    RBDVBT_FEC_AUTO = -1,
    RBDVBT_FEC_1_2,
    RBDVBT_FEC_2_3,
    RBDVBT_FEC_3_4,
    RBDVBT_FEC_5_6,
    RBDVBT_FEC_7_8
} rbdvbt_fec_t;

typedef struct {
    int use_stdin;
    int probe_constellation;
    int resample_x4;
    int resample_to_dvbt_rate;
    uint32_t dvbt_ir;
    uint32_t sample_rate_hz;
    rbdvbt_symbol_rate_t symbol_rate;
    rbdvbt_guard_interval_t guard_interval;
    rbdvbt_fec_t fec;
    rbdvbt_input_format_t input_format;
    uint64_t max_samples;
    uint32_t fft_size;
    uint32_t probe_symbols;
    const char *constellation_out;
    const char *constellation_svg;
    const char *demap_out;
    const char *viterbi_out;
    const char *ts_out;
    const char *status_json;
    uint32_t status_period_packets;
    const char *spectrum_out;
    const char *spectrum_svg;
    const char *carrier_metrics_out;
    const char *equalized_out;
    const char *equalized_svg;
    const char *window_sweep_out;
    const char *window_sweep_svg;
    uint32_t window_sweep_radius;
    const char *acquisition_scan_out;
    const char *acquisition_scan_svg;
    const char *mapping_scan_out;
    const char *mapping_scan_svg;
    const char *highres_spectrum_out;
    const char *highres_spectrum_svg;
    const char *carrier_spacing_out;
    const char *carrier_map_out;
    const char *carrier_map_svg;
    const char *logical_constellation_out;
    const char *logical_constellation_svg;
    const char *cp_timing_out;
    const char *cp_timing_svg;
    const char *fine_timing_out;
    const char *fine_timing_svg;
    uint32_t highres_fft_size;
    int verbose;
} rbdvbt_config_t;

int rbdvbt_parse_args(int argc, char **argv, rbdvbt_config_t *cfg);
void rbdvbt_print_usage(const char *argv0);
const char *rbdvbt_input_format_name(rbdvbt_input_format_t fmt);
const char *rbdvbt_symbol_rate_name(rbdvbt_symbol_rate_t sr);
const char *rbdvbt_guard_interval_name(rbdvbt_guard_interval_t gi);
const char *rbdvbt_fec_name(rbdvbt_fec_t fec);
double rbdvbt_guard_interval_fraction(rbdvbt_guard_interval_t gi);

#endif
