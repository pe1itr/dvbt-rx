#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

typedef struct replay_config {
    const char *input_path;
    uint32_t sample_rate_hz;
    uint64_t offset_samples;
    uint64_t max_samples;
    uint32_t chunk_samples;
    size_t bytes_per_sample;
    int quiet;
} replay_config_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --in FILE --sample-rate HZ [--input-format s16|u8]\n"
            "       [--offset-s SEC] [--duration-s SEC] [--max-samples N]\n"
            "       [--chunk-samples N] [--quiet]\n\n"
            "Replay raw complex IQ to stdout at the requested sample rate.\n"
            "All diagnostics go to stderr; stdout contains only IQ bytes.\n",
            argv0);
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

static int parse_double_seconds(const char *text, double *out)
{
    char *end = NULL;
    double value;

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || value < 0.0) {
        return -1;
    }
    *out = value;
    return 0;
}

static uint64_t seconds_to_samples(double seconds, uint32_t sample_rate_hz)
{
    return (uint64_t)(seconds * (double)sample_rate_hz + 0.5);
}

static double monotonic_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

static void sleep_until(double target_time)
{
    for (;;) {
        double now = monotonic_seconds();
        double remaining = target_time - now;

        if (remaining <= 0.0) {
            return;
        }
#ifdef _WIN32
        DWORD ms = (DWORD)(remaining * 1000.0);
        if (ms == 0) {
            ms = 1;
        }
        Sleep(ms);
#else
        struct timespec req;

        req.tv_sec = (time_t)remaining;
        req.tv_nsec = (long)((remaining - (double)req.tv_sec) * 1000000000.0);
        if (req.tv_nsec < 0) {
            req.tv_nsec = 0;
        }
        nanosleep(&req, NULL);
#endif
    }
}

