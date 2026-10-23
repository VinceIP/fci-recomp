/* Visible stops: what the toolkit would log and run past ends the process with the guest address and
 * a distinct exit code, plus the game's relaunches into itself. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <string.h>

#include "recomp_types.h"
#include "fci_launch.h"
#include "fci_image.h"

void fci_log_session_end(void); /* fci_config.c: a clean exit, for the log */

/* audio buffers XAudio2 refused because its queue was full */
int fci_apu_drops;
/* frames in which an active voice could not supply all its samples */
int fci_apu_underruns;
/* XAudio2 submissions that found its queue empty */
int fci_apu_starved;

/* The game rebooting into itself or the map editor with a launch data page: the page goes to a file and
 * the target's executable starts with RECOMP_LAUNCH_DATA. Returns only on failure. */
extern int fci_default_launch; /* fci_main.c */
extern const char *fci_xbe_path, *fci_game_dir, *fci_save_dir;
extern int fci_launch_argc;
extern char **fci_launch_argv;
unsigned long fci_window_handoff(void **frame); /* fci_xbr_glue.cpp */
void fci_window_pass_on(void);                  /* fci_xbr_glue.cpp */
int fci_host_symbol(DWORD64 pc, char *name, size_t size, DWORD64 *disp,
                    DWORD64 *symbol_size); /* fci_threads.c */

uint32_t fci_launch_page_hash(uint32_t page)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < 0x1000; i++)
        hash = (hash ^ MEM8(page + i)) * 16777619u;
    return hash;
}

/* The first instance after it rebooted: wait until the job is empty, keeping the last exit code. */
/* the next instance hides this frame once its own is up (fci_xbr_glue.cpp); a minute, or its end, also do:
 * until then the session keeps a window */
static void wait_for_next_frame(HWND frame, HANDLE next)
{
    int i;
    for (i = 0; i < 6000 && frame && IsWindowVisible(frame); i++)
        if (WaitForSingleObject(next, 10) != WAIT_TIMEOUT)
            break;
}

static void wait_for_chain(HANDLE job, HANDLE port, HANDLE first)
{
    struct {
        DWORD pid;
        HANDLE h;
    } procs[64];
    int n = 0, i;
    DWORD code = 0, msg;
    ULONG_PTR key;
    LPOVERLAPPED ov;
    procs[n].pid = GetProcessId(first);
    procs[n++].h = first;
    for (;;) {
        if (!GetQueuedCompletionStatus(port, &msg, &key, &ov, INFINITE))
            break;
        DWORD pid = (DWORD)(uintptr_t)ov;
        if (msg == JOB_OBJECT_MSG_NEW_PROCESS && n < 64) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (h) {
                procs[n].pid = pid;
                procs[n++].h = h;
            }
        } else if (msg == JOB_OBJECT_MSG_EXIT_PROCESS || msg == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) {
            for (i = 0; i < n; i++)
                if (procs[i].pid == pid)
                    GetExitCodeProcess(procs[i].h, &code);
        } else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
            break;
        }
    }
    (void)job;
    ExitProcess(code);
}

/* A message over the game, owned by its frame so it shows in front even when fullscreen. The caller holds
 * the guest CPU, so the game's threads wait; the audio output plays silence until it is answered. */
