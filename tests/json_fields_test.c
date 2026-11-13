/* Driver for tests/test_json_fields_c.py: runs the shipping JSON field readers (src/fci_json.c)
 * and the connect parser (src/fci_connect.c) natively, one line per check. Each reader is tried
 * with the field present, absent, the wrong type, spaces around the colon, and a key that is a
 * prefix of another key ("seq" vs "seqx"); fci_json_u64 also with 0, a 15-digit number and a
 * non-number. fci_connect_parse is tried with a full direct entry, a relay entry, an entry with
 * local, a missing field, a 31-char nonce, a bad mac, and a seq-less entry. Only the documentation
 * address blocks (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24) are used.
 *
 * Compiled and run by tests/test_json_fields_c.py:
 *
 *   cc -std=gnu11 -O2 -Wall -Werror src/fci_json.c src/fci_connect.c tests/json_fields_test.c -o ...
 */
#include <stdio.h>
#include <string.h>

#include "fci_connect.h"
#include "fci_json.h"

static int nfail, ntotal;

static void check(const char *name, int ok, const char *detail)
{
    ntotal++;
    if (ok) {
        printf("ok   %s:\n", name);
    } else {
        nfail++;
        printf("FAIL %s: %s\n", name, detail);
    }
}

/* a 32-char nonce hex: 00..0f (16 bytes). */
#define NONCE_HEX "000102030405060708090a0b0c0d0e0f"
/* a valid mac (documentation MAC, 02:00:02:01:02:03). */
#define MAC_STR "02:00:02:01:02:03"

/* the expected nonce bytes for NONCE_HEX. */
static const uint8_t WANT_NONCE[16] = {0x00,  0x01,  0x02,  0x03,  0x04,  0x05,  0x06,  0x07,
                                       0x08,  0x09,  0x0a,  0x0b,  0x0c,  0x0d, 0x0e, 0x0f};
static const uint8_t WANT_MAC[6] = {0x02, 0x00, 0x02, 0x01, 0x02, 0x03};

/* --- fci_json_str: present, absent, wrong type, spaces, key prefix --- */
static void t_json_str(void)
{
    const char *p;

    p = fci_json_str("{\"a\":\"hello\"}", "a");
    check("str-present", p != NULL && strncmp(p, "hello", 5) == 0, "want a pointer at \"hello\"");

    p = fci_json_str("{\"b\":\"x\"}", "a");
    check("str-absent", p == NULL, "want NULL");

    p = fci_json_str("{\"a\":123}", "a");
    check("str-wrong-type", p == NULL, "a number is not a string");

    p = fci_json_str("{\"a\" : \"hello\"}", "a");
    check("str-spaces", p != NULL && strncmp(p, "hello", 5) == 0, "want a pointer at \"hello\"");

    /* "seq" is a prefix of "seqx": the reader must find "seq", not "seqx" */
    p = fci_json_str("{\"seq\":\"1\",\"seqx\":\"2\"}", "seq");
    check("str-key-prefix", p != NULL && p[0] == '1', "want the value of \"seq\" (1), not \"seqx\" (2)");
}

/* --- fci_json_str_val: present, absent, wrong type, spaces, key prefix, empty --- */
static void t_json_str_val(void)
{
    char out[32];
    int r;

    memset(out, 0, sizeof out);
    r = fci_json_str_val("{\"a\":\"hello\"}", "a", out, sizeof out);
    check("strval-present", r == 1 && !strcmp(out, "hello"), "want 1 and \"hello\"");

    r = fci_json_str_val("{\"b\":\"x\"}", "a", out, sizeof out);
    check("strval-absent", r == 0, "want 0");

    r = fci_json_str_val("{\"a\":123}", "a", out, sizeof out);
    check("strval-wrong-type", r == 0, "a number is not a string");

    memset(out, 0, sizeof out);
    r = fci_json_str_val("{\"a\" : \"hello\"}", "a", out, sizeof out);
    check("strval-spaces", r == 1 && !strcmp(out, "hello"), "want 1 and \"hello\"");

    memset(out, 0, sizeof out);
    r = fci_json_str_val("{\"seq\":\"1\",\"seqx\":\"2\"}", "seq", out, sizeof out);
    check("strval-key-prefix", r == 1 && !strcmp(out, "1"), "want \"1\" (seq), not \"2\" (seqx)");

    r = fci_json_str_val("{\"a\":\"\"}", "a", out, sizeof out);
    check("strval-empty", r == 0, "an empty value is not found");
}

/* --- fci_json_bool: present true/false, absent, wrong type, spaces after the colon --- */
static void t_json_bool(void)
{
    int r;

    r = fci_json_bool("{\"a\":true}", "a");
    check("bool-true", r == 1, "want 1");

    r = fci_json_bool("{\"a\":false}", "a");
    check("bool-false", r == 0, "want 0");

    r = fci_json_bool("{\"b\":true}", "a");
    check("bool-absent", r == -1, "want -1");

    r = fci_json_bool("{\"a\":\"true\"}", "a");
    check("bool-wrong-type", r == -1, "the string \"true\" is not a boolean");

    r = fci_json_bool("{\"a\": true}", "a");
    check("bool-spaces-after", r == 1, "want 1 (space after the colon)");
}

