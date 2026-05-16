#ifndef RBDVBT_UDP_TS_OUTPUT_H
#define RBDVBT_UDP_TS_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#define RBDVBT_UDP_TS_PACKET_SIZE 1316u

typedef struct rbdvbt_udp_ts_output rbdvbt_udp_ts_output_t;

int rbdvbt_udp_ts_output_open(rbdvbt_udp_ts_output_t **out,
                              const char *host,
                              uint16_t port);
int rbdvbt_udp_ts_output_write(rbdvbt_udp_ts_output_t *out,
                               const uint8_t *bytes,
                               size_t byte_count);
void rbdvbt_udp_ts_output_close(rbdvbt_udp_ts_output_t *out);

#endif
