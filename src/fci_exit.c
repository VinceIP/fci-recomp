/* User exits wait for the save log to go quiet, so the process does not end between a save's
 * file writes and leave a set the game reports as damaged (Q22c,
 * research/qwen/notes/q22-saves.md). The source of truth is the save log's state in the kernel
 * (patches/runtime/0027, savelog_busy_ms): quiet means no UDATA file is open for writing and no
 * save log line was emitted within the quiet window. Crash exits (the STOP codes,
 * docs/internals.md) do not wait: a fault is not a user exit, and the game's state is already
 * gone. */
#include <stdio.h>
#include <stdint.h>
#include <windows.h>

extern uint64_t savelog_busy_ms(uint64_t now_ms, uint64_t quiet_ms); /* kernel_file.c */

#define FCI_EXIT_QUIET_MS 250  /* no save log line within this window: the save is done */
#define FCI_EXIT_MAX_MS 3000   /* never wait longer than this */

static uint64_t s_begin; /* the exit wait's start tick; 0: none running */
static int s_busy_seen;  /* the save log was busy since the wait began */

/* starts the wait (idempotent while one runs); the window paths then poll fci_exit_ready from
 * their message loop's timer, the loopless paths call fci_exit_wait */
void fci_exit_begin(void)
{
    if (!s_begin)
        s_begin = GetTickCount64();
}

/* 1 when the process may end: the save log is quiet, or the bound is reached (a line says it
 * gave up). One stderr line when it waited. */
int fci_exit_ready(void)
{
    uint64_t now, waited;
    int busy;
    if (!s_begin)
        return 1;
    now = GetTickCount64();
    waited = now - s_begin;
    busy = savelog_busy_ms(now, FCI_EXIT_QUIET_MS) != 0;
    if (busy)
        s_busy_seen = 1;
    if (busy && waited < FCI_EXIT_MAX_MS)
        return 0;
    s_begin = 0;
    if (s_busy_seen)
        fprintf(stderr, "[FCI] exit waited %llu ms for a save to finish%s\n",
                (unsigned long long)waited, busy ? "; giving up: a save may be unfinished" : "");
    fflush(stderr);
    s_busy_seen = 0;
    return 1;
}

/* a path with no message loop to keep up (the console close, the relaunch): sleep between
 * polls; the guest is not running on this thread while it runs */
void fci_exit_wait(void)
{
    fci_exit_begin();
    while (!fci_exit_ready())
        Sleep(10);
}
