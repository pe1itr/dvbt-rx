/* AArch64 floating-point Viterbi ACS. Preserve the scalar addition order,
 * first-predecessor ties and lowest-index best-state ties. */
#ifndef RBDVBT_VITERBI_NEON_H
#define RBDVBT_VITERBI_NEON_H
#include <arm_neon.h>
#include <stdint.h>

static uint8_t viterbi_neon_exp0[64*4], viterbi_neon_exp1[64*4];

static void viterbi_neon_init(const uint8_t expected[64][2])
{
    for (unsigned s=0; s<64; ++s) {
        for (unsigned b=0; b<4; ++b) {
            viterbi_neon_exp0[s*4+b]=(uint8_t)(expected[s][0]*4+b);
            viterbi_neon_exp1[s*4+b]=(uint8_t)(expected[s][1]*4+b);
        }
    }
}

static void viterbi_acs_neon(const float *old_metrics, float *new_metrics,
                           const float cost[4], uint8_t history[8],
                           float *best_metric, int *best_state)
{
    const uint32_t weight_values[4]={1,2,4,8};
    const uint32x4_t weights=vld1q_u32(weight_values);
    const uint8x16_t costs=vreinterpretq_u8_f32(vld1q_f32(cost));
    for (unsigned s=0; s<64; s+=8) {
        unsigned bits=0;
        for (unsigned half=0; half<8; half+=4) {
            unsigned state=s+half;
            float32x4x2_t previous=vld2q_f32(old_metrics+(state & 31)*2);
            float32x4_t c0=vreinterpretq_f32_u8(vqtbl1q_u8(costs,vld1q_u8(viterbi_neon_exp0+state*4)));
            float32x4_t c1=vreinterpretq_f32_u8(vqtbl1q_u8(costs,vld1q_u8(viterbi_neon_exp1+state*4)));
            float32x4_t m0=vaddq_f32(previous.val[0],c0);
            float32x4_t m1=vaddq_f32(previous.val[1],c1);
            uint32x4_t choose1=vcltq_f32(m1,m0);
            vst1q_f32(new_metrics+state,vbslq_f32(choose1,m1,m0));
            bits|=vaddvq_u32(vandq_u32(choose1,weights))<<half;
        }
        history[s/8]=(uint8_t)bits;
    }
    float best=1.0e30f;
    int state=0;
    for (unsigned s=0; s<64; ++s) {
        if (new_metrics[s]<best) { best=new_metrics[s];state=(int)s; }
    }
    *best_metric=best;
    *best_state=state;
}
#endif
