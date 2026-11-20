/* The pages of the game's state (engine.c, "What a state holds"), and which of them were written.

   The state is split into pieces: one per page of the game's variables (gstate_data, gstate_bss), one for the
   memory after the heap (port_hw), and one per page of port_ram up to the heap's top. Each piece has a version: a
   number that names its content. Two pieces with the same version hold the same bytes. A save stores each piece's
   version with its bytes. A load copies only the pieces whose version differs from the memory's; a save into a
   buffer that holds an earlier state of the same layout copies only the pieces that changed.

   Versions come from page protection. After a save or a load (a sync) the pieces are read-only. The first write to
   one faults; the handler makes its page writable and lists it as dirty. At the next sync every dirty piece gets a
   new version, and its page becomes read-only again. A page that is dirty at several syncs in a row is hot: it
   stays writable and gets a new version at every sync, which saves the fault. Every COOL syncs the hot pages are
   protected again, so a page that is no longer written stops being copied.

   Some pieces are pinned: they are never protected and get a new version at every sync, so they are always copied.
   These are the pieces that share a page with memory outside the state (the first and last page of each section,
   and port_hw), and every piece when GLOVER_FULL_STATES is set: the plain full copy, kept as the reference.

   The heap's gap is part of this. The game's allocator takes dynamic blocks from the bottom of the heap up and
   static blocks from the top down, so the heap is in use below gap_hi and from gap_lo up. The pages between (the
   gap) have never been written: they hold zeros, are read-only and are not part of a state. A write there moves
   the nearer edge of the gap past the page. */
#ifndef _WIN32
#define _GNU_SOURCE /* REG_EIP in ucontext.h */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE 4096u
/* A page dirty at this many syncs in a row becomes hot. */
#define HOT 2
/* Every this many syncs, hot pages are protected again. */
#define COOL 256

extern unsigned char port_ram[];

/* The heap is in use below gap_hi and from gap_lo up: port_ram offsets, multiples of PAGE. */
volatile uint32_t gap_hi;
volatile uint32_t gap_lo;

enum { CLEAN, DIRTY, GAP };

typedef struct Piece {
    unsigned char *addr;
    uint32_t size;
    uint8_t state;
    uint8_t pinned;
    uint8_t hot;
    uint8_t listed; /* in the dirty list */
    uint8_t streak; /* syncs in a row at which it was dirty */
    uint32_t lastSync;
} Piece;

/* The pieces: the variables' pages (data, then bss), port_hw, then one per page of port_ram up to heapTop. */
static Piece *pieces;
static uint64_t *versions;
static uint32_t npieces;
static uint32_t nvars;    /* the pieces before port_ram's */
static uint32_t heapTop;  /* port_ram offset */
static unsigned char *dataBase, *dataEnd, *bssBase, *bssEnd; /* the variables' pages, rounded out to whole pages */
static uint32_t dataFirst, bssFirst;                         /* their first piece */
/* The dirty list: listed pieces, which are writable. */
static uint32_t *dirty;
static uint32_t ndirty;
/* Scratch lists for a sync. */
static uint32_t *work;
static uint32_t *work2;
static uint32_t syncs;  /* syncs with a fault before them */
static uint32_t faults; /* faults since the last sync */
static uint64_t nextVersion;

static int set_prot(unsigned char *addr, uint32_t size, int writable);
static void fail(void);

static uint64_t fresh(void) {
    if (++nextVersion == 0) {
        ++nextVersion;
    }
    return nextVersion;
}

static void list_add(uint32_t i) {
    if (!pieces[i].listed) {
        pieces[i].listed = 1;
        dirty[ndirty++] = i;
    }
}

/* Makes pages [from, to) of port_ram leave the gap: writable and dirty. */
static int leave_gap(uint32_t from, uint32_t to) {
    uint32_t p;

    if (from >= to) {
        return 0;
    }
    if (set_prot(port_ram + from, to - from, 1) != 0) {
        return -1;
    }
    for (p = from; p < to; p += PAGE) {
        pieces[nvars + p / PAGE].state = DIRTY;
        list_add(nvars + p / PAGE);
    }
    return 0;
}

