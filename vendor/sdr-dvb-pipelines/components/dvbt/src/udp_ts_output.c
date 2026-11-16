#include "udp_ts_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET rbdvbt_socket_t;
#define RBDVBT_INVALID_SOCKET INVALID_SOCKET
#define rbdvbt_socket_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int rbdvbt_socket_t;
#define RBDVBT_INVALID_SOCKET (-1)
#define rbdvbt_socket_close close
#endif

struct rbdvbt_udp_ts_output {
    rbdvbt_socket_t sock;
    char host[96];
    uint16_t port;
};

static int udp_ts_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int udp_ts_init_network(void)
{
#ifdef _WIN32
    static int wsa_ready;
    if (!wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return -1;
        }
        wsa_ready = 1;
    }
#endif
    return 0;
}

static int udp_ts_set_nonblocking(rbdvbt_socket_t sock)
{
#ifdef _WIN32
    u_long mode = 1ul;
    return ioctlsocket(sock, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

int rbdvbt_udp_ts_output_open(rbdvbt_udp_ts_output_t **out,
                              const char *host,
                              uint16_t port)
{
    rbdvbt_udp_ts_output_t *udp;
    struct sockaddr_in addr;

    if (out == NULL || host == NULL || host[0] == '\0' || port == 0u) {
        return -1;
    }
    *out = NULL;

    if (udp_ts_init_network() != 0) {
        fprintf(stderr, "[udp-ts] send_error errno=%d\n", udp_ts_last_error());
        return -1;
    }

    udp = (rbdvbt_udp_ts_output_t *)calloc(1u, sizeof(*udp));
    if (udp == NULL) {
        return -1;
    }
    udp->sock = RBDVBT_INVALID_SOCKET;
    snprintf(udp->host, sizeof(udp->host), "%s", host);
    udp->port = port;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        free(udp);
        return -1;
    }

    udp->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp->sock == RBDVBT_INVALID_SOCKET) {
        fprintf(stderr, "[udp-ts] send_error errno=%d\n", udp_ts_last_error());
        free(udp);
        return -1;
    }
    if (udp_ts_set_nonblocking(udp->sock) != 0) {
        fprintf(stderr, "[udp-ts] send_error errno=%d\n", udp_ts_last_error());
        rbdvbt_socket_close(udp->sock);
        free(udp);
        return -1;
    }
    if (connect(udp->sock, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[udp-ts] send_error errno=%d\n", udp_ts_last_error());
        rbdvbt_socket_close(udp->sock);
        free(udp);
        return -1;
    }

    fprintf(stderr,
            "[udp-ts] enabled host=%s port=%u packet_size=%u\n",
            udp->host,
            (unsigned)udp->port,
            (unsigned)RBDVBT_UDP_TS_PACKET_SIZE);
    *out = udp;
    return 0;
}

int rbdvbt_udp_ts_output_write(rbdvbt_udp_ts_output_t *out,
                               const uint8_t *bytes,
                               size_t byte_count)
{
    const char *p;

    if (out == NULL || bytes == NULL) {
        return -1;
    }

    p = (const char *)bytes;
    while (byte_count > 0u) {
        int chunk = byte_count > RBDVBT_UDP_TS_PACKET_SIZE ?
            (int)RBDVBT_UDP_TS_PACKET_SIZE : (int)byte_count;
        int sent = (int)send(out->sock, p, chunk, 0);
        if (sent != chunk) {
            fprintf(stderr, "[udp-ts] send_error errno=%d\n", udp_ts_last_error());
            return 0;
        }
        p += sent;
        byte_count -= (size_t)sent;
    }
    return 0;
}

void rbdvbt_udp_ts_output_close(rbdvbt_udp_ts_output_t *out)
{
    if (out == NULL) {
        return;
    }
    if (out->sock != RBDVBT_INVALID_SOCKET) {
        rbdvbt_socket_close(out->sock);
        out->sock = RBDVBT_INVALID_SOCKET;
    }
    free(out);
}
