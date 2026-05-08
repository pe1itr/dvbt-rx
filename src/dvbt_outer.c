#include "dvbt_outer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RS_DATA_LEN 188u
#define RS_PARITY_LEN 16u
#define RS_BLOCK_LEN 204u
#define OUTER_I 12u
#define OUTER_M 17u
#define OUTER_TRANSIENT ((OUTER_I - 1u) * OUTER_M * OUTER_I)
#define SCRAMBLE_SEQ_LEN 1503u

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_tables_ready;
static uint32_t next_status_update_seq;

typedef struct {
    uint8_t *storage;
    uint8_t *rows[OUTER_I];
    uint32_t pos[OUTER_I];
    uint32_t len[OUTER_I];
} outer_deinterleaver_t;

typedef struct {
    uint32_t deint_phase;
    uint32_t rs_phase;
    uint32_t block_phase;
    uint32_t score;
    uint32_t rs_ok;
    uint32_t sync_ok;
    uint32_t blocks;
} outer_candidate_t;

typedef struct {
    uint32_t packets;
    uint32_t sync_bad;
    uint32_t transport_errors;
    uint32_t pat_packets;
    uint32_t pmt_packets;
    uint32_t sdt_packets;
    uint32_t pmt_pid;
    uint32_t service_id;
    uint32_t program_id;
    char service_name[64];
    char service_provider[64];
    uint32_t pcr_pid;
    uint32_t video_pid;
    uint32_t audio_pid;
    uint8_t video_stream_type;
    uint8_t audio_stream_type;
    uint8_t cc_seen[8192];
    uint8_t cc_value[8192];
    uint32_t cc_errors;
} ts_validator_t;

static void gf_tables_init(void)
{
    uint16_t x = 1;

    if (gf_tables_ready) {
        return;
    }

    for (uint32_t i = 0; i < 255u; ++i) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if ((x & 0x100u) != 0u) {
            x = (uint16_t)((x & 0xffu) ^ 0x1du);
        }
    }
    for (uint32_t i = 255u; i < 512u; ++i) {
        gf_exp[i] = gf_exp[i - 255u];
    }

    gf_tables_ready = 1;
}

static uint8_t gf_mul_fast(uint8_t a, uint8_t b)
{
    if (a == 0u || b == 0u) {
        return 0;
    }
    return gf_exp[(uint32_t)gf_log[a] + gf_log[b]];
}

static uint8_t gf_div(uint8_t a, uint8_t b)
{
    int32_t e;

    if (a == 0u) {
        return 0;
    }
    if (b == 0u) {
        return 0;
    }

    e = (int32_t)gf_log[a] - (int32_t)gf_log[b];
    if (e < 0) {
        e += 255;
    }
    return gf_exp[(uint32_t)e];
}

static uint8_t gf_inv(uint8_t a)
{
    if (a == 0u) {
        return 0;
    }
    return gf_exp[(255u - gf_log[a]) % 255u];
}

static uint8_t gf_pow_alpha(int32_t exponent)
{
    while (exponent < 0) {
        exponent += 255;
    }
    return gf_exp[(uint32_t)exponent % 255u];
}

static uint8_t gf_pow_elem(uint8_t a, uint32_t exponent)
{
    if (exponent == 0u) {
        return 1;
    }
    if (a == 0u) {
        return 0;
    }
    return gf_exp[((uint32_t)gf_log[a] * exponent) % 255u];
}

static uint8_t gf_poly_eval(const uint8_t *poly, uint32_t degree, uint8_t x)
{
    uint8_t y = 0;

    for (int32_t i = (int32_t)degree; i >= 0; --i) {
        y = (uint8_t)(gf_mul_fast(y, x) ^ poly[i]);
    }

    return y;
}

static void rs_syndromes_204(const uint8_t *block, uint8_t *syndrome)
{
    gf_tables_init();

    for (uint32_t r = 0; r < RS_PARITY_LEN; ++r) {
        uint8_t x = gf_pow_alpha((int32_t)r);
        uint8_t y = 0;

        for (uint32_t i = 0; i < RS_BLOCK_LEN; ++i) {
            y = (uint8_t)(gf_mul_fast(y, x) ^ block[i]);
        }
        syndrome[r] = y;
    }
}

