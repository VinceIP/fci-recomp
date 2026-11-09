/* The TURN relay client (RFC 8656 over the RFC 8489 wire). See fci_turn.h for the contract.
 *
 * The state machine: fci_turn_start sends the Allocate bare; the server's 401 carries the realm
 * and nonce, the key MD5(user:realm:pass) is computed, and the Allocate is resent with USERNAME,
 * REALM, NONCE, MESSAGE-INTEGRITY and FINGERPRINT. A 438 (stale nonce) takes the new nonce and
 * resends the same request once; a 401 on an already-authenticated request, a 437 (no
 * allocation), any other error code, or 7 unanswered retransmits puts the state to failed.
 * While ready: Refresh at half the LIFETIME, CreatePermission for every peer every 240 s
 * (permissions live 300 s) and ChannelBind for every peer every 480 s (channels live 600 s).
 * There is one in-flight request at a time; the tick feeds the next from the queue.
 *
 * Frames: ChannelData (the channel is bound to the peer's address) and Data indications (the
 * server sends those for a peer whose port changed) are queued for fci_turn_recv. A Data
 * indication from a known peer's IP on a new port rebinds the peer's channel to that address;
 * the frame is reported with the new address so the NIC can update the peer.
 *
 * The MD5, SHA-1, HMAC-SHA1 and CRC-32 below are small embedded implementations (the module
 * must also build standalone in tools/turn_probe.c, and the self-test pins them to the RFC 5769
 * 2.1 and 2.4 vectors instead of a provider); the code they protect is the same code the
 * client uses to sign its requests. */
#include "fci_turn.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define TURN_MAGIC 0x2112A442u
#define TURN_RTO_MS 500ULL
#define TURN_MAX_TRIES 7
#define TURN_PERM_REFRESH_MS 240000ULL
#define TURN_CHAN_REFRESH_MS 480000ULL
#define TURN_FRAME_MAX 1514
#define TURN_FRAMES 64
#define TURN_PEERS 16
#define TURN_CHANNEL_FIRST 0x4000
#define TURN_CHANNEL_LAST 0x4FFF

/* the STUN method numbers (RFC 8489 2, RFC 8656) and message classes (RFC 8489 5) */
#define M_ALLOCATE 3
#define M_REFRESH 4
#define M_CREATE_PERM 8
#define M_CHANNEL_BIND 9
#define M_SEND 6
#define M_DATA 7
#define C_REQ 0
#define C_IND 1
#define C_SUCCESS 2
#define C_ERROR 3

/* the STUN message type (RFC 8489 5): the two class bits are interleaved with the method
 * bits, so Allocate is 0x0003/0x0103/0x0113, Refresh 0x0004/0x0104/0x0114, the Send and
 * Data indications 0x0016 and 0x0017, CreatePermission 0x0008/0x0108/0x0118 and ChannelBind
 * 0x0009/0x0109/0x0119 (pinned by fci_turn_selftest and the captured Cloudflare 401 in
 * tests/test_stun_vectors.py) */
static uint16_t stun_type(int method, int cls)
{
    return (uint16_t)(((method & 0x0F80) << 2) | ((method & 0x0070) << 1) | (method & 0x000F) |
                      ((cls & 2) << 7) | ((cls & 1) << 4));
}

/* the method and the class of a message type (RFC 8489 5) */
static int type_method(uint16_t t)
{
    return (((t & 0x3E00) >> 9) << 7) | (((t & 0x0060) >> 5) << 4) | (t & 0x000F);
}

static int type_class(uint16_t t)
{
    return ((t & 0x0100) >> 7) | ((t & 0x0010) >> 4);
}

/* the STUN attribute types */
#define A_USERNAME 0x0006
#define A_MESSAGE_INTEGRITY 0x0008
#define A_ERROR_CODE 0x0009
#define A_CHANNEL 0x000c
#define A_LIFETIME 0x000d
#define A_XOR_PEER 0x0012
#define A_DATA 0x0013
#define A_REALM 0x0014
#define A_NONCE 0x0015
#define A_XOR_RELAYED 0x0016
#define A_TRANSPORT 0x0019
#define A_FINGERPRINT 0x8028

/* --- logging: [FCI] network: turn lines, like the NIC's; the password and the key never appear */
static void log_turn(const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[340];
    sprintf_s(line, sizeof(line), "[FCI] network: turn %s", buf);
    OutputDebugStringA(line);
    const char *path = getenv("RECOMP_FCI_TURN_LOG");
    if (path && *path) {
        FILE *f = fopen(path, "a");
        if (f) {
            fputs(line, f);
            fputc('\n', f);
            fclose(f);
        }
    }
}

/* --- crypto: MD5 (RFC 1321), SHA-1 (FIPS 180), HMAC-SHA1 (RFC 2104), CRC-32 (IEEE, the zlib
 *     convention) — pinned by fci_turn_selftest --- */

static uint32_t rol32(uint32_t v, int s)
{
    return (v << s) | (v >> (32 - s));
}

static void md5(const uint8_t *in, int inlen, uint8_t out[16])
{
    static const uint32_t K[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
        0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
        0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
        0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
        0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
        0xeb86d391u,
    };
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint8_t buf[128];
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t bits;
    int i, n, total;
    if (inlen > 72)
        return;
    memcpy(buf, in, (size_t)inlen);
    buf[inlen] = 0x80;
    total = inlen < 56 ? 64 : 128;
    memset(buf + inlen + 1, 0, (size_t)(total - inlen - 1));
    bits = (uint64_t)inlen * 8u;
    memcpy(buf + total - 8, &bits, 8);
    for (n = 0; n < total; n += 64) {
        uint32_t M[16];
        uint32_t a, b, c, d;
        for (i = 0; i < 16; i++)
            memcpy(&M[i], buf + n + i * 4, 4);
        a = h[0];
        b = h[1];
        c = h[2];
        d = h[3];
        for (i = 0; i < 64; i++) {
            uint32_t f, g, t;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = (uint32_t)i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5u * i + 1u) & 15u;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3u * i + 5u) & 15u;
            } else {
                f = c ^ (b | ~d);
                g = (7u * i) & 15u;
            }
            t = a + f + K[i] + M[g];
            a = d;
            d = c;
            c = b;
            b = b + rol32(t, S[i]);
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    for (i = 0; i < 4; i++)
        memcpy(out + i * 4, &h[i], 4);
}

