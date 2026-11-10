/* The frame sanity check (q24): a received Ethernet frame reaches the guest's RX queue only when
 * it is what this game's XNET is known to send - an unfragmented IPv4/UDP frame, 14..1514 bytes,
 * with the length fields consistent. src/fci_nic.c calls fci_frame_sane at every enqueue (the
 * card socket, the relay rooms, the TURN relay); a failed frame is dropped, counted, and logged
 * rate-limited.
 *
 * The rules are based on the recorded traffic (build/qwen-in/q23/run: every frame is
 * 14 + 20 + 8 + payload, 66..1354 bytes; the 342-byte DHCP is 14 + 20 + 8 + 300). A frame the
 * game sends is built by the guest's XNET (destination and source MAC, EtherType 0x0800, IPv4,
 * UDP); nothing else is observed in the runs, and the size distribution has no room for other
 * headers. tools/net_fuzz.py exercises the check against a running card; net_check (Q23) proves
 * the game's own traffic passes. research/qwen/notes/q24-harden.md */
#ifndef FCI_FRAME_H
#define FCI_FRAME_H

#include <stdint.h>

enum {
    FCI_FRAME_OK = 0, /* an unfragmented IPv4/UDP frame with consistent lengths */
    FCI_FRAME_SHORT,  /* fewer than 14 bytes: no full Ethernet header */
    FCI_FRAME_LONG,   /* more than 1514 bytes */
    FCI_FRAME_ETHER,  /* the EtherType is not IPv4 (0x0800) */
    FCI_FRAME_IPVER,  /* the IP version is not 4 */
    FCI_FRAME_IPLEN,  /* the IP header length (IHL) does not fit the frame */
    FCI_FRAME_IPTOT,  /* the IP total length is inconsistent (too big, or no room for a UDP header) */
    FCI_FRAME_FRAG,   /* an IP fragment (the more-fragments bit or a nonzero fragment offset) */
    FCI_FRAME_PROTO,  /* the IP protocol is not UDP (17) */
    FCI_FRAME_UDPLEN, /* the UDP length is not the IP payload */
    FCI_FRAME_PAD,    /* more than 46 bytes of padding after the IP packet */
    FCI_FRAME_NREASONS
};

/* 0 (FCI_FRAME_OK) when the frame may reach the guest; otherwise the reason. Pure: no I/O, no
 * globals, safe to call from any thread. */
int fci_frame_sane(const uint8_t *f, int n);

/* the reason's name, for the log line */
const char *fci_frame_reason(int r);

#endif
