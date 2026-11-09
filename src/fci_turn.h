/* The TURN relay client (RFC 8656 over the RFC 8489 wire). One allocation per instance: the
 * guest allocates so a match whose punch fails can route over the relay; the host, if it also
 * relays, allocates its own, and the server forwards between the two allocated addresses
 * (research/qwen/notes/q17-turn-design.md, the guest-only design).
 *
 * Owned by the NIC: fci_turn_start once with the Cloudflare credentials, fci_turn_tick from the
 * NIC's tick under its lock (single-threaded, non-blocking), add_peer/send/recv on the relay
 * path, fci_turn_stop with the card. Long-term auth: the first request goes out bare, the
 * server's 401 carries the realm and nonce, and every request after carries USERNAME, REALM,
 * NONCE, MESSAGE-INTEGRITY (HMAC-SHA1, key MD5(user:realm:pass)) and FINGERPRINT. The integrity
 * and fingerprint code is pinned by fci_turn_selftest against the RFC 5769 2.1 and 2.4 vectors
 * (tests/test_stun_vectors.py). Retransmits are 500 ms apart, 7 tries, then failed; while ready
 * the module refreshes at half the LIFETIME, re-creates the permissions every 240 s and
 * re-binds the channels every 480 s. The password and the integrity key are never logged. */
#ifndef FCI_TURN_H
#define FCI_TURN_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define FCI_TURN_IDLE 0
#define FCI_TURN_ALLOCATING 1
#define FCI_TURN_READY 2
#define FCI_TURN_FAILED 3

/* Resolve the server (the only blocking call in the module), open a connected non-blocking UDP
 * socket to it and send the Allocate. Returns 1 on success, 0 on failure (the state is then
 * idle). */
int fci_turn_start(const char *server_hostport, const char *user, const char *pass);
/* Retransmits (500 ms RTO, 7 tries), the half-LIFETIME Refresh, the periodic permission and
 * channel re-issues, and drains the socket (STUN responses are handled internally). */
void fci_turn_tick(ULONGLONG now);
/* FCI_TURN_IDLE, FCI_TURN_ALLOCATING, FCI_TURN_READY or FCI_TURN_FAILED. */
int fci_turn_state(void);
/* The relayed address once ready: fills *out and returns 1, else returns 0. */
int fci_turn_relayed(struct sockaddr_in *out);
/* CreatePermission plus ChannelBind for the peer, done asynchronously by the tick. Returns the
 * channel (0x4000-0x4FFF) to send on, or -1 (not ready, or the peer table is full). */
int fci_turn_add_peer(const struct sockaddr_in *peer);
/* A frame on the channel: a ChannelData datagram to the server. Returns 1 when it went out. */
int fci_turn_send(int channel, const void *frame, int len);
/* Returns the next queued frame from a ChannelData or Data indication, with the peer's address
 * in *from; returns 0 when there is none. A Data indication from a known peer's IP on a new
 * port rebinds the peer's channel to that address, and the frame is reported with it. */
int fci_turn_recv(void *buf, int cap, struct sockaddr_in *from);
/* A Refresh with LIFETIME 0, then close the socket and forget the state. */
void fci_turn_stop(void);
/* The MESSAGE-INTEGRITY and FINGERPRINT code against the RFC 5769 2.1 (short-term) and 2.4
 * (long-term) vectors. Returns 1 when both match the published bytes. */
int fci_turn_selftest(void);

#endif
