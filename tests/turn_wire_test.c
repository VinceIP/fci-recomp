/* The TURN wire protocol's unit test (Q43): the pure byte-level logic that src/fci_turn.c
 * moved into src/fci_turn_wire.c, exercised byte for byte.
 *
 * The vectors are the RFC 5769 samples (2.1 request rebuilt with fci_tw_build, 2.2/2.3/2.4
 * parsed with fci_tw_parse, MESSAGE-INTEGRITY verified with the documented keys) plus known
 * digests for the embedded crypto (MD5, SHA-1, the RFC 2202 HMAC-SHA1 case, CRC-32), the
 * XOR-ADDRESS vector, the client's own error and success round trips (401, 438, Allocate
 * success with integrity) and a set of hostile inputs (truncated message, attribute past the
 * end, wrong cookie, a corrupted integrity).
 *
 * Compiled and run natively by tests/test_turn_wire_c.py against the shipping source
 * (src/fci_turn_wire.c), so the test exercises the card's own code, not a copy:
 *
 *   cc -std=gnu11 -O2 -Wall -Werror src/fci_turn_wire.c tests/turn_wire_test.c -o turn_wire_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fci_turn_wire.h"

#define M_BIND     1
#define M_ALLOCATE 3

static int nfail, ntotal;

static void pass(const char *name, const char *detail)
{
    ntotal++;
    printf("ok   %s: %s\n", name, detail);
}
static void fail(const char *name, const char *detail)
{
    ntotal++;
    nfail++;
    printf("FAIL %s: %s\n", name, detail);
}
static void check1(const char *name, int cond, const char *detail)
{
    if (cond)
        pass(name, detail);
    else
        fail(name, detail);
}

/* 192.0.2.1:32853 as a sockaddr_in */
static struct sockaddr_in addr_192_0_2_1_32853(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(32853);
    a.sin_addr.s_addr = htonl(0xc0000201u);
    return a;
}
static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{ return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr; }

/* --- the RFC 5769 2.1 sample request, rebuilt with fci_tw_build ---
 * Key: the raw password (the 2.1-2.3 vectors sign with it, per the RFC's parameter list).
 * Body: SOFTWARE + PRIORITY + ICE-CONTROLLED + USERNAME (56 bytes; the USERNAME padding is
 * the RFC's arbitrary spaces, which are part of the signed bytes). */
