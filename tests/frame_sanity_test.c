/* The frame sanity check's unit test (q24): builds the frames this game's XNET is known to send
 * (the sizes and headers from the recorded run build/qwen-in/q23/run: 342-byte DHCP, 443-byte
 * System Link broadcast, 66-byte unicast game frame, a 60-byte padded minimum) and a set of
 * hostile frames (truncated, oversized, non-IPv4 EtherTypes, IP fragments, inconsistent length
 * fields), then asserts fci_frame_sane accepts exactly the legitimate ones.
 *
 * Compiled and run natively by tests/test_frame_sanity.py against the shipping source
 * (src/fci_frame.c), so the test exercises the card's own code, not a copy:
 *
 *   cc -std=gnu11 -O2 -Wall -Werror src/fci_frame.c tests/frame_sanity_test.c -o frame_sanity_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fci_frame.h"

#define CARD_MAC {0x02, 0x46, 0x43, 0xBB, 0x49, 0x60} /* the host card in the recorded run */
#define PEER_MAC {0x02, 0x46, 0x43, 0x32, 0xFC, 0x03} /* the joiner card in the recorded run */

static int nfail, ntotal;

/* one full Ethernet/IPv4/UDP frame. The Ethernet header is 14 bytes (dst, src, EtherType), the
 * IPv4 header ihl*4 (version/IHL, ToS, total length, id, flags/offset, TTL, protocol, checksum,
 * source, destination), then the 8-byte UDP header (ports, length, checksum) and the payload
 * (fill pattern, the guest's real payload is arbitrary to the check). total is the frame length:
 * when it exceeds 14 + totlen the tail is zero padding, as a NIC pads to the 60-byte minimum. */
static void mkframe(uint8_t *f, int total, uint16_t ethertype, int ver, int ihl, int totlen, int flags,
                    int fragoff, int proto, int udplen)
{
    int i;
    memset(f, 0, (size_t)total);
    memcpy(f + 0, "\xff\xff\xff\xff\xff\xff", 6); /* broadcast destination */
    memcpy(f + 6, (const uint8_t[]){0x02, 0x46, 0x43, 0xBB, 0x49, 0x60}, 6);
    f[12] = (uint8_t)(ethertype >> 8), f[13] = (uint8_t)ethertype;
    f[14] = (uint8_t)((ver << 4) | (ihl & 0xF));
    f[16] = (uint8_t)(totlen >> 8), f[17] = (uint8_t)totlen;
    f[18] = 0x12, f[19] = 0x34; /* identification: arbitrary */
    f[20] = (uint8_t)((flags & 0xE0) | ((fragoff >> 8) & 0x1F));
    f[21] = (uint8_t)(fragoff & 0xFF);
    f[22] = 128; /* TTL */
    f[23] = (uint8_t)proto;
    f[26] = 192, f[27] = 168, f[28] = 1, f[29] = 144; /* the host card's address in the run */
    f[30] = 192, f[31] = 168, f[32] = 1, f[33] = 145; /* the joiner's */
    {
        int u = 14 + ihl * 4;
        f[u + 0] = 0, f[u + 1] = 68; /* source port 68 (the DHCP client; the check takes any) */
        f[u + 2] = 0, f[u + 3] = 67; /* destination port 67 */
        f[u + 4] = (uint8_t)(udplen >> 8), f[u + 5] = (uint8_t)udplen;
    }
    for (i = 14 + ihl * 4 + 8; i < 14 + totlen; i++)
        f[i] = (uint8_t)(0xA0 + (i & 7)); /* a payload pattern, not zeros */
}

static void check(const char *name, const uint8_t *f, int n, int want)
{
    int got = fci_frame_sane(f, n);
    ntotal++;
    if (got != want) {
        nfail++;
        printf("FAIL %s: want %d (%s), got %d (%s)\n", name, want, fci_frame_reason(want), got,
               fci_frame_reason(got));
    } else {
        printf("ok   %s: %d (%s)\n", name, got, fci_frame_reason(got));
    }
}

/* stdin mode: judge each line "<name> <hex>" (tools/net_fuzz.py's vectors, cross-checked against
 * the Python builder by tests/test_net_fuzz.py); prints "<name> <code> <reason>" per line */