static int rs_has_syndrome(const uint8_t *syndrome)
{
    for (uint32_t i = 0; i < RS_PARITY_LEN; ++i) {
        if (syndrome[i] != 0u) {
            return 1;
        }
    }
    return 0;
}

static int rs_berlekamp_massey(const uint8_t *syndrome,
                               uint8_t *locator,
                               uint32_t *out_degree)
{
    uint8_t c[RS_PARITY_LEN + 1u];
    uint8_t b[RS_PARITY_LEN + 1u];
    uint32_t l = 0;
    uint32_t m = 1;
    uint8_t bb = 1;

    memset(c, 0, sizeof(c));
    memset(b, 0, sizeof(b));
    c[0] = 1;
    b[0] = 1;

    for (uint32_t n = 0; n < RS_PARITY_LEN; ++n) {
        uint8_t d = syndrome[n];

        for (uint32_t i = 1; i <= l; ++i) {
            d ^= gf_mul_fast(c[i], syndrome[n - i]);
        }

        if (d == 0u) {
            m++;
            continue;
        }

        {
            uint8_t t[RS_PARITY_LEN + 1u];
            uint8_t coef = gf_div(d, bb);

            memcpy(t, c, sizeof(t));
            for (uint32_t i = 0; i + m <= RS_PARITY_LEN; ++i) {
                if (b[i] != 0u) {
                    c[i + m] ^= gf_mul_fast(coef, b[i]);
                }
            }

            if (2u * l <= n) {
                l = n + 1u - l;
                memcpy(b, t, sizeof(b));
                bb = d;
                m = 1;
            } else {
                m++;
            }
        }
    }

    if (l > RS_PARITY_LEN / 2u) {
        return -1;
    }

    memset(locator, 0, RS_PARITY_LEN + 1u);
    memcpy(locator, c, RS_PARITY_LEN + 1u);
    *out_degree = l;
    return 0;
}

static int rs_correct_204(uint8_t *block)
{
    uint8_t syndrome[RS_PARITY_LEN];
    uint8_t locator[RS_PARITY_LEN + 1u];
    uint8_t omega[RS_PARITY_LEN];
    uint32_t degree = 0;
    uint32_t positions[RS_PARITY_LEN / 2u];
    uint32_t position_count = 0;

    rs_syndromes_204(block, syndrome);
    if (!rs_has_syndrome(syndrome)) {
        return 0;
    }

    if (rs_berlekamp_massey(syndrome, locator, &degree) != 0) {
        return -1;
    }
    if (degree == 0u || degree > RS_PARITY_LEN / 2u) {
        return -1;
    }

    for (uint32_t k = 0; k < RS_BLOCK_LEN; ++k) {
        uint8_t x = gf_pow_alpha(203 - (int32_t)k);
        uint8_t inv_x = gf_inv(x);

        if (gf_poly_eval(locator, degree, inv_x) == 0u) {
            if (position_count >= RS_PARITY_LEN / 2u) {
                return -1;
            }
            positions[position_count++] = k;
        }
    }

    if (position_count != degree) {
        return -1;
    }

    memset(omega, 0, sizeof(omega));
    for (uint32_t i = 0; i < RS_PARITY_LEN; ++i) {
        if (syndrome[i] == 0u) {
            continue;
        }
        for (uint32_t j = 0; j <= degree; ++j) {
            if (i + j < RS_PARITY_LEN && locator[j] != 0u) {
                omega[i + j] ^= gf_mul_fast(syndrome[i], locator[j]);
            }
        }
    }

    for (uint32_t p = 0; p < position_count; ++p) {
        uint32_t k = positions[p];
        uint8_t x = gf_pow_alpha(203 - (int32_t)k);
        uint8_t inv_x = gf_inv(x);
        uint8_t numerator = gf_poly_eval(omega, RS_PARITY_LEN - 1u, inv_x);
        uint8_t denominator = 0;
        uint8_t magnitude;

        for (uint32_t i = 1; i <= degree; i += 2u) {
            denominator ^= gf_mul_fast(locator[i], gf_pow_elem(inv_x, i - 1u));
        }
        if (denominator == 0u) {
            return -1;
        }

        magnitude = gf_mul_fast(x, gf_div(numerator, denominator));
        block[k] ^= magnitude;
    }

    rs_syndromes_204(block, syndrome);
    if (rs_has_syndrome(syndrome)) {
        return -1;
    }

    return (int)position_count;
}

