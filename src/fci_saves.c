/* Story saves stay usable. A profile folder (UDATA\<title>\<profile>) holds two .fcs: a level file, named as
 * the level its header names, and a checkpoint of a level. Sign-in rejects the profile as damaged unless
 * there are exactly two and each one's level file is present. A save started from the menu deletes the older
 * .fcs first; checkpoints end newer than their level file, so after any checkpoint (ours from F5 too) that is
 * the level file, and the checkpoint left behind has lost its pair. The game's own choice is steered to the
 * checkpoint, and a profile already damaged that way is repaired at startup. Evidence: re/notes/SAVES.md. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "recomp_types.h"

extern const char *fci_save_dir;

/* the level a .fcs header names (length at +2, name at +6), lower case; 0 when it is not one */
static int fcs_level(const WCHAR *path, char *level, size_t cap)
{
    uint8_t head[6 + 64];
    DWORD got = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    const BOOL ok = ReadFile(h, head, sizeof head, &got, NULL);
    CloseHandle(h);
    const size_t n = head[2];
    if (!ok || got < 6 || n == 0 || n >= cap || 6 + n > got)
        return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = (char)head[6 + i];
        if (!(c == '_' || c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
              || (c >= 'A' && c <= 'Z')))
            return 0;
        level[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    level[n] = 0;
    return 1;
}

/* a file's stem, lower case */
static void stem_of(const WCHAR *name, char *out, size_t cap)
{
    size_t i = 0;
    for (; name[i] && name[i] != L'.' && i + 1 < cap; i++)
        out[i] = (char)(name[i] >= L'A' && name[i] <= L'Z' ? name[i] + 32 : name[i]);
    out[i] = 0;
}

struct fcs {
    WCHAR path[MAX_PATH], name[MAX_PATH];
    char stem[64], level[64];
    FILETIME written;
};

/* the .fcs in a profile folder, up to max; how many there are */
static int list_fcs(const WCHAR *dir, struct fcs *out, int max)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    int n = 0;
    if (_snwprintf(pattern, MAX_PATH, L"%ls\\*.fcs", dir) < 0)
        return -1;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (n < max) {
            struct fcs *e = &out[n];
            _snwprintf(e->path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            wcsncpy(e->name, fd.cFileName, MAX_PATH - 1);
            e->name[MAX_PATH - 1] = 0;
            stem_of(fd.cFileName, e->stem, sizeof e->stem);
            if (!fcs_level(e->path, e->level, sizeof e->level))
                e->level[0] = 0;
            e->written = fd.ftLastWriteTime;
        }
        n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

static int is_level_file(const struct fcs *e) { return e->level[0] && !strcmp(e->stem, e->level); }

/* The save writer's room-maker: when the new file is not there yet and two .fcs are, it deletes the older,
 * unless that one is named as the game object's level string (the level being played; read in the lift). From
 * the menu no level is loaded, so after a checkpoint the older file is the level file and its checkpoint is
 * orphaned. When the two are a level file and its own checkpoint and the level file is older and unprotected,
 * the level file's time is set just past the checkpoint's, so the game's rule deletes the checkpoint.
 * Instincts sub_00056320: esi the profile folder, [esp+4] the object, [esp+8] the name; level string +0x2A4.
 * Evolution sub_00042BA9: [esp+4] the object, [esp+8] the folder, [esp+0xC] the name; level string +0x2BC. */
__attribute__((weak)) void sub_00056320_fci_body(void);
__attribute__((weak)) void sub_00042BA9_fci_body(void);
static int keep_level_file(uint32_t folder_va, uint32_t object, uint32_t name_va, uint32_t level_at);
int fci_gate_00056320(void)
{
    if (!sub_00056320_fci_body)
        return 0;
    return keep_level_file(g_esi, MEM32(g_esp + 4), MEM32(g_esp + 8), 0x2A4);
}
int fci_gate_00042BA9(void)
{
    if (!sub_00042BA9_fci_body)
        return 0;
    return keep_level_file(MEM32(g_esp + 8), MEM32(g_esp + 4), MEM32(g_esp + 0xC), 0x2BC);
}
static int guest_string(uint32_t va, char *out, size_t cap)
{
    size_t i = 0;
    if (!va)
        return 0;
    for (; i + 1 < cap; i++) {
        const char c = (char)MEM8(va + i);
        if (!c)
            break;
        out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    out[i] = 0;
    return i > 0 && i + 1 < cap;
}
static int keep_level_file(uint32_t folder_va, uint32_t object, uint32_t name_va, uint32_t level_at)
{
    if (!fci_save_dir)
        return 0;
    char folder[MAX_PATH], name[64], protect[64] = "";
    if (!guest_string(folder_va, folder, sizeof folder) || !guest_string(name_va, name, sizeof name))
        return 0;
    if (object && MEM32(object + level_at + 0x14) < sizeof protect) /* the protected name, a std::string */
        guest_string(MEM32(object + level_at + 0x18) >= 16 ? MEM32(object + level_at + 4)
                                                           : object + level_at + 4,
                     protect, sizeof protect);
    /* the profile: the folder's last part, under UDATA\<title> */
    size_t n = strlen(folder);
    while (n && (folder[n - 1] == '\\' || folder[n - 1] == '/'))
        folder[--n] = 0;
    const char *profile = strrchr(folder, '\\');
    profile = profile ? profile + 1 : folder;
    WCHAR pattern[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAW t;
    _snwprintf(pattern, MAX_PATH, L"%hs\\UDATA\\*", fci_save_dir);
    HANDLE ft = FindFirstFileW(pattern, &t);
    if (ft == INVALID_HANDLE_VALUE)
        return 0;
    dir[0] = 0;
    do {
        WCHAR probe[MAX_PATH];
        _snwprintf(probe, MAX_PATH, L"%hs\\UDATA\\%ls\\%hs", fci_save_dir, t.cFileName, profile);
        if (t.cFileName[0] != L'.' && (GetFileAttributesW(probe) & FILE_ATTRIBUTE_DIRECTORY)
            && GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
            wcsncpy(dir, probe, MAX_PATH - 1);
            dir[MAX_PATH - 1] = 0;
            break;
        }
    } while (FindNextFileW(ft, &t));
    FindClose(ft);
    if (!dir[0])
        return 0;
    struct fcs f[3];
    if (list_fcs(dir, f, 3) != 2)
        return 0;
    for (int i = 0; i < 2; i++)
        if (!strcmp(f[i].stem, name))
            return 0; /* rewritten in place: nothing is deleted */
    const int l = is_level_file(&f[0]) ? 0 : is_level_file(&f[1]) ? 1 : -1;
    if (l < 0 || is_level_file(&f[1 - l]) || strcmp(f[1 - l].level, f[l].level)
        || !strcmp(f[l].stem, protect))
        return 0;
    if (CompareFileTime(&f[l].written, &f[1 - l].written) > 0)
        return 0; /* the checkpoint is older already: the game deletes it */
    ULARGE_INTEGER later
            = {.LowPart = f[1 - l].written.dwLowDateTime, .HighPart = f[1 - l].written.dwHighDateTime};
    later.QuadPart += 10000000ull; /* a second past the checkpoint */
    const FILETIME ft2 = {later.LowPart, later.HighPart};
    HANDLE h = CreateFileW(f[l].path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE || !SetFileTime(h, NULL, NULL, &ft2)) {
        fprintf(stderr,
                "[FCI] saves: could not keep %ls before saving %s (%lu); its checkpoint %ls may be left "
                "without it\n",
                f[l].name, name, GetLastError(), f[1 - l].name);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    fprintf(stderr,
            "[FCI] saves: saving %s with no level loaded: %ls kept, its checkpoint %ls goes instead\n", name,
            f[l].name, f[1 - l].name);
    return 0;
}

/* A profile damaged the known way (two .fcs: a level file and a checkpoint of a level whose file is gone)
 * gets the checkpoint moved to save-backups and a copy of the level file in its place, a little older so
 * Story Resume still loads the level file. */
static void repair_profile(const WCHAR *dir, const WCHAR *title, const WCHAR *profile)
{
    struct fcs f[3];
    const int n = list_fcs(dir, f, 3);
    if (n != 2)
        return;
    for (int i = 0; i < 2; i++) {
        const struct fcs *level = &f[i], *orphan = &f[1 - i];
        if (!is_level_file(level) || is_level_file(orphan) || !orphan->level[0]
            || !strcmp(orphan->level, level->level))
            continue;
        WCHAR want[MAX_PATH];
        _snwprintf(want, MAX_PATH, L"%ls\\%hs.fcs", dir, orphan->level);
        if (GetFileAttributesW(want) != INVALID_FILE_ATTRIBUTES)
            return;
        SYSTEMTIME now;
        GetLocalTime(&now);
        WCHAR backup[MAX_PATH], moved[MAX_PATH], copy[MAX_PATH];
        _snwprintf(backup, MAX_PATH, L"%hs\\save-backups\\%ls-%ls-%04u%02u%02u-%02u%02u%02u", fci_save_dir,
                   title, profile, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        _snwprintf(moved, MAX_PATH, L"%ls\\%ls", backup, orphan->name);
        _snwprintf(copy, MAX_PATH, L"%ls\\Backup_%hs.fcs", dir, level->level);
        WCHAR parent[MAX_PATH];
        _snwprintf(parent, MAX_PATH, L"%hs\\save-backups", fci_save_dir);
        CreateDirectoryW(parent, NULL);
        if (!CreateDirectoryW(backup, NULL) || !MoveFileW(orphan->path, moved)) {
            fprintf(stderr,
                    "[FCI] saves: profile %ls is damaged (checkpoint %ls without %hs.fcs); the repair "
                    "could not move it aside (%lu)\n",
                    profile, orphan->name, orphan->level, GetLastError());
            return;
        }
        if (!CopyFileW(level->path, copy, TRUE)) {
            fprintf(stderr,
                    "[FCI] saves: profile %ls: moved %ls to %ls but could not copy the level file (%lu)\n",
                    profile, orphan->name, backup, GetLastError());
            return;
        }
        ULARGE_INTEGER t
                = {.LowPart = level->written.dwLowDateTime, .HighPart = level->written.dwHighDateTime};
        t.QuadPart -= 10000000ull * 60; /* a minute older than the level file */
        const FILETIME older = {t.LowPart, t.HighPart};
        HANDLE h = CreateFileW(copy, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, NULL, NULL, &older);
            CloseHandle(h);
        }
        fprintf(stderr,
                "[FCI] saves: profile %ls repaired: its checkpoint %ls had lost %hs.fcs; moved to %ls, "
                "%ls kept with a copy beside it\n",
                profile, orphan->name, orphan->level, backup, level->name);
        return;
    }
}

/* every profile folder of every title under UDATA, before the game starts */
void fci_saves_repair(void)
{
    if (!fci_save_dir)
        return;
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW t, p;
    _snwprintf(pattern, MAX_PATH, L"%hs\\UDATA\\*", fci_save_dir);
    HANDLE ft = FindFirstFileW(pattern, &t);
    if (ft == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(t.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || t.cFileName[0] == L'.')
            continue;
        WCHAR inner[MAX_PATH];
        _snwprintf(inner, MAX_PATH, L"%hs\\UDATA\\%ls\\*", fci_save_dir, t.cFileName);
        HANDLE fp = FindFirstFileW(inner, &p);
        if (fp == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(p.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || p.cFileName[0] == L'.')
                continue;
            WCHAR dir[MAX_PATH];
            _snwprintf(dir, MAX_PATH, L"%hs\\UDATA\\%ls\\%ls", fci_save_dir, t.cFileName, p.cFileName);
            repair_profile(dir, t.cFileName, p.cFileName);
        } while (FindNextFileW(fp, &p));
        FindClose(fp);
    } while (FindNextFileW(ft, &t));
    FindClose(ft);
}
