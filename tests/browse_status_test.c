/* Test driver for fci_browse_status: the "NO MATCHES FOUND" relabel cause logic.
 * Compiles natively with the shipping source (src/fci_browse_status.c).
 *
 * fetch_state: 0 = never fetched, 1 = last fetch ok, 2 = last fetch failed
 */
#include <stdio.h>
#include <string.h>

#include "fci_browse_status.h"

static int s_failed;
static int s_n;

static void check(const char *name, int got, int want)
{
    s_n++;
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", name, got, want);
        s_failed++;
    } else {
        printf("ok   %s: %d\n", name, got);
    }
}

int main(void)
{
    /* not in list mode: always BROWSE_NONE */
    check("none-not-in-list", fci_browse_status(0, 1, 5, 0, 0, 0, 0), BROWSE_NONE);
    check("none-in-list-off", fci_browse_status(0, 0, 0, 0, 0, 0, 0), BROWSE_NONE);
    check("none-never-fetched", fci_browse_status(0, 0, 0, 0, 0, 0, 0), BROWSE_NONE);

    /* version refused: highest priority (after in_list) */
    check("version", fci_browse_status(1, 1, 3, 0, 0, 0, 1), BROWSE_UPDATE_REQUIRED);
    check("version-over-relay", fci_browse_status(1, 1, 3, 0, 0, 1, 1), BROWSE_UPDATE_REQUIRED);
    check("version-over-fetch-fail", fci_browse_status(1, 2, 0, 0, 0, 0, 1), BROWSE_UPDATE_REQUIRED);
    check("version-never-fetched", fci_browse_status(1, 0, 0, 0, 0, 0, 1), BROWSE_UPDATE_REQUIRED);

    /* fetch failed: LIST OFFLINE */
    check("offline", fci_browse_status(1, 2, 0, 0, 0, 0, 0), BROWSE_LIST_OFFLINE);
    check("offline-pending", fci_browse_status(1, 2, 3, 2, 0, 0, 0), BROWSE_LIST_OFFLINE);
    check("offline-dropped", fci_browse_status(1, 2, 3, 0, 2, 0, 0), BROWSE_LIST_OFFLINE);
    check("offline-relay", fci_browse_status(1, 2, 3, 0, 0, 1, 0), BROWSE_LIST_OFFLINE);

    /* never fetched: SEARCHING (not LIST OFFLINE) */
    check("searching-never", fci_browse_status(1, 0, 0, 0, 0, 0, 0), BROWSE_SEARCHING);
    check("searching-never-dropped", fci_browse_status(1, 0, 0, 0, 2, 0, 0), BROWSE_SEARCHING);
    check("searching-never-relay", fci_browse_status(1, 0, 0, 0, 0, 1, 0), BROWSE_SEARCHING);

    /* fetch ok, zero hosts: LIST_EMPTY (no change from original text) */
    check("empty", fci_browse_status(1, 1, 0, 0, 0, 0, 0), BROWSE_LIST_EMPTY);
    check("empty-relay", fci_browse_status(1, 1, 0, 0, 0, 1, 0), BROWSE_LIST_EMPTY);

    /* fetch ok, hosts pending: SEARCHING */
    check("pending", fci_browse_status(1, 1, 3, 2, 0, 0, 0), BROWSE_SEARCHING);
    check("pending-1", fci_browse_status(1, 1, 1, 1, 0, 0, 0), BROWSE_SEARCHING);

    /* fetch ok, hosts dropped: HOST_UNREACHABLE */
    check("dropped", fci_browse_status(1, 1, 3, 1, 2, 0, 0), BROWSE_HOST_UNREACHABLE);
    check("dropped-only", fci_browse_status(1, 1, 2, 0, 2, 0, 0), BROWSE_HOST_UNREACHABLE);

    /* relay refused: takes precedence over dropped/pending */
    check("relay", fci_browse_status(1, 1, 3, 1, 0, 1, 0), BROWSE_RELAY_OFFLINE);
    check("relay-dropped", fci_browse_status(1, 1, 3, 1, 2, 1, 0), BROWSE_RELAY_OFFLINE);
    check("relay-pending", fci_browse_status(1, 1, 3, 3, 0, 1, 0), BROWSE_RELAY_OFFLINE);

    /* all connected: no "no matches" row (LIST_EMPTY) */
    check("all-connected", fci_browse_status(1, 1, 3, 0, 0, 0, 0), BROWSE_LIST_EMPTY);

    /* precedence: relay > dropped > pending > empty */
    check("prec-relay-dropped-pending", fci_browse_status(1, 1, 3, 2, 1, 1, 0), BROWSE_RELAY_OFFLINE);
    check("prec-dropped-pending", fci_browse_status(1, 1, 3, 2, 1, 0, 0), BROWSE_HOST_UNREACHABLE);
    check("prec-pending-only", fci_browse_status(1, 1, 3, 2, 0, 0, 0), BROWSE_SEARCHING);

    printf("%d checks, %d failed\n", s_n, s_failed);
    return s_failed ? 1 : 0;
}
