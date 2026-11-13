/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Rob Hardenberg / PE1ITR */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TS_SIZE 188
#define DATAGRAM_SIZE (7 * TS_SIZE)
#define QUEUE_PACKETS 5577 /* Approximately 1 MiB, at most one second. */

typedef struct {
    uint8_t bytes[TS_SIZE];
    int64_t time;
} Packet;

typedef struct {
    Packet queue[QUEUE_PACKETS];
    unsigned head, count;
    uint8_t sync[3 * TS_SIZE], datagram[DATAGRAM_SIZE];
    unsigned used, pending;
    int synchronized, video_pid, video_cc, active;
    int64_t last_video, loss_ms, last_error;
    int output;
    struct sockaddr_in destination;
} Relay;

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping = 1; }

static int64_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int endpoint(const char *text, struct sockaddr_in *address, int destination)
{
    char host[INET_ADDRSTRLEN];
    const char *colon = strrchr(text, ':');
    if (!colon || colon == text || (size_t)(colon - text) >= sizeof(host)) return -1;
    memcpy(host, text, (size_t)(colon - text)); host[colon - text] = 0;
    for (const char *p = colon + 1; *p; ++p) if (*p < '0' || *p > '9') return -1;
    char *end;
    errno = 0;
    long port = strtol(colon + 1, &end, 10);
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    if (errno || end == colon + 1 || *end || port < 1 || port > 65535 ||
        inet_pton(AF_INET, host, &address->sin_addr) != 1) return -1;
    uint32_t ip = ntohl(address->sin_addr.s_addr);
    if (destination && (!ip || ip >= 0xe0000000u)) return -1;
    address->sin_port = htons((unsigned short)port);
    return 0;
}

static void flush(Relay *r)
{
    if (!r->pending) return;
    ssize_t n = sendto(r->output, r->datagram, r->pending, 0,
                       (struct sockaddr *)&r->destination, sizeof(r->destination));
    if (n != (ssize_t)r->pending) {
        int64_t now = milliseconds();
        if (!r->last_error || now - r->last_error >= 1000) {
            fprintf(stderr, "[udp] send failed: %s\n", strerror(errno));
            r->last_error = now;
        }
    }
    r->pending = 0;
}

static void expire(Relay *r, int64_t now)
{
    if (r->active && now - r->last_video >= r->loss_ms) {
        fprintf(stderr, "[udp] Video lost; output silent, waiting for reception\n");
        /* Retain the last PID/CC: repeated stale video must not reopen output. */
        r->active = 0;
        r->head = r->count = r->pending = 0;
        r->used = 0; r->synchronized = 0;
    }
}

static void packet(Relay *r, const uint8_t *p, int64_t now)
{
    unsigned pid = ((p[1] & 31) << 8) | p[2];
    unsigned control = (p[3] >> 4) & 3;
    unsigned at = control & 2 ? 5u + p[4] : 4u;
    /* Inspect only for the gate. Forward every PID and every packet byte,
     * including SI, audio, subtitles, teletext and private data, unchanged. */
    if (!(p[1] & 0x80) && !(p[3] & 0xc0) && (control & 1) && at < TS_SIZE && pid != 8191) {
        if ((p[1] & 0x40) && at + 9 <= TS_SIZE && !p[at] && !p[at+1] &&
            p[at+2] == 1 && (p[at+3] & 0xf0) == 0xe0 && (p[at+6] & 0xc0) == 0x80) {
            if (r->video_pid != (int)pid) r->video_cc = -1;
            r->video_pid = (int)pid;
        }
        if (r->video_pid == (int)pid && r->video_cc != (p[3] & 15)) {
            r->video_cc = p[3] & 15; r->last_video = now;
            if (!r->active) fprintf(stderr, "[udp] Video present; forwarding complete MPEG-TS\n");
            r->active = 1;
        }
    }
    while (r->count && (r->count == QUEUE_PACKETS || now - r->queue[r->head].time > 1000)) {
        r->head = (r->head + 1) % QUEUE_PACKETS; --r->count;
    }
    Packet *queued = &r->queue[(r->head + r->count++) % QUEUE_PACKETS];
    memcpy(queued->bytes, p, TS_SIZE); queued->time = now;
    if (r->active) {
        while (r->count) {
            memcpy(r->datagram + r->pending, r->queue[r->head].bytes, TS_SIZE);
            r->pending += TS_SIZE;
            r->head = (r->head + 1) % QUEUE_PACKETS; --r->count;
            if (r->pending == DATAGRAM_SIZE) flush(r);
        }
    }
}

