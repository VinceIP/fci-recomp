/* The TURN probe: a standalone console exe built from the relay client (src/fci_turn.c), for
 * checking it outside the game. Build it with the same toolchain as tools/compile.py (the
 * host group's flags, minus -g):
 *
 *   x86_64-w64-mingw32-clang -std=gnu11 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
 *       -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES -fno-strict-aliasing -fwrapv -O2 \
 *       -Isrc -o turn_probe.exe tools/turn_probe.c src/fci_turn.c -lws2_32
 *
 *   turn_probe --selftest
 *       the RFC 5769 2.1 and 2.4 vectors (tests/test_stun_vectors.py): exit 0 when the
 *       MESSAGE-INTEGRITY and FINGERPRINT code matches the published bytes.
 *
 *   turn_probe <server_hostport> <user> <pass> [seconds=30] [peer_hostport]
 *       the relay echo: allocate from the server, print the relayed address, add the peer
 *       (when given), and print every frame the server relays to us for up to [seconds].
 *       Exit 0 when at least one frame arrived, 1 otherwise. tests/test_turn_probe.py runs
 *       this against tools/fake_turn.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "fci_turn.h"

static int parse_peer(const char *spec, struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char host[100];
    char port[8];
    const char *colon = strrchr(spec, ':');
    int rc;
    if (!colon || colon == spec || colon[1] == 0)
        return 0;
    strncpy_s(host, sizeof(host), spec, (size_t)(colon - spec));
    strncpy_s(port, sizeof(port), colon + 1, 7);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        if (res)
            freeaddrinfo(res);
        return 0;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return 1;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 2;
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        int ok = fci_turn_selftest();
        printf("turn_probe: selftest %s\n", ok ? "ok" : "FAILED");
        WSACleanup();
        return ok ? 0 : 1;
    }
    if (argc < 4) {
        printf("usage: turn_probe <server_hostport> <user> <pass> [seconds] [peer_hostport]\n");
        printf("       turn_probe --selftest\n");
        WSACleanup();
        return 2;
    }
    int seconds = 30;
    if (argc > 4)
        seconds = atoi(argv[4]);
    struct sockaddr_in peer = {0};
    int have_peer = 0;
    if (argc > 5)
        have_peer = parse_peer(argv[5], &peer);
    if (!fci_turn_start(argv[1], argv[2], argv[3])) {
        printf("turn_probe: start failed\n");
        WSACleanup();
        return 1;
    }
    {
        ULONGLONG start = GetTickCount64();
        int frames = 0;
        int printed_addr = 0;
        while (GetTickCount64() - start < (ULONGLONG)seconds * 1000u) {
            char buf[2048];
            struct sockaddr_in from;
            fci_turn_tick(GetTickCount64());
            if (!printed_addr && fci_turn_state() == FCI_TURN_READY) {
                struct sockaddr_in rel;
                if (fci_turn_relayed(&rel)) {
                    printf("turn_probe: ready relayed=%s:%d\n", inet_ntoa(rel.sin_addr),
                           ntohs(rel.sin_port));
                    fflush(stdout);
                    printed_addr = 1;
                    if (have_peer)
                        fci_turn_add_peer(&peer);
                }
            }
            {
                int n = fci_turn_recv(buf, (int)sizeof(buf), &from);
                if (n > 0) {
                    frames++;
                    printf("turn_probe: frame %d: %d bytes from %s:%d\n", frames, n,
                           inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    fflush(stdout);
                    /* echo the frame back to the address it came from */
                    int ech = fci_turn_add_peer(&from);
                    if (ech >= 0)
                        fci_turn_send(ech, buf, n);
                }
            }
            Sleep(10);
        }
        fci_turn_stop();
        printf("turn_probe: done: %d frames in %d s\n", frames, seconds);
        WSACleanup();
        return frames > 0 ? 0 : 1;
    }
}
