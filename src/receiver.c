#include "receiver.h"

#include "iq_input.h"
#include "probe_constellation.h"

#include <stdio.h>

#define RBDVBT_BLOCK_SAMPLES 2048u
#define RBDVBT_DVBT_2K_FFT_SIZE 2048u
#define RBDVBT_LIVE_TARGET_SYMBOLS_DEFAULT 64u

static void print_config(const rbdvbt_config_t *cfg)
{
    if (!rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
        return;
    }

    fprintf(stderr,
            "[config] input=stdin format=%s sample_rate=%u sr=%s gi=%s qpsk=1 ts_out=%s stdout=ts-only live=%d gui=%d live_symbols=%u loglevel=%s\n",
            rbdvbt_input_format_name(cfg->input_format),
            cfg->sample_rate_hz,
            rbdvbt_symbol_rate_name(cfg->symbol_rate),
            rbdvbt_guard_interval_name(cfg->guard_interval),
            cfg->ts_out != NULL ? cfg->ts_out : "-",
            cfg->live_mode,
            cfg->gui,
            cfg->live_frontend_symbols != 0u ? cfg->live_frontend_symbols : RBDVBT_LIVE_TARGET_SYMBOLS_DEFAULT,
            rbdvbt_log_level_name(cfg->log_level));
}

static int run_live_constellation_probe(const rbdvbt_config_t *cfg)
{
    rbdvbt_config_t live_cfg = *cfg;
    uint64_t chunks = 0;
    uint64_t ok_chunks = 0;
    uint64_t failed_chunks = 0;
    uint32_t live_target_symbols = cfg->live_frontend_symbols != 0u ?
        cfg->live_frontend_symbols :
        RBDVBT_LIVE_TARGET_SYMBOLS_DEFAULT;

    if (live_cfg.resample_to_dvbt_rate) {
        uint32_t gi_len;
        double target_rate = ((double)live_cfg.symbol_rate * 8.0 / 7.0) * (double)live_cfg.dvbt_ir;

        switch (live_cfg.guard_interval) {
        case RBDVBT_GI_1_32:
            gi_len = RBDVBT_DVBT_2K_FFT_SIZE / 32u;
            break;
        case RBDVBT_GI_1_16:
            gi_len = RBDVBT_DVBT_2K_FFT_SIZE / 16u;
            break;
        case RBDVBT_GI_1_8:
            gi_len = RBDVBT_DVBT_2K_FFT_SIZE / 8u;
            break;
        case RBDVBT_GI_AUTO:
        default:
            gi_len = RBDVBT_DVBT_2K_FFT_SIZE / 8u;
            break;
        }

        if (target_rate > 0.0) {
            uint32_t symbol_len = RBDVBT_DVBT_2K_FFT_SIZE + gi_len;
            uint64_t target_input_samples = (uint64_t)(((double)live_target_symbols *
                                                        (double)symbol_len *
                                                        (double)live_cfg.sample_rate_hz /
                                                        target_rate) + 0.5);

            if (target_input_samples == 0u) {
                target_input_samples = live_cfg.max_samples;
            }
            if (live_cfg.max_samples == 0u ||
                (cfg->live_frontend_symbols != 0u && live_cfg.max_samples > target_input_samples)) {
                if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                    fprintf(stderr,
                            "[live] using frontend chunk target=%u symbols max_samples=%llu%s\n",
                            live_target_symbols,
                            (unsigned long long)target_input_samples,
                            cfg->max_samples != 0u ? " capped-from-config" : "");
                }
                live_cfg.max_samples = target_input_samples;
            } else if (rbdvbt_log_enabled(RBDVBT_LOG_INFO) && cfg->max_samples != 0u) {
                fprintf(stderr,
                        "[live] using explicit max_samples=%llu; live target=%u symbols is not capping this run\n",
                        (unsigned long long)live_cfg.max_samples,
                        live_target_symbols);
            }
            if (live_cfg.probe_symbols < live_target_symbols) {
                if (rbdvbt_log_enabled(RBDVBT_LOG_INFO)) {
                    fprintf(stderr,
                            "[live] expanding probe_symbols from %u to %u for live frontend chunks\n",
                            live_cfg.probe_symbols,
                            live_target_symbols);
                }
                live_cfg.probe_symbols = live_target_symbols;
            }
        }
    } else if (live_cfg.max_samples == 0u) {
        fprintf(stderr,
                "[live] --max-samples is not set; using the probe default chunk size. "
                "For lower restart latency set --max-samples and --probe-symbols explicitly.\n");
    }

    for (;;) {
        int rc;

        chunks++;
        if (rbdvbt_log_enabled(RBDVBT_LOG_DEBUG)) {
            fprintf(stderr, "[live] chunk=%llu start\n", (unsigned long long)chunks);
        }
        rc = rbdvbt_run_constellation_probe(&live_cfg);
        if (rc == RBDVBT_PROBE_EOF) {
            rbdvbt_live_decoder_shutdown();
            fprintf(stderr,
                    "[live] eof chunks=%llu ok=%llu failed=%llu\n",
                    (unsigned long long)(chunks - 1u),
                    (unsigned long long)ok_chunks,
                    (unsigned long long)failed_chunks);
            return 0;
        }
        if (rc != 0) {
            failed_chunks++;
            fprintf(stderr,
                    "[live] chunk=%llu failed rc=%d; continuing acquisition\n",
                    (unsigned long long)chunks,
                    rc);
            continue;
        }
        ok_chunks++;
        fprintf(stderr,
                "[live] chunk=%llu done ok=%llu failed=%llu\n",
                (unsigned long long)chunks,
                (unsigned long long)ok_chunks,
                (unsigned long long)failed_chunks);
    }
}

int rbdvbt_run_receiver(const rbdvbt_config_t *cfg)
{
    rbdvbt_complex_t samples[RBDVBT_BLOCK_SAMPLES];
    rbdvbt_iq_stats_t stats;
    uint64_t remaining = cfg->max_samples;

    print_config(cfg);

    if (cfg->probe_constellation) {
        if (cfg->live_mode) {
            return run_live_constellation_probe(cfg);
        }
        return rbdvbt_run_constellation_probe(cfg);
    }

    rbdvbt_iq_stats_init(&stats);

    for (;;) {
        size_t want = RBDVBT_BLOCK_SAMPLES;
        size_t got;

        if (cfg->max_samples != 0) {
            if (remaining == 0) {
                break;
            }
            if (remaining < want) {
                want = (size_t)remaining;
            }
        }

        got = rbdvbt_read_iq(stdin, cfg->input_format, samples, want);
        if (got == 0) {
            break;
        }

        rbdvbt_iq_stats_update(&stats, samples, got);

        if (cfg->max_samples != 0) {
            remaining -= got;
        }
    }

    rbdvbt_iq_stats_print(stderr, &stats);
    fprintf(stderr, "[stage0] decoder not implemented yet; stdout intentionally contains no debug text\n");

    return 0;
}
