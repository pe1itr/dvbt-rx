/* Compare cached acquisition with its allocation-failure (uncached) fallback.
 * Exercise known OFDM pilots, truncated inputs, changing CFO/input, hypothesis
 * ranges and tie ordering, and check the amount of FFT work actually avoided. */
#include <assert.h>
#include <stdlib.h>
#include <fftw3.h>

static int fail_cache;
static unsigned fft_calls, cache_allocations, cache_frees;
static void *cache_pointer;

static void *test_calloc(size_t n, size_t size)
{
    if (fail_cache) return NULL;
    assert(cache_pointer == NULL);
    cache_pointer = calloc(n, size);
    if (cache_pointer != NULL) cache_allocations++;
    return cache_pointer;
}

static void test_free(void *p)
{
    if (p != NULL && p == cache_pointer) {
        cache_pointer = NULL;
        cache_frees++;
    }
    free(p);
}

static void test_execute(fftwf_plan plan)
{
    fft_calls++;
    fftwf_execute(plan);
}

#define calloc test_calloc
#define free test_free
#define fftwf_execute test_execute
#include "../components/dvbt/src/probe_constellation.c"
#undef calloc
#undef free
#undef fftwf_execute

enum { FFT = RBDVBT_DVBT_2K_FFT_SIZE, SYMBOLS = 4, START = 17 };

