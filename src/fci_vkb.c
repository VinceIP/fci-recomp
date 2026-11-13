/* Typing on the PC keyboard into the game's on-screen keyboard (profile names, cheat codes).
 * While it is open, keys type instead of pressing pad buttons: characters, Backspace deletes, Enter
 * accepts, Esc goes back. The window thread queues them; the game thread replays them each frame through
 * the keyboard's own functions, as its buttons would. Evidence: re/notes/KEYBOARD.md. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

/* Instincts; Evolution's are in its profile's constants */
#ifndef FCI_VKB_OBJECT
#define FCI_VKB_OBJECT 0x006B3250u /* the keyboard object's pointer: every open call (sub_0010B500) passes it */
#endif
#ifndef FCI_VKB_INSERT
#define FCI_VKB_INSERT 0x0010BFF0u /* insert a string at the cursor: eax keyboard, edi the string (X's space) */
#endif
#ifndef FCI_VKB_CHAR
#define FCI_VKB_CHAR 0u /* insert a string at the cursor: eax keyboard, the string on the stack (Evolution) */
#endif
#ifndef FCI_VKB_DELETE
#define FCI_VKB_DELETE 0x0010C0B0u /* Y: eax keyboard */
#endif
#ifndef FCI_VKB_A
#define FCI_VKB_A 0x0010BBD0u /* A: eax keyboard; accepts with the wheel on its first slot */
#endif
#ifndef FCI_VKB_BACK
#define FCI_VKB_BACK 0x0010B7D0u /* B: esi keyboard */
#endif

#ifndef FCI_VKB_MODAL
#define FCI_VKB_MODAL 0x20u /* in the UI input manager: a popup's input root, which then takes all pad input */
#endif

/* the keyboard object: the callback set while it is open, the letter wheel (its slot is +0x18 / +0x1C) */
#define VKB_CALLBACK 0x58u
#define VKB_WHEEL 0xACu
#define VKB_SIZE 0x114u

enum { EV_DELETE = 0x10000, EV_ACCEPT, EV_BACK };

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t va);
int guest_ok(uint32_t va, uint32_t len); /* fci_menu.c */
void fci_input_typed(void);              /* fci_input.c */

static volatile LONG s_open;   /* the keyboard is open and has focus (game thread writes, window thread reads) */
static volatile LONG s_opened; /* between the keyboard's open and close calls */
static uint32_t s_ui;          /* the UI input manager, as its router last ran */
static SRWLOCK s_lock = SRWLOCK_INIT;
static uint32_t s_queue[64];
static unsigned s_head, s_tail;

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_FCI_VKB_TYPING");
        on = !(e && *e == '0');
    }
    return on;
}

static void push(uint32_t ev)
{
    AcquireSRWLockExclusive(&s_lock);
    if (s_tail - s_head < sizeof s_queue / sizeof *s_queue)
        s_queue[s_tail++ % (sizeof s_queue / sizeof *s_queue)] = ev;
    ReleaseSRWLockExclusive(&s_lock);
}

static int pop(uint32_t *ev)
{
    int got = 0;
    AcquireSRWLockExclusive(&s_lock);
    if (s_head != s_tail) {
        *ev = s_queue[s_head++ % (sizeof s_queue / sizeof *s_queue)];
        got = 1;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return got;
}

/* the characters the on-screen keyboard's own pages offer (its font has them) */
static int typable(unsigned c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c && c < 128 && strchr(" @/*-+#_()!?;,:.$", (int)c));
}

/* window thread: a key down. 1 when the keyboard is open and the key is ours (it presses no pad button);
 * function keys stay with their own uses */
int fci_vkb_key(int vk)
{
    if (!s_open || (vk >= VK_F1 && vk <= VK_F24))
        return 0;
    if (vk == VK_BACK)
        push(EV_DELETE);
    else if (vk == VK_RETURN)
        push(EV_ACCEPT);
    else if (vk == VK_ESCAPE)
        push(EV_BACK);
    fci_input_typed(); /* the prompts switch to the keyboard's keys */
    return 1;
}

/* the keyboard takes typing now (the button prompts show the keys that press its buttons) */
int fci_vkb_typing(void)
{
    return s_open != 0;
}

/* window thread: a typed character (WM_CHAR) */
void fci_vkb_char(unsigned c)
{
    if (s_open && typable(c))
        push(c);
}

