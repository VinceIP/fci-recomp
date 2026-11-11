#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"

/* fci_menu.c helpers (made non-static for sharing) */
int read_action_hash(uint32_t *out_hash);
void menu_log_line(const char *handler, const char *ev, const char *extra);

/* fci_nic.c (menu mode + the transport entry) */
int fci_nic_menu_mode(void);
int fci_nic_start_menu_transport(int mode); /* 0 = LAN, 1 = LIST */

/* fci_nic.c / fci_net_worker.c (the browse status, Q29 round 3) */
void fci_nic_browse_state(int *in_list, int *n_pending, int *n_dropped);
void fci_net_worker_browse_state(int *fetch_state, int *n_hosts, int *relay_refused, int *version_refused);

#include "fci_browse_status.h" /* the cause logic, unit-tested */

/* Match the complete writable string, including the terminator, before changing guest memory. */
static const uint16_t browse_original[17] = {
    'N', 'O', ' ', 'M', 'A', 'T', 'C', 'H', 'E', 'S', ' ', 'F', 'O', 'U', 'N', 'D', 0,
};

static uint16_t *browse_find_text(void)
{
    for (uint32_t va = 0x80000000u; va < 0x84000000u;) {
        MEMORY_BASIC_INFORMATION mbi;
        const uint8_t *start = (const uint8_t *)XBOX_PTR(va);
        if (!VirtualQuery(start, &mbi, sizeof mbi))
            break;
        size_t bytes = mbi.RegionSize - ((uintptr_t)start - (uintptr_t)mbi.BaseAddress);
        if (bytes > 0x84000000u - va)
            bytes = 0x84000000u - va;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE) {
            for (size_t offset = 0; offset + sizeof browse_original <= bytes; offset += 2) {
                const uint16_t *text = (const uint16_t *)(start + offset);
                if (*text == browse_original[0] && !memcmp(text, browse_original, sizeof browse_original))
                    return (uint16_t *)text;
            }
        }
        va += (uint32_t)bytes;
    }
    return NULL;
}

static void browse_relabel(void)
{
    static const char *const texts[] = {
        [BROWSE_NONE] = "NO MATCHES FOUND",
        [BROWSE_LIST_OFFLINE] = "LIST OFFLINE",
        [BROWSE_LIST_EMPTY] = "NO MATCHES FOUND",
        [BROWSE_SEARCHING] = "SEARCHING...",
        [BROWSE_HOST_UNREACHABLE] = "HOST UNREACHABLE",
        [BROWSE_RELAY_OFFLINE] = "RELAY OFFLINE",
        [BROWSE_UPDATE_REQUIRED] = "UPDATE REQUIRED",
    };
    static uint16_t *text;
    static uint16_t previous[17];
    static ULONGLONG next_scan;
    int in_list, n_pending, n_dropped, fetch_state, n_hosts, relay_refused, version_refused;
    fci_nic_browse_state(&in_list, &n_pending, &n_dropped);
    if (!in_list && !text)
        return;

    if (text) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(text, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            mbi.Protect != PAGE_READWRITE ||
            (uintptr_t)text - (uintptr_t)mbi.BaseAddress + sizeof previous > mbi.RegionSize ||
            (memcmp(text, previous, sizeof previous) && memcmp(text, browse_original, sizeof previous)))
            text = NULL;
    }
    if (!text && in_list) {
        ULONGLONG now = GetTickCount64();
        if (now >= next_scan) {
            next_scan = now + 3000;
            text = browse_find_text();
        }
    }
    if (!text)
        return;

    fci_net_worker_browse_state(&fetch_state, &n_hosts, &relay_refused, &version_refused);
    int status = fci_browse_status(in_list, fetch_state, n_hosts, n_pending, n_dropped, relay_refused, version_refused);
    const char *label = texts[status];
    uint16_t replacement[17] = {0};
    for (size_t i = 0; label[i] && i < 16; i++)
        replacement[i] = (uint16_t)label[i];
    if (memcmp(text, replacement, sizeof replacement)) {
        memcpy(text, replacement, sizeof replacement);
        fprintf(stderr, "[FCI] network: browser status: %s (cause %d)\n", label, status);
        fflush(stderr);
    }
    memcpy(previous, replacement, sizeof previous);
    if (!in_list)
        text = NULL;
}

void fci_multiplayer_menu_action(uint32_t hash)
{
    if (!fci_nic_menu_mode())
        return;
    if (hash == 0xf65128aau) {
        fci_nic_start_menu_transport(0);
        menu_log_line("multiplayer", "pick", ", \"item\": \"LAN\"");
    } else if (hash == 0x9b680e53u) {
        fci_nic_start_menu_transport(1);
        menu_log_line("multiplayer", "pick", ", \"item\": \"INTERNET\"");
        /* Both dispatchers read the hash again when selecting the System Link branch. */
        MEM32(MEM32(g_esp + 4) + 4) = 0xf65128aau;
    }
}

int fci_gate_000FB0A2(void)
{
    uint32_t hash;
    if (fci_nic_menu_mode() && read_action_hash(&hash))
        fci_multiplayer_menu_action(hash);
    return 0;
}

/* Refresh the empty browser row on the game thread. */
int fci_gate_0004ABC6(void)
{
    browse_relabel();
    return 0;
}
