/* RECOMP_FCI_TELEMETRY=<file>: the live player state as one JSON line per tick, for comparing two
 * instances in a match (tools/sync_check.py). Evolution only: the entity vtable and the member
 * offsets come from the profile's constants (data/evolution/default.profile.json); a title without
 * them logs "not mapped" once and writes nothing.
 *
 * The players are the CEntities (vtable FCI_TELEMETRY_ENTITY_VTABLE) whose CPlayer back-ref
 * (+FCI_TELEMETRY_ENTITY_PLAYER) is a CPlayer (vtable FCI_TELEMETRY_PLAYER_VTABLE). The position is
 * the CPlayer's foot (+FCI_TELEMETRY_PLAYER_FOOT), the member comparable across instances
 * (re/notes/q20 §10); the angles are the CEntity's m_angles (+FCI_TELEMETRY_ENTITY_ANG).
 *
 * The CEntity constructor (sub_00063F01, which stores the vtable) and destructor body (sub_000648E7) keep
 * a bounded set of live entities; a writer thread (as RECOMP_FCI_STATUS) samples it at
 * RECOMP_FCI_TELEMETRY_HZ (default 10). Reading guest memory from the writer thread races the game
 * thread, so each entity's fields are copied in one pass and the vtable and id are read before and
 * after; a sample is dropped if either changed (the entity was destroyed and reused in between).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"

/* the entity and player layout (data/evolution/default.profile.json); 0 = not mapped for this title */
#ifndef FCI_TELEMETRY_ENTITY_VTABLE
#define FCI_TELEMETRY_ENTITY_VTABLE 0u
#endif
#ifndef FCI_TELEMETRY_PLAYER_VTABLE
#define FCI_TELEMETRY_PLAYER_VTABLE 0u
#endif
#ifndef FCI_TELEMETRY_ENTITY_ID
#define FCI_TELEMETRY_ENTITY_ID 0u
#endif
#ifndef FCI_TELEMETRY_ENTITY_PLAYER
#define FCI_TELEMETRY_ENTITY_PLAYER 0u
#endif
#ifndef FCI_TELEMETRY_ENTITY_ANG
#define FCI_TELEMETRY_ENTITY_ANG 0u
#endif
#ifndef FCI_TELEMETRY_PLAYER_FOOT
#define FCI_TELEMETRY_PLAYER_FOOT 0u
#endif
#ifndef FCI_TELEMETRY_CLOCK
#define FCI_TELEMETRY_CLOCK 0u
#endif
/* the local player's CPlayer: the actor (Player_ProcessInput's this, fci_trace.c) points to it */
#ifndef FCI_ACTOR_ENTITY
#define FCI_ACTOR_ENTITY 0x4u
#endif

#define TELEMETRY_MAX_ENTITIES 4096

static CRITICAL_SECTION cs;
static volatile int cs_init;
static uint32_t entities[TELEMETRY_MAX_ENTITIES];
static int n_entities;
static int not_mapped_logged;

uint32_t fci_trace_player(void); /* fci_trace.c: the local player's actor */

static void lock(void)
{
    if (!cs_init) {
        InitializeCriticalSection(&cs);
        cs_init = 1;
    }
    EnterCriticalSection(&cs);
}
static void unlock(void) { LeaveCriticalSection(&cs); }

static void add_entity(uint32_t e)
{
    if (e < 0x10000u || e >= 0x04000000u)
        return;
    lock();
    for (int i = 0; i < n_entities; i++)
        if (entities[i] == e) {
            unlock();
            return;
        }
    if (n_entities < TELEMETRY_MAX_ENTITIES)
        entities[n_entities++] = e;
    unlock();
    if (getenv("RECOMP_FCI_TELEMETRY_DEBUG")) /* which objects the constructor hook sees */
        fprintf(stderr, "[FCI] telemetry: entity %08X\n", e);
}
static void remove_entity(uint32_t e)
{
    lock();
    for (int i = 0; i < n_entities; i++) {
        if (entities[i] == e) {
            entities[i] = entities[--n_entities];
            break;
        }
    }
    unlock();
}

/* the CEntity constructor takes the entity as its first stack argument (it stores the vtable into
 * [esp+4]); the destructor body is thiscall. Hooking the base constructor sub_0005B2F9 by ecx saw nothing. */
void fci_prehook_00063F01(void) { add_entity(MEM32(g_esp + 4)); }
void fci_prehook_000648E7(void) { remove_entity(g_ecx); }

static uint64_t wall_ms(void)
{
    FILETIME ft;
    union {
        FILETIME ft;
        uint64_t q;
    } u;
    GetSystemTimeAsFileTime(&ft);
    u.ft = ft;
    /* FILETIME is 100-ns intervals since 1601; the Unix epoch is 116444736000000000 of them later */
    return (u.q > 116444736000000000ULL ? u.q - 116444736000000000ULL : 0) / 10000;
}

