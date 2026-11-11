#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"

/* fci_menu.c helpers (made non-static for sharing) */
int read_action_hash(uint32_t *out_hash);
void menu_log_line(const char *handler, const char *ev, const char *extra);

/* fci_nic.c (menu mode + the transport entry) */
int fci_nic_menu_mode(void);
int fci_nic_transport_active(void);
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

static int s_browser_mode = -1;

void fci_multiplayer_menu_action(uint32_t hash)
{
    if (!fci_nic_menu_mode())
        return;
    if (!fci_nic_transport_active() && (hash == 0xf65128aau || hash == 0x9b680e53u))
        s_browser_mode = hash == 0x9b680e53u;
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

__attribute__((weak)) void sub_00109670_fci_body(void);
__attribute__((weak)) void sub_000CF7EE_fci_body(void);
__attribute__((weak)) void sub_001536F0_fci_body(void);
__attribute__((weak)) void sub_000FE553_fci_body(void);

static uint32_t s_browser_page;

struct guest_menu_string {
    uint32_t allocator;
    uint32_t text;
    uint32_t unused[3];
    uint32_t length;
    uint32_t capacity;
};

struct browser_menu_call {
    uint32_t frame[5];
    struct guest_menu_string label, icon;
    uint16_t label_text[16];
    char icon_text[20];
};

static uint32_t browser_call_frame(uint32_t sp)
{
    const uint32_t frame = sp - sizeof(struct browser_menu_call);
    MEMORY_BASIC_INFORMATION mbi;
    const void *start = (const void *)XBOX_PTR(frame);
    if (frame > sp || !VirtualQuery(start, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        mbi.Protect != PAGE_READWRITE ||
        (uintptr_t)start - (uintptr_t)mbi.BaseAddress + sizeof(struct browser_menu_call) > mbi.RegionSize)
        return 0;
    return frame;
}

static uint32_t set_menu_label(uint32_t frame, const char *label)
{
    struct browser_menu_call *call = (struct browser_menu_call *)XBOX_PTR(frame);
    const uint32_t length = (uint32_t)strlen(label);
    call->label = (struct guest_menu_string){
        .text = frame + offsetof(struct browser_menu_call, label_text),
        .length = length,
        .capacity = (sizeof call->label_text / sizeof *call->label_text) - 1,
    };
    for (uint32_t i = 0; i <= length; i++)
        call->label_text[i] = (uint16_t)label[i];
    return frame + offsetof(struct browser_menu_call, label);
}

static uint32_t set_menu_icon(uint32_t frame, const char *icon)
{
    struct browser_menu_call *call = (struct browser_menu_call *)XBOX_PTR(frame);
    const uint32_t length = (uint32_t)strlen(icon);
    call->icon = (struct guest_menu_string){
        .text = frame + offsetof(struct browser_menu_call, icon_text),
        .length = length,
        .capacity = sizeof call->icon_text - 1,
    };
    memcpy(call->icon_text, icon, length + 1);
    return frame + offsetof(struct browser_menu_call, icon);
}

static void run_browser_setter(uint32_t sp, uint32_t frame, recomp_func_t body)
{
    /* The lifted setter copies its borrowed strings. Its frame grows below the temporary data. */
    g_esp = frame;
    body();
    g_esp += sp - frame;
}

static int browser_title(uint32_t caller, recomp_func_t body)
{
    if (s_browser_mode < 0 || MEM32(g_esp) != caller || !body)
        return 0;
    const uint32_t sp = g_esp, frame = browser_call_frame(sp);
    if (!frame)
        return 0;

    s_browser_page = MEM32(sp + 4);
    const char *title = s_browser_mode == 1 ? "INTERNET" : "LOCAL NETWORK";
    MEM32(frame) = caller;
    MEM32(frame + 4) = s_browser_page;
    MEM32(frame + 8) = set_menu_label(frame, title);
    char extra[96];
    snprintf(extra, sizeof extra, ", \"page\": \"%08x\", \"title\": \"%s\"", s_browser_page, title);
    menu_log_line("multiplayer", "browser-title", extra);
    run_browser_setter(sp, frame, body);
    return 1;
}

int fci_gate_00109670(void) { return browser_title(0x00119C45u, sub_00109670_fci_body); }
int fci_gate_000CF7EE(void) { return browser_title(0x000DA1F5u, sub_000CF7EE_fci_body); }

static int browser_badge(uint32_t page, unsigned icon_arg, unsigned label_arg, recomp_func_t body)
{
    if (s_browser_mode < 0 || !page || page != s_browser_page || !body)
        return 0;
    const uint32_t sp = g_esp, frame = browser_call_frame(sp);
    if (!frame)
        return 0;

    const char *label = "", *icon = "";
    int fetch_state = -1;
    if (s_browser_mode == 1) {
        int n_hosts, relay_refused, version_refused;
        fci_net_worker_browse_state(&fetch_state, &n_hosts, &relay_refused, &version_refused);
        if (!fci_nic_transport_active())
            fetch_state = 2;
        if (fetch_state == 0) {
            label = "CHECKING SERVER";
        } else if (fetch_state == 1) {
            label = "SERVER ONLINE";
            icon = "Live_Icon_Online";
        } else {
            label = "SERVER OFFLINE";
            icon = "Live_Icon_Offline";
        }
    }

    memcpy((void *)XBOX_PTR(frame), (const void *)XBOX_PTR(sp), (label_arg + 1) * sizeof(uint32_t));
    MEM32(frame + icon_arg * 4) = set_menu_icon(frame, icon);
    MEM32(frame + label_arg * 4) = set_menu_label(frame, label);
    static int last_fetch_state = -2;
    if (fetch_state != last_fetch_state) {
        last_fetch_state = fetch_state;
        char extra[96];
        snprintf(extra, sizeof extra, ", \"state\": %d, \"label\": \"%s\"", fetch_state, label);
        menu_log_line("multiplayer", "server-badge", extra);
    }
    run_browser_setter(sp, frame, body);
    return 1;
}

int fci_gate_001536F0(void) { return browser_badge(g_eax, 2, 3, sub_001536F0_fci_body); }
int fci_gate_000FE553(void) { return browser_badge(MEM32(g_esp + 8), 3, 4, sub_000FE553_fci_body); }
