/* Raw hard disk partitions: <save>\PartitionN.img starts empty and grows as the game writes; a partition
 * has its table length and no end of file, so never-written sectors read as zeros. Also movie skipping. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "recomp_types.h"

static const ULONGLONG part_sectors[6] = {
        0, 0x00465400ull, 0x000FA000ull, 0x00177000ull, 0x00177000ull, 0x00177000ull,
};

/* FileNameInfo needs no rights on the parent directories, unlike GetFinalPathNameByHandleW */
static int partition_of(HANDLE h)
{
    union {
        FILE_NAME_INFO info;
        uint8_t raw[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(wchar_t)];
    } u;
    wchar_t path[MAX_PATH];
    size_t chars;
    const wchar_t *name;
    if (!h || h == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileInformationByHandleEx(h, FileNameInfo, &u, sizeof u))
        return -1;
    chars = u.info.FileNameLength / sizeof(wchar_t);
    if (chars >= MAX_PATH)
        return -1;
    memcpy(path, u.info.FileName, chars * sizeof(wchar_t));
    path[chars] = 0;
    if (!(name = wcsrchr(path, L'\\')))
        return -1;
    if (_wcsnicmp(name, L"\\Partition", 10) || !iswdigit(name[10]) || _wcsicmp(name + 11, L".img"))
        return -1;
    return name[10] - L'0';
}

int fci_partition_length(HANDLE h, LARGE_INTEGER *length)
{
    int p = partition_of(h);
    if (p < 1 || p > 5)
        return 0;
    length->QuadPart = (LONGLONG)(part_sectors[p] * 512ull);
    return 1;
}

uint32_t fci_partition_read_fill(HANDLE h, void *buf, uint32_t length, const LARGE_INTEGER *off,
                                 uint32_t status, uint32_t *got)
{
    int p;
    ULONGLONG end;
    if (*got >= length || (status != 0 && status != 0xC0000011u /* END_OF_FILE */) || !off)
        return status;
    if ((p = partition_of(h)) < 1 || p > 5)
        return status;
    end = (ULONGLONG)off->QuadPart + length;
    if (off->QuadPart < 0 || end > part_sectors[p] * 512ull)
        return status;
    memset((uint8_t *)buf + *got, 0, length - *got);
    *got = length;
    return 0;
}

/* FSCTL_DISMOUNT_VOLUME on a raw partition succeeds (no mounted state is kept); any other FSCTL is left
 * to the runtime, which fails it */
static int s_utility_partition; /* last dismounted cache partition (3..5) */
static wchar_t s_save_dir[MAX_PATH];

void fci_disk_init(const char *save_dir)
{ MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH); }

int fci_partition_fsctl(HANDLE h, uint32_t fsctl, uint32_t *status)
{
    int p = partition_of(h);
    if (p < 1 || p > 5 || fsctl != 0x00090020u)
        return 0;
    if (p >= 3)
        s_utility_partition = p;
    fprintf(stderr, "  [FCI] FSCTL_DISMOUNT_VOLUME on partition %d\n", p);
    *status = 0;
    return 1;
}

/* FileFsSizeInformation for the utility volume, from the FATX superblock XAPI wrote: it checks the cluster
 * size it formatted (004AC838). Free space counts the host directory's files (approximate). */
static int is_cache_dir(HANDLE h)
{
    union {
        FILE_NAME_INFO info;
        uint8_t raw[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(wchar_t)];
    } u;
    size_t chars;
    if (!GetFileInformationByHandleEx(h, FileNameInfo, &u, sizeof u))
        return 0;
    chars = u.info.FileNameLength / sizeof(wchar_t);
    return chars >= 6 && !_wcsnicmp(u.info.FileName + chars - 6, L"\\Cache", 6);
}

