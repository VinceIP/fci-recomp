/* Live STUN check (Q44): send one binding request over UDP to the server named on the command
 * line (host:port), wait up to 3 s (one retry), and print the mapped address the shipping parser
 * (src/fci_stun.c, compiled in here, not copied) returns:
 *
 *   c <ip>:<port>
 *
 * It writes the address nowhere on disk; the user runs it on their own machine. The address is the
 * one a STUN server reports for the source of this request, i.e. the public endpoint behind the
 * user's NAT. Exit 0 = a mapped address was parsed, 1 = no usable answer (no server, no response,
 * or the parser rejected the response).
 *
 *   stun_live_check stun.cloudflare.com:3478
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fci_stun.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define CLOSE_S closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define CLOSE_S close
#endif

/* monotonic milliseconds (for the 3 s deadline) */
static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* send one request, then wait for one datagram within the remaining time. Returns 1 and fills
 * *out_n when a datagram arrived, 0 on timeout or a send failure. */
static int send_wait(int sock, const struct sockaddr_in *srv, const uint8_t *req, long long deadline,
                     uint8_t *out, int bufsz, int *out_n)
{
    long long rem = deadline - now_ms();
    if (rem <= 0)
        return 0;
    if (sendto(sock, (const char *)req, 20, 0, (const struct sockaddr *)srv, sizeof *srv) < 0)
        return 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv;
    tv.tv_sec = (long)(rem / 1000);
    tv.tv_usec = (long)((rem % 1000) * 1000);
    if (select((int)sock + 1, &rfds, NULL, NULL, &tv) <= 0)
        return 0;
    int n = (int)recvfrom(sock, (char *)out, (size_t)bufsz, 0, NULL, NULL);
    if (n <= 0)
        return 0;
    *out_n = n;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: stun_live_check <host:port>\n");
        return 2;
    }
    char host[128], port[16];
    const char *spec = argv[1];
    const char *colon = strrchr(spec, ':');
    if (!colon || (size_t)(colon - spec) >= sizeof host || strlen(colon + 1) >= sizeof port || colon[1] == 0)
    {
        fprintf(stderr, "usage: stun_live_check <host:port>\n");
        return 2;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = 0;
    memcpy(port, colon + 1, strlen(colon + 1) + 1);

#ifdef _WIN32
    WSADATA wsa; /* before getaddrinfo: without it Winsock resolves nothing */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "stun_live_check: WSAStartup failed\n");
        return 1;
    }
#endif
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai)
    {
        fprintf(stderr, "stun_live_check: %s:%s did not resolve\n", host, port);
        return 1;
    }
    struct sockaddr_in srv;
    memcpy(&srv, ai->ai_addr, sizeof srv);
    freeaddrinfo(ai);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "stun_live_check: socket failed\n");
        return 1;
    }

    uint8_t req[20], resp[512];
    uint32_t txn = 0x01020304u; /* the server echoes it back; the parser checks it matches */
    fci_stun_build_request(req, txn);

    long long deadline = now_ms() + 3000; /* 3 s total, one retry */
    uint8_t ip[4];
    uint16_t port_out;
    int xor, n;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (!send_wait(sock, &srv, req, deadline, resp, (int)sizeof resp, &n))
            continue;
        int r = fci_stun_parse_response(resp, n, txn, ip, &port_out, &xor);
        if (r != FCI_STUN_OK)
        {
            fprintf(stderr, "stun_live_check: %s\n", fci_stun_reason(r));
            break;
        }
        printf("c %u.%u.%u.%u:%u\n", ip[0], ip[1], ip[2], ip[3], (unsigned)port_out);
        CLOSE_S(sock);
        return 0;
    }
    CLOSE_S(sock);
    return 1;
}
