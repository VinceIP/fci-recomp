/* Parse one connect object from the host list's /v1/connects (see fci_connect.h). The field
 * readers are in src/fci_json.c; the seq bookkeeping, the NIC locking and the log lines stay in the
 * Worker's on_connect_obj (src/fci_net_worker.c). */
#include "fci_connect.h"

#include <stdio.h>
#include <string.h>

#include "fci_json.h"

int fci_connect_parse(const char *obj, fci_connect_t *out)
{
    char nonce_hex[64], macstr[32];
    int n;

    memset(out, 0, sizeof *out);
    if (!fci_json_str_val(obj, "endpoint", out->endpoint, (int)sizeof out->endpoint))
        return 0;
    if (!fci_json_str_val(obj, "nonce", nonce_hex, (int)sizeof nonce_hex))
        return 0;
    if (!fci_json_str_val(obj, "mac", macstr, (int)sizeof macstr))
        return 0;

    if (sscanf(macstr, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &out->mac[0], &out->mac[1], &out->mac[2],
               &out->mac[3], &out->mac[4], &out->mac[5])
        != 6)
        return 0;
    if (strlen(nonce_hex) != 32)
        return 0;
    for (n = 0; n < 16; n++)
        if (sscanf(nonce_hex + n * 2, "%2hhx", &out->nonce[n]) != 1)
            return 0;

    out->relay = fci_json_bool(obj, "relay");
    if (out->relay < 0)
        out->relay = 0;

    /* the guest's LAN address (when the Worker forwarded one: both sides are behind the same NAT) */
    fci_json_str_val(obj, "local", out->local, (int)sizeof out->local);
    return 1;
}
