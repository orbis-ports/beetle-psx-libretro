/* PlayStation 4 backing store for Lightrec's mirrored RAM and its code buffer.
 *
 * libretro.c builds the maps the recompiler needs out of five macros - MAP, MAP_SHM,
 * MAP_CODE, UNMAP and a memfd - and every existing arm implements them with POSIX shared
 * memory, ashmem or Win32 file mappings. This console has none of the three. It has its own
 * physical allocator instead, and the shapes turn out to line up one for one:
 *
 *     memfd_create + ftruncate   ->  sceKernelAllocateDirectMemory   (physical pages)
 *     mmap(MAP_SHARED, memfd)    ->  sceKernelMapDirectMemory        (a view of those pages)
 *     mmap(MAP_ANON|MAP_PRIVATE) ->  the same call on a private allocation of its own
 *     PROT_EXEC at mmap time     ->  REFUSED - see orbis_lr_map_code
 *
 * ⚠ NONE OF THIS IS INFERRED FROM DOCUMENTATION. Every mechanism below was measured on this
 * console by `orbis_test_mirror_mapping()` in mesa-ps4's ac_orbis_drm.c, behind
 * ORBIS_TEST_MIRROR, on 2026-08-23. That probe's ladder is the reason this file exists at
 * all; RetroArch/ps4/HANDOFF.md carries the results and the one refusal.
 */
#ifndef ORBIS_LIGHTREC_MEM_H
#define ORBIS_LIGHTREC_MEM_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The physical allocation that stands in for the memfd. Returns the direct-memory offset,
 * or -1. Everything else here is meaningless until this has succeeded. */
off_t orbis_lr_backing_create(size_t size);

/* Give back the shared allocation and every private one this file is still holding. Safe to
 * call when nothing was ever allocated. */
void orbis_lr_backing_destroy(void);

/* A second, third, fourth view of the SAME physical pages, at an address of our choosing.
 * This is the whole reason the port is possible - the emulated machine's RAM appears at
 * 0x00000000, 0x00200000, 0x00400000 and 0x00600000 and all four have to be one memory. */
void *orbis_lr_map_shared(void *addr, size_t size, off_t phys, off_t offset);

/* Private pages at an address of our choosing: the BIOS image and the scratch pad. Backed by
 * their own physical allocation, which this file remembers so that unmapping releases it. */
void *orbis_lr_map_private(void *addr, size_t size);

/* Writable AND executable pages for the recompiler's output. Mapped read-write and then
 * promoted with sceKernelMprotect, because asking for execute up front is refused. */
void *orbis_lr_map_code(void *addr, size_t size);

/* Drop one mapping made by any of the three above. Returns 0 on success. */
int orbis_lr_unmap(void *addr, size_t size);

/* Did orbis_lr_map_code actually get execute rights on this run? ⚠ A caller that recompiles
 * without checking this jumps into pages the kernel may have refused to make executable, and
 * that ends the process rather than returning an error. */
int orbis_lr_have_code_buffer(void);

/* Distinguished from the above because the core reads its options before it maps anything:
 * "not asked yet" must not read as "refused", or the recompiler is switched off on the way in
 * every time and nothing says why. */
int orbis_lr_code_refused(void);

#ifdef __cplusplus
}
#endif

#endif /* ORBIS_LIGHTREC_MEM_H */
