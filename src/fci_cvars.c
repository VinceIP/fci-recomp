/* Engine console variables: prehooks on Console_RegisterFloat/Int record each variable's storage and
 * replace its default with RECOMP_CVAR_<name>=<value> (RECOMP_CVAR_LIST=1 logs them; re/notes/CVARS.md). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"

#define CVAR_MAX  512
#define CVAR_NAME 64

static struct {
    char name[CVAR_NAME];
    uint32_t storage; /* guest address of the int or float */
    int is_float;
} cvars[CVAR_MAX];
static volatile LONG cvar_count;
static SRWLOCK cvar_lock = SRWLOCK_INIT;

static void guest_string(uint32_t va, char *out, size_t size)
{
    size_t i;
    for (i = 0; va && i + 1 < size && MEM8(va + i); i++)
        out[i] = (char)MEM8(va + i);
    out[i] = 0;
}

static void store(uint32_t storage, int is_float, double value)
{
    if (is_float)
        MEMF(storage) = (float)value;
    else
        MEM32(storage) = (uint32_t)(int32_t)(value < 0 ? value - 0.5 : value + 0.5);
}

/* the shared part of both prehooks: [esp+4] name, [esp+8] storage, [esp+0xC] float default */
static void registered(int is_float)
{
    char name[CVAR_NAME], key[CVAR_NAME + 16];
    const char *value;
    uint32_t storage = MEM32(g_esp + 8);
    float def;
    LONG i;
    guest_string(MEM32(g_esp + 4), name, sizeof name);
    if (!name[0] || !storage)
        return;
    {
        uint32_t bits = MEM32(g_esp + 0xC);
        memcpy(&def, &bits, 4);
    }
    AcquireSRWLockExclusive(&cvar_lock);
    for (i = 0; i < cvar_count && strcmp(cvars[i].name, name); i++)
        ;
    if (i == cvar_count && i < CVAR_MAX) {
        memcpy(cvars[i].name, name, sizeof name);
        cvar_count++;
    }
    if (i < CVAR_MAX) {
        cvars[i].storage = storage;
        cvars[i].is_float = is_float;
    }
    ReleaseSRWLockExclusive(&cvar_lock);
    snprintf(key, sizeof key, "RECOMP_CVAR_%s", name);
    value = getenv(key);
    if (value && *value) {
        float v = (float)atof(value);
        uint32_t bits;
        memcpy(&bits, &v, 4);
        MEM32(g_esp + 0xC) = bits; /* the default the console creates it with */
        fprintf(stderr, "[FCI] cvar %s = %s (default %g)\n", name, value, def);
    } else {
        const char *list = getenv("RECOMP_CVAR_LIST");
        if (list && *list == '1')
            fprintf(stderr, "[FCI] cvar %s %s default %g at %08X\n", name, is_float ? "float" : "int", def,
                    storage);
    }
}

/* Console_CreateVariable (thiscall; [esp+4] name, [esp+8] default string, flags, help): the variable keeps
 * its own value, so RECOMP_CVAR_<name> replaces the default string with a copy in guest memory */
void fci_prehook_001FD9E0(void)
{
    extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
    char name[CVAR_NAME], def[64], key[CVAR_NAME + 16];
    const char *value;
    guest_string(MEM32(g_esp + 4), name, sizeof name);
    if (!name[0])
        return;
    guest_string(MEM32(g_esp + 8), def, sizeof def);
    snprintf(key, sizeof key, "RECOMP_CVAR_%s", name);
    value = getenv(key);
    if (value && *value) {
        size_t len = strlen(value), i;
        uint32_t copy = xbox_HeapAlloc((uint32_t)len + 1, 4);
        if (!copy)
            return;
        for (i = 0; i <= len; i++)
            MEM8(copy + i) = (uint8_t)value[i];
        MEM32(g_esp + 8) = copy;
        fprintf(stderr, "[FCI] cvar %s = %s (default \"%s\")\n", name, value, def);
    } else {
        const char *list = getenv("RECOMP_CVAR_LIST");
        if (list && *list == '1')
            fprintf(stderr, "[FCI] cvar %s string default \"%s\"\n", name, def);
    }
}

void fci_prehook_001FDCD0(void) { registered(1); } /* Console_RegisterFloat */
void fci_prehook_001FDED0(void) { registered(0); } /* Console_RegisterInt */
/* the map editor (ige.xbe, its image profile): the same functions built as thiscall (ecx = console, the
 * float returned on the x87 stack), the same arguments on the stack */
void fci_prehook_0006A1D0(void) { registered(1); } /* Console_RegisterFloat (ige.xbe) */
void fci_prehook_0006A380(void) { registered(0); } /* Console_RegisterInt (ige.xbe) */

/* "RECOMP_CVAR_<name>=<value>": kept for variables registered later, written now to a registered one.
 * 0 when the line is not a cvar setting. */
int fci_cvar_set(const char *key_value)
{
    char name[CVAR_NAME];
    const char *eq = strchr(key_value, '=');
    size_t len;
    LONG i;
    if (strncmp(key_value, "RECOMP_CVAR_", 12) || !eq)
        return 0;
    len = (size_t)(eq - key_value - 12);
    if (!len || len >= sizeof name)
        return 0;
    memcpy(name, key_value + 12, len);
    name[len] = 0;
    _putenv(key_value);
    AcquireSRWLockShared(&cvar_lock);
    for (i = 0; i < cvar_count && strcmp(cvars[i].name, name); i++)
        ;
    if (i < cvar_count && eq[1]) {
        store(cvars[i].storage, cvars[i].is_float, atof(eq + 1));
        fprintf(stderr, "[FCI] cvar %s = %s (set)\n", name, eq + 1);
    } else {
        fprintf(stderr, "[FCI] cvar %s = %s (kept for its registration)\n", name, eq + 1);
    }
    ReleaseSRWLockShared(&cvar_lock);
    return 1;
}
