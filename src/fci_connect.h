/* One connect from the host list's /v1/connects, parsed from its JSON object (src/fci_connect.c).
 * The field readers it uses live in src/fci_json.c. The Worker's on_connect_obj (src/fci_net_worker.c)
 * calls fci_connect_parse and keeps the seq bookkeeping (a connect is re-sent until acknowledged,
 * so it is deduplicated against s_conn_seq), the NIC locking and the log lines.
 *
 * Pure: no I/O, no globals, safe to call from any thread, so a test (tests/json_test.c) runs this
 * exact code natively. */
#ifndef FCI_CONNECT_H
#define FCI_CONNECT_H

#include <stdint.h>

/* one parsed connect. endpoint and local are the addresses the Worker forwards to the NIC; nonce
 * and mac identify the guest; relay is 1 when the guest reached us over the TURN relay. */
typedef struct fci_connect {
    char endpoint[128]; /* the guest's public address:port */
    char local[64];     /* the guest's LAN address:port, when both sides are behind the same NAT */
    uint8_t nonce[16];
    uint8_t mac[6];
    int relay;
} fci_connect_t;

/* Parse one connect object (the {...} fci_json_array hands on). Fills *out and returns 1 on a
 * well-formed entry (endpoint, nonce and mac present and valid; relay and local optional), 0 on a
 * malformed one (a required field missing, the mac or nonce not hex, or the nonce not 32 chars). */
int fci_connect_parse(const char *obj, fci_connect_t *out);

#endif
