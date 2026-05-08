#ifndef RBDVBT_DVBT_2K_MODEL_H
#define RBDVBT_DVBT_2K_MODEL_H

#include <stdint.h>

#define RBDVBT_DVBT_2K_FFT_SIZE 2048u
#define RBDVBT_DVBT_2K_ACTIVE_CARRIERS 1705u
#define RBDVBT_DVBT_2K_DATA_CELLS 1512u
#define RBDVBT_DVBT_2K_FFT_START_BIN 172u
#define RBDVBT_DVBT_SYMBOLS_PER_FRAME 68u
#define RBDVBT_DVBT_SYMBOLS_PER_SUPERFRAME 272u

typedef enum {
    RBDVBT_DVBT_CARRIER_UNUSED = 0,
    RBDVBT_DVBT_CARRIER_DATA,
    RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT,
    RBDVBT_DVBT_CARRIER_SCATTERED_PILOT,
    RBDVBT_DVBT_CARRIER_TPS
} rbdvbt_dvbt_carrier_role_t;

void rbdvbt_dvbt_2k_model_init(void);

uint32_t rbdvbt_dvbt_2k_logical_to_fft_bin(uint32_t logical_k);
int rbdvbt_dvbt_2k_prbs_bit(uint32_t logical_k);
int rbdvbt_dvbt_2k_pilot_sign(uint32_t logical_k);
rbdvbt_dvbt_carrier_role_t rbdvbt_dvbt_2k_carrier_role(uint32_t symbol_index,
                                                        uint32_t logical_k);
int rbdvbt_dvbt_2k_data_carrier(uint32_t symbol_index,
                                uint32_t data_index,
                                uint32_t *logical_k);
int rbdvbt_dvbt_2k_symbol_interleaver(uint32_t in_index, uint32_t *out_index);
int rbdvbt_dvbt_2k_symbol_deinterleaver(uint32_t in_index, uint32_t *out_index);

#endif