static int outer_deinterleaver_init(outer_deinterleaver_t *d)
{
    uint32_t total = 0;

    memset(d, 0, sizeof(*d));
    for (uint32_t i = 0; i < OUTER_I; ++i) {
        d->len[i] = (OUTER_I - 1u - i) * OUTER_M;
        total += d->len[i] > 0u ? d->len[i] : 1u;
    }

    d->storage = calloc(total, sizeof(*d->storage));
    if (d->storage == NULL) {
        return -1;
    }

    total = 0;
    for (uint32_t i = 0; i < OUTER_I; ++i) {
        uint32_t len = d->len[i] > 0u ? d->len[i] : 1u;

        d->rows[i] = &d->storage[total];
        total += len;
    }

    return 0;
}

static uint8_t outer_deinterleaver_step(outer_deinterleaver_t *d,
                                        uint32_t branch,
                                        uint8_t in)
{
    uint32_t len = d->len[branch];
    uint32_t pos;
    uint8_t out;

    if (len == 0u) {
        return in;
    }

    pos = d->pos[branch];
    out = d->rows[branch][pos];
    d->rows[branch][pos] = in;
    d->pos[branch] = (pos + 1u) % len;
    return out;
}

static void outer_deinterleaver_free(outer_deinterleaver_t *d)
{
    free(d->storage);
    memset(d, 0, sizeof(*d));
}

static uint8_t *outer_deinterleave_phase(const uint8_t *inner,
                                         size_t inner_count,
                                         uint32_t phase,
                                         size_t *out_count)
{
    outer_deinterleaver_t d;
    uint8_t *out;
    size_t n = 0;

    if (phase >= OUTER_I || inner_count <= phase) {
        return NULL;
    }
    if (outer_deinterleaver_init(&d) != 0) {
        return NULL;
    }

    out = malloc(inner_count - phase);
    if (out == NULL) {
        outer_deinterleaver_free(&d);
        return NULL;
    }

    for (size_t i = phase; i < inner_count; ++i) {
        uint32_t branch = (uint32_t)(n % OUTER_I);

        out[n++] = outer_deinterleaver_step(&d, branch, inner[i]);
    }

    outer_deinterleaver_free(&d);
    *out_count = n;
    return out;
}

static void build_scrambler_table(uint8_t *table)
{
    uint32_t reg = 0x4a80u;

    for (uint32_t i = 0; i < SCRAMBLE_SEQ_LEN; ++i) {
        uint8_t b = 0;

        for (uint32_t j = 0; j < 8u; ++j) {
            uint32_t out = reg & 1u;

            reg >>= 1;
            out ^= reg & 1u;
            if (out != 0u) {
                reg |= 0x4000u;
            }
            b = (uint8_t)((b << 1) | (out & 1u));
        }
        table[i] = b;
    }
}

static int sync_matches(uint8_t sync, uint32_t block_phase)
{
    return sync == (block_phase == 0u ? 0xb8u : 0x47u);
}

static outer_candidate_t score_candidate(const uint8_t *deint,
                                         size_t deint_count,
                                         uint32_t deint_phase,
                                         uint32_t rs_phase,
                                         uint32_t block_phase)
{
    outer_candidate_t c;
    size_t offset = OUTER_TRANSIENT + rs_phase;
    uint32_t max_blocks = 0;

    memset(&c, 0, sizeof(c));
    c.deint_phase = deint_phase;
    c.rs_phase = rs_phase;
    c.block_phase = block_phase;

    if (offset >= deint_count) {
        return c;
    }

    max_blocks = (uint32_t)((deint_count - offset) / RS_BLOCK_LEN);
    if (max_blocks > 256u) {
        max_blocks = 256u;
    }

    for (uint32_t b = 0; b < max_blocks; ++b) {
        const uint8_t *block = &deint[offset + (size_t)b * RS_BLOCK_LEN];
        uint32_t phase = (block_phase + b) & 7u;

        c.blocks++;
        if (sync_matches(block[0], phase)) {
            c.sync_ok++;
        }
    }

    c.score = c.sync_ok;
    return c;
}

static void descramble_packet(const uint8_t *rs_data,
                              uint32_t block_phase,
                              const uint8_t *scramble,
                              uint8_t *ts)
{
    ts[0] = 0x47u;
    for (uint32_t i = 1; i < RS_DATA_LEN; ++i) {
        uint32_t sc = block_phase * RS_DATA_LEN + i - 1u;

        ts[i] = (uint8_t)(rs_data[i] ^ scramble[sc]);
    }
}