typedef struct
{
    uint32_t h[5];
    uint64_t bits;
    uint8_t block[64];
    int n;
} sha1_ctx_t;

static void sha1_begin(sha1_ctx_t *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->bits = 0;
    c->n = 0;
}

static void sha1_block(sha1_ctx_t *c, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, cc, d, e;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_update(sha1_ctx_t *c, const uint8_t *data, int len)
{
    while (len > 0) {
        int take = 64 - c->n;
        if (take > len)
            take = len;
        memcpy(c->block + c->n, data, (size_t)take);
        c->n += take;
        c->bits += (uint64_t)take * 8u;
        data += take;
        len -= take;
        if (c->n == 64) {
            sha1_block(c, c->block);
            c->n = 0;
        }
    }
}

static void sha1_finish(sha1_ctx_t *c, uint8_t out[20])
{
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    uint8_t zero = 0;
    uint8_t lenb[8];
    int i;
    sha1_update(c, &pad, 1);
    while (c->n != 56)
        sha1_update(c, &zero, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

static void sha1(const uint8_t *data, int len, uint8_t out[20])
{
    sha1_ctx_t c;
    sha1_begin(&c);
    sha1_update(&c, data, len);
    sha1_finish(&c, out);
}

static void hmac_sha1(const uint8_t *key, int keylen, const uint8_t *data, int datalen, uint8_t out[20])
{
    uint8_t k[64];
    uint8_t inner[20];
    sha1_ctx_t c;
    int i;
    if (keylen > 64) {
        sha1(key, keylen, k);
        keylen = 20;
    } else {
        memcpy(k, key, (size_t)keylen);
    }
    memset(k + keylen, 0, (size_t)(64 - keylen));
    for (i = 0; i < 64; i++)
        k[i] ^= 0x36;
    sha1_begin(&c);
    sha1_update(&c, k, 64);
    sha1_update(&c, data, datalen);
    sha1_finish(&c, inner);
    for (i = 0; i < 64; i++)
        k[i] ^= 0x36 ^ 0x5c;
    sha1_begin(&c);
    sha1_update(&c, k, 64);
    sha1_update(&c, inner, 20);
    sha1_finish(&c, out);
}

static uint32_t s_crctab[256];
static int s_crc_ready;

static void crc32_init(void)
{
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crctab[n] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc32(const uint8_t *data, int len)
{
    uint32_t c = 0xFFFFFFFFu;
    if (!s_crc_ready)
        crc32_init();
    for (int i = 0; i < len; i++)
        c = s_crctab[(c ^ (uint32_t)data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* --- STUN message building (RFC 8489 6, 14, RFC 8656 15) --- */

static void put_u16(char *p, uint16_t v)
{
    p[0] = (char)(v >> 8);
    p[1] = (char)v;
}

static void put_u32(char *p, uint32_t v)
{
    p[0] = (char)(v >> 24);
    p[1] = (char)(v >> 16);
    p[2] = (char)(v >> 8);
    p[3] = (char)v;
}

static uint16_t get_u16(const char *p)
{
    return (uint16_t)(((uint16_t)(uint8_t)p[0] << 8) | (uint16_t)(uint8_t)p[1]);
}

/* append a STUN attribute, the value zero-padded to a 4-byte boundary; the bytes written */
static int put_attr(char *p, uint16_t type, const void *v, int len)
{
    int pad = (4 - (len & 3)) & 3;
    put_u16(p, type);
    put_u16(p + 2, (uint16_t)len);
    memcpy(p + 4, v, (size_t)len);
    memset(p + 4 + len, 0, (size_t)pad);
    return 4 + len + pad;
}

/* the attribute's stride in a message: the header plus the padded value */
static int attr_stride(uint16_t len)
{
    return 4 + ((len + 3) & ~3);
}

/* XOR-ADDRESS, IPv4: the port XOR 0x2112 and the address XOR 0x2112A442 (RFC 8656 15.12) */
static void put_xaddr(char *p, const struct sockaddr_in *a)
{
    uint16_t port = (uint16_t)(ntohs(a->sin_port) ^ 0x2112u);
    uint32_t addr = ntohl(a->sin_addr.s_addr) ^ TURN_MAGIC;
    p[0] = 0;
    p[1] = 1;
    put_u16(p + 2, port);
    put_u32(p + 4, addr);
}

static int get_xaddr(const char *p, struct sockaddr_in *a)
{
    uint32_t addr;
    if (p[1] != 1)
        return 0;
    /* the wire is big-endian: read it as a number before the XOR (as put_xaddr writes it) */
    addr = ((uint32_t)(uint8_t)p[4] << 24) | ((uint32_t)(uint8_t)p[5] << 16) | ((uint32_t)(uint8_t)p[6] << 8) |
           (uint32_t)(uint8_t)p[7];
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)(get_u16(p + 2) ^ 0x2112u));
    a->sin_addr.s_addr = htonl(addr ^ TURN_MAGIC);
    return 1;
}

/* the header and the attributes of a message; with integrity, MESSAGE-INTEGRITY (HMAC-SHA1
 * with key) is appended, and with fingerprint, FINGERPRINT (CRC-32 XOR 0x5354554E) is appended
 * after it. The length field holds the message length up to the attribute being computed (the
 * final length for the FINGERPRINT, which is last); the RFC 5769 2.1 vector fixes the
 * convention. Returns the message length in bytes. */
static int finish_msg(char *out, int method, int cls, const uint8_t txn[12], const char *body,
                      int bodylen, int integrity, int fingerprint, const uint8_t key[16], int keylen)
{
    int off = 20 + bodylen;
    int len = bodylen;
    put_u16(out, stun_type(method, cls));
    out[2] = 0;
    out[3] = 0;
    put_u32(out + 4, TURN_MAGIC);
    memcpy(out + 8, txn, 12);
    memcpy(out + 20, body, (size_t)bodylen);
    if (integrity) {
        uint8_t mi[20];
        len += 24;
        put_u16(out + 2, (uint16_t)len);
        hmac_sha1(key, keylen, (const uint8_t *)out, off, mi);
        off += put_attr(out + off, A_MESSAGE_INTEGRITY, mi, 20);
    }
    if (fingerprint) {
        uint32_t fp;
        char fpb[4];
        len += 8;
        put_u16(out + 2, (uint16_t)len);
        fp = crc32((const uint8_t *)out, off) ^ 0x5354554Eu;
        put_u32(fpb, fp);
        off += put_attr(out + off, A_FINGERPRINT, fpb, 4);
    }
    if (!integrity && !fingerprint)
        put_u16(out + 2, (uint16_t)len);
    return off;
}

/* --- the module state: one allocation, one in-flight request at a time, 16 peers, 64 frames --- */

typedef struct
{
    struct sockaddr_in addr;
    int channel;
    int want_perm;
    int want_chan;
    int have_perm;
    int have_chan;
} turn_peer_t;

typedef struct
{
    int method;
    int auth;
    uint8_t txn[12];
    int tries;
    ULONGLONG next;
    int peer;
    int lifetime;
} turn_req_t;

static int s_state = FCI_TURN_IDLE;
static SOCKET s_sock = INVALID_SOCKET;
static char s_server[128];
static char s_user[128];
static char s_pass[128];
static char s_realm[128];
static char s_nonce[256];
static uint8_t s_key[16];
static int s_have_key;
static int s_stale_done;

static struct sockaddr_in s_relayed;
static ULONGLONG s_next_refresh;
static ULONGLONG s_next_perm;
static ULONGLONG s_next_chan;

static turn_req_t s_req;
static int s_have_req;
static turn_peer_t s_peers[TURN_PEERS];
static int s_npeers;
static int s_next_channel;
static int s_drop_noperm;

/* the live channel->peer map: a peer keeps its old channel mapped while that binding
 * still lives on the server (RFC 8656 12), so inbound data on either channel reaches it */
#define TURN_CHAN_SLOTS 64
#define TURN_CHAN_LIFETIME_MS 600000ULL
typedef struct
{
    int channel;
    int peer;
    ULONGLONG deadline;
} turn_chan_t;
static turn_chan_t s_chans[TURN_CHAN_SLOTS];
static int s_nchans;

static char s_fq[TURN_FRAMES][TURN_FRAME_MAX];
static int s_fq_len[TURN_FRAMES];
static struct sockaddr_in s_fq_from[TURN_FRAMES];
static int s_fq_head;
static int s_fq_count;

static void new_txn(uint8_t txn[12])
{
    static uint32_t seed;
    LARGE_INTEGER pc;
    if (!seed) {
        QueryPerformanceCounter(&pc);
        seed = (uint32_t)pc.QuadPart;
    }
    for (int i = 0; i < 12; i += 4) {
        seed = seed * 1664525u + 1013904223u;
        put_u32((char *)txn + i, seed ^ (uint32_t)GetTickCount64());
    }
}

static void frame_enqueue(const struct sockaddr_in *from, const void *data, int len)
{
    int slot;
    if (len > TURN_FRAME_MAX)
        len = TURN_FRAME_MAX;
    if (s_fq_count == TURN_FRAMES) {
        s_fq_head = (s_fq_head + 1) % TURN_FRAMES;
        s_fq_count--;
    }
    slot = (s_fq_head + s_fq_count) % TURN_FRAMES;
    memcpy(s_fq[slot], data, (size_t)len);
    s_fq_len[slot] = len;
    s_fq_from[slot] = *from;
    s_fq_count++;
}

static int send_msg(const char *msg, int len)
{
    if ((int)send(s_sock, msg, len, 0) != len) {
        log_turn("send failed: %d", WSAGetLastError());
        return 0;
    }
    return 1;
}

static void state_failed(const char *why)
{
    log_turn("failed: %s", why);
    s_state = FCI_TURN_FAILED;
    s_have_req = 0;
}

/* the attributes of a request body (without the auth attributes, which the caller appends
 * when auth is set); the bytes written */
static int build_req_body(int method, int peer, int lifetime, int auth, char *body)
{
    char x[8];
    int blen = 0;
    if (method == M_ALLOCATE) {
        static const uint8_t rt[4] = {0x11, 0, 0, 0}; /* IANA protocol 17, UDP: the protocol is the first byte */
        blen += put_attr(body, A_TRANSPORT, rt, 4);
    }
    if (method == M_REFRESH) {
        char lb[4];
        put_u32(lb, (uint32_t)lifetime);
        blen += put_attr(body, A_LIFETIME, lb, 4);
    }
    if (method == M_CREATE_PERM) {
        put_xaddr(x, &s_peers[peer].addr);
        blen += put_attr(body, A_XOR_PEER, x, 8);
    }
    if (method == M_CHANNEL_BIND) {
        char cb[2];
        put_u16(cb, (uint16_t)s_peers[peer].channel);
        blen += put_attr(body, A_CHANNEL, cb, 2);
        put_xaddr(x, &s_peers[peer].addr);
        blen += put_attr(body + blen, A_XOR_PEER, x, 8);
    }
    if (auth) {
        blen += put_attr(body + blen, A_USERNAME, s_user, (int)strlen(s_user));
        blen += put_attr(body + blen, A_REALM, s_realm, (int)strlen(s_realm));
        blen += put_attr(body + blen, A_NONCE, s_nonce, (int)strlen(s_nonce));
    }
    return blen;
}

/* send one request and start (or restart, when retry is set) its retransmit clock */
static void req_send(turn_req_t *r, ULONGLONG now, int retry)
{
    char body[768];
    char msg[1152];
    int blen = build_req_body(r->method, r->peer, r->lifetime, r->auth, body);
    int len = finish_msg(msg, r->method, C_REQ, r->txn, body, blen, r->auth, r->auth, s_key, 16);
    if (retry) {
        r->tries = 1;
        r->next = now + TURN_RTO_MS;
    }
    s_have_req = 1;
    if (!send_msg(msg, len))
        s_have_req = 0;
}

static void req_start(int method, int auth, int peer, int lifetime, ULONGLONG now)
{
    s_req.method = method;
    s_req.auth = auth;
    s_req.peer = peer;
    s_req.lifetime = lifetime;
    s_req.tries = 1;
    s_req.next = now + TURN_RTO_MS;
    new_txn(s_req.txn);
    req_send(&s_req, now, 0);
}

/* the next request: a peer's pending permission or channel first, then the half-LIFETIME
 * Refresh, then the periodic permission and channel re-issues; nothing when none is due */
static void schedule(ULONGLONG now)
{
    int i;
    if (s_have_req || s_state != FCI_TURN_READY)
        return;
    for (i = 0; i < s_npeers; i++)
        if (s_peers[i].want_perm) {
            req_start(M_CREATE_PERM, 1, i, 0, now);
            return;
        }
    for (i = 0; i < s_npeers; i++)
        if (s_peers[i].want_chan) {
            req_start(M_CHANNEL_BIND, 1, i, 0, now);
            return;
        }
    if (now >= s_next_refresh) {
        req_start(M_REFRESH, 1, -1, 0, now);
        return;
    }
    if (now >= s_next_perm) {
        s_next_perm = now + TURN_PERM_REFRESH_MS;
        for (i = 0; i < s_npeers; i++)
            s_peers[i].want_perm = 1;
        schedule(now);
        return;
    }
    if (now >= s_next_chan) {
        s_next_chan = now + TURN_CHAN_REFRESH_MS;
        for (i = 0; i < s_npeers; i++)
            s_peers[i].want_chan = 1;
        schedule(now);
        return;
    }
}

static void handle_error(const char *d, int n, uint16_t type, ULONGLONG now)
{
    uint16_t code = 0;
    int off;
    for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
        uint16_t at = get_u16(d + off);
        uint16_t al = get_u16(d + off + 2);
        if (al > (uint16_t)(n - off - 4))
            return;
        if (at == A_ERROR_CODE && al >= 4) {
            /* two zero bytes, then the class (hundreds) and the number (RFC 8489 14.8) */
            code = (uint16_t)(((uint32_t)(d[off + 6] & 7) * 100u + (uint32_t)(uint8_t)d[off + 7]));
            break;
        }
    }
    if (code == 401) {
        if (s_have_key) {
            state_failed("the server rejected the credentials");
            return;
        }
        for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
            uint16_t at = get_u16(d + off);
            uint16_t al = get_u16(d + off + 2);
            if (al > (uint16_t)(n - off - 4))
                return;
            if (at == A_REALM && al < (uint16_t)sizeof(s_realm)) {
                memcpy(s_realm, d + off + 4, al);
                s_realm[al] = 0;
            }
            if (at == A_NONCE && al < (uint16_t)sizeof(s_nonce)) {
                memcpy(s_nonce, d + off + 4, al);
                s_nonce[al] = 0;
            }
        }
        if (!s_realm[0] || !s_nonce[0]) {
            state_failed("the 401 carried no realm and nonce");
            return;
        }
        {
            char mdbuf[384];
            int ml = (int)strlen(s_user) + 1 + (int)strlen(s_realm) + 1 + (int)strlen(s_pass);
            if (ml > (int)sizeof(mdbuf)) {
                state_failed("the credentials are too long");
                return;
            }
            sprintf_s(mdbuf, sizeof(mdbuf), "%s:%s:%s", s_user, s_realm, s_pass);
            md5((const uint8_t *)mdbuf, ml, s_key);
            s_have_key = 1;
        }
        log_turn("authenticated (realm %d chars, nonce %d chars)", (int)strlen(s_realm),
                 (int)strlen(s_nonce));
    } else if (code == 438) {
        if (s_stale_done) {
            state_failed("the nonce went stale twice");
            return;
        }
        s_stale_done = 1;
        for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
            uint16_t at = get_u16(d + off);
            uint16_t al = get_u16(d + off + 2);
            if (al > (uint16_t)(n - off - 4))
                return;
            if (at == A_NONCE && al < (uint16_t)sizeof(s_nonce)) {
                memcpy(s_nonce, d + off + 4, al);
                s_nonce[al] = 0;
            }
        }
        log_turn("stale nonce, retrying once");
    } else {
        char name[64];
        switch (code) {
        case 400: strcpy_s(name, sizeof(name), "Bad Request"); break;
        case 437: strcpy_s(name, sizeof(name), "No such allocation"); break;
        case 441: strcpy_s(name, sizeof(name), "Alternate server"); break;
        case 508: strcpy_s(name, sizeof(name), "Out of resources"); break;
        default:
            sprintf_s(name, sizeof(name), "error %u", (unsigned)code);
            break;
        }
        state_failed(name);
        return;
    }
    /* 401 (first) and 438: the same request again, authenticated, with a fresh clock */
    s_req.auth = 1;
    req_send(&s_req, now, 1);
}

static void handle_success(const char *d, int n, uint16_t type, ULONGLONG now)
{
    int method = type_method(type);
    int off;
    if (method == M_ALLOCATE) {
        int lifetime = 600;
        for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
            uint16_t at = get_u16(d + off);
            uint16_t al = get_u16(d + off + 2);
            if (al > (uint16_t)(n - off - 4))
                return;
            if (at == A_XOR_RELAYED && al == 8)
                get_xaddr(d + off + 4, &s_relayed);
            if (at == A_LIFETIME && al == 4) {
                uint32_t lt;
                memcpy(&lt, d + off + 4, 4);
                lifetime = (int)ntohl(lt);
            }
        }
        s_state = FCI_TURN_READY;
        s_stale_done = 0;
        s_next_refresh = now + (ULONGLONG)lifetime * 1000u / 2u;
        s_next_perm = now + TURN_PERM_REFRESH_MS;
        s_next_chan = now + TURN_CHAN_REFRESH_MS;
        log_turn("ready: relayed %s:%d, lifetime %d s", inet_ntoa(s_relayed.sin_addr),
                 ntohs(s_relayed.sin_port), lifetime);
    } else if (method == M_REFRESH) {
        for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
            uint16_t at = get_u16(d + off);
            uint16_t al = get_u16(d + off + 2);
            if (al > (uint16_t)(n - off - 4))
                return;
            if (at == A_LIFETIME && al == 4) {
                uint32_t lt;
                memcpy(&lt, d + off + 4, 4);
                s_next_refresh = now + (ULONGLONG)ntohl(lt) * 1000u / 2u;
            }
        }
    } else if (method == M_CREATE_PERM && s_req.peer >= 0) {
        s_peers[s_req.peer].want_perm = 0;
        s_peers[s_req.peer].have_perm = 1;
    } else if (method == M_CHANNEL_BIND && s_req.peer >= 0) {
        s_peers[s_req.peer].want_chan = 0;
        s_peers[s_req.peer].have_chan = 1;
        log_turn("channel 0x%04x bound to %s:%d", (unsigned)s_peers[s_req.peer].channel,
                 inet_ntoa(s_peers[s_req.peer].addr.sin_addr),
                 ntohs(s_peers[s_req.peer].addr.sin_port));
    }
}