/* the call frame, below the caller's stack: [return address][argument], then room for a string at +0x10;
 * the callee's own stack grows down from it */
static uint32_t frame_at(void)
{
    return (g_esp - 64) & ~3u;
}

/* call a lifted function with the keyboard in a register and one stack argument; the caller's
 * registers are kept */
static void call(uint32_t va, uint32_t k, int reg_esi, uint32_t arg, uint32_t edi)
{
    recomp_func_t fn = va ? recomp_lookup(va) : NULL;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, sedi = g_edi, ebp = g_ebp, sp = g_esp;
    uint32_t frame = frame_at();
    if (!fn) {
        fprintf(stderr, "[FCI] keyboard: no function at %08X\n", va);
        fflush(stderr);
        return;
    }
    MEM32(frame) = 0; /* the return address: the lifted callee only pops it */
    MEM32(frame + 4) = arg;
    g_esp = frame;
    if (reg_esi)
        g_esi = k;
    else
        g_eax = k;
    g_edi = edi;
    fn();
    g_esp = sp;
    g_eax = eax, g_ecx = ecx, g_edx = edx, g_ebx = ebx, g_esi = esi, g_edi = sedi, g_ebp = ebp;
}

static void type_char(uint32_t k, uint32_t c)
{
    /* both insert a string, as X's space does: a one-character std::wstring (buffer inline below 8);
     * Evolution takes it on the stack, Instincts in edi */
    uint32_t s = frame_at() + 0x10;
    memset((void *)XBOX_PTR(s), 0, 0x1C);
    MEM16(s + 4) = (uint16_t)c;
    MEM32(s + 0x14) = 1; /* length */
    MEM32(s + 0x18) = 7; /* capacity: the inline buffer */
    if (FCI_VKB_CHAR)
        call(FCI_VKB_CHAR, k, 0, s, g_edi);
    else
        call(FCI_VKB_INSERT, k, 0, 0, s);
}

/* A with the wheel held on its first slot: the keyboard's own accept (an empty text gets its warning) */
static void accept(uint32_t k)
{
    uint32_t w = MEM32(k + VKB_WHEEL), slot;
    if (!guest_ok(w, 0x20))
        return;
    slot = MEM32(w + 0x18);
    MEM32(w + 0x18) = 0;
    call(FCI_VKB_A, k, 0, 0, g_edi);
    if (MEM32(k + VKB_CALLBACK)) /* still open: restore where the wheel was */
        MEM32(w + 0x18) = slot;
}

/* the keyboard's open and close (Evolution's alias these): before its first open the object holds
 * leftovers, so its callback alone does not say it is open */
void fci_prehook_0010B500(void)
{
    s_opened = 1;
}

void fci_prehook_0010B5E0(void)
{
    s_opened = 0;
}

/* the UI input manager's router, once a frame (ecx the manager; Evolution's aliases it): with a popup's root
 * set it gives the pad input to the popup alone */
void fci_prehook_000675B0(void)
{
    s_ui = g_ecx;
}

/* open, and no popup (the empty-name warning) over it */
static int has_focus(uint32_t k)
{
    uint32_t ui = s_ui;
    if (!s_opened || !guest_ok(k, VKB_SIZE) || !MEM32(k + VKB_CALLBACK))
        return 0;
    return !(guest_ok(ui, FCI_VKB_MODAL + 4) && MEM32(ui + FCI_VKB_MODAL));
}

/* once a frame, on the game thread (the pad poll) */
void fci_vkb_frame(void)
{
    static int said;
    uint32_t k, ev;
    if (!enabled())
        return;
    k = MEM32(FCI_VKB_OBJECT);
    s_open = has_focus(k);
    while (pop(&ev)) {
        if (!s_open) /* typed before it closed or a popup came up */
            continue;
        if (!said) {
            said = 1;
            fprintf(stderr, "[FCI] keyboard: typing into the on-screen keyboard\n");
            fflush(stderr);
        }
        if (ev == EV_DELETE)
            call(FCI_VKB_DELETE, k, 0, 0, g_edi);
        else if (ev == EV_ACCEPT)
            accept(k);
        else if (ev == EV_BACK)
            call(FCI_VKB_BACK, k, 1, 0, g_edi);
        else
            type_char(k, ev);
        s_open = has_focus(k);
    }
}
