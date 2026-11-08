/* The kernel's modular exponentiation and DES services (XcModExp, XcDESKeyParity, XcKeyTable, XcBlockCrypt,
   XcBlockCryptCBC), which the toolkit left as stubs; the network stack's key exchange needs them
   (runtime patch 0027, research/docs/multiplayer.md). Recomps only talk to recomps, so the key table's layout is
   our own; the arithmetic is standard DES (FIPS 46-3) and Montgomery exponentiation. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void fci_kernel_unsupported(const char *what); /* fci_manual.c: exit 87 */

/* ---- XcModExp: a = b^c mod d, each n 32-bit words, least significant first ---- */

#define MODEXP_MAX 128 /* 4096 bits */

/* t = a * b * R^-1 mod d (CIOS), with a, b < d and d odd */
static void mont_mul(uint32_t *out, const uint32_t *a, const uint32_t *b, const uint32_t *d, uint32_t d0inv, int n)
{
    uint32_t t[MODEXP_MAX + 2];
    int i, j;
    memset(t, 0, sizeof(uint32_t) * (size_t)(n + 2));
    for (i = 0; i < n; i++) {
        uint64_t c = 0, s;
        uint32_t m;
        for (j = 0; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s, c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n] = (uint32_t)s, t[n + 1] = (uint32_t)(s >> 32);
        m = t[0] * d0inv;
        s = (uint64_t)t[0] + (uint64_t)m * d[0];
        c = s >> 32;
        for (j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)m * d[j] + c;
            t[j - 1] = (uint32_t)s, c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n - 1] = (uint32_t)s;
        t[n] = t[n + 1] + (uint32_t)(s >> 32);
    }
    /* the result is below 2d: subtract d once if it is not below d */
    {
        int ge = t[n] != 0;
        if (!ge) {
            ge = 1;
            for (j = n - 1; j >= 0; j--)
                if (t[j] != d[j]) {
                    ge = t[j] > d[j];
                    break;
                }
        }
        if (ge) {
            int64_t br = 0;
            for (j = 0; j < n; j++) {
                int64_t s = (int64_t)t[j] - d[j] + br;
                t[j] = (uint32_t)s, br = s >> 32;
            }
        }
    }
    memcpy(out, t, sizeof(uint32_t) * (size_t)n);
}

unsigned long fci_xc_modexp(unsigned long *pa, unsigned long *pb, unsigned long *pc, unsigned long *pd, unsigned long n)
{
    uint32_t *a = (uint32_t *)pa; /* unsigned long is 32 bits on Windows: the kernel's ULONG */
    const uint32_t *b = (const uint32_t *)pb, *c = (const uint32_t *)pc, *d = (const uint32_t *)pd;
    uint32_t one[MODEXP_MAX] = {1}, r2[MODEXP_MAX] = {1}, x[MODEXP_MAX], acc[MODEXP_MAX], inv = 1;
    int i, bit, top, nn = (int)n;
    static int logged;
    typedef char ulong_is_32_bits[sizeof(unsigned long) == 4 ? 1 : -1];
    if (n == 0 || n > MODEXP_MAX || !(d[0] & 1) || !d[n - 1])
        fci_kernel_unsupported("XcModExp: a modulus that is not odd and of the stated length");
    if (!logged++)
        fprintf(stderr, "[FCI] crypto: XcModExp, %lu-bit modulus\n", n * 32), fflush(stderr);
    for (i = 0; i < 5; i++) /* Newton: d[0]^-1 mod 2^32 */
        inv *= 2 - d[0] * inv;
    inv = 0u - inv;
    for (i = 0; i < 2 * 32 * nn; i++) { /* R^2 mod d, by doubling 1 */
        uint32_t carry = 0;
        int j, ge;
        for (j = 0; j < nn; j++) {
            uint32_t v = r2[j];
            r2[j] = (v << 1) | carry, carry = v >> 31;
        }
        ge = carry;
        if (!ge) {
            ge = 1;
            for (j = nn - 1; j >= 0; j--)
                if (r2[j] != d[j]) {
                    ge = r2[j] > d[j];
                    break;
                }
        }
        if (ge) {
            int64_t br = 0;
            for (j = 0; j < nn; j++) {
                int64_t s = (int64_t)r2[j] - d[j] + br;
                r2[j] = (uint32_t)s, br = s >> 32;
            }
        }
    }
    mont_mul(x, b, one, d, inv, nn); /* b R^-1, below d even when b is not */
    mont_mul(x, x, r2, d, inv, nn);  /* b mod d */
    mont_mul(x, x, r2, d, inv, nn);  /* b R */
    mont_mul(acc, one, r2, d, inv, nn); /* R */
    for (top = nn * 32 - 1; top >= 0 && !(c[top >> 5] >> (top & 31) & 1); top--)
        ;
    for (bit = top; bit >= 0; bit--) {
        mont_mul(acc, acc, acc, d, inv, nn);
        if (c[bit >> 5] >> (bit & 31) & 1)
            mont_mul(acc, acc, x, d, inv, nn);
    }
    mont_mul(a, acc, one, d, inv, nn);
    return 1;
}

