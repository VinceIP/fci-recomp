/* The network worker thread: one WinHTTP session for the host list (RECOMP_FCI_NET_LIST).
 * It never touches the card's UDP socket; it takes s_lock only around fci_nic_list_* calls.
 * research/docs/multiplayer.md, tools/fake_hostlist.py */
#ifndef FCI_NET_WORKER_H
#define FCI_NET_WORKER_H

#include <stdint.h>

void fci_net_report_probe(const char *receipt, const char *path, const char *stage, uint64_t started);

/* start the worker thread (LIST mode only; called from fci_nic_init) */
void fci_net_worker_start(void);
/* stop the worker thread and close its WinHTTP session (called from fci_nic_shutdown) */
void fci_net_worker_stop(void);
/* leave the host list on a clean exit: unregisters this host (idempotent, safe from any thread,
 * bounded to about 1.5 s). Called from the exit paths -- the window's WM_CLOSE, the toolkit's exit
 * hook (quit-to-dashboard / shutdown), and fci_net_worker_stop -- so the listing is removed
 * instead of waiting for its TTL. */
void fci_net_leave(void);

/* the game's XNetRegisterKey import thunk: the worker registers as a host from here */
void fci_prehook_0051CDD4(void);

#endif
