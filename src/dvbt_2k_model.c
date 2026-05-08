#include "dvbt_2k_model.h"

#include <stddef.h>

static const uint16_t continual_pilots_2k[] = {
    0,    48,   54,   87,   141,  156,  192,  201,  255,
    279,  282,  333,  432,  450,  483,  525,  531,  618,
    636,  714,  759,  765,  780,  804,  873,  888,  918,
    939,  942,  969,  984,  1050, 1101, 1107, 1110, 1137,
    1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704,
};

static const uint16_t tps_carriers_2k[] = {
    34,  50,   209,  346,  413,  569,  595,  688,  790,
    901, 1073, 1219, 1262, 1286, 1469, 1594, 1687,
};

static uint8_t prbs_2k[RBDVBT_DVBT_2K_ACTIVE_CARRIERS];
static uint16_t data_carriers_mod4[4][RBDVBT_DVBT_2K_DATA_CELLS];
static uint16_t symbol_interleaver_2k[RBDVBT_DVBT_2K_DATA_CELLS];
static uint16_t symbol_deinterleaver_2k[RBDVBT_DVBT_2K_DATA_CELLS];
static int initialized;

static int list_contains_u16(const uint16_t *values, size_t count, uint32_t needle)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (values[i] == needle) {
            return 1;
        }
    }

    return 0;
}

static int is_continual_pilot(uint32_t logical_k)
{
    return list_contains_u16(continual_pilots_2k,
                             sizeof(continual_pilots_2k) / sizeof(continual_pilots_2k[0]),
                             logical_k);
}

static int is_tps_carrier(uint32_t logical_k)
{
    return list_contains_u16(tps_carriers_2k,
                             sizeof(tps_carriers_2k) / sizeof(tps_carriers_2k[0]),
                             logical_k);
}

static int is_scattered_pilot(uint32_t symbol_index, uint32_t logical_k)
{
    uint32_t phase = (symbol_index % 4u) * 3u;

    if (logical_k >= RBDVBT_DVBT_2K_ACTIVE_CARRIERS - 1u || logical_k < phase) {
        return 0;
    }

    return ((logical_k - phase) % 12u) == 0u;
}

static uint32_t permutate_bits_2k(uint32_t in)
{
    uint32_t out = 0;

    out |= (in & 0x001u) ? (1u << 4) : 0u;
    out |= (in & 0x002u) ? (1u << 3) : 0u;
    out |= (in & 0x004u) ? (1u << 9) : 0u;
    out |= (in & 0x008u) ? (1u << 6) : 0u;
    out |= (in & 0x010u) ? (1u << 2) : 0u;
    out |= (in & 0x020u) ? (1u << 8) : 0u;
    out |= (in & 0x040u) ? (1u << 1) : 0u;
    out |= (in & 0x080u) ? (1u << 5) : 0u;
    out |= (in & 0x100u) ? (1u << 7) : 0u;
    out |= (in & 0x200u) ? (1u << 0) : 0u;

    return out;
}

static void build_prbs(void)
{
    uint32_t k;
    uint32_t s = 0x7ffu;

    for (k = 0; k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++k) {
        prbs_2k[k] = (uint8_t)(s & 1u);
        s |= ((s & 1u) ^ ((s >> 2) & 1u)) ? 0x800u : 0u;
        s >>= 1;
    }
}

static void build_symbol_interleaver(void)
{
    uint32_t tmp[RBDVBT_DVBT_2K_FFT_SIZE];
    uint32_t i;
    uint32_t q = 0;
    uint32_t sr = 1;

    tmp[0] = 0;
    tmp[1] = 0;
    tmp[2] = 1;

    for (i = 3; i < RBDVBT_DVBT_2K_FFT_SIZE; ++i) {
        uint32_t b0 = sr & 0x01u;
        uint32_t b3 = (sr >> 3) & 0x01u;
        uint32_t b9 = (b0 ^ b3) ? 0x200u : 0u;

        sr >>= 1;
        sr |= b9;
        tmp[i] = sr;
    }

    for (i = 0; i < RBDVBT_DVBT_2K_FFT_SIZE; ++i) {
        tmp[i] = ((i % 2u) << 10) + permutate_bits_2k(tmp[i]);
    }

    for (i = 0; i < RBDVBT_DVBT_2K_FFT_SIZE && q < RBDVBT_DVBT_2K_DATA_CELLS; ++i) {
        if (tmp[i] < RBDVBT_DVBT_2K_DATA_CELLS) {
            symbol_interleaver_2k[q] = (uint16_t)tmp[i];
            symbol_deinterleaver_2k[tmp[i]] = (uint16_t)q;
            q++;
        }
    }
}

