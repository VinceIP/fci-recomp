/* The STUN-5 wire protocol (RFC 5389, the binding request/response only): the pure byte-level
 * logic, in its own file so the unit tests (tests/test_stun_c.py) compile and run this exact code
 * natively. src/fci_nic.c calls it at STUN send and receive; the socket, the server address check,
 * and the log lines stay there.
 *
 * The address in the XOR-MAPPED-ADDRESS attribute is the 32-bit address XORed with the magic
 * cookie, both in network order (RFC 5389 15.2). A bug that XORed the network-order address with
 * the host-order cookie produced a wrong public address for two days (Q28, fixed in ba69b03). */
#ifndef FCI_STUN_H
#define FCI_STUN_H

#include <stdint.h>

#define FCI_STUN_MAGIC 0x2112A442u

/* The result of parsing a binding response. */
enum {
    FCI_STUN_OK = 0, /* a mapped address was found */
    FCI_STUN_SHORT,  /* fewer than 20 bytes: no full STUN header */
    FCI_STUN_TYPE,   /* the message type is not a binding response (0x0101) */
    FCI_STUN_COOKIE, /* the magic cookie does not match */
    FCI_STUN_TXN,    /* the transaction ID does not match */
    FCI_STUN_NOADDR, /* no MAPPED-ADDRESS or XOR-MAPPED-ADDRESS attribute */
    FCI_STUN_TRUNC,  /* an attribute's length extends past the end of the buffer */
};

/* Build a 20-byte binding request into buf. The transaction ID (4 bytes, big-endian) is placed
 * in the first four bytes of the 12-byte field; the remaining eight are zero. Returns 20. */
int fci_stun_build_request(uint8_t *buf, uint32_t txn);

/* Parse a binding response. On success (FCI_STUN_OK), out_ip holds the IPv4 address as its four
 * bytes in wire order (192.0.2.1 is c0 00 02 01: copy them into sin_addr, no byte swap to get wrong)
 * and *out_port is the port in host order. *out_xor is 1 when the address came from a
 * XOR-MAPPED-ADDRESS attribute (0x0020), 0 for MAPPED-ADDRESS (0x0001). The checks: message type,
 * magic cookie, transaction ID, then a walk of the attributes (XOR-MAPPED-ADDRESS preferred,
 * MAPPED-ADDRESS fallback, 4-byte padding, truncated or lying lengths refused). */
int fci_stun_parse_response(const uint8_t *buf, int n, uint32_t txn, uint8_t out_ip[4], uint16_t *out_port,
                            int *out_xor);

/* the reason's name, for the log line */
const char *fci_stun_reason(int r);

#endif