static uint16_t ts_read16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void ts_validator_init(ts_validator_t *v)
{
    memset(v, 0, sizeof(*v));
    v->pmt_pid = 0x1fffu;
    v->service_id = 0;
    v->program_id = 0;
    v->service_name[0] = '\0';
    v->service_provider[0] = '\0';
    v->pcr_pid = 0x1fffu;
    v->video_pid = 0x1fffu;
    v->audio_pid = 0x1fffu;
}

static void ts_validator_parse_pat(ts_validator_t *v, const uint8_t *section, size_t len)
{
    uint16_t section_length;
    size_t end;

    if (len < 8u || section[0] != 0x00u) {
        return;
    }

    section_length = (uint16_t)(ts_read16(&section[1]) & 0x0fffu);
    if ((size_t)section_length + 3u > len || section_length < 9u) {
        return;
    }

    end = 3u + section_length - 4u;
    for (size_t pos = 8u; pos + 4u <= end; pos += 4u) {
        uint16_t program_number = ts_read16(&section[pos]);
        uint16_t pid = (uint16_t)(ts_read16(&section[pos + 2u]) & 0x1fffu);

        if (program_number != 0u) {
            v->service_id = program_number;
            v->program_id = program_number;
            v->pmt_pid = pid;
            return;
        }
    }
}

static void copy_dvb_text(char *out, size_t out_len, const uint8_t *src, size_t src_len)
{
    size_t n;

    if (out_len == 0u) {
        return;
    }
    n = src_len < out_len - 1u ? src_len : out_len - 1u;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = src[i];

        out[i] = (c >= 32u && c <= 126u) ? (char)c : '?';
    }
    out[n] = '\0';
}

static void ts_validator_parse_pmt(ts_validator_t *v, const uint8_t *section, size_t len)
{
    uint16_t section_length;
    uint16_t program_info_length;
    size_t end;
    size_t pos;

    if (len < 12u || section[0] != 0x02u) {
        return;
    }

    section_length = (uint16_t)(ts_read16(&section[1]) & 0x0fffu);
    if ((size_t)section_length + 3u > len || section_length < 13u) {
        return;
    }

    v->pcr_pid = (uint16_t)(ts_read16(&section[8]) & 0x1fffu);
    program_info_length = (uint16_t)(ts_read16(&section[10]) & 0x0fffu);
    pos = 12u + program_info_length;
    end = 3u + section_length - 4u;

    while (pos + 5u <= end) {
        uint8_t stream_type = section[pos];
        uint16_t elementary_pid = (uint16_t)(ts_read16(&section[pos + 1u]) & 0x1fffu);
        uint16_t es_info_length = (uint16_t)(ts_read16(&section[pos + 3u]) & 0x0fffu);

        if (stream_type == 0x1bu && v->video_pid == 0x1fffu) {
            v->video_pid = elementary_pid;
            v->video_stream_type = stream_type;
        } else if (stream_type == 0x0fu && v->audio_pid == 0x1fffu) {
            v->audio_pid = elementary_pid;
            v->audio_stream_type = stream_type;
        }

        pos += 5u + es_info_length;
    }
}

