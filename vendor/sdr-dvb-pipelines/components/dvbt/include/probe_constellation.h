#ifndef RBDVBT_PROBE_CONSTELLATION_H
#define RBDVBT_PROBE_CONSTELLATION_H

#include "config.h"

int rbdvbt_run_constellation_probe(const rbdvbt_config_t *cfg);
int rbdvbt_run_viterbi_benchmark(uint32_t target_pairs);
void rbdvbt_live_decoder_shutdown(void);

#define RBDVBT_PROBE_EOF 1

#endif
