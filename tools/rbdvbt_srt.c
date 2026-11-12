#define _POSIX_C_SOURCE 200809L
#include "srt_output.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The upstream supervisor reads TS from stdin. A datagram socket also works
 * with its poll/read loop, preserving the receiver's nonblocking UDP output. */
static int udp_input(const char *endpoint)
{
    char host[INET_ADDRSTRLEN];
    const char *colon = strrchr(endpoint, ':');
    char *end = NULL;
    if (!colon || colon == endpoint || (size_t)(colon - endpoint) >= sizeof(host))
        goto invalid;
    memcpy(host, endpoint, (size_t)(colon - endpoint));
    host[colon - endpoint] = 0;
    errno = 0;
    long port = strtol(colon + 1, &end, 10);
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (errno || end == colon + 1 || *end || port < 1 || port > 65535 ||
        inet_pton(AF_INET, host, &address.sin_addr) != 1)
        goto invalid;
    address.sin_port = htons((unsigned short)port);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[srt] socket"); return -1; }
    /* No SO_REUSEADDR: a second receiver must not steal an existing stream. */
    int size = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        dup2(fd, STDIN_FILENO) < 0) {
        perror("[srt] UDP input"); close(fd); return -1;
    }
    if (fd != STDIN_FILENO) close(fd);
    /* FFmpeg gets its own pipe on stdin; do not retain this UDP socket there. */
    if (fcntl(STDIN_FILENO, F_SETFD, FD_CLOEXEC) < 0) {
        perror("[srt] cloexec"); return -1;
    }
    fprintf(stderr, "[srt] Listening for MPEG-TS on UDP %s\n", endpoint);
    return 0;
invalid:
    fprintf(stderr, "[srt] --udp-input requires an IPv4 address and port: %s\n", endpoint);
    return -1;
}

int main(int argc, char **argv)
{
    const char *endpoint = NULL;
    int count = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--udp-input")) {
            if (endpoint || i + 1 == argc) {
                fprintf(stderr, "[srt] Specify --udp-input IPv4:PORT once\n");
                return 2;
            }
            endpoint = argv[++i];
        } else {
            if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
                fprintf(stderr, "Input: MPEG-TS on stdin, or --udp-input IPv4:PORT\n");
            argv[count++] = argv[i];
        }
    }
    argv[count] = NULL;
    if (endpoint && udp_input(endpoint) < 0) return 2;
    return srt_output_main(count, argv);
}