/* add a live channel->peer mapping (refreshing it when the channel is re-bound); the old
 * entry for a re-bound peer stays until it lapses, so both channels reach the peer */
static void chan_add(int channel, int peer, ULONGLONG now)
{
    for (int i = 0; i < s_nchans; i++)
        if (s_chans[i].channel == channel) {
            s_chans[i].peer = peer;
            s_chans[i].deadline = now + TURN_CHAN_LIFETIME_MS;
            return;
        }
    if (s_nchans >= TURN_CHAN_SLOTS)
        return;
    s_chans[s_nchans].channel = channel;
    s_chans[s_nchans].peer = peer;
    s_chans[s_nchans].deadline = now + TURN_CHAN_LIFETIME_MS;
    s_nchans++;
}

/* drop the lapsed channel mappings (the server dropped them at the same time) */
static void chan_expire(ULONGLONG now)
{
    for (int i = 0; i < s_nchans; i++)
        if (now >= s_chans[i].deadline) {
            s_chans[i] = s_chans[s_nchans - 1];
            s_nchans--;
            i--;
        }
}

static int peer_by_channel(int channel)
{
    for (int i = 0; i < s_nchans; i++)
        if (s_chans[i].channel == channel)
            return s_chans[i].peer;
    return -1;
}

static int peer_by_ip(uint32_t ip)
{
    for (int i = 0; i < s_npeers; i++)
        if (s_peers[i].addr.sin_addr.s_addr == ip)
            return i;
    return -1;
}