static int hexval(int c)
{
    c |= 0x20;
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static int run_stdin(void)
{
    static uint8_t f[8192];
    char line[16384];
    int ntotal = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *space = strchr(line, ' ');
        if (!space)
            continue;
        *space = 0;
        char *hex = space + 1;
        while (*hex == ' ')
            hex++;
        int n = 0;
        while (hex[2 * n] && hex[2 * n + 1] && n < (int)sizeof f) {
            int hi = hexval((unsigned char)hex[2 * n]);
            int lo = hexval((unsigned char)hex[2 * n + 1]);
            if (hi < 0 || lo < 0)
                break;
            f[n++] = (uint8_t)((hi << 4) | lo);
        }
        int got = fci_frame_sane(f, n);
        printf("%s %d %s\n", line, got, fci_frame_reason(got));
        ntotal++;
    }
    printf("%d judged\n", ntotal);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t f[2048];
    if (argc > 1 && !strcmp(argv[1], "stdin"))
        return run_stdin();

    /* --- the frames the game sends (the recorded run's sizes and headers) --- */

    /* DHCP discover, 342 bytes: 14 + 20 + 8 + 300, the run's broadcast */
    mkframe(f, 342, 0x0800, 4, 5, 328, 0, 0, 17, 308);
    check("dhcp-342", f, 342, FCI_FRAME_OK);

    /* the System Link broadcast, 443 bytes: 14 + 20 + 8 + 401 */
    mkframe(f, 443, 0x0800, 4, 5, 421, 0, 0, 17, 401);
    check("slp-443", f, 443, FCI_FRAME_OK);

    /* the unicast game frame, 66 bytes: 14 + 20 + 8 + 24 (the UDP length is 8 + 24) */
    mkframe(f, 66, 0x0800, 4, 5, 52, 0, 0, 17, 32);
    check("game-66", f, 66, FCI_FRAME_OK);

    /* the padded minimum: an IP packet of 28 bytes (no payload) in a 60-byte frame, 18 bytes of
     * zero padding, as the card pads (src/fci_nic.c tx_run) */
    mkframe(f, 60, 0x0800, 4, 5, 28, 0, 0, 17, 8);
    check("padded-60", f, 60, FCI_FRAME_OK);

    /* the largest frame, 1514 bytes: 14 + 20 + 8 + 1472 */
    mkframe(f, 1514, 0x0800, 4, 5, 1500, 0, 0, 17, 1480);
    check("max-1514", f, 1514, FCI_FRAME_OK);

    /* an IPv4 header with options (IHL 6, 24 bytes) and the don't-fragment bit: both ordinary */
    mkframe(f, 70, 0x0800, 4, 6, 56, 0x40, 0, 17, 32);
    check("ip-options-df", f, 70, FCI_FRAME_OK);

    /* --- the hostile frames --- */

    /* truncated: no full Ethernet header */
    memset(f, 0, 10);
    check("truncated-10", f, 10, FCI_FRAME_SHORT);

    /* just short of the Ethernet header */
    mkframe(f, 13, 0x0800, 4, 5, 52, 0, 0, 17, 40);
    check("truncated-13", f, 13, FCI_FRAME_SHORT);

    /* oversized: past the 1514-byte limit */
    mkframe(f, 1515, 0x0800, 4, 5, 1500, 0, 0, 17, 1480);
    check("oversized-1515", f, 1515, FCI_FRAME_LONG);

    /* an ARP frame (EtherType 0x0806): no ARP was observed in the run (SLP carries the MACs) */
    mkframe(f, 66, 0x0806, 4, 5, 52, 0, 0, 17, 40);
    check("arp-0806", f, 66, FCI_FRAME_ETHER);

    /* IPv6 (EtherType 0x86DD) */
    mkframe(f, 66, 0x86DD, 4, 5, 52, 0, 0, 17, 40);
    check("ipv6-86dd", f, 66, FCI_FRAME_ETHER);

    /* an 802.3 length field (52) where the EtherType should be: a short-frame encoding the game
     * does not use */
    mkframe(f, 66, 0x0034, 4, 5, 52, 0, 0, 17, 40);
    check("8023-length", f, 66, FCI_FRAME_ETHER);

    /* an IPv6 payload behind an IPv4 EtherType */
    mkframe(f, 66, 0x0800, 6, 5, 52, 0, 0, 17, 40);
    check("ip-version-6", f, 66, FCI_FRAME_IPVER);

    /* an IHL that does not fit the frame (60-byte header in a 66-byte frame) */
    mkframe(f, 66, 0x0800, 4, 15, 52, 0, 0, 17, 40);
    check("ihl-too-big", f, 66, FCI_FRAME_IPLEN);

    /* an IP total length past the frame's payload */
    mkframe(f, 66, 0x0800, 4, 5, 500, 0, 0, 17, 40);
    check("totlen-oversize", f, 66, FCI_FRAME_IPTOT);

    /* an IP total length with no room for the UDP header */
    mkframe(f, 66, 0x0800, 4, 5, 20, 0, 0, 17, 40);
    check("totlen-short", f, 66, FCI_FRAME_IPTOT);

    /* an Ethernet frame too short for an IP header and a UDP header */
    memset(f, 0, 20);
    f[12] = 0x08, f[13] = 0x00;
    f[14] = 0x45;
    check("too-short-for-ip", f, 20, FCI_FRAME_IPTOT);

    /* a fragment: the more-fragments bit set */
    mkframe(f, 66, 0x0800, 4, 5, 52, 0x20, 0, 17, 40);
    check("fragment-mf", f, 66, FCI_FRAME_FRAG);

    /* a fragment: a nonzero offset */
    mkframe(f, 66, 0x0800, 4, 5, 52, 0, 185, 17, 40);
    check("fragment-offset", f, 66, FCI_FRAME_FRAG);

    /* an ICMP packet (protocol 1) */
    mkframe(f, 66, 0x0800, 4, 5, 52, 0, 0, 1, 40);
    check("icmp", f, 66, FCI_FRAME_PROTO);

    /* a UDP length that does not fill the IP payload */
    mkframe(f, 66, 0x0800, 4, 5, 52, 0, 0, 17, 100);
    check("udp-len-long", f, 66, FCI_FRAME_UDPLEN);
    mkframe(f, 66, 0x0800, 4, 5, 52, 0, 0, 17, 8);
    check("udp-len-short", f, 66, FCI_FRAME_UDPLEN);

    /* 50 bytes of padding: past the 46 a NIC pads to the 60-byte minimum */
    mkframe(f, 100, 0x0800, 4, 5, 36, 0, 0, 17, 16);
    check("overpad-100", f, 100, FCI_FRAME_PAD);

    printf("%d checks, %d failed\n", ntotal, nfail);
    return nfail ? 1 : 0;
}
