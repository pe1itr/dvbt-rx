#include "dvb_t.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static FILE *g_out = NULL;
static uint64_t g_max_samples = 0;
static uint64_t g_written_samples = 0;

static void write_iq_samples(scmplx *samples, int len)
{
    int writable = len;

    if (g_out == NULL || len <= 0) {
        return;
    }

    if (g_max_samples != 0) {
        uint64_t remaining = g_written_samples < g_max_samples ? g_max_samples - g_written_samples : 0;

        if (remaining == 0) {
            return;
        }
        if ((uint64_t)writable > remaining) {
            writable = (int)remaining;
        }
    }

    if (writable > 0) {
        size_t wrote = fwrite(samples, sizeof(scmplx), (size_t)writable, g_out);
        g_written_samples += (uint64_t)wrote;
    }
}

static void null_packet(uint8_t *pkt)
{
    pkt[0] = 0x47;
    pkt[1] = 0x1f;
    pkt[2] = 0xff;
    pkt[3] = 0x10;
    memset(pkt + 4, 0xff, 184);
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    *out = (uint64_t)value;
    return 0;
}

static int parse_u32(const char *text, uint32_t *out)
{
    uint64_t value;

    if (parse_u64(text, &value) != 0 || value > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t)value;
    return 0;
}

static int parse_fec(const char *text)
{
    if (strcmp(text, "1/2") == 0) {
        return FEC_12;
    }
    if (strcmp(text, "2/3") == 0) {
        return FEC_23;
    }
    if (strcmp(text, "3/4") == 0) {
        return FEC_34;
    }
    if (strcmp(text, "5/6") == 0) {
        return FEC_56;
    }
    if (strcmp(text, "7/8") == 0) {
        return FEC_78;
    }
    return -1;
}

static int parse_guard(const char *text)
{
    if (strcmp(text, "1/32") == 0) {
        return GI_132;
    }
    if (strcmp(text, "1/16") == 0) {
        return GI_116;
    }
    if (strcmp(text, "1/8") == 0) {
        return GI_18;
    }
    if (strcmp(text, "1/4") == 0) {
        return GI_14;
    }
    return -1;
}