/* the peer's port changed: its old channel stays bound to the old address until it lapses
 * (RFC 8656 12), so this address takes a fresh channel number; the permission (per IP,
 * RFC 8656 9) is kept. */
static void peer_rebind(int pi, uint16_t new_port)
{
    ULONGLONG now = GetTickCount64();
    if (s_next_channel > TURN_CHANNEL_LAST)
        return;
    log_turn("peer %s moved to port %d, new channel 0x%04x (old 0x%04x lapses)",
             inet_ntoa(s_peers[pi].addr.sin_addr), (int)new_port, (unsigned)s_next_channel,
             (unsigned)s_peers[pi].channel);
    s_peers[pi].channel = s_next_channel++;
    chan_add(s_peers[pi].channel, pi, now);
    s_peers[pi].addr.sin_port = htons(new_port);
    s_peers[pi].have_chan = 0;
    s_peers[pi].want_chan = 1;
}

/* a Data indication: the peer's port changed (the server relays it unbound), so rebind the
 * peer's channel to the new address; the frame is reported with the new address */
static void handle_data_indication(const char *d, int n)
{
    struct sockaddr_in peer = {0};
    const char *data = NULL;
    int datalen = 0;
    int off;
    for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
        uint16_t at = get_u16(d + off);
        uint16_t al = get_u16(d + off + 2);
        if (al > (uint16_t)(n - off - 4))
            return;
        if (at == A_XOR_PEER && al == 8)
            get_xaddr(d + off + 4, &peer);
        if (at == A_DATA && al > 0) {
            data = d + off + 4;
            datalen = al;
        }
    }
    if (!data)
        return;
    {
        int pi = peer_by_ip(peer.sin_addr.s_addr);
        if (pi < 0)
            return;
        if (ntohs(s_peers[pi].addr.sin_port) != ntohs(peer.sin_port))
            peer_rebind(pi, ntohs(peer.sin_port));
        /* the server relays only from permitted IPs, so data proves the permission exists even
         * before its success response arrives (else the first replies race it and are dropped) */
        s_peers[pi].have_perm = 1;
        frame_enqueue(&s_peers[pi].addr, data, datalen);
    }
}

