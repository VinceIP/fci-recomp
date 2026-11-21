/* The TURN wire protocol (RFC 8489 over the RFC 8656 methods): the pure byte-level logic,
 * in its own file so the unit tests (tests/test_turn_wire_c.py) compile and run this exact
 * code natively. src/fci_turn.c calls it at message build and parse; the socket, the state
 * machine and the log lines stay there.
 *
 * No sockets, no I/O, no globals beyond the CRC-32 table. The MD5, SHA-1, HMAC-SHA1 and
 * CRC-32 here are small embedded implementations (the client must also build standalone in
 * tools/turn_probe.c); they are pinned by tests/turn_wire_test.c to the RFC 5769 2.1 and
 * 2.4 vectors and the RFC 2202 HMAC vectors instead of a provider. The same code the client
 * uses to sign its requests is the code the tests exercise. */
#ifndef FCI_TURN_WIRE_H
#define FCI_TURN_WIRE_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#define FCI_TW_MAGIC 0x2112A442u

/* the STUN message classes (RFC 8489 5) */
#define FCI_TW_C_REQ     0
#define FCI_TW_C_IND     1
#define FCI_TW_C_SUCCESS 2
#define FCI_TW_C_ERROR   3

/* the STUN attribute types (RFC 8489 15, RFC 8656) */
#define FCI_TW_A_USERNAME          0x0006
#define FCI_TW_A_MESSAGE_INTEGRITY 0x0008
#define FCI_TW_A_ERROR_CODE        0x0009
#define FCI_TW_A_CHANNEL           0x000c
#define FCI_TW_A_LIFETIME          0x000d
#define FCI_TW_A_XOR_PEER          0x0012
#define FCI_TW_A_DATA              0x0013
#define FCI_TW_A_XOR_MAPPED        0x0020
#define FCI_TW_A_REALM             0x0014
#define FCI_TW_A_NONCE             0x0015
#define FCI_TW_A_XOR_RELAYED       0x0016
#define FCI_TW_A_TRANSPORT         0x0019
#define FCI_TW_A_FINGERPRINT       0x8028

/* the result of parsing a message */
enum {
    FCI_TW_OK = 0, /* the header and every attribute parsed */
    FCI_TW_SHORT,  /* fewer than 20 bytes: no full STUN header */
    FCI_TW_COOKIE, /* the magic cookie does not match */
    FCI_TW_TRUNC,  /* an attribute's length extends past the end of the buffer */
};

/* a parsed message: the header fields, then the attributes the state machine reads. The
 * realm and nonce are copied only when they fit (as the client's buffers are sized). */
typedef struct {
    uint16_t type;
    int method;
    int cls;
    uint8_t txn[12];
    uint16_t error_code; /* 0 when there is no ERROR-CODE attribute */
    int have_relayed;
    struct sockaddr_in relayed;
    int have_mapped;
    struct sockaddr_in mapped;
    int have_realm; /* the attribute was present (realm is empty when it did not fit) */
    int have_nonce;
    char realm[128];
    char nonce[256];
    int have_lifetime;
    uint32_t lifetime;
    int integrity_present;
    int integrity_valid; /* 1 only when a key was given and the HMAC-SHA1 matched */
} fci_tw_msg_t;

/* the STUN message type (RFC 8489 5): the two class bits interleaved with the method bits,
 * so Allocate is 0x0003/0x0103/0x0113, Refresh 0x0004/0x0104/0x0114, the Send and Data
 * indications 0x0016 and 0x0017, CreatePermission 0x0008/0x0108/0x0118 and ChannelBind
 * 0x0009/0x0109/0x0119 */
uint16_t fci_tw_type(int method, int cls);
/* the method and the class of a message type (RFC 8489 5) */
int fci_tw_type_method(uint16_t t);
int fci_tw_type_class(uint16_t t);

/* big-endian 16/32-bit values on the wire */
void fci_tw_put_u16(char *p, uint16_t v);
void fci_tw_put_u32(char *p, uint32_t v);
uint16_t fci_tw_get_u16(const char *p);

/* the attribute's stride in a message: the header plus the padded value */
int fci_tw_attr_stride(uint16_t len);

/* append a STUN attribute, the value zero-padded to a 4-byte boundary; the bytes written */
int fci_tw_put_attr(char *p, uint16_t type, const void *v, int len);

/* XOR-ADDRESS, IPv4: the port XOR 0x2112 and the address XOR 0x2112A442 (RFC 8656 15.12) */
void fci_tw_put_xaddr(char *p, const struct sockaddr_in *a);
int fci_tw_get_xaddr(const char *p, struct sockaddr_in *a);

/* the header and the attributes of a message; with integrity, MESSAGE-INTEGRITY (HMAC-SHA1
 * with key) is appended, and with fingerprint, FINGERPRINT (CRC-32 XOR 0x5354554E) is
 * appended after it. The length field holds the message length up to the attribute being
 * computed (the final length for the FINGERPRINT, which is last); the RFC 5769 2.1 vector
 * fixes the convention. Returns the message length in bytes. */
int fci_tw_build(char *out, int method, int cls, const uint8_t txn[12], const char *body, int bodylen,
                 int integrity, int fingerprint, const uint8_t key[16], int keylen);

/* Parse a message. On FCI_TW_OK, *out holds the header fields and the attributes above
 * (error code, XOR-RELAYED-ADDRESS, XOR-MAPPED-ADDRESS, REALM, NONCE, LIFETIME, and the
 * MESSAGE-INTEGRITY's presence; its validity when key is non-NULL, checked over the message
 * up to the integrity attribute with the length field set as if the message ended there;
 * the RFC 5769 2.1-2.4 vectors fix the convention). */
int fci_tw_parse(const char *d, int n, fci_tw_msg_t *out, const uint8_t key[16], int keylen);

/* --- the crypto the wire uses: MD5 (RFC 1321), SHA-1 (FIPS 180), HMAC-SHA1 (RFC 2104),
 *     CRC-32 (IEEE, the zlib convention) --- */
void fci_tw_md5(const uint8_t *in, int inlen, uint8_t out[16]);
void fci_tw_sha1(const uint8_t *data, int len, uint8_t out[20]);
void fci_tw_hmac_sha1(const uint8_t *key, int keylen, const uint8_t *data, int datalen, uint8_t out[20]);
uint32_t fci_tw_crc32(const uint8_t *data, int len);

#endif