static void compare(const complexf_t *samples, size_t count, unsigned gi,
                    unsigned rate, double cfo, unsigned scan_symbols,
                    int bin_first, int bin_last, int conj_first, int conj_last,
                    int phase_first, int phase_last, int expect_signal)
{
    rbdvbt_config_t cfg = {0};
    cfg.sample_rate_hz = rate;
    fftwf_complex *in = fftwf_malloc(FFT * sizeof(*in));
    fftwf_complex *out = fftwf_malloc(FFT * sizeof(*out));
    complexf_t shifted[FFT];
    assert(in && out);
    fftwf_plan plan = fftwf_plan_dft_1d(FFT, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    assert(plan);
    int rc[2], bin[2] = {-99,-99}, conj[2] = {-99,-99}, phase[2] = {-99,-99};
    double lock[2] = {-99,-99};
    unsigned calls[2], valid = 0;
    for (unsigned s = 0; s < scan_symbols; ++s) {
        if (START + (size_t)s * (FFT + gi) + gi + FFT >= count) break;
        valid++;
    }
    for (int pass = 0; pass < 2; ++pass) {
        fail_cache = pass;
        fft_calls = 0;
        rc[pass] = scan_dvbt2k_pilot_alignment(&cfg, samples, count, START, gi,
            FFT + gi, cfo, scan_symbols, bin_first, bin_last, conj_first, conj_last,
            phase_first, phase_last, in, out, shifted, plan,
            &bin[pass], &conj[pass], &phase[pass], &lock[pass]);
        calls[pass] = fft_calls;
        assert(cache_pointer == NULL);
    }
    fail_cache = 0;
    unsigned hypotheses = (unsigned)(bin_last-bin_first+1) *
        (unsigned)(conj_last-conj_first+1) * (unsigned)(phase_last-phase_first+1);
    assert(calls[0] == valid);
    assert(calls[1] == valid * hypotheses);
    assert(rc[0] == rc[1]);
    assert(bin[0] == bin[1] && conj[0] == conj[1] && phase[0] == phase[1]);
    assert(memcmp(&lock[0], &lock[1], sizeof(lock[0])) == 0);
    if (valid == 0) {
        assert(rc[0] == -1 && lock[0] == -99);
    } else if (expect_signal) {
        assert(rc[0] == 0);
        if (expect_signal == 1) {
            assert(bin[0] == 1 && phase[0] == 2 && lock[0] > 0.99999);
        } else {
            assert(bin[0] == bin_first && conj[0] == conj_first);
        }
    } else {
        assert(rc[0] == -1);
    }
    fftwf_destroy_plan(plan);
    fftwf_free(in);
    fftwf_free(out);
}

static void check_signal(unsigned gi, unsigned rate, double cfo)
{
    size_t count = START + SYMBOLS * (FFT + gi) + 1;
    complexf_t *samples = calloc(count, sizeof(*samples));
    fftwf_complex *bins = fftwf_malloc(FFT * sizeof(*bins));
    fftwf_complex *time = fftwf_malloc(FFT * sizeof(*time));
    assert(samples && bins && time);
    fftwf_plan inverse = fftwf_plan_dft_1d(FFT, bins, time, FFTW_BACKWARD, FFTW_ESTIMATE);
    assert(inverse);
    for (unsigned s = 0; s < SYMBOLS; ++s) {
        memset(bins, 0, FFT * sizeof(*bins));
        for (unsigned k = 0; k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++k) {
            rbdvbt_dvbt_carrier_role_t role = rbdvbt_dvbt_2k_carrier_role(s+2, k);
            if (role != RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT &&
                role != RBDVBT_DVBT_CARRIER_SCATTERED_PILOT) continue;
            unsigned bin = (rbdvbt_dvbt_2k_logical_to_fft_bin(k) + 1 + FFT/2) % FFT;
            double angle = 0.3 + 0.0004 * ((int)k-852);
            int sign = rbdvbt_dvbt_2k_pilot_sign(k);
            bins[bin][0] = (float)(sign * cos(angle));
            bins[bin][1] = (float)(sign * sin(angle));
        }
        fftwf_execute(inverse);
        size_t base = START + s * (FFT + gi) + gi;
        for (unsigned k = 0; k < FFT; ++k) {
            complexf_t z = {time[k][0]/FFT, time[k][1]/FFT};
            samples[base+k] = c_rotate(z, 2*M_PI*cfo*((double)(base+k)/rate));
        }
    }
    compare(samples,count,gi,rate,cfo,4,-2,2,0,1,0,3,1);
    compare(samples,count,gi,rate,cfo,24,-1,1,1,1,1,3,1);
    compare(samples,count,gi,rate,cfo,4,1,1,0,0,2,2,1);
    compare(samples,START+gi+FFT,gi,rate,cfo,4,-2,2,0,1,0,3,0);
    compare(samples,START+gi+FFT+1,gi,rate,cfo,4,-2,2,0,1,0,3,1);
    compare(samples,count,gi,rate,cfo,0,-2,2,0,1,0,3,0);
    /* Fresh input at the same addresses must not reuse the previous spectra. */
    memset(samples,0,count*sizeof(*samples));
    compare(samples,count,gi,rate,cfo+31.25,4,-2,2,0,1,0,3,0);
    /* Flat real spectra give equal scores for different bin/conjugate choices. */
    for (unsigned s = 0; s < SYMBOLS; ++s) samples[START+s*(FFT+gi)+gi].re = 1;
    compare(samples,count,gi,rate,0.0,4,-2,2,0,1,0,3,2);
    fftwf_destroy_plan(inverse);
    fftwf_free(bins);
    fftwf_free(time);
    free(samples);
}

static void check_carrier_layout(void)
{
    /* Lookup must work before explicit model initialization. At the last
     * active carrier the continual pilot takes precedence over data. */
    assert(rbdvbt_dvbt_2k_carrier_role(UINT32_MAX, 1704) ==
           RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT);
    const uint32_t symbols[] = {0, 1, 2, 3, 67, 68, 271, 272,
                               UINT32_MAX - 3, UINT32_MAX - 2,
                               UINT32_MAX - 1, UINT32_MAX};
    for (size_t s = 0; s < sizeof(symbols)/sizeof(symbols[0]); ++s) {
        uint32_t symbol = symbols[s];
        unsigned data = 0, tps = 0, pilots = 0;
        for (uint32_t k = 0; k < RBDVBT_DVBT_2K_ACTIVE_CARRIERS; ++k) {
            rbdvbt_dvbt_carrier_role_t role = rbdvbt_dvbt_2k_carrier_role(symbol, k);
            assert(role == rbdvbt_dvbt_2k_carrier_role(symbol % 4u, k));
            if (role == RBDVBT_DVBT_CARRIER_DATA) {
                uint32_t mapped = UINT32_MAX;
                assert(rbdvbt_dvbt_2k_data_carrier(symbol, data, &mapped) == 0);
                assert(mapped == k);
                ++data;
            } else if (role == RBDVBT_DVBT_CARRIER_TPS) {
                ++tps;
            } else {
                assert(role == RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT ||
                       role == RBDVBT_DVBT_CARRIER_SCATTERED_PILOT);
                ++pilots;
            }
        }
        assert(data == 1512 && tps == 17 && pilots == 176);
        assert(rbdvbt_dvbt_2k_carrier_role(symbol, 34) == RBDVBT_DVBT_CARRIER_TPS);
        assert(rbdvbt_dvbt_2k_carrier_role(symbol, 0) ==
               (symbol % 4u == 0 ? RBDVBT_DVBT_CARRIER_SCATTERED_PILOT :
                                  RBDVBT_DVBT_CARRIER_CONTINUAL_PILOT));
        assert(rbdvbt_dvbt_2k_carrier_role(symbol, 1705) == RBDVBT_DVBT_CARRIER_UNUSED);
        assert(rbdvbt_dvbt_2k_carrier_role(symbol, UINT32_MAX) == RBDVBT_DVBT_CARRIER_UNUSED);
        uint32_t untouched = UINT32_MAX;
        assert(rbdvbt_dvbt_2k_data_carrier(symbol, 1512, &untouched) == -1);
        assert(untouched == UINT32_MAX);
        assert(rbdvbt_dvbt_2k_data_carrier(symbol, 0, NULL) == -1);
    }
}

static void check_iq_ring_overrun_between_reads(void)
{
    enum { CAPACITY = 8 };
    complexf_t output[CAPACITY];
    rbdvbt_complex_t input[20];
    uint64_t dropped = 99;
    for (unsigned i = 0; i < 20; ++i) {
        input[i].i = (float)i;
        input[i].q = -(float)i;
    }
    live_iq_ring.items = calloc(CAPACITY, sizeof(*live_iq_ring.items));
    assert(live_iq_ring.items);
    live_iq_ring.cap = CAPACITY;
    live_iq_ring.eof = 1; /* No hardware/reader thread; short reads must return. */

    live_iq_ring_push(input, 3);
    assert(live_iq_ring_read(output, 2, &dropped) == 2 && dropped == 0);
    for (unsigned i = 0; i < 2; ++i)
        assert(output[i].re == (float)i && output[i].im == -(float)i);

    /* Overflow occurs entirely between consumer reads, with wraparound. */
    live_iq_ring_push(input + 3, 8);
    assert(live_iq_ring_read(output, CAPACITY, &dropped) == CAPACITY && dropped == 1);
    for (unsigned i = 0; i < CAPACITY; ++i)
        assert(output[i].re == (float)(i + 3) && output[i].im == -(float)(i + 3));

    /* Input larger than the ring, followed by another overflow before read. */
    live_iq_ring_push(input + 8, 12);
    assert(live_iq_ring_read(output, 5, &dropped) == 5 && dropped == 4);
    for (unsigned i = 0; i < 5; ++i)
        assert(output[i].re == (float)(i + 12) && output[i].im == -(float)(i + 12));
    live_iq_ring_push(input, 7);
    assert(live_iq_ring_read(output, CAPACITY, &dropped) == CAPACITY && dropped == 2);
    assert(output[0].re == 19.0f && output[0].im == -19.0f);
    for (unsigned i = 1; i < CAPACITY; ++i)
        assert(output[i].re == (float)(i - 1) && output[i].im == -(float)(i - 1));
    assert(live_iq_ring_read(output, CAPACITY, &dropped) == 0 && dropped == 0);
    assert(live_iq_ring.overrun_samples == 7);
    free(live_iq_ring.items);
    live_iq_ring.items = NULL;
    live_iq_ring.cap = live_iq_ring.head = live_iq_ring.count = live_iq_ring.max_count = 0;
    live_iq_ring.overrun_samples = live_iq_ring.reported_overrun_samples = 0;
    live_iq_ring.eof = 0;
}

int main(void)
{
    check_iq_ring_overrun_between_reads();
    check_carrier_layout();
    rbdvbt_dvbt_2k_model_init();
    check_signal(FFT/32,40000,0.0);      /* 35 kHz BW */
    check_signal(FFT/16,45714,17.125);   /* 40 kHz BW, different GI */
    check_signal(FFT/8,571429,-239.5);  /* 500 kHz BW */
    assert(cache_allocations == cache_frees && cache_allocations > 0);
    puts("DVB-T frontend: exact cached/fallback alignment, FFT reuse, GI/CFO changes, truncation, ties and cleanup passed");
    return 0;
}