static void handle_channel_data(const char *d, int n)
{
    int channel = get_u16(d);
    int flen = get_u16(d + 2);
    int pi;
    if (4 + flen > n)
        return;
    pi = peer_by_channel(channel);
    if (pi < 0)
        return;
    frame_enqueue(&s_peers[pi].addr, d + 4, flen);
}

static void handle_stun(const char *d, int n, ULONGLONG now)
{
    uint16_t type = get_u16(d);
    uint16_t len = get_u16(d + 2);
    uint32_t magic;
    if (n < 20 || n < 20 + len)
        return;
    memcpy(&magic, d + 4, 4);
    if (ntohl(magic) != TURN_MAGIC)
        return;
    int method = type_method(type);
    int cls = type_class(type);
    if (type == stun_type(M_DATA, C_IND)) {
        handle_data_indication(d, n);
        return;
    }
    if (cls != C_SUCCESS && cls != C_ERROR)
        return;
    if (method > M_CHANNEL_BIND)
        return;
    if (!s_have_req)
        return;
    if (memcmp(d + 8, s_req.txn, 12) != 0)
        return;
    if (method != s_req.method)
        return;
    s_have_req = 0;
    if (cls == C_SUCCESS)
        handle_success(d, n, type, now);
    else
        handle_error(d, n, type, now);
    schedule(now);
}

