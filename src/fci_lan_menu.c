/* The online menu gate and relabel for Evolution (Q19 phase 2, round 2).
 *
 * Menu mode (RECOMP_FCI_NET_MENU=1, fci.ini network-menu): the card reports link up at boot
 * without a transport (fci_nic.c), so the online items in the pre-game menu are enabled. The
 * player's pick starts the transport:
 *   - SYSTEM LINK (B_SystemLink) -> the LAN transport (fci_nic_start_lan).
 *   - INTERNET (B_Live, relabeled from XBOX LIVE) -> the LIST transport (fci_nic_start_list_lazy);
 *     the hash is rewritten to B_SystemLink so the dispatcher takes the System Link lobby branch
 *     (the redirect target, run-B confirmed).
 * "First choice wins until restart": a second pick logs and leaves the first transport.
 *
 * Two hooks (Evolution profile, compile.py REPLACE):
 *   - gate on sub_000FB0A2 (the pre-game multiplayer action dispatcher, recomp_0023.c:27569)
 *   - prehook on sub_000FA5B4 (the P_Multiplayer menu construction, recomp_0023.c:25290)
 *
 * The action hash is CRC-32 of the ASCII action name (sub_0026ACB2, poly 0xEDB88320), the same
 * transform used for element IDs. B_Live = 0x9b680e53, B_SystemLink = 0xf65128aa.
 *
 * The relabel: the Magma string lookup is vtable-nested (no clean small hook), so the label is
 * rewritten in memory after the string table loads (the task's "rewrite the parsed string" option).
 * The prehook scans the string table (MEM32(0x56BAC4)) for "XBOX LIVE" and overwrites it with
 * "INTERNET" (one-shot, idempotent; a second scan finds nothing). The text is one line in s_relabel.
 *
 * Inert unless menu mode is on; each log line is flushed.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

/* fci_menu.c helpers (made non-static for sharing) */
int guest_ok(uint32_t va, uint32_t len);
void menu_log_line(const char *handler, const char *ev, const char *extra);

/* fci_nic.c (menu mode + the transport entry) */
int fci_nic_menu_mode(void);
int fci_nic_start_menu_transport(int mode); /* 0 = LAN, 1 = LIST */

/* the label table: one line per relabeled item. "INTERNET" for B_Live; SYSTEM LINK is left as the
 * game has it. Changing the wording is one line here. (The description line is not changed: the
 * English description "Prove your skills..." does not mention Xbox Live.) */
static const struct {
    uint32_t crc;
    const char *from; /* the game's text (the search) */
    const char *to;   /* our text (the replacement) */
} s_relabel[] = {
    { 0x9b680e53u, "XBOX LIVE", "INTERNET" }, /* B_Live */
};
#define RELABEL_MAX (sizeof s_relabel / sizeof s_relabel[0])

/* the string table (the current language's); loaded at boot, before any menu */
#define STRING_TABLE_VA 0x56BAC4u
/* the scan bound: the table's strings are in a contiguous pool near the table object; 1 MB is a
 * generous bound for a pre-game menu string table (a few KB of strings). guest_ok stops the scan
 * at the first uncommitted page, so the bound is a ceiling, not a requirement. */
#define RELABEL_SCAN_BOUND 0x100000u

/* Scan the string table's memory for s_relabel[i].from and overwrite it with s_relabel[i].to.
 * One-shot and idempotent: once the text is replaced, a second scan finds nothing. The replacement
 * is written with a null terminator; if .to is shorter than .from, the trailing bytes of .from are
 * overwritten with nulls (the string is shorter, the rest is padding). Returns 1 when a replacement
 * is made. */