static void build_data_carriers(void)
{
    uint32_t sym_mod;

    for (sym_mod = 0; sym_mod < 4u; ++sym_mod) {
        uint32_t logical_k;
        uint32_t data_index = 0;

        for (logical_k = 0; logical_k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++logical_k) {
            if (rbdvbt_dvbt_2k_carrier_role(sym_mod, logical_k) == RBDVBT_DVBT_CARRIER_DATA) {
                if (data_index < RBDVBT_DVBT_2K_DATA_CELLS) {
                    data_carriers_mod4[sym_mod][data_index] = (uint16_t)logical_k;
                }
                data_index++;
            }
        }
    }
}

void rbdvbt_dvbt_2k_model_init(void)
{
    if (initialized) {
        return;
    }

    build_prbs();
    build_symbol_interleaver();
    initialized = 1;
    build_data_carriers();
}

uint32_t rbdvbt_dvbt_2k_logical_to_fft_bin(uint32_t logical_k)
{
    if (logical_k >= RBDVBT_DVBT_2K_ACTIVE_CARRIERS) {
        return RBDVBT_DVBT_2K_FFT_SIZE;
    }

    return RBDVBT_DVBT_2K_FFT_START_BIN + logical_k;
}

int rbdvbt_dvbt_2k_prbs_bit(uint32_t logical_k)
{
    rbdvbt_dvbt_2k_model_init();

    if (logical_k >= RBDVBT_DVBT_2K_ACTIVE_CARRIERS) {
        return -1;
    }

    return prbs_2k[logical_k] ? 1 : 0;
}

int rbdvbt_dvbt_2k_pilot_sign(uint32_t logical_k)
{
    int bit = rbdvbt_dvbt_2k_prbs_bit(logical_k);

    if (bit < 0) {
        return 0;
    }

    return bit ? -1 : 1;
}

rbdvbt_dvbt_carrier_role_t rbdvbt_dvbt_2k_carrier_role(uint32_t symbol_index,
                                                        uint32_t logical_k)
{
    if (logical_k >= RBDVBT_DVBT_2K_ACTIVE_CARRIERS) {
        return RBDVBT_DVBT_CARRIER_UNUSED;
    }

    if (is_tps_carrier(logical_k)) {
        return RBDVBT_DVBT_CARRIER_TPS;
    }
    if (is_scattered_pilot(symbol_index, logical_k)) {
        return RBDVBT_DVBT_CARRIER_SCATTERED_PILOT;
    }
    if (is_continual_pilot(logical_k)) {
        return RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT;
    }

    return RBDVBT_DVBT_CARRIER_DATA;
}

int rbdvbt_dvbt_2k_data_carrier(uint32_t symbol_index,
                                uint32_t data_index,
                                uint32_t *logical_k)
{
    rbdvbt_dvbt_2k_model_init();

    if (logical_k == NULL || data_index >= RBDVBT_DVBT_2K_DATA_CELLS) {
        return -1;
    }

    *logical_k = data_carriers_mod4[symbol_index % 4u][data_index];
    return 0;
}

int rbdvbt_dvbt_2k_symbol_interleaver(uint32_t in_index, uint32_t *out_index)
{
    rbdvbt_dvbt_2k_model_init();

    if (out_index == NULL || in_index >= RBDVBT_DVBT_2K_DATA_CELLS) {
        return -1;
    }

    *out_index = symbol_interleaver_2k[in_index];
    return 0;
}

int rbdvbt_dvbt_2k_symbol_deinterleaver(uint32_t in_index, uint32_t *out_index)
{
    rbdvbt_dvbt_2k_model_init();

    if (out_index == NULL || in_index >= RBDVBT_DVBT_2K_DATA_CELLS) {
        return -1;
    }

    *out_index = symbol_deinterleaver_2k[in_index];
    return 0;
}