static void ts_validator_parse_sdt(ts_validator_t *v, const uint8_t *section, size_t len)
{
    uint16_t section_length;
    size_t end;
    size_t pos = 11u;

    if (len < 14u || section[0] != 0x42u) {
        return;
    }

    section_length = (uint16_t)(ts_read16(&section[1]) & 0x0fffu);
    if ((size_t)section_length + 3u > len || section_length < 12u) {
        return;
    }

    end = 3u + section_length - 4u;
    while (pos + 5u <= end) {
        uint16_t service_id = ts_read16(&section[pos]);
        uint16_t descriptor_loop_length = (uint16_t)(ts_read16(&section[pos + 3u]) & 0x0fffu);
        size_t desc_pos = pos + 5u;
        size_t desc_end = desc_pos + descriptor_loop_length;

        if (desc_end > end) {
            return;
        }

        while (desc_pos + 2u <= desc_end) {
            uint8_t tag = section[desc_pos];
            uint8_t desc_len = section[desc_pos + 1u];
            const uint8_t *desc = &section[desc_pos + 2u];

            if (desc_pos + 2u + desc_len > desc_end) {
                break;
            }
            if (tag == 0x48u && desc_len >= 3u &&
                (v->service_id == 0u || v->service_id == service_id)) {
                uint8_t provider_len = desc[1];
                size_t service_name_len_pos = 2u + provider_len;

                if (service_name_len_pos < desc_len) {
                    uint8_t service_name_len = desc[service_name_len_pos];
                    const uint8_t *provider = &desc[2];
                    const uint8_t *name = &desc[service_name_len_pos + 1u];

                    if (2u + provider_len <= desc_len &&
                        service_name_len_pos + 1u + service_name_len <= desc_len) {
                        copy_dvb_text(v->service_provider, sizeof(v->service_provider), provider, provider_len);
                        copy_dvb_text(v->service_name, sizeof(v->service_name), name, service_name_len);
                        if (v->service_id == 0u) {
                            v->service_id = service_id;
                            v->program_id = service_id;
                        }
                    }
                }
            }

            desc_pos += 2u + desc_len;
        }

        pos = desc_end;
    }
}

static void ts_validator_parse_psi(ts_validator_t *v,
                                   const uint8_t *packet,
                                   uint16_t pid,
                                   size_t payload_start,
                                   int payload_unit_start)
{
    size_t pos = payload_start;
    size_t section_len;

    if (!payload_unit_start || pos >= RS_DATA_LEN) {
        return;
    }

    pos += packet[pos] + 1u;
    if (pos + 3u > RS_DATA_LEN) {
        return;
    }

    section_len = (size_t)(ts_read16(&packet[pos + 1u]) & 0x0fffu) + 3u;
    if (pos + section_len > RS_DATA_LEN) {
        return;
    }

    if (pid == 0x0000u) {
        v->pat_packets++;
        ts_validator_parse_pat(v, &packet[pos], section_len);
    } else if (pid == v->pmt_pid) {
        v->pmt_packets++;
        ts_validator_parse_pmt(v, &packet[pos], section_len);
    } else if (pid == 0x0011u) {
        v->sdt_packets++;
        ts_validator_parse_sdt(v, &packet[pos], section_len);
    }
}

static void ts_validator_observe(ts_validator_t *v, const uint8_t *packet)
{
    uint16_t pid;
    uint8_t afc;
    uint8_t cc;
    int payload_unit_start;
    int has_payload;
    size_t payload_start = 4u;

    v->packets++;
    if (packet[0] != 0x47u) {
        v->sync_bad++;
        return;
    }

    if ((packet[1] & 0x80u) != 0u) {
        v->transport_errors++;
    }

    payload_unit_start = (packet[1] & 0x40u) != 0u;
    pid = (uint16_t)(((uint16_t)(packet[1] & 0x1fu) << 8) | packet[2]);
    afc = (uint8_t)((packet[3] >> 4) & 0x03u);
    cc = (uint8_t)(packet[3] & 0x0fu);
    has_payload = afc == 1u || afc == 3u;

    if (pid != 0x1fffu) {
        if (!v->cc_seen[pid]) {
            v->cc_seen[pid] = 1u;
            v->cc_value[pid] = cc;
        } else if (has_payload) {
            uint8_t expected = (uint8_t)((v->cc_value[pid] + 1u) & 0x0fu);

            if (cc != expected) {
                v->cc_errors++;
            }
            v->cc_value[pid] = cc;
        } else {
            v->cc_value[pid] = cc;
        }
    }

    if (afc == 2u || afc == 3u) {
        uint8_t adaptation_length;

        if (payload_start >= RS_DATA_LEN) {
            return;
        }
        adaptation_length = packet[payload_start++];
        if (payload_start + adaptation_length > RS_DATA_LEN) {
            return;
        }
        payload_start += adaptation_length;
    }

    if (has_payload && payload_start < RS_DATA_LEN) {
        ts_validator_parse_psi(v, packet, pid, payload_start, payload_unit_start);
    }
}

static void ts_pid_format(uint32_t pid, char *out, size_t out_len)
{
    if (pid == 0x1fffu) {
        snprintf(out, out_len, "-");
    } else {
        snprintf(out, out_len, "0x%04x", pid);
    }
}

