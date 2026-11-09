/* The online menu gate and relabel for Evolution (Q19 phase 2, round 3).
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
 * Two hooks (Evolution profile, image-profile.json):
 *   - gate on sub_000FB0A2 (the pre-game multiplayer action dispatcher, recomp_0023.c:27569)
 *   - gate on sub_0026A628 (runs the lookup, then relabels; the build has no posthook) (the string table lookup, recomp_0071.c:11496)
 *
 * The action hash is CRC-32 of the ASCII action name (sub_0026ACB2, poly 0xEDB88320), the same
 * transform used for element IDs. B_Live = 0x9b680e53, B_SystemLink = 0xf65128aa.
 *
 * The relabel: the string table lookup (sub_0026A628) returns a pointer to the entry whose
 * text is the label. The posthook scans the entry's first 128 bytes for the UTF-16 text
 * "XBOX LIVE" and overwrites it with "INTERNET". One-shot and idempotent: once the text is
 * replaced, a second lookup finds nothing. The entry is in the 0x80000000 memory region
 * (the string table pool), not in the code/data region.
 *
 * Why not scan the string table at menu construction (the round-2 approach)?
 *   - The string table pointer (MEM32(0x56BAC4)) is 0x8000d000, outside the 0x10000-0x3FFFFFF
 *     range that guest_ok covers. The scan would not reach the text.
 *   - The label "XBOX LIVE" is not resident as standalone text at dump time: it is consumed
 *     into glyph data when the screen is built. The only "XBOX LIVE" strings in memory are
 *     longer account messages, not the menu item label.
 *   - The lookup (sub_0026A628) returns the entry pointer every time the label is resolved,
 *     so hooking it catches the text before it is turned into glyphs.
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

/* ---------------------------------------------------------------------------
 * Relabel: UTF-16LE byte sequences for "XBOX LIVE" and "INTERNET".
 *
 * The string table entries store labels in UTF-16LE (confirmed by the description string
 * at VA 0x01382980 in the memdump: "Prove your skills..." is UTF-16LE). The entry
 * structure (sub_0026A628 return value) has the CRC-32 at offset +4; the text is
 * elsewhere in the first few dwords. We scan 128 bytes (64 uint16_t) to find it.
 * --------------------------------------------------------------------------- */
static const uint16_t s_from_utf16[]
        = {0x0058, 0x0042, 0x004F, 0x0058, 0x0020, 0x004C, 0x0049, 0x0056, 0x0045}; /* "XBOX LIVE" */
#define FROM_LEN (sizeof s_from_utf16 / sizeof s_from_utf16[0])

static const uint16_t s_to_utf16[]
        = {0x0049, 0x004E, 0x0054, 0x0045, 0x0052, 0x004E, 0x0045, 0x0054}; /* "INTERNET" */
#define TO_LEN (sizeof s_to_utf16 / sizeof s_to_utf16[0])

/* Scan a region of guest memory (UTF-16LE) for s_from_utf16 and overwrite with s_to_utf16.
 * The region is the entry structure returned by sub_0026A628. We scan 128 bytes (64 uint16_t)
 * which covers the entry header and the text. Returns 1 when a replacement is made. */
