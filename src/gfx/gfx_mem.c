/* The renderer's reads of the game's memory (gfx_internal.h, gfx.h): the address range and base that the display
   lists' addresses need, and without a range, a check of each page with the system. */
#include "gfx_host.h"
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "gfx.h"
#include "gfx_internal.h"

#define PAGE_SHIFT 12
#define PAGE_CACHE 4096

static struct {
    uintptr_t base;
    u32 lo, hi;
    /* Without a range, which 4 KB pages the system says exist. A page that exists is remembered; one that does
       not is asked about again in the next frame. */
    struct {
        uintptr_t page;
        u32 frame;
        u8 ok;
    } cache[PAGE_CACHE];
    u32 frame;
    uintptr_t sysPageMask;
} mem;

/* Asks the system whether the page at p is mapped. */
static int page_exists(uintptr_t p) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info;

    if (VirtualQuery((const void *)p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
        return 0;
    }
    return !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
#else
    if (mem.sysPageMask == 0) {
        long size = sysconf(_SC_PAGESIZE);
        mem.sysPageMask = ~(uintptr_t)((size > 0 ? size : 4096) - 1);
    }
    /* msync fails with ENOMEM for an address that is not mapped. */
    return msync((void *)(p & mem.sysPageMask), 1, MS_ASYNC) == 0;
#endif
}

static int readable(uintptr_t p, u32 size) {
    uintptr_t first = p >> PAGE_SHIFT;
    uintptr_t last = (p + (size ? size - 1 : 0)) >> PAGE_SHIFT;
    uintptr_t page;

    if (last < first) {
        return 0;
    }
    for (page = first; page <= last; page++) {
        unsigned i = (unsigned)(page & (PAGE_CACHE - 1));
        if (mem.cache[i].page != page || (!mem.cache[i].ok && mem.cache[i].frame != mem.frame)) {
            mem.cache[i].page = page;
            mem.cache[i].frame = mem.frame;
            mem.cache[i].ok = (u8)page_exists(page << PAGE_SHIFT);
        }
        if (!mem.cache[i].ok) {
            return 0;
        }
    }
    return 1;
}

const u8 *gfx_mem(u32 addr, u32 size) {
    uintptr_t p;

    if (addr == 0) {
        return NULL;
    }
    if (mem.hi != 0) {
        if (addr < mem.lo || addr >= mem.hi || size > mem.hi - addr) {
            return NULL;
        }
        return (const u8 *)(uintptr_t)(mem.base + addr);
    }
    p = (uintptr_t)(mem.base + addr);
    return readable(p, size) ? (const u8 *)p : NULL;
}

void gfx_set_memory_base(uintptr_t base) {
    mem.base = base;
}

void gfx_set_memory_range(unsigned int lo, unsigned int hi) {
    mem.lo = lo;
    mem.hi = hi;
}

void gfx_mem_next_frame(void) {
    mem.frame++;
}