/* Called with the address of a write that faulted. Returns 1 when it was a page of the state and may go on. */
static int on_write(uintptr_t addr) {
    uintptr_t ram = (uintptr_t)port_ram;
    uint32_t i;

    if (pieces == NULL) {
        return 0;
    }
    if (addr >= ram && addr < ram + heapTop) {
        uint32_t page = (uint32_t)(addr - ram) & ~(PAGE - 1);

        i = nvars + page / PAGE;
        if (pieces[i].state == GAP) {
            if (page - gap_hi <= gap_lo - (page + PAGE)) {
                if (leave_gap(gap_hi, page + PAGE) != 0) {
                    return 0;
                }
                gap_hi = page + PAGE;
            } else {
                if (leave_gap(page, gap_lo) != 0) {
                    return 0;
                }
                gap_lo = page;
            }
            faults++;
            return 1;
        }
    } else if (addr >= (uintptr_t)dataBase && addr < (uintptr_t)dataEnd) {
        i = dataFirst + (uint32_t)(addr - (uintptr_t)dataBase) / PAGE;
    } else if (addr >= (uintptr_t)bssBase && addr < (uintptr_t)bssEnd) {
        i = bssFirst + (uint32_t)(addr - (uintptr_t)bssBase) / PAGE;
    } else {
        return 0;
    }
    if (pieces[i].state != CLEAN || pieces[i].pinned || set_prot(pieces[i].addr, PAGE, 1) != 0) {
        return 0;
    }
    pieces[i].state = DIRTY;
    list_add(i);
    faults++;
    return 1;
}

#ifdef _WIN32
#include <windows.h>

/* The variables' pages are part of the program's image, where Windows may want copy-on-write instead. */
static int set_prot(unsigned char *addr, uint32_t size, int writable) {
    DWORD old;

    if (VirtualProtect(addr, size, writable ? PAGE_READWRITE : PAGE_READONLY, &old)) {
        return 0;
    }
    return writable && VirtualProtect(addr, size, PAGE_WRITECOPY, &old) ? 0 : -1;
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;

    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
        on_write((uintptr_t)r->ExceptionInformation[1])) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int install_handler(void) {
    return AddVectoredExceptionHandler(1, on_fault) != NULL ? 0 : -1;
}

static uint64_t salt(void) {
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32);
}
#else
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static int set_prot(unsigned char *addr, uint32_t size, int writable) {
    return mprotect(addr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ);
}

/* The handler that was installed before (a program's crash handler). Faults that are not ours go to it. */
static struct sigaction previous;

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    if (!on_write((uintptr_t)si->si_addr)) {
        /* The default action follows when the instruction faults again: the previous handler only reports. */
        if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction != NULL) {
            previous.sa_sigaction(sig, si, ctx);
            signal(sig, SIG_DFL);
            return;
        }
        if (!(previous.sa_flags & SA_SIGINFO) && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(sig);
            signal(sig, SIG_DFL);
            return;
        }
        /* Not ours: say where, and the default action follows when the instruction faults again. */
        char msg[96];
#if defined(__x86_64__)
        void *ip = (void *)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
#else
        void *ip = (void *)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EIP];
#endif
        int n = snprintf(msg, sizeof(msg), "glover: fault at address %p, instruction %p\n", si->si_addr, ip);

        if (write(2, msg, n) < 0) {
            /* Nothing more can be done. */
        }
        signal(sig, SIG_DFL);
    }
}

static int install_handler(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGSEGV, &sa, &previous);
}

static uint64_t salt(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec) ^ ((uint64_t)getpid() << 32);
}
#endif

/* A page whose protection cannot be changed would make the versions wrong. */
static void fail(void) {
    static const char msg[] = "glover: cannot change the protection of the game's memory\n";

    fwrite(msg, 1, sizeof(msg) - 1, stderr);
    abort();
}

/* Sets the protection of the pieces in list (full pages), joining neighbouring pages into one call. */
static void protect(uint32_t *list, uint32_t n, int writable) {
    uint32_t a;
    uint32_t b;

    /* Insertion sort: the lists are short, and mostly in order. */
    for (a = 1; a < n; a++) {
        uint32_t v = list[a];

        for (b = a; b > 0 && list[b - 1] > v; b--) {
            list[b] = list[b - 1];
        }
        list[b] = v;
    }
    for (a = 0; a < n; a = b) {
        for (b = a + 1; b < n && pieces[list[b]].addr == pieces[list[b - 1]].addr + PAGE; b++) {
        }
        if (set_prot(pieces[list[a]].addr, (b - a) * PAGE, writable) != 0) {
            fail();
        }
    }
}

/* The first part of a sync: every dirty piece gets a new version. Returns in work the pieces to protect again (the
   ones that are not pinned and not hot). A sync without a fault before it (a load right after a save, say) does
   not count for the hot pages: their writes could not show. */
