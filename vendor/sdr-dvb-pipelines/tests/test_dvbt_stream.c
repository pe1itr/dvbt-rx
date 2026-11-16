/* Exercise the real streaming Viterbi implementation with independently encoded
 * DVB mother-code bits, puncturing, errors, and arbitrary whole-symbol chunks. */
#include "../components/dvbt/src/probe_constellation.c"
#include <assert.h>

static unsigned parity(unsigned v) {
    unsigned p=0;
    while (v) { p^=v&1; v>>=1; }
    return p;
}

static void check(rbdvbt_fec_t fec, int errors) {
    /* Whole puncturing periods for every rate, including the final chunk. */
    enum { BYTES=22680, MAX_BITS=BYTES*16 };
    uint8_t input[BYTES], bits[MAX_BITS], output[BYTES];
    uint32_t random=1234567;
    for (unsigned i=0;i<BYTES;++i) {
        random=1664525u*random+1013904223u;
        input[i]=i<32 ? 0 : random>>24;
    }
    unsigned reg=0, n=0;
    /* Independent encoder masks from DVB BlueBook A012, section 4.3.3/table 2. */
    const char *x, *y;
    switch (fec) {
    case RBDVBT_FEC_1_2: x="1"; y="1"; break;
    case RBDVBT_FEC_2_3: x="10"; y="11"; break;
    case RBDVBT_FEC_3_4: x="101"; y="110"; break;
    case RBDVBT_FEC_5_6: x="10101"; y="11010"; break;
    case RBDVBT_FEC_7_8: x="1000101"; y="1111010"; break;
    default: abort();
    }
    unsigned period=strlen(x);
    for (unsigned i=0;i<BYTES*8;++i) {
        reg=(reg>>1) | (((input[i/8]>>(7-i%8))&1)<<6);
        unsigned phase=i%period;
        if (x[phase]=='1') bits[n++]=parity(reg & 0171);
        if (y[phase]=='1') bits[n++]=parity(reg & 0133);
    }
    if (errors) for (unsigned i=1001;i<n;i+=2003) bits[i]^=1;
    qpsk_dibit_t *dibits=calloc(n/2,sizeof(*dibits)); assert(dibits);
    for (unsigned i=0;i<n/2;++i) {
        dibits[i].dibit=bits[2*i] | (bits[2*i+1]<<1);
        dibits[i].x_cost[0]=bits[2*i] ? 1 : 0;
        dibits[i].x_cost[1]=bits[2*i] ? 0 : 1;
        dibits[i].y_cost[0]=bits[2*i+1] ? 1 : 0;
        dibits[i].y_cost[1]=bits[2*i+1] ? 0 : 1;
    }
    assert(live_viterbi_stream_supported(fec));
    for (unsigned split=0;split<2;++split) {
        live_viterbi_stream_reset();
        size_t position=0,total=0;
        for (unsigned chunk=0;position<n/2;++chunk) {
            size_t count=split ? 1512*(1+chunk%3) : n/2;
            if (count>n/2-position) count=n/2-position;
            uint8_t *decoded=NULL; size_t decoded_size=0;
            assert(!live_viterbi_stream_decode_bytes(fec,dibits+position,count,position!=0,
                &decoded,&decoded_size,NULL,NULL,NULL));
            assert(total+decoded_size<=sizeof(output));
            memcpy(output+total,decoded,decoded_size); total+=decoded_size; position+=count;
        }
        assert(total==BYTES-viterbi_traceback_bytes(fec));
        assert(!memcmp(output,input,total));
    }
    free(dibits);
}

/* A separately written scalar reference checks metric bytes and tie decisions,
 * including arbitrary incoming path metrics, without calling production ACS. */
static void check_acs(void) {
#if defined(RBDVBT_HAVE_VITERBI_AVX2) || defined(RBDVBT_HAVE_VITERBI_NEON)
#ifdef RBDVBT_HAVE_VITERBI_AVX2
    if (!__builtin_cpu_supports("avx2")) return;
#endif
    uint32_t random=78123;
    for (unsigned trial=0;trial<10000;++trial) {
        float previous[64],cost[4],expected[64],actual[64];
        uint8_t history[8]={0},actual_history[8];
        float best=1.0e30f,actual_best;int state=0,actual_state;
        for (unsigned i=0;i<68;++i) {
            random=1664525u*random+1013904223u;
            float value=(float)(int32_t)random/16777216.0f;
            if (trial%4==0) value=0.0f;
            if (trial%4==1) value=(float)(random%4);
            if (trial%4==2) value*=10000.0f;
            if (i<64) previous[i]=value;else cost[i-64]=value;
        }
        for (unsigned s=0;s<64;++s) {
            unsigned reg0=s<<1,reg1=reg0|1;
            unsigned e0=parity(reg0&0171)|(parity(reg0&0133)<<1);
            unsigned e1=parity(reg1&0171)|(parity(reg1&0133)<<1);
            float a=previous[reg0&63]+cost[e0];
            float b=previous[reg1&63]+cost[e1];
            expected[s]=b<a ? b : a;
            if (b<a) history[s/8]|=(uint8_t)(1u<<(s%8));
            if (expected[s]<best) { best=expected[s];state=(int)s; }
        }
#ifdef RBDVBT_HAVE_VITERBI_AVX2
        viterbi_acs_avx2(previous,actual,cost,actual_history,&actual_best,&actual_state);
#else
        viterbi_acs_neon(previous,actual,cost,actual_history,&actual_best,&actual_state);
#endif
        assert(!memcmp(expected,actual,sizeof(expected)));
        assert(!memcmp(history,actual_history,sizeof(history)));
        assert(!memcmp(&best,&actual_best,sizeof(best)) && state==actual_state);
    }
#endif
}