static int relabel_in_table(uint32_t tbl)
{
    int i;
    for (i = 0; i < (int)RELABEL_MAX; i++) {
        const char *from = s_relabel[i].from, *to = s_relabel[i].to;
        int fromlen = (int)strlen(from), tolen = (int)strlen(to);
        int span = fromlen > tolen ? fromlen : tolen;
        uint32_t off;
        for (off = 0; off < RELABEL_SCAN_BOUND; off += 4) {
            uint32_t va = tbl + off;
            if (!guest_ok(va, (uint32_t)(fromlen + 1)))
                break; /* past the committed region */
            if (memcmp((const void *)XBOX_PTR(va), from, (size_t)fromlen) != 0)
                continue;
            if (MEM8(va + fromlen) != 0)
                continue; /* not null-terminated: not the string */
            /* found: overwrite with .to, null-padded to .from's length (the string is shorter) */
            memcpy((void *)XBOX_PTR(va), to, (size_t)tolen);
            memset((void *)XBOX_PTR(va + tolen), 0, (size_t)(span - tolen));
            {
                char extra[128];
                snprintf(extra, sizeof extra, ", \"item\": %08x, \"from\": \"%s\", \"to\": \"%s\", \"va\": %08x",
                         s_relabel[i].crc, from, to, va);
                menu_log_line("evo-mp", "relabel", extra);
            }
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Prehook on sub_000FA5B4: the P_Multiplayer menu construction (recomp_0023.c:25290).
 *
 * Runs after the string table loads (boot) and before the menu renders. Rewrites the label text
 * in the string table (one-shot, idempotent). Inert unless menu mode is on.
 * --------------------------------------------------------------------------- */
void fci_prehook_000FA5B4(void)
{
    uint32_t tbl;
    if (!fci_nic_menu_mode())
        return;
    tbl = MEM32(STRING_TABLE_VA);
    if (!guest_ok(tbl, 4))
        return;
    if (!relabel_in_table(tbl)) {
        /* the label was not found (already relabeled, or the table layout differs): leave it as-is */
        menu_log_line("evo-mp", "relabel-miss", ", \"note\": \"label not found; left as the game has it\"");
    }
}

/* ---------------------------------------------------------------------------
 * Gate on sub_000FB0A2: the pre-game multiplayer action dispatcher (recomp_0023.c:27569).
 *
 * Dispatch (string VA -> name -> CRC-32), the B_Live/B_SystemLink branches (recomp_0023.c:27796/27840):
 *   0x4BEFA0  B_SystemLink   0xf65128aa  -> sub_000FBF7E (System Link lobby)
 *   0x4BEFB0  B_Live         0x9b680e53  -> sub_000FBFA2 (Xbox Live flow)
 *
 * In menu mode: B_SystemLink starts the LAN transport; B_Live starts the LIST transport and rewrites
 * the hash to B_SystemLink (the dispatcher re-reads it per branch, so it takes the System Link lobby
 * branch -- the redirect target, run-B confirmed). Both return 0 (the body runs; the redirect is the
 * hash rewrite, not a skip). "First choice wins until restart": a second pick logs and no-ops.
 *
 * Returns 1 to skip the body, 0 to run the body.
 * --------------------------------------------------------------------------- */
int fci_gate_000FB0A2(void)
{
    uint32_t ev_obj, hash;
    if (!fci_nic_menu_mode())
        return 0; /* not menu mode: the original Xbox Live / System Link */
    ev_obj = MEM32(g_esp + 4);
    if (!guest_ok(ev_obj, 8))
        return 0;
    hash = MEM32(ev_obj + 4);
    if (hash == 0xf65128aau) { /* B_SystemLink -> the LAN transport */
        fci_nic_start_menu_transport(0);
        menu_log_line("evo-mp", "pick", ", \"elem\": f65128aa, \"item\": \"SYSTEM LINK\", \"transport\": \"lan\"");
        return 0; /* the body runs: the System Link lobby (the redirect target) */
    }
    if (hash == 0x9b680e53u) { /* B_Live -> the LIST transport; rewrite the hash to B_SystemLink */
        fci_nic_start_menu_transport(1);
        MEM32(ev_obj + 4) = 0xf65128aa; /* the dispatcher re-reads it: the System Link branch */
        menu_log_line("evo-mp", "pick",
                      ", \"elem\": 9b680e53, \"item\": \"INTERNET\", \"transport\": \"list\", \"redirect\": \"f65128aa\"");
        return 0;
    }
    return 0; /* other items: the body runs */
}
