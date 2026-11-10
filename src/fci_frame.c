/* The frame sanity check (q24): the pure rule set, in its own file so the unit tests
 * (tests/test_frame_sanity.py) compile and run this exact code natively, and tools/net_fuzz.py
 * exercises it inside a running card. See fci_frame.h for the evidence behind each rule. */
#include <stdint.h>

#include "fci_frame.h"

#define ETHER_HDR    14   /* destination MAC 6, source MAC 6, EtherType 2 */
#define ETHER_MIN    60   /* the smallest frame the card pads to (src/fci_nic.c tx_run) */
#define ETHER_MAX    1514 /* the largest frame without its checksum (src/fci_nic.c) */
#define IP_HDR_MIN   20   /* the IPv4 header without options */
#define UDP_HDR      8    /* source port, destination port, length, checksum */
#define IP_PROTO_UDP 17
#define ETHER_IPV4   0x0800u
#define PAD_MAX      (ETHER_MIN - ETHER_HDR) /* 46: the zero padding up to the minimum frame */

static int be16(const uint8_t *p) { return (int)(((int)p[0] << 8) | p[1]); }

int fci_frame_sane(const uint8_t *f, int n)
{
    int iplen, ihl, totlen, pad;
    if (n < ETHER_HDR)
        return FCI_FRAME_SHORT;
    if (n > ETHER_MAX)
        return FCI_FRAME_LONG;
    if (be16(f + 12) != ETHER_IPV4)
        return FCI_FRAME_ETHER;
    iplen = n - ETHER_HDR;
    if (iplen < IP_HDR_MIN + UDP_HDR)
        return FCI_FRAME_IPTOT; /* too short for an IPv4 header and a UDP header */
    if ((f[14] >> 4) != 4)
        return FCI_FRAME_IPVER;
    ihl = (f[14] & 0xF) * 4;
    if (ihl < IP_HDR_MIN || ihl > iplen)
        return FCI_FRAME_IPLEN;
    totlen = be16(f + 16);
    if (totlen < ihl + UDP_HDR || totlen > iplen)
        return FCI_FRAME_IPTOT;
    pad = iplen - totlen;
    if (pad > PAD_MAX)
        return FCI_FRAME_PAD;
    /* the flags/fragment-offset field: bit 1 is more-fragments, the low 13 bits the offset;
     * the don't-fragment bit alone is a normal unfragmented packet */
    if ((f[20] & 0x20) || (((f[20] & 0x1F) << 8) | f[21]))
        return FCI_FRAME_FRAG;
    if (f[14 + 9] != IP_PROTO_UDP)
        return FCI_FRAME_PROTO;
    if (be16(f + 14 + ihl + 4) != totlen - ihl)
        return FCI_FRAME_UDPLEN;
    return FCI_FRAME_OK;
}

const char *fci_frame_reason(int r)
{
    switch (r) {
    case FCI_FRAME_OK:
        return "ok";
    case FCI_FRAME_SHORT:
        return "short";
    case FCI_FRAME_LONG:
        return "long";
    case FCI_FRAME_ETHER:
        return "ethertype";
    case FCI_FRAME_IPVER:
        return "ip-version";
    case FCI_FRAME_IPLEN:
        return "ip-header";
    case FCI_FRAME_IPTOT:
        return "ip-total-length";
    case FCI_FRAME_FRAG:
        return "fragment";
    case FCI_FRAME_PROTO:
        return "not-udp";
    case FCI_FRAME_UDPLEN:
        return "udp-length";
    case FCI_FRAME_PAD:
        return "padding";
    default:
        return "unknown";
    }
}