/* drain the socket: STUN for the state machine, frames for the queue */
static void drain(ULONGLONG now)
{
    char buf[2048];
    int i;
    for (i = 0; i < TURN_FRAMES; i++) {
        int n = (int)recv(s_sock, buf, (int)sizeof(buf), 0);
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK)
                break;
            if (e == WSAEMSGSIZE || e == WSAECONNRESET)
                continue;
            break;
        }
        if (n >= 4) {
            uint16_t t = get_u16(buf);
            if (t >= TURN_CHANNEL_FIRST && t <= TURN_CHANNEL_LAST) {
                handle_channel_data(buf, n);
                continue;
            }
        }
        handle_stun(buf, n, now);
    }
}

/* --- the NIC's calls --- */

int fci_turn_start(const char *server_hostport, const char *user, const char *pass)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char host[100];
    char port[8];
    const char *colon;
    int rc;
    ULONGLONG now = GetTickCount64();
    if (s_sock != INVALID_SOCKET)
        fci_turn_stop();
    memset(s_peers, 0, sizeof(s_peers));
    s_npeers = 0;
    s_next_channel = TURN_CHANNEL_FIRST;
    s_nchans = 0;
    s_drop_noperm = 0;
    s_fq_head = 0;
    s_fq_count = 0;
    s_have_req = 0;
    s_have_key = 0;
    s_stale_done = 0;
    s_realm[0] = 0;
    s_nonce[0] = 0;
    s_relayed = (struct sockaddr_in){0};
    strcpy_s(s_server, sizeof(s_server), server_hostport);
    strcpy_s(s_user, sizeof(s_user), user);
    strcpy_s(s_pass, sizeof(s_pass), pass);
    colon = strrchr(server_hostport, ':');
    if (!colon || colon == server_hostport || colon[1] == 0) {
        log_turn("bad server %s", server_hostport);
        return 0;
    }
    strncpy_s(host, sizeof(host), server_hostport, (size_t)(colon - server_hostport));
    strncpy_s(port, sizeof(port), colon + 1, 7);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        log_turn("resolve %s failed: %d", server_hostport, rc);
        if (res)
            freeaddrinfo(res);
        s_state = FCI_TURN_IDLE;
        return 0;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        log_turn("socket failed: %d", WSAGetLastError());
        freeaddrinfo(res);
        s_state = FCI_TURN_IDLE;
        return 0;
    }
    if (connect(s_sock, (const struct sockaddr *)res->ai_addr, (int)res->ai_addrlen) != 0) {
        log_turn("connect %s failed: %d", server_hostport, WSAGetLastError());
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        freeaddrinfo(res);
        s_state = FCI_TURN_IDLE;
        return 0;
    }
    freeaddrinfo(res);
    {
        unsigned long nb = 1;
        ioctlsocket(s_sock, FIONBIO, &nb);
    }
    s_state = FCI_TURN_ALLOCATING;
    req_start(M_ALLOCATE, 0, -1, 0, now);
    log_turn("allocating from %s (user %d chars)", server_hostport, (int)strlen(s_user));
    return 1;
}

void fci_turn_tick(ULONGLONG now)
{
    if (s_sock == INVALID_SOCKET)
        return;
    chan_expire(now);
    drain(now);
    if (s_have_req && now >= s_req.next) {
        if (s_req.tries >= TURN_MAX_TRIES) {
            state_failed("the server stopped answering");
            return;
        }
        s_req.tries++;
        s_req.next = now + TURN_RTO_MS;
        req_send(&s_req, now, 0);
    }
    schedule(now);
}

int fci_turn_state(void)
{
    return s_state;
}

int fci_turn_relayed(struct sockaddr_in *out)
{
    if (s_state != FCI_TURN_READY)
        return 0;
    *out = s_relayed;
    return 1;
}

int fci_turn_add_peer(const struct sockaddr_in *peer)
{
    int i;
    if (s_state != FCI_TURN_READY)
        return -1;
    for (i = 0; i < s_npeers; i++)
        if (s_peers[i].addr.sin_addr.s_addr == peer->sin_addr.s_addr) {
            if (s_peers[i].addr.sin_port != peer->sin_port)
                peer_rebind(i, (uint16_t)ntohs(peer->sin_port));
            return s_peers[i].channel;
        }
    if (s_npeers >= TURN_PEERS || s_next_channel > TURN_CHANNEL_LAST)
        return -1;
    s_peers[s_npeers].addr = *peer;
    s_peers[s_npeers].channel = s_next_channel++;
    s_peers[s_npeers].want_perm = 1;
    s_peers[s_npeers].want_chan = 1;
    chan_add(s_peers[s_npeers].channel, s_npeers, GetTickCount64());
    s_npeers++;
    log_turn("peer %s:%d on channel 0x%04x", inet_ntoa(peer->sin_addr), ntohs(peer->sin_port),
             (unsigned)s_peers[s_npeers - 1].channel);
    return s_peers[s_npeers - 1].channel;
}

/* a Send indication (RFC 8656 15.13): XOR-PEER-ADDRESS + DATA, no auth, no response. Used
 * while the peer's permission is confirmed but its channel is not (yet) bound. */
