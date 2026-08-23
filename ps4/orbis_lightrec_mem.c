/* PlayStation 4 backing store for Lightrec. See orbis_lightrec_mem.h for the shape.
 *
 * ⚠ WHY THIS IS NOT mmap(). The SDK ships musl's headers behind a FreeBSD triple, so the
 * POSIX names here are a thin and occasionally wrong layer over Sony's own calls - MAP_ANON
 * is 0x1002 on this kernel and musl's header says 0x0020. More to the point, none of the
 * POSIX shapes this file needs are reachable through it: there is no shm_open, no
 * memfd_create, and mmap has no way to say "the same physical pages again, over there".
 * Sony's allocator does, so this file talks to it directly.
 *
 * ⚠ AND THE ONE REFUSAL. sceKernelMapDirectMemory asked for READ|EXECUTE up front returns
 * 0x8002000d, EACCES - understood and declined on policy, not malformed. The policy lives at
 * MAP time, not at PROTECT time: mapping read-write and then promoting with sceKernelMprotect
 * is granted, and code written that way was executed on this console. Anyone who tries the
 * direct form first will conclude a recompiler is impossible here, which is what the probe
 * concluded one rung before it turned out true.
 */
#include "orbis_lightrec_mem.h"

#include <stdint.h>
#include <string.h>

#include <orbis/libkernel.h>
#include <libretro.h>

extern retro_log_printf_t log_cb;

/* FreeBSD's MAP_FIXED, which is what Sony's mapper takes: place this mapping AT the address
 * given. Spelled out because the SDK headers do not name it. Production use of the same
 * constant is mesa-ps4's ac_orbis_drm.c, which moves buffers between buses with it. */
#define ORBIS_MAP_FIXED 0x0010

/* Direct memory is handed out in 16 KiB units whatever getpagesize() says, so every size and
 * every alignment below is rounded to this. The scratch pad is 1 KiB and would otherwise be
 * rejected as malformed rather than refused. */
#define ORBIS_GRANULE 0x4000ull

#define ORBIS_PROT_RW  (ORBIS_KERNEL_PROT_CPU_RW)            /* 0x03 */
#define ORBIS_PROT_RWX (ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_CPU_EXEC) /* 0x07 */

/* Private and code allocations own physical memory of their own and must give it back. Four
 * would do - BIOS, scratch pad, code buffer - and eight leaves room for a retry. */
#define ORBIS_LR_MAX_OWNED 8

static struct {
   void  *addr;
   size_t size;
   off_t  phys;
} orbis_lr_owned[ORBIS_LR_MAX_OWNED];

static off_t  orbis_lr_shared_phys = -1;
static size_t orbis_lr_shared_size;
/* -1 nothing has asked yet, 0 the kernel refused, 1 we have executable pages. The three are
 * not two: "not tried" and "refused" lead to opposite decisions about the recompiler, and
 * collapsing them would turn the first frame of every run into a silent fall back to the
 * interpreter, since the core reads its options before it maps anything. */
static int    orbis_lr_code_state = -1;

static size_t orbis_lr_round(size_t n)
{
   return (n + (ORBIS_GRANULE - 1)) & ~(size_t)(ORBIS_GRANULE - 1);
}

/* Sony returns 0x8002xxxx where the low half is an errno. Two of them mean opposite things
 * and the difference is the whole content of a failed run: EACCES says the kernel understood
 * the request and declined it, EINVAL says the request was malformed and no policy was ever
 * consulted. Anything not listed is printed raw. */
static const char *orbis_lr_err(int32_t rc)
{
   switch ((uint32_t)rc)
   {
      case 0x80020001u: return "EPERM - not permitted";
      case 0x80020009u: return "EBADF - the flags word was not anonymous";
      case 0x8002000Cu: return "ENOMEM - out of memory or address space";
      case 0x8002000Du: return "EACCES - understood and refused on policy";
      case 0x8002000Eu: return "EFAULT - bad address argument";
      case 0x80020016u: return "EINVAL - malformed request, policy never reached";
      default:          return "not in this file's table";
   }
}