static ULONGLONG used_clusters(const wchar_t *dir, ULONGLONG cluster, int depth)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    ULONGLONG n = 0;
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    if ((f = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub[MAX_PATH];
            swprintf_s(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            n += 1 + (depth < 8 ? used_clusters(sub, cluster, depth + 1) : 0);
        } else {
            ULONGLONG bytes = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            n += (bytes + cluster - 1) / cluster;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

int fci_volume_size(HANDLE h, void *out, uint32_t length, uint32_t cls, uint32_t *status, uint32_t *written)
{
    wchar_t image[MAX_PATH], dir[MAX_PATH];
    uint8_t super[12];
    DWORD got = 0;
    HANDLE f;
    uint32_t spc;
    ULONGLONG bytes, cluster, clusters, fat, total, used;
    uint8_t *o = (uint8_t *)out;

    if (cls != 3 || !s_utility_partition || !s_save_dir[0] || !h || h == INVALID_HANDLE_VALUE
        || !is_cache_dir(h))
        return 0;
    swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img", s_save_dir, s_utility_partition);
    f = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(f, super, sizeof super, &got, NULL);
    CloseHandle(f);
    memcpy(&spc, super + 8, 4);
    if (got != sizeof super || memcmp(super, "FATX", 4) || !spc || (spc & (spc - 1)) || spc > 1024) {
        fprintf(stderr, "  [FCI] utility partition %d has no FATX superblock\n", s_utility_partition);
        return 0;
    }
    if (length < 24) {
        *status = 0xC0000023u;
        *written = 0; /* BUFFER_TOO_SMALL */
        return 1;
    }
    bytes = part_sectors[s_utility_partition] * 512ull;
    cluster = (ULONGLONG)spc * 512u;
    clusters = bytes / cluster;
    fat = (clusters < 0xFFF0u ? 2 : 4) * clusters;
    fat = (fat + 4095) & ~4095ull;
    total = (bytes - 4096 - fat) / cluster;
    swprintf_s(dir, MAX_PATH, L"%s\\Cache", s_save_dir);
    used = 1 + used_clusters(dir, cluster, 0);
    *(ULONGLONG *)(o + 0) = total;
    *(ULONGLONG *)(o + 8) = used < total ? total - used : 0;
    *(uint32_t *)(o + 16) = spc;
    *(uint32_t *)(o + 20) = 512;
    *status = 0;
    *written = 24;
    fprintf(stderr, "  [FCI] utility volume P%d: %u-sector clusters, %llu total, %llu free\n",
            s_utility_partition, spc, total, total - (used < total ? used : total));
    return 1;
}

/* Skipped movies return from the game's movie player (00220200; Evolution's 00196D41) as if played:
 * RECOMP_FCI_SKIP_INTROS=1 (startup logos and intro, and Evolution's attract video, which also plays on every
 * return to the main menu), RECOMP_FCI_SKIP_MOVIES=<name>[,...] (re/notes/MOVIES.md) */
static int movie_listed(const char *list, const char *name, size_t len)
{
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        if (n == len && !_strnicmp(list, name, len))
            return 1;
        list = end ? end + 1 : NULL;
    }
    return 0;
}

/* Evolution's other video player (0019674C: PREY.bik and videos named by data), cdecl, the path first:
 * logged and counted like Movie_Play's (re/notes/MOVIES.md) */
void fci_prehook_0019674C(void)
{
    extern void fci_status_movie(const char *path); /* RECOMP_FCI_STATUS (fci_threads.c) */
    char path[260];
    uint32_t arg = MEM32(g_esp + 4), i;
    for (i = 0; arg && i < sizeof path - 1 && MEM8(arg + i); i++)
        path[i] = (char)MEM8(arg + i);
    path[i] = 0;
    fci_status_movie(path);
    fprintf(stderr, "  [FCI] video opened: %s\n", path);
}

int fci_gate_00220200(void)
{
    static const char intros[] = "Ubisoft,Crytek,Intro_FCI,Attract_Mode";
    static int skip_intros = -1;
    const char *custom = getenv("RECOMP_FCI_SKIP_MOVIES"), *name, *dot;
    char path[260];
    uint32_t arg, i;
    if (skip_intros < 0) {
        const char *e = getenv("RECOMP_FCI_SKIP_INTROS");
        skip_intros = e && *e == '1';
    }
    arg = MEM32(g_esp + 4);
    for (i = 0; arg && i < sizeof path - 1 && MEM8(arg + i); i++)
        path[i] = (char)MEM8(arg + i);
    path[i] = 0;
    {
        extern void fci_status_movie(const char *path); /* RECOMP_FCI_STATUS (fci_threads.c) */
        fci_status_movie(path);
    }
    fprintf(stderr, "  [FCI] movie: %s\n", path);
    if (!skip_intros && !custom)
        return 0;
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    dot = strrchr(name, '.');
    if (!dot)
        dot = name + strlen(name);
    if (!(skip_intros && movie_listed(intros, name, (size_t)(dot - name)))
        && !movie_listed(custom, name, (size_t)(dot - name)))
        return 0;
    fprintf(stderr, "  [FCI] movie skipped: %s\n", path);
    g_eax = 0;
    g_esp += 4; /* ret: the caller pops the arguments */
    return 1;
}
