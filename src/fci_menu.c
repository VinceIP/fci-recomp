/* RECOMP_FCI_MENU_LOG=<file>: JSON-lines trace of menu action dispatch.
 * QUIT GAME on the main menu (both titles): fci_disk.c turns the hidden CHEATS (DEBUG) item into B_Quit;
 * fci_menu_quit_action closes the game as the window's close button does. RECOMP_FCI_MENU_QUIT=0: off.
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
void menu_log_line(const char *handler, const char *ev, const char *extra);

void fci_menu_log_init(void)
{
    const char *path = getenv("RECOMP_FCI_MENU_LOG");
    if (path && *path)
        menu_log = fopen(path, "w");
}

/* QUIT GAME on the main menu, on unless RECOMP_FCI_MENU_QUIT=0 (then the item stays hidden) */
int fci_menu_quit_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_FCI_MENU_QUIT");
        on = !(e && *e == '0');
    }
    return on;
}

/* a main menu action: B_Quit (0x8bcdf569) closes the game through the window's close path, so a save in
 * progress finishes, the host list is left and the log is kept. 1 when it was B_Quit. */
int fci_menu_quit_action(uint32_t hash)
{
    extern void fci_window_close(void);
    if (!fci_menu_quit_enabled() || hash != 0x8bcdf569u)
        return 0;
    static volatile LONG asked; /* Evolution's dispatcher re-reads the action, so one request per press */
    if (InterlockedExchange(&asked, 1))
        return 1;
    menu_log_line("main", "quit", ", \"elem\": 8bcdf569, \"action\": \"close\"");
    fprintf(stderr, "[FCI] menu: QUIT GAME chosen, closing\n");
    fflush(stderr);
    fci_window_close();
    return 1;
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

    extern void fci_multiplayer_menu_action(uint32_t hash);
    fci_multiplayer_menu_action(hash);

    fci_menu_quit_action(hash); /* the body ignores B_Quit */

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