/* ⚠ MAP_FIXED ON THIS KERNEL REPLACES WHATEVER IS THERE. It has no MAP_FIXED_NOREPLACE, so
 * the Linux arm's "ask for the address and check what came back" does not protect anything -
 * by the time we could check, the frontend's heap would already be gone. Every fixed mapping
 * below therefore asks first, and this is the function that answers.
 *
 * ⚠ IT WALKS THE RANGE A GRANULE AT A TIME RATHER THAN ASKING ONE QUESTION, and the reason is
 * worth keeping. sceKernelVirtualQuery takes a flags word, and the obvious shape for this
 * check is flags=1 - "give me the first mapping at or above this address" - which answers the
 * whole range in one call. Nothing in this workshop has ever passed 1. The only value with a
 * meaning established here is 0, from mesa-ps4's ac_orbis_drm.c, where a nonzero RETURN is
 * read as "nothing is mapped at this address".
 *
 * The failure modes are not symmetric. If a guessed flag makes the call fail, this returns
 * "free" for every address, MAP_FIXED lands on top of the frontend's heap, and the failure is
 * a corrupted process rather than a refusal. A guess that is too conservative merely loses the
 * recompiler. So this uses the semantics that were measured, at the cost of one call per 16
 * KiB - about eighteen thousand of them across the whole address search, once per content
 * load, against a recompiler that is worth two and a half times the frame rate.
 *
 * If flags=1 is ever established on this platform, this collapses back to a single call. */
static int orbis_lr_range_free(void *addr, size_t size)
{
   const uintptr_t lo = (uintptr_t)addr;
   const uintptr_t hi = lo + size;
   uintptr_t p;

   for (p = lo; p < hi; p += ORBIS_GRANULE)
   {
      OrbisKernelVirtualQueryInfo info;
      memset(&info, 0, sizeof(info));

      if (sceKernelVirtualQuery((const void *)p, 0, &info, sizeof(info)) == 0)
      {
         /* Something is there. Say what and where once per rejection: this is the only account
          * anyone gets of where this process's address space actually is, and the io_base
          * search is otherwise a silent list of refusals. */
         log_cb(RETRO_LOG_DEBUG, "[PS4] lightrec: %p is taken (0x%llx-0x%llx, direct %u flexible %u "
                                 "stack %u pooled %u, '%.31s') - not placing %zu KiB at %p\n",
                (void *)p, (unsigned long long)(uintptr_t)info.unk01,
                (unsigned long long)(uintptr_t)info.unk02, info.isDirectMemory,
                info.isFlexibleMemory, info.isStack, info.isPooledMemory, info.name,
                size / 1024, addr);
         return 0;
      }
   }

   return 1;
}

static int orbis_lr_remember(void *addr, size_t size, off_t phys)
{
   unsigned i;
   for (i = 0; i < ORBIS_LR_MAX_OWNED; i++)
   {
      if (!orbis_lr_owned[i].addr)
      {
         orbis_lr_owned[i].addr = addr;
         orbis_lr_owned[i].size = size;
         orbis_lr_owned[i].phys = phys;
         return 1;
      }
   }
   /* Not fatal for the run, but it leaks physical memory across a core reload, which on a
    * console with no swap is the kind of leak that ends in a refusal three loads later. */
   log_cb(RETRO_LOG_WARN, "[PS4] lightrec: no slot to record the allocation at %p - %zu KiB of "
                          "direct memory will not be released\n", addr, size / 1024);
   return 0;
}

/* Allocate physical pages and put them at exactly `addr`. Every mapping in this file goes
 * through here; they differ only in whether the pages are new (private) or already ours
 * (shared), which is what a NULL `phys_in` selects. */