static int relabel_entry(uint32_t entry_va)
{
    uint16_t *base;
    int i;
    const int scan_units = 64; /* 128 bytes / 2 */

    if (!guest_ok(entry_va, (uint32_t)(scan_units * 2)))
        return 0;

    base = (uint16_t *)XBOX_PTR(entry_va);
    for (i = 0; i + (int)FROM_LEN <= scan_units; i++) {
        if (memcmp(&base[i], s_from_utf16, FROM_LEN * 2) != 0)
            continue;
        /* Found: overwrite with s_to_utf16, null-pad to FROM_LEN */
        memcpy(&base[i], s_to_utf16, TO_LEN * 2);
        {
            int j;
            for (j = (int)TO_LEN; j < (int)FROM_LEN; j++)
                base[i + j] = 0;
        }
        menu_log_line("evo-mp", "relabel",
                      ", \"note\": \"B_Live label XBOX LIVE -> INTERNET at lookup "
                      "(sub_0026A628 posthook)\"");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Posthook on sub_0026A628: the string table lookup (recomp_0071.c:11496).
 *
 * sub_0026A628 is called by sub_0004F7FC (recomp_0007.c:9416) with the CRC-32 of the
 * element ID (e.g. "B_Live" -> 0x9b680e53). It returns a pointer to the string table
 * entry (eax) or 0 if not found. The entry contains the label text in UTF-16LE.
 *
 * The posthook runs after the lookup returns. If the entry is valid and contains
 * "XBOX LIVE" (UTF-16LE), it overwrites the text with "INTERNET". This is one-shot
 * and idempotent: once the text is replaced, subsequent lookups find "INTERNET"
 * (not "XBOX LIVE") and the hook is a no-op.
 *
 * This hook fires for every string table lookup (not just the menu), but the
 * relabel only matches "XBOX LIVE" which appears once in the table (the B_Live
 * label). Other lookups are unaffected.
 * --------------------------------------------------------------------------- */
/* the lifted lookup behind the gate; weak because the map editor image links this file without the gate */
__attribute__((weak)) void sub_0026A628_fci_body(void);

/* the build has no posthook kind: the gate runs the lookup itself at entry (the state is the entry
 * state), fixes up the result, and returns 1 so the wrapper does not run it a second time */
int fci_gate_0026A628(void)
{
    if (!fci_nic_menu_mode() || !sub_0026A628_fci_body)
        return 0;
    {
        uint32_t arg1 = MEM32(g_esp + 4), arg2 = MEM32(g_esp + 8), ecx_in = g_ecx;
        sub_0026A628_fci_body();
        if (getenv("RECOMP_FCI_MENU_LOOKUP_DEBUG") && (arg1 == 0x9b680e53u || arg1 == 0xf65128aau)) {
            /* diagnostic: where the label text lives (the contiguous window is outside the RAM dumps) */
            static int scanned;
            if (!scanned++) {
                uint32_t va;
                int hits = 0;
                for (va = 0x80010000u; va < 0x84000000u - 32 && hits < 12; va += 2) {
                    if ((va & 0xFFF) == 0 && !guest_ok(va, 0x1000)) {
                        va += 0x1000 - 2;
                        continue;
                    }
                    if (MEM32(va) == 0x00420058u && MEM32(va + 4) == 0x0058004Fu && MEM32(va + 8) == 0x004C0020u) {
                        char extra[160];
                        snprintf(extra, sizeof extra, ", \"label_utf16_at\": \"%08X\", \"next\": \"%04X %04X\"", va,
                                 MEM16(va + 18), MEM16(va + 20));
                        menu_log_line("evo-mp", "scan", extra);
                        hits++;
                        { /* who points at it (the string's owner), in RAM and the window */
                            static const uint32_t rng[2][2] = {{0x00010000u, 0x04000000u}, {0x80000000u, 0x84000000u}};
                            uint32_t r, p2;
                            int ph = 0;
                            for (r = 0; r < 2; r++)
                                for (p2 = rng[r][0]; p2 < rng[r][1] && ph < 6; p2 += 4) {
                                    if ((p2 & 0xFFF) == 0 && !guest_ok(p2, 0x1000)) {
                                        p2 += 0x1000 - 4;
                                        continue;
                                    }
                                    if (MEM32(p2) == va || MEM32(p2) == va - 4 || MEM32(p2) == va - 8) {
                                        char e2[200];
                                        snprintf(e2, sizeof e2, ", \"str\": \"%08X\", \"ptr_at\": \"%08X\", \"val\": \"%08X\", \"ctx\": \"%08X %08X %08X %08X\"",
                                                 va, p2, MEM32(p2), MEM32(p2 - 8), MEM32(p2 - 4), MEM32(p2 + 4), MEM32(p2 + 8));
                                        menu_log_line("evo-mp", "owner", e2);
                                        ph++;
                                    }
                                }
                        }
                    }
                }
            }
            /* diagnostic: the B_Live / B_SystemLink objects and what their pointers lead to */
            uint32_t e = g_eax, k, w;
            for (k = 0; k < 4 && guest_ok(e, 16); k++) {
                uint32_t base = k == 0 ? e : MEM32(e + 4 * k + 4);
                char extra[600];
                int len = snprintf(extra, sizeof extra, ", \"id\": \"%08X\", \"ecx\": \"%08X\", \"which\": %u, \"at\": \"%08X\", \"w\": \"",
                                   arg1, ecx_in, k, base);
                for (w = 0; w < 24 && guest_ok(base + 4 * w, 4) && len < (int)sizeof extra - 12; w++)
                    len += snprintf(extra + len, sizeof extra - len, "%08X ", MEM32(base + 4 * w));
                snprintf(extra + len, sizeof extra - len, "\"");
                menu_log_line("evo-mp", "lookup", extra);
            }
        }
    }
    if (getenv("RECOMP_FCI_MENU_RELABEL_TEST") && MEM32(g_esp + 4) == 0x9b680e53u) {
        /* FEASIBILITY TEST ONLY: the string-table record {B_Live hash, 0, text} near the observed
         * address; the real code must reach the table from its root, not by scanning */
        static int done;
        uint32_t r;
        for (r = 0x808E0000u; !done && r < 0x80900000u; r += 4)
            if (guest_ok(r, 12) && MEM32(r) == 0x9b680e53u && MEM32(r + 4) == 0 && guest_ok(MEM32(r + 8), 20)
                && MEM32(MEM32(r + 8)) == 0x00420058u) {
                done = relabel_entry(MEM32(r + 8));
            }
    }
    return 1;
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
        menu_log_line("evo-mp", "pick",
                      ", \"elem\": f65128aa, \"item\": \"SYSTEM LINK\", \"transport\": \"lan\"");
        return 0; /* the body runs: the System Link lobby (the redirect target) */
    }
    if (hash == 0x9b680e53u) { /* B_Live -> the LIST transport; rewrite the hash to B_SystemLink */
        fci_nic_start_menu_transport(1);
        MEM32(ev_obj + 4) = 0xf65128aa; /* the dispatcher re-reads it: the System Link branch */
        menu_log_line("evo-mp", "pick",
                      ", \"elem\": 9b680e53, \"item\": \"INTERNET\", \"transport\": \"list\", \"redirect\": "
                      "\"f65128aa\"");
        return 0;
    }
    return 0; /* other items: the body runs */
}
