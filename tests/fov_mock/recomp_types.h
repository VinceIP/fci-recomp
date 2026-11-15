/* Minimal mock of the runtime's recomp_types.h for the display-fov unit test.
 *
 * fci_fov.c includes "recomp_types.h". The real recomp_types.h is a generated
 * header full of runtime externs; this mock provides only what fci_fov.c uses: the MEMF macro
 * (a float read/write at a guest VA) and the g_esp register. g_xbox_mem_offset is the mapping
 * offset (0 here, so guest VAs are host addresses). The test driver defines the storage.
 */
#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_esp;

#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEMF(addr) (*(volatile float *)XBOX_PTR(addr))

#endif