/* one pass over a live entity: copy the fields, then re-check the vtable and id; 0 if it changed */
static int sample_entity(uint32_t e, uint32_t local_player, char *out, int out_len)
{
    if (MEM32(e) != FCI_TELEMETRY_ENTITY_VTABLE)
        return 0;
    uint32_t id0 = MEM32(e + FCI_TELEMETRY_ENTITY_ID);
    uint32_t player = MEM32(e + FCI_TELEMETRY_ENTITY_PLAYER);
    float ang[3] = {MEMF(e + FCI_TELEMETRY_ENTITY_ANG), MEMF(e + FCI_TELEMETRY_ENTITY_ANG + 4),
                    MEMF(e + FCI_TELEMETRY_ENTITY_ANG + 8)};
    float pos[3] = {0.0f, 0.0f, 0.0f};
    int is_player
            = player >= 0x10000u && player < 0x04000000u && MEM32(player) == FCI_TELEMETRY_PLAYER_VTABLE;
    if (is_player) {
        pos[0] = MEMF(player + FCI_TELEMETRY_PLAYER_FOOT);
        pos[1] = MEMF(player + FCI_TELEMETRY_PLAYER_FOOT + 4);
        pos[2] = MEMF(player + FCI_TELEMETRY_PLAYER_FOOT + 8);
    }
    if (MEM32(e) != FCI_TELEMETRY_ENTITY_VTABLE || MEM32(e + FCI_TELEMETRY_ENTITY_ID) != id0)
        return 0; /* destroyed and reused while we read it */
    if (!is_player)
        return 0; /* a non-player CEntity */
    return snprintf(out, out_len,
                    "{\"id\": %u, \"pos\": [%.4f, %.4f, %.4f], \"ang\": [%.2f, %.2f, %.2f], "
                    "\"local\": %s, \"health\": null}",
                    id0, pos[0], pos[1], pos[2], ang[0], ang[1], ang[2],
                    player == local_player ? "true" : "false");
}

static DWORD WINAPI telemetry_thread(LPVOID param)
{
    const char *path = (const char *)param;
    const char *hzs = getenv("RECOMP_FCI_TELEMETRY_HZ");
    long hz = hzs && *hzs ? atol(hzs) : 10;
    if (hz < 1)
        hz = 1;
    if (hz > 100)
        hz = 100;
    FILE *tele = fopen(path, "w");
    if (!tele)
        return 1;
    ULONGLONG start = GetTickCount64();
    uint32_t snap[TELEMETRY_MAX_ENTITIES];
    for (;;) {
        lock();
        memcpy(snap, entities, (size_t)n_entities * sizeof *snap);
        int n = n_entities;
        unlock();

        uint32_t actor = fci_trace_player();
        uint32_t local_player = actor ? MEM32(actor + FCI_ACTOR_ENTITY) : 0;
        if (local_player && MEM32(local_player) != FCI_TELEMETRY_PLAYER_VTABLE)
            local_player = 0; /* the actor's pointer is not a CPlayer: no owner marked */

        fprintf(tele, "{\"ms\": %llu, \"wall\": %llu, \"entities\": %d, \"tick\": %u, \"players\": [",
                (unsigned long long)(GetTickCount64() - start), (unsigned long long)wall_ms(), n,
                FCI_TELEMETRY_CLOCK ? MEM32(FCI_TELEMETRY_CLOCK) : 0);
        char field[160];
        int first = 1;
        for (int i = 0; i < n; i++) {
            int len = sample_entity(snap[i], local_player, field, sizeof field);
            if (len <= 0)
                continue;
            if (!first)
                fputc(',', tele);
            first = 0;
            fwrite(field, 1, (size_t)len, tele);
        }
        fprintf(tele, "]}\n");
        fflush(tele); /* a test run ends the game with a kill */
        Sleep(1000 / (unsigned)hz);
    }
    return 0;
}

void fci_telemetry_start(void)
{
    if (!FCI_TELEMETRY_ENTITY_VTABLE) {
        if (!not_mapped_logged) {
            fprintf(stderr, "[FCI] telemetry: not mapped for this title yet\n");
            not_mapped_logged = 1;
        }
        return;
    }
    const char *path = getenv("RECOMP_FCI_TELEMETRY");
    if (path && *path) {
        HANDLE h = CreateThread(NULL, 0, telemetry_thread, (LPVOID)path, 0, NULL);
        if (!h) {
            fprintf(stderr, "[FCI] telemetry: CreateThread failed (error %lu)\n", GetLastError());
            return;
        }
        CloseHandle(h);
    }
}