static void *orbis_lr_place(void *addr, size_t size, const off_t *phys_in, int32_t prot)
{
   const size_t len = orbis_lr_round(size);
   off_t   phys     = phys_in ? *phys_in : 0;
   void   *at       = addr;
   int32_t rc;

   if (!orbis_lr_range_free(addr, len))
      return NULL;

   if (!phys_in)
   {
      rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len,
                                         ORBIS_GRANULE, ORBIS_KERNEL_WB_ONION, &phys);
      if (rc != 0)
      {
         log_cb(RETRO_LOG_WARN, "[PS4] lightrec: no direct memory for %zu KiB -> 0x%08x (%s)\n",
                len / 1024, (unsigned)rc, orbis_lr_err(rc));
         return NULL;
      }
   }

   rc = sceKernelMapDirectMemory(&at, len, prot, ORBIS_MAP_FIXED, phys, ORBIS_GRANULE);
   if (rc != 0 || at != addr)
   {
      log_cb(RETRO_LOG_DEBUG, "[PS4] lightrec: %zu KiB at %p -> 0x%08x (%s), landed at %p\n",
             len / 1024, addr, (unsigned)rc, orbis_lr_err(rc), at);
      if (rc == 0)
         sceKernelMunmap(at, len);
      if (!phys_in)
         sceKernelReleaseDirectMemory(phys, len);
      return NULL;
   }

   if (!phys_in)
      orbis_lr_remember(at, len, phys);

   return at;
}

off_t orbis_lr_backing_create(size_t size)
{
   const size_t len = orbis_lr_round(size);
   off_t phys       = 0;
   const int32_t rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len,
                                                    ORBIS_GRANULE, ORBIS_KERNEL_WB_ONION, &phys);

   if (rc != 0)
   {
      log_cb(RETRO_LOG_ERROR, "[PS4] lightrec: could not reserve %zu KiB of direct memory for the "
                              "emulated machine's RAM -> 0x%08x (%s)\n",
             len / 1024, (unsigned)rc, orbis_lr_err(rc));
      return (off_t)-1;
   }

   orbis_lr_shared_phys = phys;
   orbis_lr_shared_size = len;
   orbis_lr_code_state  = -1;
   return phys;
}

void orbis_lr_backing_destroy(void)
{
   unsigned i;

   for (i = 0; i < ORBIS_LR_MAX_OWNED; i++)
   {
      if (!orbis_lr_owned[i].addr)
         continue;
      sceKernelReleaseDirectMemory(orbis_lr_owned[i].phys, orbis_lr_owned[i].size);
      memset(&orbis_lr_owned[i], 0, sizeof(orbis_lr_owned[i]));
   }

   if (orbis_lr_shared_phys >= 0)
   {
      sceKernelReleaseDirectMemory(orbis_lr_shared_phys, orbis_lr_shared_size);
      orbis_lr_shared_phys = -1;
      orbis_lr_shared_size = 0;
   }
   orbis_lr_code_state = -1;
}

void *orbis_lr_map_shared(void *addr, size_t size, off_t phys, off_t offset)
{
   const off_t at_phys = phys + offset;
   return orbis_lr_place(addr, size, &at_phys, ORBIS_PROT_RW);
}

void *orbis_lr_map_private(void *addr, size_t size)
{
   return orbis_lr_place(addr, size, NULL, ORBIS_PROT_RW);
}