/* ---- DES and triple DES ---- */

static const uint8_t PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                                35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                                46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const uint8_t PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const uint8_t IP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                               62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                               57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                               61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t FP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                               38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                               36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                               34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
static const uint8_t E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                              12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                              22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const uint8_t P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                              2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const uint8_t S[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0, 15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5,  3,  8,  4,  1,  14, 8,  13, 6, 2,  11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4,  7,  15, 2,
     8,  14, 12, 0,  1,  10, 6,  9,  11, 5, 0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,
     9,  3,  2,  15, 13, 8,  10, 1,  3,  15, 4, 2,  11, 6,  7, 12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3, 4,
     6,  10, 2,  8,  5,  14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2, 12,
     5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3, 0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4, 15, 13, 8,  11, 5,  6,  15,
     0,  3,  4,  7, 2,  12, 1,  10, 14, 9, 10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
     5,  2,  8,  4, 3,  15, 0,  6,  10, 1, 13, 8,  9,  4,  5, 11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6, 8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5,  0,  15, 10, 3,  9, 8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0,  14, 11, 8,  12, 7, 1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6,  8,  0, 13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
     9,  5,  6,  1,  13, 14, 0,  11, 3, 8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
     1,  13, 11, 6,  4,  3,  2,  12, 9, 5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0,  8, 13, 3,  12, 9,  7,  5,  10, 6, 1,  13, 0,  11, 7,  4,  9,
     1,  10, 14, 3,  5,  12, 2, 15, 8,  6,  1,  4,  11, 13, 12, 3, 7,  14, 10, 15, 6,  8,
     0,  5,  9,  2,  6,  11, 13, 8, 1,  4,  10, 7,  9,  5,  0,  15, 14, 2, 3,  12},
    {13, 2,  8,  4, 6,  15, 11, 1,  10, 9,  3, 14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,
     7,  4,  12, 5, 6,  11, 0,  14, 9,  2,  7, 11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13,
     15, 3,  5,  8, 2,  1,  14, 7,  4,  10, 8, 13, 15, 12, 9,  0,  3,  5,  6,  11}};

/* bit i (1-based, from the most significant of an n-bit value) of v */
static uint64_t permute(uint64_t v, const uint8_t *table, int out_bits, int in_bits)
{
    uint64_t r = 0;
    int i;
    for (i = 0; i < out_bits; i++)
        r = (r << 1) | ((v >> (in_bits - table[i])) & 1);
    return r;
}

static void des_schedule(uint64_t sub[16], const uint8_t key[8])
{
    uint64_t k = 0, cd;
    uint32_t c, d;
    int i;
    for (i = 0; i < 8; i++)
        k = (k << 8) | key[i];
    cd = permute(k, PC1, 56, 64);
    c = (uint32_t)(cd >> 28) & 0x0FFFFFFF, d = (uint32_t)cd & 0x0FFFFFFF;
    for (i = 0; i < 16; i++) {
        c = ((c << SHIFTS[i]) | (c >> (28 - SHIFTS[i]))) & 0x0FFFFFFF;
        d = ((d << SHIFTS[i]) | (d >> (28 - SHIFTS[i]))) & 0x0FFFFFFF;
        sub[i] = permute(((uint64_t)c << 28) | d, PC2, 48, 56);
    }
}