static int send_indication(const struct sockaddr_in *peer, const void *frame, int len)
{
    char body[TURN_FRAME_MAX + 24]; /* XOR-PEER (12) + DATA header (4) + len + pad (<=3) */
    char msg[TURN_FRAME_MAX + 64];
    uint8_t txn[12];
    char x[8];
    int blen = 0;
    put_xaddr(x, peer);
    blen += put_attr(body + blen, A_XOR_PEER, x, 8);
    blen += put_attr(body + blen, A_DATA, frame, len);
    new_txn(txn);
    int mlen = finish_msg(msg, M_SEND, C_IND, txn, body, blen, 0, 0, NULL, 0);
    return send_msg(msg, mlen);
}

int fci_turn_send(int channel, const void *frame, int len)
{
    char msg[TURN_FRAME_MAX + 4];
    uint16_t ch = (uint16_t)channel;
    int pi;
    if (s_sock == INVALID_SOCKET || len < 0 || len > TURN_FRAME_MAX)
        return 0;
    pi = peer_by_channel(channel);
    if (pi < 0)
        return 0;
    if (s_peers[pi].have_chan) {
        /* the channel is bound: ChannelData (RFC 8656 15.5) */
        put_u16(msg, ch);
        put_u16(msg + 2, (uint16_t)len);
        memcpy(msg + 4, frame, (size_t)len);
        return send_msg(msg, len + 4);
    }
    if (s_peers[pi].have_perm) {
        /* the channel is not bound yet: a Send indication (the permission covers it) */
        return send_indication(&s_peers[pi].addr, frame, len);
    }
    /* no permission yet: the server would reject a Send indication, so drop, counted */
    s_drop_noperm++;
    log_turn("send dropped (%d): permission for %s:%d is not confirmed", s_drop_noperm,
             inet_ntoa(s_peers[pi].addr.sin_addr), ntohs(s_peers[pi].addr.sin_port));
    return 0;
}

int fci_turn_recv(void *buf, int cap, struct sockaddr_in *from)
{
    if (s_fq_count == 0 || cap <= 0)
        return 0;
    {
        int slot = s_fq_head;
        int len = s_fq_len[slot] < cap ? s_fq_len[slot] : cap;
        memcpy(buf, s_fq[slot], (size_t)len);
        if (from)
            *from = s_fq_from[slot];
        s_fq_head = (s_fq_head + 1) % TURN_FRAMES;
        s_fq_count--;
        return len;
    }
}

