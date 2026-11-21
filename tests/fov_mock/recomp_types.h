/* Minimal mock of the runtime's recomp_types.h for the display-fov unit test.
 *
 * fci_fov.c includes "recomp_types.h". The real recomp_types.h is a generated
 * header full of runtime externs; this mock provides only what fci_fov.c uses: the MEMF and
 * MEM32 macros (float / uint32 read-write at a guest VA) and the g_esp, g_ebx and g_edi registers.
 * g_xbox_mem_offset is the mapping offset (0 here, so guest VAs are host addresses). The test
 * driver defines the storage. _stricmp is a Windows CRT function fci_fov.c uses (fov_at_camera);
 * on a non-Windows host it maps to strcasecmp so the shipping source compiles natively.
 */
#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_esp;
extern uint32_t g_ebx;
extern uint32_t g_edi;

#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEMF(addr) (*(volatile float *)XBOX_PTR(addr))
#define MEM32(addr) (*(volatile uint32_t *)XBOX_PTR(addr))

#endif