/* --- fci_json_u64: present, 0, 15 digits, absent, non-number, spaces --- */
static void t_json_u64(void)
{
    unsigned long long v;

    v = fci_json_u64("{\"seq\":5}", "seq");
    check("u64-present", v == 5, "want 5");

    v = fci_json_u64("{\"seq\":0}", "seq");
    check("u64-zero", v == 0, "want 0");

    /* 999999999999999 is 15 digits, fits a u64 and is not truncated to a 32-bit value */
    v = fci_json_u64("{\"seq\":999999999999999}", "seq");
    check("u64-15-digits", v == 999999999999999ULL, "want 999999999999999");

    v = fci_json_u64("{\"other\":5}", "seq");
    check("u64-absent", v == 0, "want 0 when the key is absent");

    v = fci_json_u64("{\"seq\":\"5\"}", "seq");
    check("u64-non-number", v == 0, "a quoted string is not a number");

    v = fci_json_u64("{\"seq\":-5}", "seq");
    check("u64-negative", v == 0, "a sign is not a seq (strtoull would wrap it)");

    v = fci_json_u64("{\"seq\": 5}", "seq");
    check("u64-spaces-after", v == 5, "want 5 (space after the colon)");
}

/* --- fci_connect_parse --- */
static int nonce_mac_ok(const fci_connect_t *c)
{
    return !memcmp(c->nonce, WANT_NONCE, 16) && !memcmp(c->mac, WANT_MAC, 6);
}

static void t_connect_parse(void)
{
    fci_connect_t c;

    /* a full direct entry: endpoint, nonce, mac, no relay, no local */
    int r = fci_connect_parse("{\"seq\":7,\"endpoint\":\"192.0.2.10:3074\",\"nonce\":\"" NONCE_HEX
                              "\",\"mac\":\"" MAC_STR "\"}",
                              &c);
    check("connect-direct", r == 1 && !strcmp(c.endpoint, "192.0.2.10:3074") && c.relay == 0
                                      && c.local[0] == 0 && nonce_mac_ok(&c),
          "want a direct connect at 192.0.2.10:3074");

    /* a relay entry: relay true, no local */
    r = fci_connect_parse("{\"seq\":8,\"endpoint\":\"198.51.100.20:3074\",\"nonce\":\"" NONCE_HEX
                          "\",\"mac\":\"" MAC_STR "\",\"relay\":true}",
                          &c);
    check("connect-relay", r == 1 && !strcmp(c.endpoint, "198.51.100.20:3074") && c.relay == 1
                                      && c.local[0] == 0 && nonce_mac_ok(&c),
          "want a relay connect at 198.51.100.20:3074");

    /* an entry with local: the guest's LAN address is forwarded (same NAT) */
    r = fci_connect_parse("{\"seq\":9,\"endpoint\":\"203.0.113.30:3074\",\"nonce\":\"" NONCE_HEX
                          "\",\"mac\":\"" MAC_STR "\",\"local\":\"192.168.1.50:3074\"}",
                          &c);
    check("connect-local", r == 1 && !strcmp(c.endpoint, "203.0.113.30:3074") && c.relay == 0
                                      && !strcmp(c.local, "192.168.1.50:3074") && nonce_mac_ok(&c),
          "want local 192.168.1.50:3074");

    /* a missing field: no mac */
    r = fci_connect_parse("{\"seq\":10,\"endpoint\":\"192.0.2.10:3074\",\"nonce\":\"" NONCE_HEX "\"}",
                          &c);
    check("connect-missing-mac", r == 0, "want 0 (no mac)");

    /* a 31-char nonce (one char short) */
    r = fci_connect_parse("{\"seq\":11,\"endpoint\":\"192.0.2.10:3074\",\"nonce\":\""
                          "000102030405060708090a0b0c0d0e0\""
                          "\"mac\":\"" MAC_STR "\"}",
                          &c);
    check("connect-nonce-31", r == 0, "want 0 (a 31-char nonce)");

    /* a bad mac (five bytes, not six) */
    r = fci_connect_parse("{\"seq\":12,\"endpoint\":\"192.0.2.10:3074\",\"nonce\":\"" NONCE_HEX
                          "\",\"mac\":\"02:00:02:01:02\"}",
                          &c);
    check("connect-bad-mac", r == 0, "want 0 (a five-byte mac)");

    /* a seq-less entry (an older list: no seq, so seq 0): the parser does not look at seq */
    r = fci_connect_parse("{\"endpoint\":\"192.0.2.10:3074\",\"nonce\":\"" NONCE_HEX
                          "\",\"mac\":\"" MAC_STR "\"}",
                          &c);
    check("connect-no-seq", r == 1 && !strcmp(c.endpoint, "192.0.2.10:3074") && nonce_mac_ok(&c),
          "want a connect even with no seq");
}

int main(void)
{
    t_json_str();
    t_json_str_val();
    t_json_bool();
    t_json_u64();
    t_connect_parse();

    printf("%d checks, %d failed\n", ntotal, nfail);
    return nfail ? 1 : 0;
}