static void feed(Relay *r, const uint8_t *data, size_t size, int64_t now)
{
    for (size_t i = 0; i < size; ++i) {
        r->sync[r->used++] = data[i];
        if (r->synchronized && r->used == TS_SIZE) {
            if (r->sync[0] == 0x47) { packet(r, r->sync, now); r->used = 0; }
            else r->synchronized = 0;
        }
        if (!r->synchronized && r->used == sizeof(r->sync)) {
            if (r->sync[0] == 0x47 && r->sync[TS_SIZE] == 0x47 && r->sync[2 * TS_SIZE] == 0x47) {
                for (unsigned j = 0; j < 3; ++j) packet(r, r->sync + j * TS_SIZE, now);
                r->synchronized = 1; r->used = 0;
            } else memmove(r->sync, r->sync + 1, --r->used);
        }
    }
    flush(r);
}

int main(int argc, char **argv)
{
    const char *destination = NULL, *input = NULL;
    long loss_ms = 2000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: rbdvbt_udp --destination IPv4:PORT [--udp-input IPv4:PORT] [--loss-ms MS]\n"
                "Forward the complete original MPEG-TS; stdin is used without --udp-input.\n"
                "Output is silent without advancing video payload (default timeout 2000 ms).\n");
            return 0;
        }
        if (i + 1 >= argc) goto invalid;
        if (!strcmp(argv[i], "--destination") && !destination) destination = argv[++i];
        else if (!strcmp(argv[i], "--udp-input") && !input) input = argv[++i];
        else if (!strcmp(argv[i], "--loss-ms")) {
            char *end;
            errno = 0; loss_ms = strtol(argv[++i], &end, 10);
            if (errno || *end || loss_ms < 100 || loss_ms > 60000) goto invalid;
        } else goto invalid;
    }
    struct sockaddr_in output_address, input_address;
    if (!destination || endpoint(destination, &output_address, 1) < 0 ||
        (input && endpoint(input, &input_address, 0) < 0)) goto invalid;
    if (input && (!input_address.sin_addr.s_addr ||
        output_address.sin_addr.s_addr == input_address.sin_addr.s_addr) &&
        output_address.sin_port == input_address.sin_port) goto invalid;
    Relay *r = calloc(1, sizeof(*r));
    if (!r) { perror("[udp] allocate"); return 1; }
    r->loss_ms = loss_ms; r->video_pid = r->video_cc = -1; r->destination = output_address;
    r->output = socket(AF_INET, SOCK_DGRAM, 0);
    int fd = input ? socket(AF_INET, SOCK_DGRAM, 0) : STDIN_FILENO;
    int result = 1;
    if (r->output < 0 || fd < 0) { perror("[udp] socket"); goto done; }
    int flags = fcntl(r->output, F_GETFL, 0);
    if (flags < 0 || fcntl(r->output, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("[udp] nonblocking"); goto done;
    }
    if (input) {
        int size = 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        if (bind(fd, (struct sockaddr *)&input_address, sizeof(input_address)) < 0) {
            perror("[udp] bind"); goto done;
        }
    }
    signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal);
    fprintf(stderr, "[udp] Waiting for video; original MPEG-TS to %s, loss timeout %ld ms\n", destination, loss_ms);
    result = 0;
    while (!stopping) {
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pollfd, 1, 20);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) { perror("[udp] poll"); result = 1; break; }
        int64_t now = milliseconds();
        expire(r, now);
        if (pollfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            uint8_t bytes[65536];
            ssize_t n = read(fd, bytes, sizeof(bytes));
            if (n > 0) feed(r, bytes, (size_t)n, now);
            else if (!n && !input) break;
            else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                perror("[udp] read"); result = 1; break;
            }
        }
    }
done:
    if (input && fd >= 0) close(fd);
    if (r->output >= 0) close(r->output);
    free(r);
    return result;
invalid:
    fprintf(stderr, "[udp] Invalid arguments; use --help. Expected unicast IPv4:PORT and loss-ms 100..60000.\n");
    return 2;
}