static uint32_t stamp(void) {
    uint32_t k;
    uint32_t n = 0;
    int counts = faults != 0;
    int cool;

    faults = 0;
    syncs += counts;
    cool = counts && syncs % COOL == 0;
    for (k = 0; k < ndirty; k++) {
        uint32_t i = dirty[k];
        Piece *p = &pieces[i];

        versions[i] = fresh();
        if (p->pinned) {
            continue;
        }
        if (counts) {
            p->streak = p->lastSync == syncs - 1 ? (uint8_t)(p->streak < 255 ? p->streak + 1 : 255) : 1;
            p->lastSync = syncs;
            if (p->hot && cool) {
                p->hot = 0;
                p->streak = 0;
            } else if (!p->hot && p->streak >= HOT) {
                p->hot = 1;
            }
        }
        if (!p->hot) {
            work[n++] = i;
        }
    }
    return n;
}

/* The last part of a sync: protects the pieces in work that are still dirty, and drops them from the dirty list. */
static void finish(uint32_t n) {
    uint32_t k;
    uint32_t m = 0;

    for (k = 0; k < n; k++) {
        if (pieces[work[k]].state == DIRTY && !pieces[work[k]].hot && !pieces[work[k]].pinned) {
            work[m++] = work[k];
        }
    }
    protect(work, m, 0);
    for (k = 0; k < m; k++) {
        pieces[work[k]].state = CLEAN;
        pieces[work[k]].listed = 0;
    }
    for (k = 0, m = 0; k < ndirty; k++) {
        if (pieces[dirty[k]].listed) {
            dirty[m++] = dirty[k];
        }
    }
    ndirty = m;
}

/* The state's pieces for marks hi and lo, in a state's order: the variables and port_hw, port_ram below hi, port_ram
   from lo to the heap's top. */
uint32_t track_count(uint32_t hi, uint32_t lo) {
    return nvars + hi / PAGE + (heapTop - lo) / PAGE;
}

static uint32_t piece_at(uint32_t k, uint32_t hi, uint32_t lo) {
    if (k < nvars + hi / PAGE) {
        return k;
    }
    return k - nvars - hi / PAGE + nvars + lo / PAGE;
}

static unsigned char *piece_addr(uint32_t i) {
    return i < nvars ? pieces[i].addr : port_ram + (i - nvars) * PAGE;
}

static uint32_t piece_size(uint32_t i) {
    return i < nvars ? pieces[i].size : PAGE;
}

/* Bytes of the state's body for marks hi and lo. */
size_t track_body_size(uint32_t hi, uint32_t lo) {
    size_t n = 0;
    uint32_t i;

    for (i = 0; i < nvars; i++) {
        n += pieces[i].size;
    }
    return n + hi + (heapTop - lo);
}

static uint64_t get64(const void *p) {
    uint64_t v;

    memcpy(&v, p, 8);
    return v;
}

/* Saves the state's versions (table: track_count entries of 8 bytes) and body. With delta, table and body hold an
   earlier state of the same marks, and only the pieces whose version changed are copied. */
void track_save(uint8_t *table, uint8_t *body, int delta) {
    uint32_t hi = gap_hi;
    uint32_t lo = gap_lo;
    uint32_t n = track_count(hi, lo);
    uint32_t k;
    size_t off = 0;

    finish(stamp());
    for (k = 0; k < n; k++) {
        uint32_t i = piece_at(k, hi, lo);
        uint32_t size = piece_size(i);

        if (!delta || get64(table + 8 * k) != versions[i]) {
            memcpy(body + off, piece_addr(i), size);
            memcpy(table + 8 * k, &versions[i], 8);
        }
        off += size;
    }
}

/* Loads a state with marks hi and lo, its versions and its body. fix runs after the copy, before the pages are
   protected again. */
