/* The STUN-5 wire protocol: build the binding request, parse the binding response.
 * No sockets, no globals, no I/O: the unit tests compile this file natively. */
#include <stdint.h>
#include <string.h>

#include "fci_stun.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

int fci_stun_build_request(uint8_t *buf, uint32_t txn)
{
    buf[0] = 0x00, buf[1] = 0x01; /* binding request */
    buf[2] = 0x00, buf[3] = 0x00; /* message length: 0 */
    /* magic cookie, network order (the bytes 21 12 a4 42) */
    buf[4] = (uint8_t)(FCI_STUN_MAGIC >> 24), buf[5] = (uint8_t)(FCI_STUN_MAGIC >> 16),
    buf[6] = (uint8_t)(FCI_STUN_MAGIC >> 8), buf[7] = (uint8_t)FCI_STUN_MAGIC;
    memset(buf + 8, 0, 12);
    buf[8] = (uint8_t)(txn >> 24), buf[9] = (uint8_t)(txn >> 16);
    buf[10] = (uint8_t)(txn >> 8), buf[11] = (uint8_t)txn;
    return 20;
}

int fci_stun_parse_response(const uint8_t *buf, int n, uint32_t txn, uint8_t out_ip[4], uint16_t *out_port,
                            int *out_xor)
{
    int off;
    if (n < 20)
        return FCI_STUN_SHORT;
    if (buf[0] != 0x01 || buf[1] != 0x01)
        return FCI_STUN_TYPE;
    if (be32(buf + 4) != FCI_STUN_MAGIC)
        return FCI_STUN_COOKIE;
    if (be32(buf + 8) != txn)
        return FCI_STUN_TXN;
    /* walk the attributes */
    off = 20;
    while (off + 4 <= n) {
        uint16_t type = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        uint16_t len = (uint16_t)((buf[off + 2] << 8) | buf[off + 3]);
        if (off + 4 + (int)len > n)
            return FCI_STUN_TRUNC;
        if (type == 0x0020 && len == 8) {
            /* XOR-MAPPED-ADDRESS: port and address are XORed with the magic cookie (RFC 5389
             * 15.2). be32 returns the network-order bytes as a big-endian number; FCI_STUN_MAGIC
             * is the same number for the cookie bytes 21 12 a4 42, so the XOR is direct. */
            uint16_t port = (uint16_t)(((buf[off + 6] << 8) | buf[off + 7]) ^ 0x2112);
            put_be32(out_ip, be32(buf + off + 8) ^ FCI_STUN_MAGIC);
            *out_port = port;
            *out_xor = 1;
            return FCI_STUN_OK;
        }
        if (type == 0x0001 && len == 8) {
            /* MAPPED-ADDRESS: plain, the address is already in network order */
            uint16_t port = (uint16_t)((buf[off + 6] << 8) | buf[off + 7]);
            memcpy(out_ip, buf + off + 8, 4);
            *out_port = port;
            *out_xor = 0;
            return FCI_STUN_OK;
        }
        off += 4 + (((int)len + 3) & ~3); /* attributes are padded to 4 bytes */
    }
    return FCI_STUN_NOADDR;
}

const char *fci_stun_reason(int r)
{
    switch (r) {
    case FCI_STUN_OK:
        return "ok";
    case FCI_STUN_SHORT:
        return "short";
    case FCI_STUN_TYPE:
        return "type";
    case FCI_STUN_COOKIE:
        return "cookie";
    case FCI_STUN_TXN:
        return "txn";
    case FCI_STUN_NOADDR:
        return "no-address";
    case FCI_STUN_TRUNC:
        return "truncated";
    default:
        return "unknown";
    }
}