static const uint8_t V21_TXN[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
static const uint8_t V21_BODY[]
        = {/* SOFTWARE "STUN test client" (type 0x8022, len 16) */
           0x80, 0x22, 0x00, 0x10, 'S', 'T', 'U', 'N', ' ', 't', 'e', 's', 't', ' ', 'c', 'l', 'i', 'e', 'n',
           't',
           /* PRIORITY (type 0x0024, len 4) */
           0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff,
           /* ICE-CONTROLLED (type 0x8029, len 8) */
           0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36,
           /* USERNAME "evtj:h6vY" (type 0x0006, len 9) + 3 spaces of padding */
           0x00, 0x06, 0x00, 0x09, 'e', 'v', 't', 'j', ':', 'h', '6', 'v', 'Y', 0x20, 0x20, 0x20};
static const uint8_t V21_FULL[]
        = {0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
           0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x10, 'S',  'T',  'U',  'N',  ' ',  't',  'e',  's',
           't',  ' ',  'c',  'l',  'i',  'e',  'n',  't',  0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff,
           0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36, 0x00, 0x06, 0x00, 0x09,
           'e',  'v',  't',  'j',  ':',  'h',  '6',  'v',  'Y',  0x20, 0x20, 0x20, 0x00, 0x08, 0x00, 0x14,
           0x9a, 0xea, 0xa7, 0x0c, 0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e, 0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49,
           0xc1, 0xb5, 0x71, 0xa2, 0x80, 0x28, 0x00, 0x04, 0xe5, 0x7a, 0x3b, 0xcf};

/* --- the RFC 5769 2.2 sample IPv4 response --- */
static const uint8_t V22[]
        = {0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
           0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 't',  'e',  's',  't',  ' ',  'v',  'e',  'c',
           't',  'o',  'r',  ' ',  0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
           0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9,
           0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};

/* --- the RFC 5769 2.3 sample IPv6 response --- */
static const uint8_t V23[]
        = {0x01, 0x01, 0x00, 0x48, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
           0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 't',  'e',  's',  't',  ' ',  'v',  'e',  'c',
           't',  'o',  'r',  ' ',  0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0xa1, 0x47, 0x01, 0x13, 0xa9, 0xfa,
           0xa5, 0xd3, 0xf1, 0x79, 0xbc, 0x25, 0xf4, 0xb5, 0xbe, 0xd2, 0xb9, 0xd9, 0x00, 0x08, 0x00, 0x14,
           0xa3, 0x82, 0x95, 0x4e, 0x4b, 0xe6, 0x7b, 0xf1, 0x17, 0x84, 0xc9, 0x7c, 0x82, 0x92, 0xc2, 0x75,
           0xbf, 0xe3, 0xed, 0x41, 0x80, 0x28, 0x00, 0x04, 0xc8, 0xfb, 0x0b, 0x4c};

/* --- the RFC 5769 2.4 sample request with long-term credentials ---
 * Key: MD5 over the UTF-8 username, realm and the SASLprep'd password, joined by colons
 * (RFC 8489 6.2): MD5("e3839ee38388e383aae38383e382afe382b9:example.org:TheMatrIX"). */
static const uint8_t V24[] = {
        0x00, 0x01, 0x00, 0x60, 0x21, 0x12, 0xa4, 0x42, 0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29,
        0xda, 0x41, 0x2e, 0x00, 0x06, 0x00, 0x12, 0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3,
        0x83, 0x83, 0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xb9, 0x00, 0x00, 0x00, 0x15, 0x00, 0x1c, 'f',  '/',  '/',
        '4',  '9',  '9',  'k',  '9',  '5',  '4',  'd',  '6',  'O',  'L',  '3',  '4',  'o',  'L',  '9',  'F',
        'S',  'T',  'v',  'y',  '6',  '4',  's',  'A',  0x00, 0x14, 0x00, 0x0b, 'e',  'x',  'a',  'm',  'p',
        'l',  'e',  '.',  'o',  'r',  'g',  0x00, 0x00, 0x08, 0x00, 0x14, 0xf6, 0x70, 0x24, 0x65, 0x6d, 0xd6,
        0x4a, 0x3e, 0x02, 0xb8, 0xe0, 0x71, 0x2e, 0x85, 0xc9, 0xa2, 0x8c, 0xa8, 0x96, 0x66};
static const char V24_USER[] = "\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83"
                               "\xe3\x82\xaf\xe3\x82\xb9"; /* the six UTF-8 characters */

int main(void)
{
    /* --- known digests for the embedded crypto --- */
    {
        uint8_t h[20];
        static const uint8_t want_md5[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                                             0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
        fci_tw_md5((const uint8_t *)"abc", 3, h);
        check1("crypto-md5", memcmp(h, want_md5, 16) == 0, "MD5(\"abc\") = 900150983cd24fb0...");
    }
    {
        uint8_t h[20];
        static const uint8_t want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                         0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
        fci_tw_sha1((const uint8_t *)"abc", 3, h);
        check1("crypto-sha1", memcmp(h, want, 20) == 0, "SHA-1(\"abc\") = a9993e364706816a...");
    }
    {
        /* RFC 2202 test case 2: key "Jefe", data "what do ya want for nothing?" */
        uint8_t h[20];
        static const uint8_t want[20] = {0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
                                         0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79};
        fci_tw_hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, h);
        check1("crypto-hmac", memcmp(h, want, 20) == 0, "RFC 2202 case 2 = effcdf6ae5eb2fa2...");
    }
    {
        uint32_t c = fci_tw_crc32((const uint8_t *)"123456789", 9);
        check1("crypto-crc32", c == 0xcbf43926u, "CRC-32(\"123456789\") = 0xcbf43926");
    }

    /* --- the XOR-ADDRESS vector: 192.0.2.1:32853 encodes as 00 01 a1 47 e1 12 a6 43 --- */
    {
        char p[8];
        struct sockaddr_in a = addr_192_0_2_1_32853(), back;
        static const uint8_t want[8] = {0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43};
        fci_tw_put_xaddr(p, &a);
        check1("xaddr-vector", memcmp(p, want, 8) == 0, "put 192.0.2.1:32853 -> a147 e112a643");
        fci_tw_get_xaddr((const char *)want, &back);
        check1("xaddr-roundtrip", same_addr(&back, &a), "get(a147 e112a643) -> 192.0.2.1:32853");
    }

    /* --- RFC 5769 2.1: rebuild the sample request byte for byte ---
     * A change to how fci_tw_build writes the length field (or the FINGERPRINT) breaks this:
     * the MI is computed over the length field, and the FINGERPRINT is computed over the MI. */
    {
        char out[128];
        static const char key[] = "VOkJxbRl1RmTxUk/WvJxBt";
        int n = fci_tw_build(out, M_BIND, FCI_TW_C_REQ, V21_TXN, (const char *)V21_BODY, (int)sizeof V21_BODY,
                             1, 1, (const uint8_t *)key, (int)strlen(key));
        check1("rfc5769-2-1-build", n == (int)sizeof V21_FULL && memcmp(out, V21_FULL, sizeof V21_FULL) == 0,
               "108 bytes, MI and FINGERPRINT match the vector");
    }

    /* --- RFC 5769 2.2: parse the sample IPv4 response, verify the integrity --- */
    {
        fci_tw_msg_t m = {0};
        static const char key[] = "VOkJxbRl1RmTxUk/WvJxBt";
        struct sockaddr_in want = addr_192_0_2_1_32853();
        int r = fci_tw_parse((const char *)V22, (int)sizeof V22, &m, (const uint8_t *)key, (int)strlen(key));
        check1("rfc5769-2-2-parse",
               r == FCI_TW_OK && m.integrity_present && m.integrity_valid && m.have_mapped
                       && same_addr(&m.mapped, &want) && m.cls == FCI_TW_C_SUCCESS && m.method == M_BIND,
               "integrity valid, mapped 192.0.2.1:32853");
    }

    /* --- RFC 5769 2.3: the sample IPv6 response (no mapped: the parser takes IPv4 only) --- */
    {
        fci_tw_msg_t m = {0};
        static const char key[] = "VOkJxbRl1RmTxUk/WvJxBt";
        int r = fci_tw_parse((const char *)V23, (int)sizeof V23, &m, (const uint8_t *)key, (int)strlen(key));
        check1("rfc5769-2-3-parse",
               r == FCI_TW_OK && m.integrity_present && m.integrity_valid && !m.have_mapped,
               "integrity valid, the IPv6 address is skipped");
    }

    /* --- RFC 5769 2.4: the long-term request; the key is MD5(user:realm:password) --- */
    {
        fci_tw_msg_t m = {0};
        char kbuf[64];
        int kl = (int)strlen(V24_USER) + (int)strlen(":example.org:") + (int)strlen("TheMatrIX");
        memcpy(kbuf, V24_USER, strlen(V24_USER));
        memcpy(kbuf + strlen(V24_USER), ":example.org:TheMatrIX", strlen(":example.org:TheMatrIX"));
        fci_tw_md5((const uint8_t *)kbuf, kl, (uint8_t *)kbuf);
        int r = fci_tw_parse((const char *)V24, (int)sizeof V24, &m, (const uint8_t *)kbuf, 16);
        check1("rfc5769-2-4-parse",
               r == FCI_TW_OK && m.integrity_present && m.integrity_valid && m.have_realm
                       && strcmp(m.realm, "example.org") == 0 && m.have_nonce
                       && strcmp(m.nonce, "f//499k954d6OL34oL9FSTvy64sA") == 0 && m.cls == FCI_TW_C_REQ,
               "integrity valid, realm and nonce parsed");
    }

    /* --- a corrupted integrity must not validate --- */
    {
        fci_tw_msg_t m = {0};
        static const char key[] = "VOkJxbRl1RmTxUk/WvJxBt";
        uint8_t bad[80];
        memcpy(bad, V22, sizeof V22);
        bad[40] ^= 0x01; /* corrupt the SOFTWARE value, before the MI */
        int r = fci_tw_parse((const char *)bad, (int)sizeof bad, &m, (const uint8_t *)key, (int)strlen(key));
        check1("mi-corrupt", r == FCI_TW_OK && m.integrity_present && !m.integrity_valid,
               "the HMAC no longer matches");
    }

    /* --- the client's 401 round trip: build with integrity, parse it back --- */
    {
        char body[96], msg[256];
        int blen = 0;
        uint8_t txn[12] = {0};
        static const uint8_t key[16] = "0123456789abcdef";
        /* ERROR-CODE 401: two zeros, the class (4) and the number (1) */
        uint8_t ec[4] = {0x00, 0x00, 0x04, 0x01};
        blen += fci_tw_put_attr(body, FCI_TW_A_ERROR_CODE, ec, 4);
        blen += fci_tw_put_attr(body + blen, FCI_TW_A_REALM, "example.org", 11);
        blen += fci_tw_put_attr(body + blen, FCI_TW_A_NONCE, "nonce-1", 7);
        int n = fci_tw_build(msg, M_ALLOCATE, FCI_TW_C_ERROR, txn, body, blen, 1, 1, key, 16);
        fci_tw_msg_t m = {0};
        int r = fci_tw_parse(msg, n, &m, key, 16);
        check1("err-401",
               r == FCI_TW_OK && m.error_code == 401 && m.cls == FCI_TW_C_ERROR && m.method == M_ALLOCATE
                       && m.integrity_valid && m.have_realm && strcmp(m.realm, "example.org") == 0
                       && m.have_nonce && strcmp(m.nonce, "nonce-1") == 0,
               "code 401, realm and nonce, integrity valid");
    }

    /* --- the 438 (stale nonce) round trip --- */
    {
        char body[96], msg[256];
        int blen = 0;
        uint8_t txn[12] = {0};
        static const uint8_t key[16] = "0123456789abcdef";
        uint8_t ec[4] = {0x00, 0x00, 0x04, 38}; /* class 4, number 38 */
        blen += fci_tw_put_attr(body, FCI_TW_A_ERROR_CODE, ec, 4);
        blen += fci_tw_put_attr(body + blen, FCI_TW_A_REALM, "example.org", 11);
        blen += fci_tw_put_attr(body + blen, FCI_TW_A_NONCE, "nonce-2", 7);
        int n = fci_tw_build(msg, M_ALLOCATE, FCI_TW_C_ERROR, txn, body, blen, 1, 1, key, 16);
        fci_tw_msg_t m = {0};
        int r = fci_tw_parse(msg, n, &m, key, 16);
        check1("err-438", r == FCI_TW_OK && m.error_code == 438 && m.integrity_valid && m.have_nonce,
               "code 438, integrity valid");
    }

    /* --- the Allocate success round trip: XOR-RELAYED-ADDRESS and LIFETIME --- */
    {
        char body[64], msg[256];
        int blen = 0;
        uint8_t txn[12] = {0};
        static const uint8_t key[16] = "0123456789abcdef";
        struct sockaddr_in relay = addr_192_0_2_1_32853(), x;
        fci_tw_put_xaddr((char *)&x, &relay);
        blen += fci_tw_put_attr(body, FCI_TW_A_XOR_RELAYED, &x, 8);
        uint8_t lb[4];
        fci_tw_put_u32((char *)lb, 30000);
        blen += fci_tw_put_attr(body + blen, FCI_TW_A_LIFETIME, lb, 4);
        int n = fci_tw_build(msg, M_ALLOCATE, FCI_TW_C_SUCCESS, txn, body, blen, 1, 1, key, 16);
        fci_tw_msg_t m = {0};
        int r = fci_tw_parse(msg, n, &m, key, 16);
        check1("allocate-ok",
               r == FCI_TW_OK && m.cls == FCI_TW_C_SUCCESS && m.method == M_ALLOCATE && m.have_relayed
                       && same_addr(&m.relayed, &relay) && m.have_lifetime && m.lifetime == 30000
                       && m.integrity_valid,
               "relayed 192.0.2.1:32853, lifetime 30000, integrity valid");
    }

    /* --- hostile inputs --- */
    {
        fci_tw_msg_t m = {0};
        int r = fci_tw_parse((const char *)V22, 10, &m, NULL, 0);
        check1("trunc-10", r == FCI_TW_SHORT, "10 bytes is no STUN header");
    }
    {
        fci_tw_msg_t m = {0};
        /* the 2.2 header, but the XOR-MAPPED-ADDRESS claims 8 value bytes of which 4 are
         * missing: the attribute runs past the end */
        uint8_t bad[28];
        memcpy(bad, V22, 28);
        int r = fci_tw_parse((const char *)bad, 28, &m, NULL, 0);
        check1("attr-trunc", r == FCI_TW_TRUNC, "the attribute length runs past the end");
    }
    {
        fci_tw_msg_t m = {0};
        uint8_t bad[80];
        memcpy(bad, V22, sizeof V22);
        bad[4] = 0x00; /* corrupt the magic cookie */
        int r = fci_tw_parse((const char *)bad, (int)sizeof bad, &m, NULL, 0);
        check1("wrong-cookie", r == FCI_TW_COOKIE, "the magic cookie does not match");
    }

    printf("%d checks, %d failed\n", ntotal, nfail);
    return nfail ? 1 : 0;
}
