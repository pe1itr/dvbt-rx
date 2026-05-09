#ifndef RBDVBT_PROBE_CONSTELLATION_H
#define RBDVBT_PROBE_CONSTELLATION_H

#include "config.h"

int rbdvbt_run_constellation_probe(const rbdvbt_config_t *cfg);
void rbdvbt_live_decoder_shutdown(void);

#define RBDVBT_PROBE_EOF 1

#endif