#if defined(RBDVBT_HAVE_VITERBI_AVX2) || defined(RBDVBT_HAVE_VITERBI_NEON)
static void check_soft_stream(rbdvbt_fec_t fec) {
#ifdef RBDVBT_HAVE_VITERBI_AVX2
    if (!__builtin_cpu_supports("avx2")) return;
#endif
    enum { N=1512*12 };
    qpsk_dibit_t *input=calloc(N,sizeof(*input));assert(input);
    uint8_t expected[N];size_t expected_size=0;
    uint32_t rng=91821;
    int selected=viterbi_use_simd;
    for (unsigned mode=0;mode<3;++mode) {
        for (unsigned i=0;i<N;++i) {
            float *costs[]={input[i].x_cost,input[i].y_cost};
            for(unsigned xy=0;xy<2;++xy) for(unsigned bit=0;bit<2;++bit) {
                rng=1664525u*rng+1013904223u;
                costs[xy][bit]=mode==0 ? 0 : (float)(rng>>8)/16777216.0f*(mode==1 ? 1 : 10000);
                if (i>=N/3 && i<N/2) costs[xy][bit]=0;
            }
        }
        for (unsigned simd=0;simd<2;++simd) {
            viterbi_use_simd=simd;live_viterbi_stream_reset();size_t total=0;
            for (unsigned offset=0;offset<N;offset+=1512) {
                uint8_t *bytes;size_t count;
                assert(!live_viterbi_stream_decode_bytes(fec,input+offset,1512,offset!=0,
                    &bytes,&count,NULL,NULL,NULL));
                assert(total+count<=sizeof(expected));
                if (!simd) memcpy(expected+total,bytes,count);
                else assert(!memcmp(expected+total,bytes,count));
                total+=count;
            }
            if (!simd) expected_size=total;else assert(total==expected_size);
        }
    }
    viterbi_use_simd=selected;free(input);
}
#endif

/* A live split must preserve both sides of the delayed FIR window. */
static void check_resampler_chunks(unsigned sr, int channel_filter)
{
    enum { COUNT = 20000 };
    complexf_t *input = malloc(COUNT * sizeof(*input));
    complexf_t *whole = NULL;
    size_t whole_count = 0, output_pos = 0;
    double ratio = (sr * 8.0 / 7.0) / 1010526.0;
    unsigned rate = (unsigned)lrint(sr * 8.0 / 7.0);
    assert(input != NULL);
    for (unsigned i = 0; i < COUNT; ++i) {
        input[i].re = (float)(sin(i * 0.12) + 0.3 * cos(i * 0.63));
        input[i].im = (float)(cos(i * 0.17) - 0.2 * sin(i * 0.55));
    }
    live_resampler_release(&live_resampler_state);
    assert(resample_sinc_ratio_live(input, COUNT, ratio, 1010526, rate,
                                    channel_filter, &whole, &whole_count) == 0);
    live_resampler_release(&live_resampler_state);
    for (unsigned offset = 0, block = 0; offset < COUNT; ++block) {
        size_t count = 701u + block % 7u * 93u;
        size_t part_count = 0;
        complexf_t *part = NULL;
        if (count > COUNT - offset) count = COUNT - offset;
        assert(resample_sinc_ratio_live(input + offset, count, ratio, 1010526,
                                        rate, channel_filter, &part, &part_count) == 0);
        assert(output_pos + part_count <= whole_count);
        for (size_t j = 0; j < part_count; ++j) {
            assert(fabsf(part[j].re - whole[output_pos + j].re) < 1e-5f);
            assert(fabsf(part[j].im - whole[output_pos + j].im) < 1e-5f);
        }
        output_pos += part_count;
        offset += (unsigned)count;
        free(part);
    }
    assert(output_pos == whole_count);
    free(whole);
    free(input);
    live_resampler_release(&live_resampler_state);
}

int main(void) {
    rbdvbt_log_set_level(RBDVBT_LOG_ERROR);
    check_resampler_chunks(250000, 0);
    check_resampler_chunks(333000, 0);
    check_resampler_chunks(250000, 1);
    check_resampler_chunks(333000, 1);
    viterbi_init_tables();
#if defined(RBDVBT_HAVE_VITERBI_AVX2) || defined(RBDVBT_HAVE_VITERBI_NEON)
#ifdef RBDVBT_HAVE_VITERBI_AVX2
    assert(viterbi_use_simd==!!(__builtin_cpu_supports("avx2") && !getenv("LINRAD_DVBT_VITERBI_SCALAR")));
#else
    assert(viterbi_use_simd==!getenv("LINRAD_DVBT_VITERBI_SCALAR"));
#endif
#endif
    puts("DVB-T live resampler: whole/split continuity at 250k and 333k passed");
    check_acs();
    const rbdvbt_fec_t rates[]={RBDVBT_FEC_1_2,RBDVBT_FEC_2_3,RBDVBT_FEC_3_4,RBDVBT_FEC_5_6,RBDVBT_FEC_7_8};
    for (unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);++i)
        for (int errors=0;errors<2;++errors) check(rates[i],errors);
#if defined(RBDVBT_HAVE_VITERBI_AVX2) || defined(RBDVBT_HAVE_VITERBI_NEON)
    for (unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);++i) check_soft_stream(rates[i]);
#endif
    puts("DVB-T streaming FEC 1/2, 2/3, 3/4, 5/6, 7/8: exact bytes, puncturing, errors, chunk continuity and reset passed");
    return 0;
}
