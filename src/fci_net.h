/* The net protocol: one number for what two builds may play together on the wire.
 *
 * A peer is playable only when all three of these match: the title, the disc's version (the XBE
 * certificate's Version, fci_xbe_version in fci_main.c), and this number. The host list hides a
 * mismatch (a peer is never listed to the other); the card refuses one (a peer is never admitted,
 * its frames never reach the game) and logs it once per peer. research/docs/multiplayer.md.
 *
 * Bump it when the wire format (the FCN1 datagrams, src/fci_nic.c) or the game's sync behavior
 * changes. A new disc may keep the old number; one bump may span several disc versions. Kept in
 * step by hand: this file (the card and the worker), PROTO in tools/hostlist/lobby.js, and
 * tools/fake_hostlist.py.
 *
 * 2: the card's hellos and knocks carry the (title, version, protocol) triple, and a peer is
 *    admitted only on a matching triple (q21, 2026-11-19).
 * 1: the first number; only the host list filtered, by title and disc version.
 */
#ifndef FCI_NET_H
#define FCI_NET_H

#define FCI_NET_PROTO 2

#endif