void track_load(const uint8_t *table, const uint8_t *body, uint32_t hi, uint32_t lo, void (*fix)(void)) {
    uint32_t nclose = stamp();
    uint32_t nopen = 0;
    uint32_t n = track_count(hi, lo);
    uint32_t k;
    uint32_t p;
    size_t off = 0;

    /* The pages that enter the gap: zero them and make them read-only. The ones that leave it: make them writable;
       their version, 0, differs from the state's, so they are copied below. */
    for (p = 0; p < heapTop; p += PAGE) {
        uint32_t i = nvars + p / PAGE;
        int inGap = p >= hi && p < lo;

        if (inGap && pieces[i].state != GAP) {
            if (pieces[i].state == CLEAN && set_prot(port_ram + p, PAGE, 1) != 0) {
                fail();
            }
            memset(port_ram + p, 0, PAGE);
            if (set_prot(port_ram + p, PAGE, 0) != 0) {
                fail();
            }
            pieces[i].state = GAP;
            pieces[i].listed = 0;
            pieces[i].hot = 0;
            versions[i] = 0;
        } else if (!inGap && pieces[i].state == GAP) {
            if (set_prot(port_ram + p, PAGE, 1) != 0) {
                fail();
            }
            pieces[i].state = DIRTY;
            list_add(i);
            work[nclose++] = i;
        }
    }
    gap_hi = hi;
    gap_lo = lo;
    /* The read-only pieces that differ: open them, then copy every piece that differs. */
    for (k = 0; k < n; k++) {
        uint32_t i = piece_at(k, hi, lo);

        if (pieces[i].state == CLEAN && get64(table + 8 * k) != versions[i]) {
            work2[nopen++] = i;
        }
    }
    protect(work2, nopen, 1);
    for (k = 0; k < nopen; k++) {
        pieces[work2[k]].state = DIRTY;
        work[nclose++] = work2[k];
    }
    for (k = 0; k < n; k++) {
        uint32_t i = piece_at(k, hi, lo);
        uint32_t size = piece_size(i);
        uint64_t v = get64(table + 8 * k);

        if (v != versions[i]) {
            memcpy(piece_addr(i), body + off, size);
            versions[i] = v;
        }
        off += size;
    }
    fix();
    finish(nclose);
}

/* Rounds a section out to whole pages and adds a piece for each page. Pieces that share a page with memory outside
   the section are pinned. */
static void add_section(unsigned char *start, unsigned char *stop, unsigned char **base, unsigned char **end,
                        uint32_t *first) {
    unsigned char *pg = (unsigned char *)((uintptr_t)start & ~(uintptr_t)(PAGE - 1));

    *base = pg;
    *first = nvars;
    for (; pg < stop; pg += PAGE) {
        unsigned char *a = pg > start ? pg : start;
        unsigned char *b = pg + PAGE < stop ? pg + PAGE : stop;
        Piece *p = &pieces[nvars++];

        p->addr = a;
        p->size = (uint32_t)(b - a);
        p->pinned = p->size != PAGE;
    }
    *end = pg;
}

/* Sets up the pieces and the gap, and installs the fault handler. The heap runs from heapStart to top; it is all in
   the gap at first, apart from the page that its start shares with the data below. Every piece starts dirty, so the
   first sync protects them. full: every piece is pinned (no protection; every save and load copies everything). */
int track_init(unsigned char *data, unsigned char *dataStop, unsigned char *bss, unsigned char *bssStop,
               unsigned char *hw, uint32_t hwSize, uint32_t heapStart, uint32_t top, int full) {
    uint32_t hi = (heapStart + PAGE - 1) & ~(PAGE - 1);
    uint32_t lo = top & ~(PAGE - 1);
    uint32_t max = (uint32_t)((dataStop - data) + (bssStop - bss)) / PAGE + 5 + top / PAGE;
    uint32_t i;

    if (((uintptr_t)port_ram & (PAGE - 1)) != 0 || (top & (PAGE - 1)) != 0) {
        return -1;
    }
    pieces = calloc(max, sizeof(*pieces));
    versions = calloc(max, sizeof(*versions));
    dirty = calloc(max, sizeof(*dirty));
    work = calloc(2 * max, sizeof(*work));
    work2 = calloc(max, sizeof(*work2));
    if (pieces == NULL || versions == NULL || dirty == NULL || work == NULL || work2 == NULL) {
        return -1;
    }
    heapTop = top;
    add_section(data, dataStop, &dataBase, &dataEnd, &dataFirst);
    add_section(bss, bssStop, &bssBase, &bssEnd, &bssFirst);
    pieces[nvars].addr = hw;
    pieces[nvars].size = hwSize;
    pieces[nvars].pinned = 1;
    nvars++;
    npieces = nvars + top / PAGE;
    for (i = nvars; i < npieces; i++) {
        pieces[i].addr = port_ram + (i - nvars) * PAGE;
        pieces[i].size = PAGE;
    }
    for (i = 0; i < npieces; i++) {
        uint32_t off = (uint32_t)(pieces[i].addr - port_ram);

        pieces[i].pinned |= full;
        if (i >= nvars && off >= hi && off < lo) {
            pieces[i].state = GAP;
        } else {
            pieces[i].state = DIRTY;
            list_add(i);
        }
    }
    nextVersion = salt() * 0x9E3779B97F4A7C15ull;
    if (install_handler() != 0) {
        return -1;
    }
    gap_hi = hi;
    gap_lo = lo;
    return set_prot(port_ram + hi, lo - hi, 0);
}
