#include "receiver.h"

#include "iq_input.h"
#include "probe_constellation.h"

#include <stdio.h>

#define RBDVBT_BLOCK_SAMPLES 2048u

static void print_config(const rbdvbt_config_t *cfg)
{
    fprintf(stderr,
            "[config] input=stdin format=%s sample_rate=%u sr=%s gi=%s qpsk=1 ts_out=%s stdout=ts-only\n",
            rbdvbt_input_format_name(cfg->input_format),
            cfg->sample_rate_hz,
            rbdvbt_symbol_rate_name(cfg->symbol_rate),
            rbdvbt_guard_interval_name(cfg->guard_interval),
            cfg->ts_out != NULL ? cfg->ts_out : "-");
}

int rbdvbt_run_receiver(const rbdvbt_config_t *cfg)
{
    rbdvbt_complex_t samples[RBDVBT_BLOCK_SAMPLES];
    rbdvbt_iq_stats_t stats;
    uint64_t remaining = cfg->max_samples;

    print_config(cfg);

    if (cfg->probe_constellation) {
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
