#ifndef RBDVBT_VISUALIZER_H
#define RBDVBT_VISUALIZER_H

#include <stddef.h>
#include <stdint.h>

#define RBDVBT_VISUALIZER_TYPE_SPECTRUM 1u
#define RBDVBT_VISUALIZER_TYPE_CONSTELLATION 2u

typedef struct rbdvbt_visualizer rbdvbt_visualizer_t;

int rbdvbt_visualizer_open(rbdvbt_visualizer_t **out, const char *endpoint);
void rbdvbt_visualizer_close(rbdvbt_visualizer_t *viz);
int rbdvbt_visualizer_enabled(const rbdvbt_visualizer_t *viz);

void rbdvbt_visualizer_send_spectrum(rbdvbt_visualizer_t *viz,
                                     uint32_t sample_rate_hz,
                                     const float *db_values,
                                     size_t count);
void rbdvbt_visualizer_send_constellation(rbdvbt_visualizer_t *viz,
                                          uint32_t sample_rate_hz,
                                          const float *iq_pairs,
                                          size_t point_count,
                                          float snr_db,
                                          float pilot_lock,
                                          float cfo_hz);

#endif