static void ts_validator_report(const ts_validator_t *v)
{
    char pmt_pid[16];
    char pcr_pid[16];
    char video_pid[16];
    char audio_pid[16];

    ts_pid_format(v->pmt_pid, pmt_pid, sizeof(pmt_pid));
    ts_pid_format(v->pcr_pid, pcr_pid, sizeof(pcr_pid));
    ts_pid_format(v->video_pid, video_pid, sizeof(video_pid));
    ts_pid_format(v->audio_pid, audio_pid, sizeof(audio_pid));

    fprintf(stderr,
            "[ts] packets=%u sync_bad=%u transport_errors=%u cc_errors=%u pat_packets=%u pmt_packets=%u sdt_packets=%u pmt_pid=%s pcr_pid=%s video_pid=%s video_type=0x%02x audio_pid=%s audio_type=0x%02x service=\"%s\" provider=\"%s\"\n",
            v->packets,
            v->sync_bad,
            v->transport_errors,
            v->cc_errors,
            v->pat_packets,
            v->pmt_packets,
            v->sdt_packets,
            pmt_pid,
            pcr_pid,
            video_pid,
            v->video_stream_type,
            audio_pid,
            v->audio_stream_type,
            v->service_name,
            v->service_provider);
}

static uint32_t status_sqi(const ts_validator_t *v, uint32_t rs_uncorrectable)
{
    uint32_t penalties = rs_uncorrectable + v->transport_errors + v->cc_errors + v->sync_bad;

    if (v->packets == 0u) {
        return 0;
    }
    if (penalties == 0u) {
        return 100;
    }
    if (penalties >= v->packets) {
        return 0;
    }
    return (uint32_t)(100u - (penalties * 100u) / v->packets);
}