static int parse_constellation(const char *text)
{
    if (strcmp(text, "qpsk") == 0) {
        return CO_QPSK;
    }
    if (strcmp(text, "16qam") == 0) {
        return CO_16QAM;
    }
    if (strcmp(text, "64qam") == 0) {
        return CO_64QAM;
    }
    return -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --out FILE [--ts FILE] [--null] [--packets N] [--max-samples N]\n"
            "       [--bandwidth HZ] [--ir 1|2|4|8] [--fec 1/2|2/3|3/4|5/6|7/8]\n"
            "       [--gi 1/32|1/16|1/8|1/4] [--constellation qpsk|16qam|64qam]\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    const char *ts_path = NULL;
    int use_null = 0;
    uint64_t packets = 2000;
    FILE *ts = NULL;
    DVBTFormat fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.co = CO_QPSK;
    fmt.sf = SF_NH;
    fmt.gi = GI_132;
    fmt.tm = TM_2K;
    fmt.fec = FEC_12;
    fmt.ir = 1;
    fmt.chan_bw_hz = 333000;
    fmt.freq = 437000000;
    fmt.level = 100;
    fmt.port = 1314;
    fmt.radio = R_PLUTO;
    snprintf(fmt.n_addr, sizeof(fmt.n_addr), "127.0.0.1");

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(arg, "--ts") == 0 && i + 1 < argc) {
            ts_path = argv[++i];
        } else if (strcmp(arg, "--null") == 0) {
            use_null = 1;
        } else if (strcmp(arg, "--packets") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &packets) != 0) {
                fprintf(stderr, "invalid --packets value\n");
                return 1;
            }
        } else if (strcmp(arg, "--max-samples") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &g_max_samples) != 0) {
                fprintf(stderr, "invalid --max-samples value\n");
                return 1;
            }
        } else if (strcmp(arg, "--bandwidth") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &fmt.chan_bw_hz) != 0) {
                fprintf(stderr, "invalid --bandwidth value\n");
                return 1;
            }
        } else if (strcmp(arg, "--ir") == 0 && i + 1 < argc) {
            uint32_t ir;
            if (parse_u32(argv[++i], &ir) != 0 || !(ir == 1 || ir == 2 || ir == 4 || ir == 8)) {
                fprintf(stderr, "invalid --ir value\n");
                return 1;
            }
            fmt.ir = (uint8_t)ir;
        } else if (strcmp(arg, "--fec") == 0 && i + 1 < argc) {
            int fec = parse_fec(argv[++i]);
            if (fec < 0) {
                fprintf(stderr, "invalid --fec value\n");
                return 1;
            }
            fmt.fec = (uint8_t)fec;
        } else if (strcmp(arg, "--gi") == 0 && i + 1 < argc) {
            int gi = parse_guard(argv[++i]);
            if (gi < 0) {
                fprintf(stderr, "invalid --gi value\n");
                return 1;
            }
            fmt.gi = (uint8_t)gi;
        } else if (strcmp(arg, "--constellation") == 0 && i + 1 < argc) {
            int co = parse_constellation(argv[++i]);
            if (co < 0) {
                fprintf(stderr, "invalid --constellation value\n");
                return 1;
            }
            fmt.co = (uint8_t)co;
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", arg);
            usage(argv[0]);
            return 1;
        }
    }

    if (out_path == NULL) {
        fprintf(stderr, "missing required --out\n");
        usage(argv[0]);
        return 1;
    }
    if (!use_null && ts_path == NULL) {
        use_null = 1;
    }

    g_out = fopen(out_path, "wb");
    if (g_out == NULL) {
        fprintf(stderr, "failed to open output %s\n", out_path);
        return 1;
    }

    if (!use_null) {
        ts = fopen(ts_path, "rb");
        if (ts == NULL) {
            fprintf(stderr, "failed to open TS input %s\n", ts_path);
            fclose(g_out);
            return 1;
        }
    }

    dvb_t_open();
    dvb_t_configure(&fmt);
    dvb_t_register_tx(write_iq_samples);

    for (uint64_t p = 0; p < packets; ++p) {
        uint8_t pkt[188];

        if (g_max_samples != 0 && g_written_samples >= g_max_samples) {
            break;
        }

        if (use_null) {
            null_packet(pkt);
        } else {
            size_t got = fread(pkt, 1, sizeof(pkt), ts);

            if (got != sizeof(pkt)) {
                if (ferror(ts)) {
                    fprintf(stderr, "failed while reading TS input\n");
                    break;
                }
                clearerr(ts);
                rewind(ts);
                got = fread(pkt, 1, sizeof(pkt), ts);
                if (got != sizeof(pkt)) {
                    fprintf(stderr, "TS input does not contain a complete 188-byte packet\n");
                    break;
                }
            }
            if (pkt[0] != 0x47) {
                fprintf(stderr, "warning: packet %llu does not start with MPEG-TS sync 0x47\n",
                        (unsigned long long)p);
            }
        }

        dvb_t_encode_and_modulate(pkt);
    }

    dvb_t_close();
    if (ts != NULL) {
        fclose(ts);
    }
    fclose(g_out);
    g_out = NULL;

    fprintf(stderr,
            "[portsdown_iq_dump] out=%s samples=%llu sample_rate=%u bandwidth=%u ir=%u fft_useful=%u gi_samples=%u symbol_samples=%u\n",
            out_path,
            (unsigned long long)g_written_samples,
            fmt.tx_sample_rate,
            fmt.chan_bw_hz,
            fmt.ir,
            2048u * (uint32_t)fmt.ir,
            (2048u * (uint32_t)fmt.ir) / 32u,
            (2048u * (uint32_t)fmt.ir) + (2048u * (uint32_t)fmt.ir) / 32u);

    return 0;
}
