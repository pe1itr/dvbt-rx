#include "visualizer.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define RBVIZ_HEADER_LEN 32u
#define RBVIZ_MAX_PACKET 8192u
#define RBVIZ_MAX_SPECTRUM_VALUES ((RBVIZ_MAX_PACKET - RBVIZ_HEADER_LEN) / sizeof(float))
#define RBVIZ_MAX_CONSTELLATION_POINTS ((RBVIZ_MAX_PACKET - RBVIZ_HEADER_LEN) / (2u * sizeof(float)))

struct rbdvbt_visualizer {
#ifdef _WIN32
    SOCKET fd;
#else
    int fd;
#endif
    struct sockaddr_in addr;
    uint32_t sequence;
    int warned_send;
};

static void put_u16le(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void put_u32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put_f32le(unsigned char *p, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    put_u32le(p, bits);
}

static int parse_endpoint(const char *endpoint, char *host, size_t host_size, uint16_t *port)
{
    const char *text = endpoint;
    const char *colon;
    char *end = NULL;
    unsigned long value;
    size_t host_len;

    if (text == NULL || host == NULL || host_size == 0u || port == NULL) {
        return -1;
    }
    if (strncmp(text, "udp://", 6) == 0) {
        text += 6;
    }
    colon = strrchr(text, ':');
    if (colon == NULL || colon == text || colon[1] == '\0') {
        return -1;
    }
    host_len = (size_t)(colon - text);
    if (host_len >= host_size) {
        return -1;
    }
    memcpy(host, text, host_len);
    host[host_len] = '\0';

    errno = 0;
    value = strtoul(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || value == 0u || value > 65535u) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

int rbdvbt_visualizer_open(rbdvbt_visualizer_t **out, const char *endpoint)
{
    rbdvbt_visualizer_t *viz;
    char host[128];
    uint16_t port;
#ifdef _WIN32
    u_long nonblock = 1u;
    WSADATA wsa;
#else
    int flags;
#endif

    if (out == NULL) {
        return -1;
    }
    *out = NULL;
    if (endpoint == NULL || endpoint[0] == '\0') {
        return 0;
    }
    if (parse_endpoint(endpoint, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "[visualizer] invalid --visualizer-udp endpoint; expected IPv4:PORT\n");
        return -1;
    }

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[visualizer] WSAStartup failed\n");
        return -1;
    }
#endif

    viz = calloc(1u, sizeof(*viz));
    if (viz == NULL) {
        return -1;
    }
#ifdef _WIN32
    viz->fd = INVALID_SOCKET;
#else
    viz->fd = -1;
#endif

#ifdef _WIN32
    viz->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (viz->fd == INVALID_SOCKET) {
#else
    viz->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (viz->fd < 0) {
#endif
        fprintf(stderr, "[visualizer] failed to create UDP socket\n");
        rbdvbt_visualizer_close(viz);
        return -1;
    }

    memset(&viz->addr, 0, sizeof(viz->addr));
    viz->addr.sin_family = AF_INET;
    viz->addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &viz->addr.sin_addr) != 1) {
        fprintf(stderr, "[visualizer] invalid IPv4 address in --visualizer-udp\n");
        rbdvbt_visualizer_close(viz);
        return -1;
    }

#ifdef _WIN32
    ioctlsocket(viz->fd, FIONBIO, &nonblock);
#else
    flags = fcntl(viz->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(viz->fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    *out = viz;
    return 0;
}

void rbdvbt_visualizer_close(rbdvbt_visualizer_t *viz)
{
    if (viz == NULL) {
        return;
    }
#ifdef _WIN32
    if (viz->fd != INVALID_SOCKET) {
        closesocket(viz->fd);
    }
    WSACleanup();
#else
    if (viz->fd >= 0) {
        close(viz->fd);
    }
#endif
    free(viz);
}

int rbdvbt_visualizer_enabled(const rbdvbt_visualizer_t *viz)
{
    return viz != NULL;
}

static void send_packet(rbdvbt_visualizer_t *viz,
                        uint16_t type,
                        uint32_t sample_rate_hz,
                        uint32_t count,
                        float meta0,
                        float meta1,
                        float meta2,
                        const float *payload,
                        size_t payload_floats)
{
    unsigned char packet[RBVIZ_MAX_PACKET];
    size_t payload_bytes = payload_floats * sizeof(float);
    int rc;

    if (viz == NULL || payload == NULL || payload_bytes > sizeof(packet) - RBVIZ_HEADER_LEN) {
        return;
    }

    memcpy(packet, "RBV1", 4u);
    put_u16le(packet + 4u, type);
    put_u16le(packet + 6u, 0u);
    put_u32le(packet + 8u, ++viz->sequence);
    put_u32le(packet + 12u, sample_rate_hz);
    put_u32le(packet + 16u, count);
    put_f32le(packet + 20u, meta0);
    put_f32le(packet + 24u, meta1);
    put_f32le(packet + 28u, meta2);
    memcpy(packet + RBVIZ_HEADER_LEN, payload, payload_bytes);

    rc = (int)sendto(viz->fd,
#ifdef _WIN32
                     (const char *)packet,
#else
                     packet,
#endif
                     (int)(RBVIZ_HEADER_LEN + payload_bytes),
                     0,
                     (const struct sockaddr *)&viz->addr,
                     sizeof(viz->addr));
    if (rc < 0 && !viz->warned_send) {
        fprintf(stderr, "[visualizer] UDP send failed; dropping visualizer frames\n");
        viz->warned_send = 1;
    }
}

void rbdvbt_visualizer_send_spectrum(rbdvbt_visualizer_t *viz,
                                     uint32_t sample_rate_hz,
                                     const float *db_values,
                                     size_t count)
{
    if (count > RBVIZ_MAX_SPECTRUM_VALUES) {
        count = RBVIZ_MAX_SPECTRUM_VALUES;
    }
    send_packet(viz,
                RBDVBT_VISUALIZER_TYPE_SPECTRUM,
                sample_rate_hz,
                (uint32_t)count,
                0.0f,
                0.0f,
                0.0f,
                db_values,
                count);
}

void rbdvbt_visualizer_send_constellation(rbdvbt_visualizer_t *viz,
                                          uint32_t sample_rate_hz,
                                          const float *iq_pairs,
                                          size_t point_count,
                                          float snr_db,
                                          float pilot_lock,
                                          float cfo_hz)
{
    if (point_count > RBVIZ_MAX_CONSTELLATION_POINTS) {
        point_count = RBVIZ_MAX_CONSTELLATION_POINTS;
    }
    send_packet(viz,
                RBDVBT_VISUALIZER_TYPE_CONSTELLATION,
                sample_rate_hz,
                (uint32_t)point_count,
                snr_db,
                pilot_lock,
                cfo_hz,
                iq_pairs,
                point_count * 2u);
}
