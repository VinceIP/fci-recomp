/* The STUN wire protocol's unit test (Q28): builds the binding request and parses binding
 * responses (the RFC 5769 2.2 sample byte for byte, a MAPPED-ADDRESS-only response, padding,
 * and a set of hostile inputs: wrong cookie, wrong transaction ID, wrong type, truncated
 * message, attribute length past the end) and asserts fci_stun_parse_response accepts exactly
 * the legitimate ones.
 *
 * Compiled and run natively by tests/test_stun_c.py against the shipping source
 * (src/fci_stun.c), so the test exercises the card's own code, not a copy:
 *
 *   cc -std=gnu11 -O2 -Wall -Werror src/fci_stun.c tests/stun_test.c -o stun_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fci_stun.h"

static int nfail, ntotal;

/* check a parse result. For FCI_STUN_OK, also verifies the address and port. */
static void check(const char *name, int got, int want, uint32_t addr, uint16_t port, int xor,
                  uint32_t want_addr, uint16_t want_port)
{
    ntotal++;
    if (got != want) {
        nfail++;
        printf("FAIL %s: want %d (%s), got %d (%s)\n", name, want, fci_stun_reason(want), got,
               fci_stun_reason(got));
    } else if (want == FCI_STUN_OK) {
        if (addr != want_addr || port != want_port) {
            nfail++;
            printf("FAIL %s: wrong address (addr=0x%08x port=%u, want 0x%08x:%u)\n", name, addr, port,
                   want_addr, want_port);
        } else {
            printf("ok   %s: addr=0x%08x port=%u (xor=%d)\n", name, addr, port, xor);
        }
    } else {
        printf("ok   %s: %d (%s)\n", name, got, fci_stun_reason(got));
    }
}

/* the RFC 5769 2.2 sample response: 80 bytes, XOR-MAPPED-ADDRESS 192.0.2.1:32853.
 * Layout: header(20) + SOFTWARE(16) + XOR-MAPPED(12) + INTEGRITY(24) + FINGERPRINT(8). */
static const uint8_t RFC5769_2_2[] = {
        /* header: type 0x0101, length 0x003c, magic 2112a442, txn b7e7a701bc34d686fa87dfae */
        0x01,
        0x01,
        0x00,
        0x3c,
        0x21,
        0x12,
        0xa4,
        0x42,
        0xb7,
        0xe7,
        0xa7,
        0x01,
        0xbc,
        0x34,
        0xd6,
        0x86,
        0xfa,
        0x87,
        0xdf,
        0xae,
        /* SOFTWARE "test vector" + 1 pad (type 0x8022, len 11, stride 16) */
        0x80,
        0x22,
        0x00,
        0x0b,
        0x74,
        0x65,
        0x73,
        0x74,
        0x20,
        0x76,
        0x65,
        0x63,
        0x74,
        0x6f,
        0x72,
        0x20,
        /* XOR-MAPPED-ADDRESS (type 0x0020, len 8): family 0x01, port a147, addr e112a643 */
        0x00,
        0x20,
        0x00,
        0x08,
        0x00,
        0x01,
        0xa1,
        0x47,
        0xe1,
        0x12,
        0xa6,
        0x43,
        /* MESSAGE-INTEGRITY (type 0x0008, len 20) */
        0x00,
        0x08,
        0x00,
        0x14,
        0x2b,
        0x91,
        0xf5,
        0x99,
        0xfd,
        0x9e,
        0x90,
        0xc3,
        0x8c,
        0x74,
        0x89,
        0xf9,
        0x2a,
        0xf9,
        0xba,
        0x53,
        0xf0,
        0x6b,
        0xe7,
        0xd7,
        /* FINGERPRINT (type 0x8028, len 4) */
        0x80,
        0x28,
        0x00,
        0x04,
        0xc0,
        0x7d,
        0x4c,
        0x96,
};

#define RFC5769_TXN    0xb7e7a701u
#define ADDR_192_0_2_1 0xc0000201u /* 192.0.2.1: the wire bytes c0 00 02 01, first byte highest */
/* the four wire-order bytes the parser returns, first byte highest: a byte-swapped answer differs */
#define IPV(ip) (((uint32_t)(ip)[0] << 24) | ((uint32_t)(ip)[1] << 16) | ((uint32_t)(ip)[2] << 8) | (uint32_t)(ip)[3])
#define PORT_32853     32853

