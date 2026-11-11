/* The browse status: compute the "NO MATCHES FOUND" relabel cause from NIC/worker state.
 * No sockets, no globals, no I/O: the unit tests compile this file natively.
 *
 * fetch_state is three-valued:
 *   0 = never fetched yet (show SEARCHING...)
 *   1 = last fetch ok
 *   2 = last fetch failed (show LIST OFFLINE)
 *
 * Precedence (highest first):
 *   1. not in list mode -> BROWSE_NONE
 *   2. version_refused -> BROWSE_UPDATE_REQUIRED
 *   3. fetch failed -> BROWSE_LIST_OFFLINE
 *   4. never fetched -> BROWSE_SEARCHING
 *   5. relay_refused -> BROWSE_RELAY_OFFLINE
 *   6. n_dropped > 0 -> BROWSE_HOST_UNREACHABLE
 *   7. n_pending > 0 -> BROWSE_SEARCHING
 *   8. n_hosts == 0 -> BROWSE_LIST_EMPTY
 *   9. else -> BROWSE_LIST_EMPTY (all connected: no "no matches" row)
 */
#include "fci_browse_status.h"

int fci_browse_status(int in_list, int fetch_state, int n_hosts,
                      int n_pending, int n_dropped, int relay_refused,
                      int version_refused)
{
    if (!in_list)
        return BROWSE_NONE;
    if (version_refused)
        return BROWSE_UPDATE_REQUIRED;
    if (fetch_state == 2)
        return BROWSE_LIST_OFFLINE;
    if (fetch_state == 0)
        return BROWSE_SEARCHING;
    /* fetch_state == 1: last fetch ok */
    if (n_hosts == 0)
        return BROWSE_LIST_EMPTY; /* no hosts: nothing to show, relay/dropped don't matter */
    if (relay_refused)
        return BROWSE_RELAY_OFFLINE;
    if (n_dropped > 0)
        return BROWSE_HOST_UNREACHABLE;
    if (n_pending > 0)
        return BROWSE_SEARCHING;
    return BROWSE_LIST_EMPTY; /* all connected: no "no matches" row */
}