void fci_turn_stop(void)
{
    if (s_sock != INVALID_SOCKET) {
        char body[768];
        char msg[1152];
        int blen;
        if (s_state == FCI_TURN_READY && s_have_key) {
            uint8_t txn[12];
            int len;
            blen = build_req_body(M_REFRESH, -1, 0, 1, body);
            new_txn(txn);
            len = finish_msg(msg, M_REFRESH, C_REQ, txn, body, blen, 1, 1, s_key, 16);
            send_msg(msg, len);
        }
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    s_state = FCI_TURN_IDLE;
    s_have_req = 0;
    s_fq_count = 0;
    s_fq_head = 0;
    s_npeers = 0;
    s_nchans = 0;
    log_turn("stopped");
}

/* --- the self-test: the RFC 5769 2.1 (short-term, with FINGERPRINT) and 2.4 (long-term,
 *     without) vectors, the same hex the tests in tests/test_stun_vectors.py pin --- */

int fci_turn_selftest(void)
{
    /* 2.1: the message, the password as the key, the expected full bytes */
    static const uint8_t v21_txn[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
                                        0xfa, 0x87, 0xdf, 0xae};
    static const uint8_t v21_body[] = {
        0x80, 0x22, 0x00, 0x10, 0x53, 0x54, 0x55, 0x4e, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20,
        0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff,
        0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36, 0x00, 0x06,
        0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76, 0x59, 0x20, 0x20, 0x20};
    static const uint8_t v21_key[] = {'V',  'O', 'k', 'J', 'x', 'b', 'R', 'l', '1', 'R',
                                      'm',  'T', 'x', 'U', 'k', '/', 'W', 'v', 'J', 'x',
                                      'B',  't'};
    static const uint8_t v21_full[] = {
        0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34,
        0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x10, 0x53, 0x54, 0x55, 0x4e,
        0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x00, 0x24,
        0x00, 0x04, 0x6e, 0x00, 0x01, 0xff, 0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1,
        0x51, 0x26, 0x3b, 0x36, 0x00, 0x06, 0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68,
        0x36, 0x76, 0x59, 0x20, 0x20, 0x20, 0x00, 0x08, 0x00, 0x14, 0x9a, 0xea, 0xa7, 0x0c,
        0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e, 0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49, 0xc1, 0xb5,
        0x71, 0xa2, 0x80, 0x28, 0x00, 0x04, 0xe5, 0x7a, 0x3b, 0xcf};
    /* 2.4: the long-term key MD5(username:realm:password), the message, the expected bytes */
    static const char v24_material[] = "マトリックス:example.org:TheMatrIX";
    static const uint8_t v24_txn[12] = {0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0,
                                        0x29, 0xda, 0x41, 0x2e};
    static const uint8_t v24_body[] = {
        0x00, 0x06, 0x00, 0x12, 0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3,
        0x83, 0x83, 0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xb9, 0x00, 0x00, 0x00, 0x15, 0x00, 0x1c,
        0x66, 0x2f, 0x2f, 0x34, 0x39, 0x39, 0x6b, 0x39, 0x35, 0x34, 0x64, 0x36, 0x4f, 0x4c,
        0x33, 0x34, 0x6f, 0x4c, 0x39, 0x46, 0x53, 0x54, 0x76, 0x79, 0x36, 0x34, 0x73, 0x41,
        0x00, 0x14, 0x00, 0x0b, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x6f, 0x72,
        0x67, 0x00};
    static const uint8_t v24_full[] = {
        0x00, 0x01, 0x00, 0x60, 0x21, 0x12, 0xa4, 0x42, 0x78, 0xad, 0x34, 0x33, 0xc6, 0xad,
        0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e, 0x00, 0x06, 0x00, 0x12, 0xe3, 0x83, 0x9e, 0xe3,
        0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3, 0x83, 0x83, 0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xb9,
        0x00, 0x00, 0x00, 0x15, 0x00, 0x1c, 0x66, 0x2f, 0x2f, 0x34, 0x39, 0x39, 0x6b, 0x39,
        0x35, 0x34, 0x64, 0x36, 0x4f, 0x4c, 0x33, 0x34, 0x6f, 0x4c, 0x39, 0x46, 0x53, 0x54,
        0x76, 0x79, 0x36, 0x34, 0x73, 0x41, 0x00, 0x14, 0x00, 0x0b, 0x65, 0x78, 0x61, 0x6d,
        0x70, 0x6c, 0x65, 0x2e, 0x6f, 0x72, 0x67, 0x00, 0x00, 0x08, 0x00, 0x14, 0xf6, 0x70,
        0x24, 0x65, 0x6d, 0xd6, 0x4a, 0x3e, 0x02, 0xb8, 0xe0, 0x71, 0x2e, 0x85, 0xc9, 0xa2,
        0x8c, 0xa8, 0x96, 0x66};
    char m21[128];
    char m24[128];
    uint8_t k24[16];
    int l21, l24;
    int ok = 1;
    l21 = finish_msg(m21, 0x0001, C_REQ, v21_txn, (const char *)v21_body, (int)sizeof(v21_body), 1, 1,
                     v21_key, (int)sizeof(v21_key));
    if (l21 != (int)sizeof(v21_full) || memcmp(m21, v21_full, (size_t)l21) != 0)
        ok = 0;
    md5((const uint8_t *)v24_material, (int)strlen(v24_material), k24);
    l24 = finish_msg(m24, 0x0001, C_REQ, v24_txn, (const char *)v24_body, (int)sizeof(v24_body), 1, 0,
                     k24, 16);
    if (l24 != (int)sizeof(v24_full) || memcmp(m24, v24_full, (size_t)l24) != 0)
        ok = 0;

    /* the message type layout (RFC 8489 5): the class bits interleaved with the method bits */
    static const int ttab[17][3] = {
        {1, C_REQ, 0x0001}, {1, C_SUCCESS, 0x0101}, {1, C_ERROR, 0x0111},
        {3, C_REQ, 0x0003}, {3, C_SUCCESS, 0x0103}, {3, C_ERROR, 0x0113},
        {4, C_REQ, 0x0004}, {4, C_SUCCESS, 0x0104}, {4, C_ERROR, 0x0114},
        {6, C_IND, 0x0016}, {7, C_IND, 0x0017},
        {8, C_REQ, 0x0008}, {8, C_SUCCESS, 0x0108}, {8, C_ERROR, 0x0118},
        {9, C_REQ, 0x0009}, {9, C_SUCCESS, 0x0109}, {9, C_ERROR, 0x0119},
    };
    for (int i = 0; i < 17; i++) {
        uint16_t t = stun_type(ttab[i][0], ttab[i][1]);
        if (t != (uint16_t)ttab[i][2] || type_method(t) != ttab[i][0] || type_class(t) != ttab[i][1])
            ok = 0;
    }

    /* the Allocate 401 captured from turn.cloudflare.com:3478 (build/qwen-in/q17/
     * cloudflare-allocate-401.txt): type 0x0113, ERROR-CODE 401, NONCE (24 bytes), REALM
     * "turn.cloudflare.com"; the parse must agree with the capture */
    static const uint8_t cf401[] = {
        0x01, 0x13, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0x19, 0xdc, 0x66, 0xa0, 0x46, 0xd7,
        0xe0, 0xe0, 0x67, 0xf8, 0x74, 0x2c, 0x00, 0x09, 0x00, 0x04, 0x00, 0x00, 0x04, 0x01,
        0x00, 0x15, 0x00, 0x18, 0x33, 0x53, 0x52, 0x36, 0x5a, 0x56, 0x42, 0x34, 0x42, 0x52,
        0x58, 0x47, 0x34, 0x32, 0x4d, 0x31, 0x31, 0x4e, 0x4a, 0x52, 0x58, 0x4f, 0x33, 0x57,
        0x00, 0x14, 0x00, 0x13, 0x74, 0x75, 0x72, 0x6e, 0x2e, 0x63, 0x6c, 0x6f, 0x75, 0x64,
        0x66, 0x6c, 0x61, 0x72, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x00};
    {
        const char *d = (const char *)cf401;
        int n = (int)sizeof(cf401);
        char realm[128] = {0};
        int nonce_len = -1;
        uint16_t code = 0;
        int off;
        if (get_u16(d) != stun_type(M_ALLOCATE, C_ERROR))
            ok = 0;
        if (type_method(get_u16(d)) != M_ALLOCATE || type_class(get_u16(d)) != C_ERROR)
            ok = 0;
        for (off = 20; off + 4 <= n; off += attr_stride(get_u16(d + off + 2))) {
            uint16_t at = get_u16(d + off);
            uint16_t al = get_u16(d + off + 2);
            if (al > (uint16_t)(n - off - 4))
                ok = 0;
            if (at == A_ERROR_CODE && al >= 4)
                code = (uint16_t)(((uint32_t)(d[off + 6] & 7) * 100u + (uint32_t)(uint8_t)d[off + 7]));
            if (at == A_NONCE)
                nonce_len = al;
            if (at == A_REALM && al < (uint16_t)sizeof(realm)) {
                memcpy(realm, d + off + 4, al);
                realm[al] = 0;
            }
        }
        if (code != 401 || nonce_len != 24 || strcmp(realm, "turn.cloudflare.com") != 0)
            ok = 0;
    }
    return ok;
}