int main(void)
{
    uint8_t buf[128];
    uint8_t ip[4];
    uint16_t port;
    int xor;

    /* --- build the binding request --- */

    memset(buf, 0, sizeof buf);
    fci_stun_build_request(buf, 0x01020304);
    if (buf[0] != 0x00 || buf[1] != 0x01 || buf[2] != 0x00 || buf[3] != 0x00) {
        nfail++;
        printf("FAIL build-request: wrong type/length\n");
    } else if (buf[4] != 0x21 || buf[5] != 0x12 || buf[6] != 0xa4 || buf[7] != 0x42) {
        nfail++;
        printf("FAIL build-request: wrong magic\n");
    } else if (buf[8] != 0x01 || buf[9] != 0x02 || buf[10] != 0x03 || buf[11] != 0x04) {
        nfail++;
        printf("FAIL build-request: wrong txn\n");
    } else if (buf[12] != 0 || buf[13] != 0 || buf[14] != 0 || buf[15] != 0 || buf[16] != 0 || buf[17] != 0
               || buf[18] != 0 || buf[19] != 0) {
        nfail++;
        printf("FAIL build-request: txn tail not zero\n");
    } else {
        ntotal++;
        printf("ok   build-request: 20 bytes, magic and txn correct\n");
    }

    /* --- RFC 5769 2.2: the sample IPv4 binding response --- */
    {
        int r = fci_stun_parse_response(RFC5769_2_2, (int)sizeof RFC5769_2_2, RFC5769_TXN, ip, &port,
                                        &xor);
        check("rfc5769-2-2", r, FCI_STUN_OK, IPV(ip), port, xor, ADDR_192_0_2_1, PORT_32853);
    }

    /* --- MAPPED-ADDRESS only (no XOR-MAPPED-ADDRESS) ---
     * Header (20) + MAPPED-ADDRESS (4+8=12) = 32 bytes. Address 192.0.2.1:32853. */
    {
        uint8_t m[32];
        memset(m, 0, sizeof m);
        m[0] = 0x01, m[1] = 0x01;                             /* binding response */
        m[2] = 0x00, m[3] = 0x0c;                             /* length 12 */
        m[4] = 0x21, m[5] = 0x12, m[6] = 0xa4, m[7] = 0x42;   /* magic */
        m[8] = 0x00, m[9] = 0x00, m[10] = 0x00, m[11] = 0x42; /* txn 0x42 */
        /* MAPPED-ADDRESS at offset 20: type 0x0001, len 8, family 0x01, port 0x8055, addr */
        m[20] = 0x00, m[21] = 0x01;
        m[22] = 0x00, m[23] = 0x08;
        m[24] = 0x00, m[25] = 0x01;                             /* family */
        m[26] = 0x80, m[27] = 0x55;                             /* port 32853 */
        m[28] = 0xc0, m[29] = 0x00, m[30] = 0x02, m[31] = 0x01; /* 192.0.2.1 */
        int r = fci_stun_parse_response(m, 32, 0x42, ip, &port, &xor);
        check("mapped-only", r, FCI_STUN_OK, IPV(ip), port, xor, ADDR_192_0_2_1, PORT_32853);
    }

    /* --- padding: a SOFTWARE attribute of odd length (3) before the address ---
     * Header (20) + SOFTWARE (4+3=7, stride 8) + XOR-MAPPED (4+8=12) = 40 bytes. */
    {
        uint8_t m[40];
        memset(m, 0, sizeof m);
        m[0] = 0x01, m[1] = 0x01;
        m[2] = 0x00, m[3] = 0x18; /* length 24 */
        m[4] = 0x21, m[5] = 0x12, m[6] = 0xa4, m[7] = 0x42;
        m[8] = 0x00, m[9] = 0x00, m[10] = 0x00, m[11] = 0x99; /* txn 0x99 */
        /* SOFTWARE at offset 20: type 0x8022, len 3, value "abc" (stride 4, total 8) */
        m[20] = 0x80, m[21] = 0x22;
        m[22] = 0x00, m[23] = 0x03;
        m[24] = 'a', m[25] = 'b', m[26] = 'c';
        m[27] = 0x00; /* padding */
        /* XOR-MAPPED-ADDRESS at offset 28: family 0x01, port a147, addr e112a643 */
        m[28] = 0x00, m[29] = 0x20;
        m[30] = 0x00, m[31] = 0x08;
        m[32] = 0x00, m[33] = 0x01;                             /* family */
        m[34] = 0xa1, m[35] = 0x47;                             /* port XOR */
        m[36] = 0xe1, m[37] = 0x12, m[38] = 0xa6, m[39] = 0x43; /* addr XOR */
        int r = fci_stun_parse_response(m, 40, 0x99, ip, &port, &xor);
        check("padding-odd", r, FCI_STUN_OK, IPV(ip), port, xor, ADDR_192_0_2_1, PORT_32853);
    }

    /* --- wrong cookie --- */
    {
        uint8_t bad[80];
        memcpy(bad, RFC5769_2_2, sizeof RFC5769_2_2);
        bad[4] = 0x00; /* corrupt the magic cookie */
        int r = fci_stun_parse_response(bad, (int)sizeof bad, RFC5769_TXN, ip, &port, &xor);
        check("wrong-cookie", r, FCI_STUN_COOKIE, 0, 0, 0, 0, 0);
    }

    /* --- wrong transaction ID --- */
    {
        int r = fci_stun_parse_response(RFC5769_2_2, (int)sizeof RFC5769_2_2, 0xdeadbeefu, ip, &port,
                                        &xor);
        check("wrong-txn", r, FCI_STUN_TXN, 0, 0, 0, 0, 0);
    }

    /* --- wrong type (binding request, not response) --- */
    {
        uint8_t m[20];
        memset(m, 0, sizeof m);
        m[0] = 0x00, m[1] = 0x01; /* binding request */
        m[4] = 0x21, m[5] = 0x12, m[6] = 0xa4, m[7] = 0x42;
        m[8] = 0x00, m[9] = 0x00, m[10] = 0x00, m[11] = 0x42;
        int r = fci_stun_parse_response(m, 20, 0x42, ip, &port, &xor);
        check("wrong-type", r, FCI_STUN_TYPE, 0, 0, 0, 0, 0);
    }

    /* --- truncated message (fewer than 20 bytes) --- */
    {
        uint8_t m[10] = {0};
        int r = fci_stun_parse_response(m, 10, 0x42, ip, &port, &xor);
        check("truncated-10", r, FCI_STUN_SHORT, 0, 0, 0, 0, 0);
    }

    /* --- attribute length past the end ---
     * Header (20) + attribute header (4) + only 4 of 8 value bytes = 28 bytes total.
     * The attribute claims 8 bytes of value but only 4 are present. */
    {
        uint8_t m[28];
        memset(m, 0, sizeof m);
        m[0] = 0x01, m[1] = 0x01;
        m[2] = 0x00, m[3] = 0x0c;
        m[4] = 0x21, m[5] = 0x12, m[6] = 0xa4, m[7] = 0x42;
        m[8] = 0x00, m[9] = 0x00, m[10] = 0x00, m[11] = 0x42;
        /* XOR-MAPPED-ADDRESS at offset 20: claims 8 bytes, only 4 present */
        m[20] = 0x00, m[21] = 0x20;
        m[22] = 0x00, m[23] = 0x08;
        m[24] = 0x00, m[25] = 0x01, m[26] = 0xa1, m[27] = 0x47;
        int r = fci_stun_parse_response(m, 28, 0x42, ip, &port, &xor);
        check("attr-trunc", r, FCI_STUN_TRUNC, 0, 0, 0, 0, 0);
    }

    /* --- no mapped address attribute at all ---
     * Header (20) + SOFTWARE (4+4=8) = 28 bytes. No MAPPED or XOR-MAPPED-ADDRESS. */
    {
        uint8_t m[28];
        memset(m, 0, sizeof m);
        m[0] = 0x01, m[1] = 0x01;
        m[2] = 0x00, m[3] = 0x08;
        m[4] = 0x21, m[5] = 0x12, m[6] = 0xa4, m[7] = 0x42;
        m[8] = 0x00, m[9] = 0x00, m[10] = 0x00, m[11] = 0x42;
        /* SOFTWARE at offset 20: type 0x8022, len 4, value "test" */
        m[20] = 0x80, m[21] = 0x22;
        m[22] = 0x00, m[23] = 0x04;
        m[24] = 't', m[25] = 'e', m[26] = 's', m[27] = 't';
        int r = fci_stun_parse_response(m, 28, 0x42, ip, &port, &xor);
        check("no-address", r, FCI_STUN_NOADDR, 0, 0, 0, 0, 0);
    }

    printf("%d checks, %d failed\n", ntotal, nfail);
    return nfail ? 1 : 0;
}
