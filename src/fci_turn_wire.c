/* The TURN wire protocol (RFC 8489 over the RFC 8656 methods): message building (header,
 * attributes, MESSAGE-INTEGRITY, FINGERPRINT) and response parsing. No sockets, no I/O,
 * no globals beyond the CRC-32 table: src/fci_turn.c calls this, and
 * tests/turn_wire_test.c compiles this exact file natively against the RFC 5769 vectors. */
#include "fci_turn_wire.h"

#include <string.h>

/* --- crypto: MD5 (RFC 1321), SHA-1 (FIPS 180), HMAC-SHA1 (RFC 2104), CRC-32 (IEEE, the zlib
 *     convention) --- */

static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

void fci_tw_md5(const uint8_t *in, int inlen, uint8_t out[16])
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
    static const int S[64]
            = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
               14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
               4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};
    /* any length: the input's whole blocks, then a padded tail of one or two blocks. Cloudflare's
     * user:realm:pass is about 150 bytes (an earlier 72-byte limit left the key unset) */
    uint8_t buf[128];
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t bits;
    int i, n, total, whole, rem;
    if (inlen < 0)
        inlen = 0;
    whole = inlen & ~63;
    rem = inlen - whole;
    memcpy(buf, in + whole, (size_t)rem);
    buf[rem] = 0x80;
    total = rem < 56 ? 64 : 128;
    memset(buf + rem + 1, 0, (size_t)(total - rem - 1));
    bits = (uint64_t)inlen * 8u;
    memcpy(buf + total - 8, &bits, 8);
    total += whole;
    for (n = 0; n < total; n += 64) {
        const uint8_t *blk = n < whole ? in + n : buf + (n - whole);
        uint32_t M[16];
        uint32_t a, b, c, d;
        for (i = 0; i < 16; i++)
            memcpy(&M[i], blk + i * 4, 4);
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

typedef struct {
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
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8)
               | (uint32_t)p[i * 4 + 3];
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
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void fci_tw_sha1(const uint8_t *data, int len, uint8_t out[20])
{
    sha1_ctx_t c;
    sha1_begin(&c);
    sha1_update(&c, data, len);
    sha1_finish(&c, out);
}

void fci_tw_hmac_sha1(const uint8_t *key, int keylen, const uint8_t *data, int datalen, uint8_t out[20])
{
    uint8_t k[64];
    uint8_t inner[20];
    sha1_ctx_t c;
    int i;
    if (keylen > 64) {
        fci_tw_sha1(key, keylen, k);
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

uint32_t fci_tw_crc32(const uint8_t *data, int len)
{
    uint32_t c = 0xFFFFFFFFu;
    if (!s_crc_ready)
        crc32_init();
    for (int i = 0; i < len; i++)
        c = s_crctab[(c ^ (uint32_t)data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* --- STUN message building (RFC 8489 6, 14, RFC 8656 15) --- */

void fci_tw_put_u16(char *p, uint16_t v)
{
    p[0] = (char)(v >> 8);
    p[1] = (char)v;
}

void fci_tw_put_u32(char *p, uint32_t v)
{
    p[0] = (char)(v >> 24);
    p[1] = (char)(v >> 16);
    p[2] = (char)(v >> 8);
    p[3] = (char)v;
}

uint16_t fci_tw_get_u16(const char *p)
{ return (uint16_t)(((uint16_t)(uint8_t)p[0] << 8) | (uint16_t)(uint8_t)p[1]); }

/* append a STUN attribute, the value zero-padded to a 4-byte boundary; the bytes written */
int fci_tw_put_attr(char *p, uint16_t type, const void *v, int len)
{
    int pad = (4 - (len & 3)) & 3;
    fci_tw_put_u16(p, type);
    fci_tw_put_u16(p + 2, (uint16_t)len);
    memcpy(p + 4, v, (size_t)len);
    memset(p + 4 + len, 0, (size_t)pad);
    return 4 + len + pad;
}

/* the attribute's stride in a message: the header plus the padded value */
int fci_tw_attr_stride(uint16_t len) { return 4 + ((len + 3) & ~3); }

/* XOR-ADDRESS, IPv4: the port XOR 0x2112 and the address XOR 0x2112A442 (RFC 8656 15.12) */
void fci_tw_put_xaddr(char *p, const struct sockaddr_in *a)
{
    uint16_t port = (uint16_t)(ntohs(a->sin_port) ^ 0x2112u);
    uint32_t addr = ntohl(a->sin_addr.s_addr) ^ FCI_TW_MAGIC;
    p[0] = 0;
    p[1] = 1;
    fci_tw_put_u16(p + 2, port);
    fci_tw_put_u32(p + 4, addr);
}

int fci_tw_get_xaddr(const char *p, struct sockaddr_in *a)
{
    uint32_t addr;
    if (p[1] != 1)
        return 0;
    /* the wire is big-endian: read it as a number before the XOR (as put_xaddr writes it) */
    addr = ((uint32_t)(uint8_t)p[4] << 24) | ((uint32_t)(uint8_t)p[5] << 16) | ((uint32_t)(uint8_t)p[6] << 8)
           | (uint32_t)(uint8_t)p[7];
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)(fci_tw_get_u16(p + 2) ^ 0x2112u));
    a->sin_addr.s_addr = htonl(addr ^ FCI_TW_MAGIC);
    return 1;
}

/* the STUN message type (RFC 8489 5): the two class bits are interleaved with the method
 * bits, so Allocate is 0x0003/0x0103/0x0113, Refresh 0x0004/0x0104/0x0114, the Send and
 * Data indications 0x0016 and 0x0017, CreatePermission 0x0008/0x0108/0x0118 and ChannelBind
 * 0x0009/0x0109/0x0119 */
uint16_t fci_tw_type(int method, int cls)
{
    return (uint16_t)(((method & 0x0F80) << 2) | ((method & 0x0070) << 1) | (method & 0x000F)
                      | ((cls & 2) << 7) | ((cls & 1) << 4));
}

/* the method and the class of a message type (RFC 8489 5) */
int fci_tw_type_method(uint16_t t)
{ return (((t & 0x3E00) >> 9) << 7) | (((t & 0x0060) >> 5) << 4) | (t & 0x000F); }

int fci_tw_type_class(uint16_t t) { return ((t & 0x0100) >> 7) | ((t & 0x0010) >> 4); }

/* the header and the attributes of a message; with integrity, MESSAGE-INTEGRITY (HMAC-SHA1
 * with key) is appended, and with fingerprint, FINGERPRINT (CRC-32 XOR 0x5354554E) is
 * appended after it. The length field holds the message length up to the attribute being
 * computed (the final length for the FINGERPRINT, which is last); the RFC 5769 2.1 vector
 * fixes the convention. Returns the message length in bytes. */
int fci_tw_build(char *out, int method, int cls, const uint8_t txn[12], const char *body, int bodylen,
                 int integrity, int fingerprint, const uint8_t key[16], int keylen)
{
    int off = 20 + bodylen;
    int len = bodylen;
    fci_tw_put_u16(out, fci_tw_type(method, cls));
    out[2] = 0;
    out[3] = 0;
    fci_tw_put_u32(out + 4, FCI_TW_MAGIC);
    memcpy(out + 8, txn, 12);
    memcpy(out + 20, body, (size_t)bodylen);
    if (integrity) {
        uint8_t mi[20];
        len += 24;
        fci_tw_put_u16(out + 2, (uint16_t)len);
        fci_tw_hmac_sha1(key, keylen, (const uint8_t *)out, off, mi);
        off += fci_tw_put_attr(out + off, FCI_TW_A_MESSAGE_INTEGRITY, mi, 20);
    }
    if (fingerprint) {
        uint32_t fp;
        char fpb[4];
        len += 8;
        fci_tw_put_u16(out + 2, (uint16_t)len);
        fp = fci_tw_crc32((const uint8_t *)out, off) ^ 0x5354554Eu;
        fci_tw_put_u32(fpb, fp);
        off += fci_tw_put_attr(out + off, FCI_TW_A_FINGERPRINT, fpb, 4);
    }
    if (!integrity && !fingerprint)
        fci_tw_put_u16(out + 2, (uint16_t)len);
    return off;
}

int fci_tw_parse(const char *d, int n, fci_tw_msg_t *out, const uint8_t key[16], int keylen)
{
    uint32_t magic;
    int off;
    /* RFC 5389 15.6: the HMAC input is the message up to the MI attribute,
     * with the length field set as if the message ended there (RFC 5769 vectors) */
    uint8_t mitmp[576];
    if (n < 20)
        return FCI_TW_SHORT;
    memcpy(&magic, d + 4, 4);
    if (ntohl(magic) != FCI_TW_MAGIC)
        return FCI_TW_COOKIE;
    out->type = fci_tw_get_u16(d);
    out->method = fci_tw_type_method(out->type);
    out->cls = fci_tw_type_class(out->type);
    memcpy(out->txn, d + 8, 12);
    out->error_code = 0;
    out->have_relayed = 0;
    out->have_mapped = 0;
    out->have_realm = 0;
    out->have_nonce = 0;
    out->realm[0] = 0;
    out->nonce[0] = 0;
    out->have_lifetime = 0;
    out->lifetime = 0;
    out->integrity_present = 0;
    out->integrity_valid = 0;
    for (off = 20; off + 4 <= n; off += fci_tw_attr_stride(fci_tw_get_u16(d + off + 2))) {
        uint16_t at = fci_tw_get_u16(d + off);
        uint16_t al = fci_tw_get_u16(d + off + 2);
        if (al > (uint16_t)(n - off - 4))
            return FCI_TW_TRUNC;
        if (at == FCI_TW_A_ERROR_CODE && al >= 4) {
            /* two zero bytes, then the class (hundreds) and the number (RFC 8489 14.8) */
            out->error_code = (uint16_t)(((uint32_t)(d[off + 6] & 7) * 100u + (uint32_t)(uint8_t)d[off + 7]));
        }
        if (at == FCI_TW_A_XOR_RELAYED && al == 8) {
            fci_tw_get_xaddr(d + off + 4, &out->relayed);
            out->have_relayed = 1;
        }
        if (at == FCI_TW_A_XOR_MAPPED && al == 8) {
            fci_tw_get_xaddr(d + off + 4, &out->mapped);
            out->have_mapped = 1;
        }
        if (at == FCI_TW_A_REALM) {
            out->have_realm = 1;
            if (al < (uint16_t)sizeof(out->realm)) {
                memcpy(out->realm, d + off + 4, al);
                out->realm[al] = 0;
            }
        }
        if (at == FCI_TW_A_NONCE) {
            out->have_nonce = 1;
            if (al < (uint16_t)sizeof(out->nonce)) {
                memcpy(out->nonce, d + off + 4, al);
                out->nonce[al] = 0;
            }
        }
        if (at == FCI_TW_A_LIFETIME && al == 4) {
            uint32_t lt;
            memcpy(&lt, d + off + 4, 4);
            out->lifetime = ntohl(lt);
            out->have_lifetime = 1;
        }
        if (at == FCI_TW_A_MESSAGE_INTEGRITY && al == 20) {
            out->integrity_present = 1;
            if (key && off < (int)sizeof(mitmp)) {
                uint8_t mi[20];
                memcpy(mitmp, d, (size_t)off);
                fci_tw_put_u16((char *)mitmp + 2, (uint16_t)(off + 4));
                fci_tw_hmac_sha1(key, keylen, mitmp, off, mi);
                out->integrity_valid = (memcmp(mi, d + off + 4, 20) == 0);
            }
        }
    }
    return FCI_TW_OK;
}
