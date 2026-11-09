/* RECOMP_FCI_MENU_LOG=<file>: JSON-lines trace of menu action dispatch.
 * RECOMP_FCI_MENU_QUIT=1: intercept B_Quit on the main menu and exit.
 *
 * Two hooks:
 *   - gate on sub_0014EFF0 (main menu handler, recomp_0020.c:1103)
 *   - prehook on sub_00148170 (MP pause menu handler, recomp_0019.c:47155)
 *
 * The action hash is CRC-32 of the ASCII action name (sub_003529B0),
 * the same transform used for element IDs and DAT2 file hashes.
 * B_Quit = 0x8bcdf569.
 *
 * Inert unless the environment variables are set; each log line is flushed.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

static FILE *menu_log;
static volatile LONG menu_seq;
static SRWLOCK menu_lock = SRWLOCK_INIT;
static int menu_quit_enabled;

void fci_menu_log_init(void)
{
    const char *path = getenv("RECOMP_FCI_MENU_LOG");
    if (path && *path)
        menu_log = fopen(path, "w");
    const char *quit = getenv("RECOMP_FCI_MENU_QUIT");
    menu_quit_enabled = (quit && quit[0] == '1');
}

/* guest address is safely readable: inside the guest window and mapped. Shared with fci_lan_menu.c. */
int guest_ok(uint32_t va, uint32_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    /* game pointers include 0x8xxxxxxx (contiguous memory); only the null page and wrap are out */
    if (va < 0x10000 || va + len < va)
        return 0;
    return VirtualQuery((void *)XBOX_PTR(va), &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT;
}

/* JSON-lines trace line. Shared with fci_lan_menu.c. */
void menu_log_line(const char *handler, const char *ev, const char *extra)
{
    if (!menu_log)
        return;
    AcquireSRWLockExclusive(&menu_lock);
    fprintf(menu_log, "{\"seq\": %ld, \"tid\": %lu, \"h\": \"%s\", \"ev\": \"%s\"%s}\n",
            (long)InterlockedIncrement(&menu_seq), (unsigned long)GetCurrentThreadId(),
            handler, ev, extra);
    fflush(menu_log);
    ReleaseSRWLockExclusive(&menu_lock);
}

/* Read the action hash from the event object (first stack param, +4). Returns 0 on failure.
 * Shared with fci_lan_menu.c. */
int read_action_hash(uint32_t *out_hash)
{
    uint32_t ev_obj = MEM32(g_esp + 4);
    if (!guest_ok(ev_obj, 8))
        return 0;
    *out_hash = MEM32(ev_obj + 4);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Gate on sub_0014EFF0: the main menu action handler (recomp_0020.c:1103).
 *
 * Dispatch table (string VA → name → CRC-32):
 *   0x6093B0  B_StoryMode         0x17130b2f
 *   0x609440  B_SplitScreen       0x8fa9611f
 *   0x6093A0  B_Multiplayer       0x344e8201
 *   0x609380  B_SpecialFeatures   0xb3c29504
 *   0x609434  B_MapEditor         0x08d70d76
 *   0x609394  B_Cheats            0x3c3bfdbb
 *   0x608D04  B_Options           0x188fc220
 *   0x609460  B_Live              0x9b680e53
 *   0x609374  B_Credits           0x89ade9d9
 *   (no B_Quit — that is the gap this hook fills)
 *
 * Returns 1 to skip the body (action handled), 0 to run the body.
 * --------------------------------------------------------------------------- */
int fci_gate_0014EFF0(void)
{
    uint32_t hash;
    char extra[256];

    if (!read_action_hash(&hash)) {
        if (menu_log)
            menu_log_line("main", "action", ", \"elem\": \"bad ptr\"");
        return 0; /* can't read the hash; let the body run */
    }

    /* Log every action for the trace. */
    snprintf(extra, sizeof extra, ", \"elem\": %08x", hash);
    menu_log_line("main", "action", extra);

    /* B_Quit intercept: only when RECOMP_FCI_MENU_QUIT=1. */
    if (menu_quit_enabled && hash == 0x8bcdf569u) {
        menu_log_line("main", "quit", ", \"elem\": 8bcdf569, \"action\": \"exit\"");
        /*
         * Draft: exit directly. This skips the confirm dialog that the MP
         * handler (sub_00148170) shows via sub_00105FF0. The dialog function
         * takes 12 parameters and interacts with the Magma UI system; calling
         * it safely from a gate requires setting up the full parameter stack
         * and ensuring the UI system is in the correct state. For a draft that
         * proves the button works end-to-end, ExitProcess(0) is the safest
         * equivalent — it is the same call the kernel bridge makes at
         * kernel_hal.c:519 when HalReturnToFirmware fires.
         *
         * Proper implementation (future):
         *   1. Set a global "quit requested" flag here, return 1.
         *   2. In a hook on the frame loop (e.g. Input_PollPads sub_001F6C00),
         *      check the flag. When set, call sub_00105FF0 with the same
         *      parameters as sub_00148170's B_Quit branch (~0x001484C2):
         *        - eax = 0x2D (45, dialog type)
         *        - stack: title string VA, body string VA, callback, ...
         *      The dialog's confirm callback then calls HalReturnToFirmware.
         *   3. This requires the Magma UI to be in the "main menu" state,
         *      which it is when the frame loop runs.
         */
        fprintf(stderr, "[MENU_QUIT] B_Quit intercepted on main menu — exiting\n");
        ExitProcess(0);
    }

    return 0; /* let the body handle it */
}

/* ---------------------------------------------------------------------------
 * Prehook on sub_00148170: the MP pause menu action handler (recomp_0019.c:47155).
 *
 * Dispatch table:
 *   0x608D10  B_Resume            0x????????
 *   0x608D04  B_Options           0x188fc220
 *   0x608CFC  B_Quit              0x8bcdf569  ← already handled by the body
 *   0x608CD4  B_ServerSettings    0x????????
 *
 * This handler already has a B_Quit branch (confirm dialog → HalReturnToFirmware).
 * The prehook is for logging only.
 * --------------------------------------------------------------------------- */
void fci_prehook_00148170(void)
{
    uint32_t hash;
    char extra[256];

    if (!menu_log)
        return;
    if (!read_action_hash(&hash)) {
        menu_log_line("mp", "action", ", \"elem\": \"bad ptr\"");
        return;
    }
    snprintf(extra, sizeof extra, ", \"elem\": %08x", hash);
    menu_log_line("mp", "action", extra);
}