static void status_write_json(const rbdvbt_status_context_t *status,
                              const ts_validator_t *v,
                              uint32_t rs_bad,
                              uint32_t rs_corrected,
                              uint32_t rs_corrected_bytes,
                              uint32_t rs_uncorrectable,
                              uint32_t written_packets,
                              const char *stage,
                              uint64_t input_samples)
{
    FILE *f;
    char tmp_path[1024];
    uint32_t sqi;
    uint32_t pe;
    uint32_t locked;
    uint32_t lock_quality;
    uint32_t ssi;
    uint32_t update_seq;
    time_t updated_unix;

    if (status == NULL || status->status_json_path == NULL) {
        return;
    }

    pe = rs_uncorrectable + v->transport_errors + v->cc_errors + v->sync_bad;
    locked = v->packets > 0u &&
        v->sync_bad == 0u &&
        v->transport_errors == 0u &&
        v->cc_errors == 0u &&
        rs_uncorrectable == 0u &&
        v->pat_packets > 0u &&
        v->pmt_packets > 0u;
    sqi = locked ? status_sqi(v, rs_uncorrectable) : 0u;
    lock_quality = locked ? 100u : status->lock_quality;
    if (lock_quality > 100u) {
        lock_quality = 100u;
    }
    ssi = locked ? status->ssi : 0u;
    update_seq = next_status_update_seq++;
    updated_unix = time(NULL);

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", status->status_json_path) >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "status JSON output path too long: %s\n", status->status_json_path);
        return;
    }

    f = fopen(tmp_path, "w");
    if (f == NULL) {
        fprintf(stderr, "failed to open status JSON output: %s\n", status->status_json_path);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"status_update\": %u,\n", update_seq);
    fprintf(f, "  \"updated_unix\": %lld,\n", (long long)updated_unix);
    fprintf(f, "  \"stage\": \"%s\",\n", stage != NULL ? stage : "unknown");
    fprintf(f, "  \"input_samples\": %llu,\n", (unsigned long long)input_samples);
    fprintf(f, "  \"symbol_rate\": \"%s\",\n", status->symbol_rate != NULL ? status->symbol_rate : "unknown");
    fprintf(f, "  \"modulation\": \"%s\",\n", status->modulation != NULL ? status->modulation : "dvb-t");
    fprintf(f, "  \"fft\": \"%s\",\n", status->fft_mode != NULL ? status->fft_mode : "2k");
    fprintf(f, "  \"constellation\": \"%s\",\n", status->constellation != NULL ? status->constellation : "QPSK");
    fprintf(f, "  \"fec\": \"%s\",\n", status->fec != NULL ? status->fec : "unknown");
    fprintf(f, "  \"guard\": \"%s\",\n", status->guard_interval != NULL ? status->guard_interval : "unknown");
    fprintf(f, "  \"locked\": %s,\n", locked ? "true" : "false");
    fprintf(f, "  \"lock_quality\": %u,\n", lock_quality);
    if (isfinite(status->pilot_lock)) {
        fprintf(f, "  \"pilot_lock\": %.5f,\n", status->pilot_lock);
    } else {
        fprintf(f, "  \"pilot_lock\": null,\n");
    }
    fprintf(f, "  \"ssi\": %u,\n", ssi);
    if (isfinite(status->snr_db)) {
        fprintf(f, "  \"sqi\": %u,\n  \"snr\": %.2f,\n", sqi, status->snr_db);
    } else {
        fprintf(f, "  \"sqi\": %u,\n  \"snr\": null,\n", sqi);
    }
    fprintf(f, "  \"pe\": %u,\n", pe);
    fprintf(f, "  \"service_id\": %u,\n", v->service_id);
    fprintf(f, "  \"program_id\": %u,\n", v->program_id);
    fprintf(f, "  \"service_name\": \"");
    for (const char *p = v->service_name; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
        }
        fputc(*p, f);
    }
    fprintf(f, "\",\n");
    fprintf(f, "  \"service_provider\": \"");
    for (const char *p = v->service_provider; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
        }
        fputc(*p, f);
    }
    fprintf(f, "\",\n");
    fprintf(f, "  \"packets\": %u,\n", v->packets);
    fprintf(f, "  \"written_packets\": %u,\n", written_packets);
    fprintf(f, "  \"sync_bad\": %u,\n", v->sync_bad);
    fprintf(f, "  \"transport_errors\": %u,\n", v->transport_errors);
    fprintf(f, "  \"cc_errors\": %u,\n", v->cc_errors);
    fprintf(f, "  \"pat_packets\": %u,\n", v->pat_packets);
    fprintf(f, "  \"pmt_packets\": %u,\n", v->pmt_packets);
    fprintf(f, "  \"sdt_packets\": %u,\n", v->sdt_packets);
    fprintf(f, "  \"pmt_pid\": %u,\n", v->pmt_pid == 0x1fffu ? 8191u : v->pmt_pid);
    fprintf(f, "  \"pcr_pid\": %u,\n", v->pcr_pid == 0x1fffu ? 8191u : v->pcr_pid);
    fprintf(f, "  \"video_pid\": %u,\n", v->video_pid == 0x1fffu ? 8191u : v->video_pid);
    fprintf(f, "  \"audio_pid\": %u,\n", v->audio_pid == 0x1fffu ? 8191u : v->audio_pid);
    fprintf(f, "  \"rs_bad\": %u,\n", rs_bad);
    fprintf(f, "  \"rs_corrected\": %u,\n", rs_corrected);
    fprintf(f, "  \"rs_corrected_bytes\": %u,\n", rs_corrected_bytes);
    fprintf(f, "  \"rs_uncorrectable\": %u\n", rs_uncorrectable);
    fprintf(f, "}\n");
    if (fclose(f) != 0) {
        fprintf(stderr, "failed to close status JSON output: %s\n", tmp_path);
        return;
    }
    if (rename(tmp_path, status->status_json_path) != 0) {
        fprintf(stderr, "failed to publish status JSON output: %s\n", status->status_json_path);
    }
}

void rbdvbt_status_publish_idle(const rbdvbt_status_context_t *status,
                                const char *stage,
                                uint64_t input_samples)
{
    ts_validator_t validator;

    ts_validator_init(&validator);
    status_write_json(status, &validator, 0, 0, 0, 0, 0, stage, input_samples);
}

