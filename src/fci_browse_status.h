/* The browse status: compute the "NO MATCHES FOUND" relabel cause from NIC/worker state.
 * No sockets, no globals, no I/O: the unit tests compile this file natively. */
#ifndef FCI_BROWSE_STATUS_H
#define FCI_BROWSE_STATUS_H

/* The browse status enum: what the "NO MATCHES FOUND" row should say.
 * Every text is at most 16 characters (the entry holds 16 UTF-16 units + terminator). */
enum {
    BROWSE_NONE = 0,             /* not in list mode: no relabel */
    BROWSE_LIST_OFFLINE = 1,     /* "LIST OFFLINE" - last fetch failed */
    BROWSE_LIST_EMPTY = 2,       /* "NO MATCHES FOUND" - fetched, zero hosts */
    BROWSE_SEARCHING = 3,        /* "SEARCHING..." - never fetched, or hosts pending */
    BROWSE_HOST_UNREACHABLE = 4, /* "HOST UNREACHABLE" - hosts listed, dropped */
    BROWSE_RELAY_OFFLINE = 5,    /* "RELAY OFFLINE" - relay mint refused */
    BROWSE_UPDATE_REQUIRED = 6,  /* "UPDATE REQUIRED" - version mismatch */
};

/* fetch_state: the three-valued fetch state.
 * 0 = never fetched yet (show SEARCHING...)
 * 1 = last fetch ok
 * 2 = last fetch failed (show LIST OFFLINE)
 *
 * n_dropped: number of peers dropped (no relay / relay failed).
 *   Clears when: a peer goes live, or a fetch lists zero hosts.
 * relay_refused: 1 when the last relay mint was refused (503).
 *   Clears after a successful mint or an empty host list.
 * version_refused: 1 when the list refused us for the version.
 */
int fci_browse_status(int in_list, int fetch_state, int n_hosts,
                      int n_pending, int n_dropped, int relay_refused,
                      int version_refused);

#endif /* FCI_BROWSE_STATUS_H */