void fci_notice(const char *text)
{
    extern void *fci_frame_window(unsigned long *thread); /* xbr/fci_xbr_glue.cpp */
    extern void fci_audio_hold(int on);                   /* xbr/audio/xaudio2.cpp */
    HWND frame = (HWND)fci_frame_window(NULL);

    fci_audio_hold(1);
    MessageBoxA(frame && IsWindow(frame) ? frame : NULL, text, FCI_TITLE_NAME " (recompiled)",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    fci_audio_hold(0);
}

void fci_relaunch(uint32_t page)
{
    char exe[MAX_PATH], image[MAX_PATH], file[MAX_PATH], cmd[32768], path[256];
    const char *target;
    size_t used = 0;
    char *slash;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE job = NULL, port = NULL, snap;
    FILE *f;
    DWORD self = GetCurrentProcessId(), me = GetCurrentThreadId(), keep;
    void *frame = NULL;
    int in_chain = getenv("RECOMP_FCI_CHAIN") != NULL;
    uint32_t i;

    for (i = 0; i < sizeof(path) - 1 && MEM8(page + 8 + i); i++)
        path[i] = (char)MEM8(page + 8 + i);
    path[i] = 0;
    target = fci_launch_target(path);
    if (!target) {
        fprintf(stderr, "[FCI] unsupported launch target %s\n", path);
        return;
    }
    if (!GetModuleFileNameA(NULL, exe, sizeof exe) || !(slash = strrchr(exe, '\\')))
        return;
    /* the same image again (a level load, back to the menu) is this executable, whatever its name; the
     * other image is the title's other executable beside it */
    if (strcmp(target, FCI_IMAGE_FILENAME))
        snprintf(slash + 1, sizeof(exe) - (size_t)(slash + 1 - exe), "%s",
                 !strcmp(target, "ige.xbe") ? FCI_EDITOR_EXE_NAME : FCI_GAME_EXE_NAME);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        /* the map editor is built separately (BuildEditor.cmd); without it the game cannot go on */
        fprintf(stderr, "[FCI] relaunch: %s is not installed\n", exe);
        fci_notice(!strcmp(target, "ige.xbe")
                           ? "The map editor is not built yet. Run BuildEditor.cmd in the installation "
                             "folder, then choose Map Editor again.\n\nThe game will close now."
                           : FCI_GAME_EXE_NAME " is missing from the installation folder.");
        fci_log_session_end();
        ExitProcess(0);
    }
    snprintf(image, sizeof image, "%s\\%s", fci_game_dir, target);
    /* the save folder is the instance's own writable place; the current directory may not be */
    snprintf(file, sizeof file, "%s\\fci-launch-data.bin", fci_save_dir);
    if (!fci_quote_arg(cmd, sizeof cmd, &used, exe) || !fci_quote_arg(cmd, sizeof cmd, &used, image)
        || !fci_quote_arg(cmd, sizeof cmd, &used, fci_game_dir)
        || !fci_quote_arg(cmd, sizeof cmd, &used, fci_save_dir))
        return;
    for (int arg = 4; arg < fci_launch_argc; arg++)
        if (!fci_quote_arg(cmd, sizeof cmd, &used, fci_launch_argv[arg]))
            return;
    fprintf(stderr, "[FCI] launch page type=%u title=%08X hash=%08X\n", MEM32(page), MEM32(page + 4),
            fci_launch_page_hash(page));
    f = fopen(file, "wb");
    if (!f) {
        fprintf(stderr, "[FCI] relaunch: cannot write %s\n", file);
        return;
    }
    for (i = 0; i < 0x1000; i++)
        fputc(MEM8(page + i), f);
    fclose(f);
    { /* RECOMP_FCI_LAUNCH_DUMP: keep a copy of the page before XGetLaunchInfo consumes it */
        const char *dump = getenv("RECOMP_FCI_LAUNCH_DUMP");
        if (dump && *dump) {
            char saved[MAX_PATH];
            snprintf(saved, sizeof saved, "%s.%s.bin", dump, target);
            if (!CopyFileA(file, saved, FALSE))
                fprintf(stderr, "[FCI] launch dump failed %lu\n", GetLastError());
        }
    }
    SetEnvironmentVariableA("RECOMP_LAUNCH_DATA", file);
    /* test input (RECOMP_PAD_LIVE): the next instance only takes lines written after this point */
    SetEnvironmentVariableA("RECOMP_PAD_LIVE_START", "end");
    if (!in_chain) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc;
        job = CreateJobObjectA(NULL, NULL);
        port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        if (!job || !port) {
            fprintf(stderr, "[FCI] relaunch: no job object (%lu)\n", GetLastError());
            return;
        }
        memset(&limit, 0, sizeof limit);
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
        assoc.CompletionKey = job;
        assoc.CompletionPort = port;
        SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &assoc, sizeof assoc);
        SetEnvironmentVariableA("RECOMP_FCI_CHAIN", "1");
    }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* started from a console or with redirected output: the new instance writes there too; started
     * without arguments (fci_main.c), it appends to the log file itself */
    si.dwFlags = fci_default_launch ? 0 : STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fprintf(stderr, "[FCI] relaunch: %s -> %s with %s\n", path, exe, file);
    fflush(stderr);
    fci_window_pass_on(); /* the next instance's frame takes this one's place */
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[FCI] relaunch: CreateProcess failed (%lu)\n", GetLastError());
        return;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        fprintf(stderr, "[FCI] relaunch: job assignment failed (%lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return;
    }
    /* the window first: under the presenter's lock, so no frame is half presented when it goes */
    keep = fci_window_handoff(&frame);
    AllowSetForegroundWindow(pi.dwProcessId); /* its frame comes to the front in this one's place */
    {
        HWND w = NULL;
        while ((w = FindWindowExA(NULL, w, NULL, NULL)) != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == self && w != (HWND)frame)
                ShowWindow(w, SW_HIDE);
        }
    }
    /* no thread of this instance runs on but the frame's */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te))
            do {
                if (te.th32OwnerProcessID == self && te.th32ThreadID != me && te.th32ThreadID != keep) {
                    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (t) {
                        SuspendThread(t);
                        CloseHandle(t);
                    }
                }
            } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    ResumeThread(pi.hThread);
    if (in_chain) { /* the first instance waits for the job; this one only keeps its frame up meanwhile */
        wait_for_next_frame((HWND)frame, pi.hProcess);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    wait_for_chain(job, port, pi.hProcess);
}