int rbdvbt_outer_recover_ts(const uint8_t *inner,
                            size_t inner_count,
                            const char *ts_path,
                            const rbdvbt_status_context_t *status)
{
    uint8_t scramble[SCRAMBLE_SEQ_LEN];
    outer_candidate_t best;
    uint8_t *best_deint = NULL;
    size_t best_deint_count = 0;
    FILE *out = NULL;
    uint32_t written = 0;
    uint32_t rs_bad = 0;
    uint32_t rs_corrected = 0;
    uint32_t rs_corrected_bytes = 0;
    uint32_t rs_uncorrectable = 0;
    ts_validator_t validator;
    int rc = -1;

    memset(&best, 0, sizeof(best));
    ts_validator_init(&validator);
    build_scrambler_table(scramble);
    status_write_json(status,
                      &validator,
                      0,
                      0,
                      0,
                      0,
                      0,
                      "outer",
                      status != NULL ? status->input_samples : 0u);

    for (uint32_t deint_phase = 0; deint_phase < OUTER_I; ++deint_phase) {
        size_t deint_count = 0;
        uint8_t *deint = outer_deinterleave_phase(inner, inner_count, deint_phase, &deint_count);

        if (deint == NULL) {
            continue;
        }

        for (uint32_t rs_phase = 0; rs_phase < RS_BLOCK_LEN; ++rs_phase) {
            for (uint32_t block_phase = 0; block_phase < 8u; ++block_phase) {
                outer_candidate_t c = score_candidate(deint,
                                                      deint_count,
                                                      deint_phase,
                                                      rs_phase,
                                                      block_phase);

                if (c.score > best.score) {
                    best = c;
                }
            }
        }

        free(deint);
    }

    if (best.blocks == 0u) {
        fprintf(stderr, "[outer] failed to find byte/RS alignment\n");
        goto done;
    }

    best_deint = outer_deinterleave_phase(inner, inner_count, best.deint_phase, &best_deint_count);
    if (best_deint == NULL) {
        fprintf(stderr, "[outer] failed to rebuild selected byte deinterleaver phase\n");
        goto done;
    }

    if (strcmp(ts_path, "-") == 0) {
        out = stdout;
    } else {
        out = fopen(ts_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "failed to open TS output: %s\n", ts_path);
            goto done;
        }
    }

    {
        size_t offset = OUTER_TRANSIENT + best.rs_phase;
        uint32_t blocks = (uint32_t)((best_deint_count - offset) / RS_BLOCK_LEN);

        for (uint32_t b = 0; b < blocks; ++b) {
            const uint8_t *block = &best_deint[offset + (size_t)b * RS_BLOCK_LEN];
            uint32_t phase = (best.block_phase + b) & 7u;
            uint8_t corrected[RS_BLOCK_LEN];
            uint8_t ts[RS_DATA_LEN];
            int correction_result;

            memcpy(corrected, block, sizeof(corrected));
            correction_result = rs_correct_204(corrected);
            if (correction_result < 0) {
                rs_bad++;
                rs_uncorrectable++;
            } else {
                best.rs_ok++;
                if (correction_result > 0) {
                    rs_corrected++;
                    rs_corrected_bytes += (uint32_t)correction_result;
                }
            }
            descramble_packet(corrected, phase, scramble, ts);
            ts_validator_observe(&validator, ts);
            if (fwrite(ts, 1, sizeof(ts), out) != sizeof(ts)) {
                fprintf(stderr, "failed to write TS output: %s\n", ts_path);
                goto done;
            }
            written++;
            if (status != NULL &&
                status->status_json_path != NULL &&
                status->status_period_packets != 0u &&
                (written % status->status_period_packets) == 0u) {
                status_write_json(status,
                                  &validator,
                                  rs_bad,
                                  rs_corrected,
                                  rs_corrected_bytes,
                                  rs_uncorrectable,
                                  written,
                                  "ts",
                                  status->input_samples);
            }
        }
    }

    fprintf(stderr,
            "[outer] deint_phase=%u rs_phase=%u block_phase=%u scan_blocks=%u scan_rs_ok=%u scan_sync_ok=%u written_packets=%u rs_bad=%u rs_corrected=%u rs_corrected_bytes=%u rs_uncorrectable=%u output=%s\n",
            best.deint_phase,
            best.rs_phase,
            best.block_phase,
            best.blocks,
            best.rs_ok,
            best.sync_ok,
            written,
            rs_bad,
            rs_corrected,
            rs_corrected_bytes,
            rs_uncorrectable,
            ts_path);
    ts_validator_report(&validator);
    status_write_json(status,
                      &validator,
                      rs_bad,
                      rs_corrected,
                      rs_corrected_bytes,
                      rs_uncorrectable,
                      written,
                      "done",
                      status != NULL ? status->input_samples : 0u);

    rc = 0;

done:
    if (out != NULL && out != stdout) {
        fclose(out);
    }
    free(best_deint);
    return rc;
}