static uint32_t feistel(uint32_t r, uint64_t k)
{
    uint64_t x = permute(r, E, 48, 32) ^ k;
    uint32_t out = 0;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned six = (unsigned)(x >> (42 - 6 * i)) & 0x3F;
        out = (out << 4) | S[i][(((six & 0x20) >> 4) | (six & 1)) * 16 + ((six >> 1) & 0xF)];
    }
    return (uint32_t)permute(out, P, 32, 32);
}

static uint64_t des_block(uint64_t v, const uint64_t sub[16], int decrypt)
{
    uint32_t l, r;
    int i;
    v = permute(v, IP, 64, 64);
    l = (uint32_t)(v >> 32), r = (uint32_t)v;
    for (i = 0; i < 16; i++) {
        uint32_t t = r;
        r = l ^ feistel(r, sub[decrypt ? 15 - i : i]);
        l = t;
    }
    return permute(((uint64_t)r << 32) | l, FP, 64, 64);
}

/* the cipher selectors and operations as the kernel takes them */
#define XC_DES 0
#define XC_DES3 1
#define XC_ENCRYPT 1

typedef struct {
    uint64_t sub[3][16];
} key_table; /* 384 bytes, triple DES's size; DES uses the first 128 */

static void check_cipher(unsigned long cipher)
{
    if (cipher != XC_DES && cipher != XC_DES3)
        fci_kernel_unsupported("XcKeyTable/XcBlockCrypt: a cipher other than DES or triple DES");
}

static uint64_t crypt_block(unsigned long cipher, uint64_t v, const key_table *t, int encrypt)
{
    if (cipher == XC_DES)
        return des_block(v, t->sub[0], !encrypt);
    if (encrypt) /* EDE with three keys */
        return des_block(des_block(des_block(v, t->sub[0], 0), t->sub[1], 1), t->sub[2], 0);
    return des_block(des_block(des_block(v, t->sub[2], 1), t->sub[1], 0), t->sub[0], 1);
}

static uint64_t load_be(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void store_be(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)v;
}

static void log_once(const char *what, unsigned long cipher, unsigned long op)
{
    static int logged;
    if (logged++ < 4)
        fprintf(stderr, "[FCI] crypto: %s, cipher %lu, operation %lu\n", what, cipher, op), fflush(stderr);
}

void fci_xc_des_key_parity(uint8_t *key, unsigned long len)
{
    unsigned long i;
    for (i = 0; i < len; i++) {
        uint8_t b = key[i] & 0xFE, ones = 0, v;
        for (v = b; v; v >>= 1)
            ones += v & 1;
        key[i] = b | (uint8_t)!(ones & 1); /* odd parity */
    }
}

void fci_xc_key_table(unsigned long cipher, void *table, const uint8_t *key)
{
    key_table *t = (key_table *)table;
    int i;
    check_cipher(cipher);
    log_once("XcKeyTable", cipher, 0);
    for (i = 0; i < (cipher == XC_DES3 ? 3 : 1); i++)
        des_schedule(t->sub[i], key + 8 * i);
}

void fci_xc_block_crypt(unsigned long cipher, void *out, const void *in, const void *table, unsigned long op)
{
    check_cipher(cipher);
    log_once("XcBlockCrypt", cipher, op);
    store_be((uint8_t *)out, crypt_block(cipher, load_be((const uint8_t *)in), (const key_table *)table, op == XC_ENCRYPT));
}

void fci_xc_block_crypt_cbc(unsigned long cipher, unsigned long len, void *out, const void *in, const void *table,
                            unsigned long op, void *feedback)
{
    uint8_t *o = (uint8_t *)out;
    const uint8_t *s = (const uint8_t *)in;
    uint64_t fb = load_be((const uint8_t *)feedback);
    unsigned long i;
    check_cipher(cipher);
    log_once("XcBlockCryptCBC", cipher, op);
    if (len % 8)
        fci_kernel_unsupported("XcBlockCryptCBC: a length that is not whole blocks");
    for (i = 0; i < len; i += 8) {
        uint64_t v = load_be(s + i);
        if (op == XC_ENCRYPT) {
            fb = crypt_block(cipher, v ^ fb, (const key_table *)table, 1);
            store_be(o + i, fb);
        } else {
            store_be(o + i, crypt_block(cipher, v, (const key_table *)table, 0) ^ fb);
            fb = v; /* read before the write: in and out may be the same buffer */
        }
    }
    store_be((uint8_t *)feedback, fb);
}