static int parse_args(int argc, char **argv, replay_config_t *cfg)
{
    int i;

    memset(cfg, 0, sizeof(*cfg));
    cfg->bytes_per_sample = 4u;
    cfg->chunk_samples = 4096u;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if ((strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0)) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--in") == 0 && i + 1 < argc) {
            cfg->input_path = argv[++i];
        } else if (strcmp(arg, "--sample-rate") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->sample_rate_hz) != 0 || cfg->sample_rate_hz == 0u) {
                fprintf(stderr, "invalid --sample-rate value\n");
                return -1;
            }
        } else if (strcmp(arg, "--input-format") == 0 && i + 1 < argc) {
            const char *fmt = argv[++i];

            if (strcmp(fmt, "s16") == 0) {
                cfg->bytes_per_sample = 4u;
            } else if (strcmp(fmt, "u8") == 0) {
                cfg->bytes_per_sample = 2u;
            } else {
                fprintf(stderr, "invalid --input-format value\n");
                return -1;
            }
        } else if (strcmp(arg, "--offset-s") == 0 && i + 1 < argc) {
            double seconds;

            if (cfg->sample_rate_hz == 0u) {
                fprintf(stderr, "--sample-rate must appear before --offset-s\n");
                return -1;
            }
            if (parse_double_seconds(argv[++i], &seconds) != 0) {
                fprintf(stderr, "invalid --offset-s value\n");
                return -1;
            }
            cfg->offset_samples = seconds_to_samples(seconds, cfg->sample_rate_hz);
        } else if (strcmp(arg, "--duration-s") == 0 && i + 1 < argc) {
            double seconds;

            if (cfg->sample_rate_hz == 0u) {
                fprintf(stderr, "--sample-rate must appear before --duration-s\n");
                return -1;
            }
            if (parse_double_seconds(argv[++i], &seconds) != 0) {
                fprintf(stderr, "invalid --duration-s value\n");
                return -1;
            }
            cfg->max_samples = seconds_to_samples(seconds, cfg->sample_rate_hz);
        } else if (strcmp(arg, "--max-samples") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &cfg->max_samples) != 0) {
                fprintf(stderr, "invalid --max-samples value\n");
                return -1;
            }
        } else if (strcmp(arg, "--chunk-samples") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->chunk_samples) != 0 || cfg->chunk_samples == 0u) {
                fprintf(stderr, "invalid --chunk-samples value\n");
                return -1;
            }
        } else if (strcmp(arg, "--quiet") == 0) {
            cfg->quiet = 1;
        } else if (arg[0] != '-' && cfg->input_path == NULL) {
            cfg->input_path = arg;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", arg);
            return -1;
        }
    }

    if (cfg->input_path == NULL) {
        fprintf(stderr, "missing --in FILE\n");
        return -1;
    }
    if (cfg->sample_rate_hz == 0u) {
        fprintf(stderr, "missing --sample-rate HZ\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    replay_config_t cfg;
    FILE *in;
    unsigned char *buf;
    uint64_t written_samples = 0u;
    double start_time;
    int rc = 0;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (parse_args(argc, argv, &cfg) != 0) {
        usage(argv[0]);
        return 2;
    }

    in = fopen(cfg.input_path, "rb");
    if (in == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", cfg.input_path, strerror(errno));
        return 1;
    }

    if (cfg.offset_samples != 0u) {
        uint64_t offset_bytes = cfg.offset_samples * (uint64_t)cfg.bytes_per_sample;

        if (fseek(in, (long)offset_bytes, SEEK_SET) != 0) {
            fprintf(stderr, "failed to seek %s to byte offset %llu\n",
                    cfg.input_path,
                    (unsigned long long)offset_bytes);
            fclose(in);
            return 1;
        }
    }

    buf = malloc((size_t)cfg.chunk_samples * cfg.bytes_per_sample);
    if (buf == NULL) {
        fprintf(stderr, "failed to allocate replay buffer\n");
        fclose(in);
        return 1;
    }

    if (!cfg.quiet) {
        fprintf(stderr,
                "[iq-replay] file=%s sample_rate=%u bytes_per_sample=%zu offset_samples=%llu max_samples=%llu chunk_samples=%u\n",
                cfg.input_path,
                cfg.sample_rate_hz,
                cfg.bytes_per_sample,
                (unsigned long long)cfg.offset_samples,
                (unsigned long long)cfg.max_samples,
                cfg.chunk_samples);
    }

    start_time = monotonic_seconds();
    for (;;) {
        uint64_t remaining_samples = cfg.max_samples != 0u ?
            (cfg.max_samples > written_samples ? cfg.max_samples - written_samples : 0u) :
            UINT64_MAX;
        uint32_t want_samples = cfg.chunk_samples;
        size_t got_bytes;
        size_t got_samples;
        double target_time;

        if (remaining_samples == 0u) {
            break;
        }
        if ((uint64_t)want_samples > remaining_samples) {
            want_samples = (uint32_t)remaining_samples;
        }

        got_bytes = fread(buf, cfg.bytes_per_sample, want_samples, in);
        got_samples = got_bytes;
        if (got_samples == 0u) {
            break;
        }

        if (fwrite(buf, cfg.bytes_per_sample, got_samples, stdout) != got_samples) {
            fprintf(stderr, "failed to write IQ to stdout\n");
            rc = 1;
            break;
        }
        fflush(stdout);

        written_samples += (uint64_t)got_samples;
        target_time = start_time + (double)written_samples / (double)cfg.sample_rate_hz;
        sleep_until(target_time);
    }

    if (!cfg.quiet) {
        double elapsed = monotonic_seconds() - start_time;
        fprintf(stderr,
                "[iq-replay] done samples=%llu elapsed=%.3fs effective_rate=%.1f samples/s\n",
                (unsigned long long)written_samples,
                elapsed,
                elapsed > 0.0 ? (double)written_samples / elapsed : 0.0);
    }

    free(buf);
    fclose(in);
    return rc;
}