/* the host call chain: which lifted functions are really running (guest stack words can be stale);
 * printed at stops and at HalReturnToFirmware */
void fci_host_chain(const char *label)
{
    void *frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, NULL);
    USHORT k;
    fprintf(stderr, "%s:", label);
    for (k = 0; k < n; k++) {
        char name[256];
        if (fci_host_symbol((DWORD64)(uintptr_t)frames[k], name, sizeof name, NULL, NULL))
            fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void stop(int code, const char *what, uint32_t va, const char *text)
{
    uint32_t i;
    {
        extern const char *fci_re_name(uint32_t va); /* re_names.c */
        const char *named = fci_re_name(va);
        fprintf(stderr, "[FCI] STOP %d %s va=%08X%s%s%s%s%s\n", code, what, va, named ? " (" : "",
                named ? named : "", named ? ")" : "", text ? " insn=" : "", text ? text : "");
    }
    fprintf(stderr, "  guest eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, g_ebp);
    fprintf(stderr, "  icall count %llu recent:", (unsigned long long)g_icall_count);
    for (i = 0; i < ICALL_TRACE_SIZE; i++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & (ICALL_TRACE_SIZE - 1)]);
    fprintf(stderr, "\n");
    if (g_xbox_mem_offset && g_esp >= 0x1000 && g_esp < 0x7FFF0000u) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0;
        fprintf(stderr, "  guest stack code words:");
        for (i = 0; i < 512 && shown < 32; i++) {
            if (sp[i] >= g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                fprintf(stderr, " [%u]%08X", i * 4, sp[i]);
                shown++;
            }
        }
        fprintf(stderr, "\n");
    }
    fci_host_chain("  host chain");
    {
        extern void fci_call_profile_dump(const char *why);
        fci_call_profile_dump("stop");
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

/* the map editor's supplemental entries (lifted from its measured entry list), else nothing */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
#if FCI_HAS_SUPPLEMENT
    extern recomp_func_t fci_supp_lookup(uint32_t va);
    return fci_supp_lookup(xbox_va);
#else
    (void)xbox_va;
    return NULL;
#endif
}

void recomp_icall_fail_log(uint32_t va) { stop(81, "unresolved indirect call", va, NULL); }
void recomp_icall_not_code_log(uint32_t va) { stop(82, "indirect call to non-code", va, NULL); }
void recomp_unimpl(const char *text, uint32_t va) { stop(83, "untranslated instruction", va, text); }
void fci_unresolved_stub(uint32_t va) { stop(84, "undetected call target", va, NULL); }
void fci_quarantined_lift(uint32_t va) { stop(85, "quarantined lift", va, NULL); }
void fci_fell_off_end(uint32_t va) { stop(89, "control ran off the end of a lifted function", va, NULL); }

/* stops called from the runtime patches (patches/runtime) */
void fci_unbridged_ordinal(unsigned ordinal, int slot)
{
    char what[64];
    snprintf(what, sizeof what, "kernel ordinal %u has no bridge (slot %d)", ordinal, slot);
    stop(87, what, g_esp >= 4 ? MEM32(g_esp - 4) : 0, NULL); /* caller return address */
}

void fci_thread_stop(const char *why, uint32_t routine) { stop(88, why, routine, NULL); }

/* DirectSound's effects-image download (00580363) writes command 3 to the DSP doorbell at image + 0x810
 * (image = [[[this+8]+0x10]]) and spins until it clears: register that doorbell for acknowledgement */
void fci_prehook_00580363(void)
{
    extern void fci_apu_dsp_ack_add(uint32_t addr);
    uint32_t a = MEM32(g_ecx + 8), image;
    if (!a || !(a = MEM32(a + 0x10)) || !(image = MEM32(a)))
        return;
    fci_apu_dsp_ack_add(image + 0x810);
}

/* KeBugCheck(Ex): the game's own fatal error, which never returns; va is the caller */
void fci_bugcheck(uint32_t code, uint32_t caller, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4)
{
    char what[128];
    snprintf(what, sizeof what, "title KeBugCheck code %08X (%08X %08X %08X %08X)", code, p1, p2, p3, p4);
    stop(97, what, caller, NULL);
}

/* a wait on a guest dispatcher object the runtime does not model */
void fci_unmodeled_object(const char *service, uint32_t object)
{
    char what[96];
    snprintf(what, sizeof what, "%s on unmodeled dispatcher object (header %08X %08X)", service,
             MEM32(object), MEM32(object + 4));
    stop(91, what, object, NULL);
}
