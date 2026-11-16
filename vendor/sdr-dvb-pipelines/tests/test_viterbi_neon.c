/* Run the production NEON kernel against an independently written scalar
 * reference, natively on AArch64 or under qemu-aarch64. The freestanding mode
 * needs no target sysroot/libc; it uses only Linux write/exit system calls. */
#include "../components/dvbt/include/viterbi_neon.h"
#ifndef VITERBI_FREESTANDING
#include <stdio.h>
#endif

static unsigned parity(unsigned value)
{
    unsigned result=0;
    while (value) { result^=value&1;value>>=1; }
    return result;
}

static uint32_t bits(float value)
{
    union { float f;uint32_t u; } v={.f=value};return v.u;
}

static int verify(void)
{
    uint8_t expected_codes[64][2];
    for (unsigned s=0;s<64;++s) for (unsigned p=0;p<2;++p) {
        unsigned reg=(s<<1)|p;
        expected_codes[s][p]=(uint8_t)(parity(reg&0171)|(parity(reg&0133)<<1));
    }
    viterbi_neon_init(expected_codes);
    uint32_t rng=78123;
    for (unsigned trial=0;trial<100000;++trial) {
        float previous[64],cost[4],expected[64],actual[64];
        uint8_t history[8]={0},actual_history[8];
        float best=1.0e30f,actual_best;int state=0,actual_state;
        for (unsigned i=0;i<68;++i) {
            rng=1664525u*rng+1013904223u;
            float value=(float)(int32_t)rng/16777216.0f;
            switch (trial%6) {
            case 0:value=0.0f;break;
            case 1:value=(float)(rng%4);break;
            case 2:value*=10000.0f;break;
            case 3:value=(rng&1) ? -0.0f : 0.0f;break;
            case 4:{ union { uint32_t u;float f; } v={.u=rng&0x807fffff};value=v.f;break; }
            default:break;
            }
            if (i<64) previous[i]=value;else cost[i-64]=value;
        }
        for (unsigned s=0;s<64;++s) {
            unsigned reg0=s<<1,reg1=reg0|1;
            unsigned e0=parity(reg0&0171)|(parity(reg0&0133)<<1);
            unsigned e1=parity(reg1&0171)|(parity(reg1&0133)<<1);
            float a=previous[reg0&63]+cost[e0],b=previous[reg1&63]+cost[e1];
            expected[s]=b<a ? b : a;
            if (b<a) history[s/8]|=(uint8_t)(1u<<(s%8));
            if (expected[s]<best) { best=expected[s];state=(int)s; }
        }
        viterbi_acs_neon(previous,actual,cost,actual_history,&actual_best,&actual_state);
        for (unsigned s=0;s<64;++s) if (bits(actual[s])!=bits(expected[s])) return 1;
        for (unsigned b=0;b<8;++b) if (history[b]!=actual_history[b]) return 2;
        if (bits(best)!=bits(actual_best) || state!=actual_state) return 3;
    }
    return 0;
}

#ifdef VITERBI_FREESTANDING
void _start(void)
{
    int rc=verify();
    static const char ok[]="NEON: 100000 cases / 6400000 exact metrics, survivor bits and best-state decisions passed\n";
    if (!rc) {
        register long x0 __asm__("x0")=1;
        register const char *x1 __asm__("x1")=ok;
        register long x2 __asm__("x2")=sizeof(ok)-1;
        register long x8 __asm__("x8")=64;
        __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x8) : "memory");
    }
    register long x0 __asm__("x0")=rc;
    register long x8 __asm__("x8")=93;
    __asm__ volatile("svc 0" : : "r"(x0),"r"(x8) : "memory");
    __builtin_unreachable();
}
#else
int main(void)
{
    int rc=verify();
    if (!rc) puts("NEON: 100000 cases / 6400000 exact metrics, survivor bits and best-state decisions passed");
    return rc;
}
#endif