void *orbis_lr_map_code(void *addr, size_t size)
{
   const size_t len = orbis_lr_round(size);
   void *at         = orbis_lr_place(addr, len, NULL, ORBIS_PROT_RW);
   int32_t rc;

   if (!at)
      return NULL;

   /* The promotion the whole port turns on. Asking for execute at map time is EACCES; asking
    * for it here is granted, and a six-byte stub written this way was called on this console
    * and returned. */
   rc = sceKernelMprotect(at, len, ORBIS_PROT_RWX);
   if (rc != 0)
   {
      log_cb(RETRO_LOG_ERROR, "[PS4] lightrec: %zu KiB at %p would not take execute -> 0x%08x (%s). "
                              "The recompiler has nowhere to put code and will not be used.\n",
             len / 1024, at, (unsigned)rc, orbis_lr_err(rc));
      orbis_lr_code_state = 0;
      orbis_lr_unmap(at, len);
      return NULL;
   }

   /* ⚠ A GRANTED PROTECTION IS NOT AN HONOURED ONE, AND THE WAY TO TELL IS TO RUN SOMETHING.
    *
    * This check used to be sceKernelQueryMemoryProtection: promote to 0x07, read the protection
    * back, refuse if execute is missing. On hardware that read-back returns 0x03 - the
    * protection the range was MAPPED with - for a range mprotect has just promoted, at every one
    * of the sixteen base addresses tried. So the veto fired sixteen times, the recompiler was
    * clamped off, and the frame rate did not move.
    *
    * The pages were executable the whole time. mesa-ps4's probe established it by CALLING a stub
    * in exactly this arrangement - direct memory, mapped read-write, promoted with
    * sceKernelMprotect - and it returned 0x00c0ffee (`MIRROR rung 7 OK`, hardware, 2026-08-23).
    * The probe never asked sceKernelQueryMemoryProtection, so nothing had caught that the query
    * does not reflect the promotion.
    *
    * ⚠ THE LESSON IS NOT "DROP THE CHECK". It is that on this console a query API is not a
    * measurement of the thing it names, and the workshop has now paid for that four times
    * (GB_ADDR_CONFIG, the tessellation registers, three readerless env knobs, and this). So the
    * check stays and becomes the same one the probe made: put six bytes of x86-64 in the buffer
    * and call them.
    *
    *     b8 ee ff c0 00   mov eax, 0x00c0ffee
    *     c3               ret
    *
    * Self-modifying code needs no cache maintenance on x86-64, so a wrong answer here is about
    * mapping rather than coherency. If the page does not execute this does not return an error,
    * it ends the process - which is why the line before it goes out on the channel that survives,
    * and why it is better here, once, at a known point, than on the first recompiled block.
    *
    * The stub is written at the START of the buffer and left there. tlsf_create_with_pool
    * overwrites it moments later when Lightrec takes the buffer over. */
   {
      static const uint8_t stub[] = {0xb8, 0xee, 0xff, 0xc0, 0x00, 0xc3};
      uint32_t (*fn)(void);
      uint32_t got;

      memcpy(at, stub, sizeof(stub));

      /* RETRO_LOG_ERROR on purpose: on this frontend that is the only level that also reaches
       * klog, and a datagram queued by a process that is about to die does not leave. */
      log_cb(RETRO_LOG_ERROR, "[PS4] lightrec: calling %p to see whether it executes. If this is the "
                              "last lightrec line, the promotion to 0x07 was granted and not "
                              "honoured, and that is the answer rather than a crash to chase.\n", at);

      memcpy(&fn, &at, sizeof(fn)); /* not a cast: object pointer to function pointer is not one */
      got = fn();

      if (got != 0x00c0ffeeu)
      {
         log_cb(RETRO_LOG_ERROR, "[PS4] lightrec: %p executes but returned 0x%08x instead of "
                                 "0x00c0ffee. Not trusting it with recompiled code.\n",
                at, (unsigned)got);
         orbis_lr_code_state = 0;
         orbis_lr_unmap(at, len);
         return NULL;
      }

      /* ⚠ THE ANSWERING HALF, AND IT IS AT ERROR LEVEL ON PURPOSE. The warning above is the
       * only lightrec line that reaches klog, because this frontend puts everything below
       * RARCH_ERR on UDP alone. A reader watching klog would see "calling %p" and then nothing,
       * which is exactly what the death it warns about looks like. A pair that only ever prints
       * its first half is worse than no instrumentation. */
      log_cb(RETRO_LOG_ERROR, "[PS4] lightrec: %p returned 0x00c0ffee - the code buffer executes, "
                              "and the line above is answered rather than final.\n", at);
   }

   orbis_lr_code_state = 1;
   log_cb(RETRO_LOG_INFO, "[PS4] lightrec: %zu KiB code buffer at %p, writable and executable\n",
          len / 1024, at);
   return at;
}

int orbis_lr_unmap(void *addr, size_t size)
{
   const size_t len = orbis_lr_round(size);
   unsigned i;

   if (!addr)
      return 0;

   if (sceKernelMunmap(addr, len) != 0)
      return -1;

   for (i = 0; i < ORBIS_LR_MAX_OWNED; i++)
   {
      if (orbis_lr_owned[i].addr != addr)
         continue;
      sceKernelReleaseDirectMemory(orbis_lr_owned[i].phys, orbis_lr_owned[i].size);
      memset(&orbis_lr_owned[i], 0, sizeof(orbis_lr_owned[i]));
      break;
   }

   return 0;
}

int orbis_lr_have_code_buffer(void)
{
   return orbis_lr_code_state == 1;
}

int orbis_lr_code_refused(void)
{
   return orbis_lr_code_state == 0;
}
