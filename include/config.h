#ifndef RBDVBT_CONFIG_H
#define RBDVBT_CONFIG_H

#include <stdint.h>

#ifndef RBDVBT_VERSION
#define RBDVBT_VERSION "0.1.3"
#endif

typedef enum {
    RBDVBT_INPUT_U8,
    RBDVBT_INPUT_S16
} rbdvbt_input_format_t;

typedef enum {
    RBDVBT_SR_125K = 125000,
    RBDVBT_SR_150K = 150000,
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

typedef enum {
    RBDVBT_LOG_QUIET = -1,
    RBDVBT_LOG_ERROR = 0,
    RBDVBT_LOG_WARN,
    RBDVBT_LOG_INFO,
    RBDVBT_LOG_DEBUG,
    RBDVBT_LOG_TRACE
} rbdvbt_log_level_t;

typedef struct {
    int use_stdin;
    int probe_constellation;
    int live_mode;
    int afc_enabled;
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
    uint32_t live_frontend_symbols;
    const char *constellation_out;
    const char *constellation_svg;
    const char *demap_out;
    const char *viterbi_out;
    const char *ts_out;
    int wait_video_start;
    const char *status_json;
    uint32_t status_period_packets;
    const char *visualizer_udp;
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
    int gui;
    int verbose;
    int show_help;
    int show_version;
    rbdvbt_log_level_t log_level;
} rbdvbt_config_t;

int rbdvbt_parse_args(int argc, char **argv, rbdvbt_config_t *cfg);
void rbdvbt_print_usage(const char *argv0);
void rbdvbt_print_info(const char *argv0);
int rbdvbt_log_enabled(rbdvbt_log_level_t level);
void rbdvbt_log_set_level(rbdvbt_log_level_t level);
const char *rbdvbt_log_level_name(rbdvbt_log_level_t level);
const char *rbdvbt_input_format_name(rbdvbt_input_format_t fmt);
const char *rbdvbt_symbol_rate_name(rbdvbt_symbol_rate_t sr);
const char *rbdvbt_guard_interval_name(rbdvbt_guard_interval_t gi);
const char *rbdvbt_fec_name(rbdvbt_fec_t fec);
double rbdvbt_guard_interval_fraction(rbdvbt_guard_interval_t gi);

#endif
