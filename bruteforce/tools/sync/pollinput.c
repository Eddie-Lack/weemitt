/* pollinput: a mupen64plus input plugin for the sync test (tests/sync.py).

   The game reads controller 1 once a game frame. On its k-th read the plugin returns the k-th entry of a table that
   the front end set (or, with pollinput_by_vi, the entry of the current video frame, as a BizHawk movie gives
   input), and records chosen words of RDRAM first: at that moment the previous game frame's logic has
   run. It also records the video frame of each read: the count of vertical interrupts so far, from the vicount
   video plugin (vicount.c). A watched word can be read through a pointer: then the word at the pointer's target
   plus an offset is recorded. Before the k-th read it applies the memory writes that the front end set for read
   k.

   Build: cc -shared -fPIC -O2 -I/usr/include/mupen64plus -o pollinput.so pollinput.c -ldl */
#define M64P_PLUGIN_PROTOTYPES 1
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <xmmintrin.h>
#include <stdlib.h>
#include <string.h>

#include "m64p_debugger.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

static CONTROL *controls;
static void *(*memPointer)(m64p_dbg_memptr_type);
static uint8_t *rdram;

static const uint32_t *table; /* the BUTTONS words, one per read */
static int tableCount;
static const uint32_t *watch; /* RDRAM addresses of 32-bit words to record at each read */
static const uint32_t *watchOffset; /* 0xFFFFFFFF: the word at the address; else follow the pointer there */
static int watchCount;
static const uint32_t *writes; /* groups of four: read index, address, size (1, 2 or 4), value */
static int writeCount;
static uint32_t *log;         /* watchCount words per read */
static uint32_t *viLog;       /* the video frame of each read */
static int logCap;
static int reads;
static int byVi;
static int firstVi;
static int frame = -1; /* >= 0: the movie frame that the front end runs now (BizHawk's core, emurun.py) */
static unsigned int (*viCount)(void);
static int fpuResets;

/* The SSE flags flush to zero (0x8000) and denormals are zero (0x0040) of the emulator thread. A library of the SDL
   audio plugin turns them on when it loads. BizHawk runs its emulator thread with neither, and so does the engine:
   the pure interpreter then keeps a denormal result where the real N64, with FCSR.FS set, would flush it. */
#define MXCSR_FLUSH 0x8040u

/* Clears the two flags in the calling thread (the emulator's). Returns how often a read found them set again. */
EXPORT int CALL pollinput_fpu(void) {
    _mm_setcsr(_mm_getcsr() & ~MXCSR_FLUSH);
    return fpuResets;
}

/* on 1: the table holds one entry per video frame instead of one per read. Entry 0 is for the video frame after
   `first` vertical interrupts: BizHawk's core runs two video frames before a movie's first frame. */
EXPORT void CALL pollinput_by_vi(int on, int first) {
    byVi = on;
    firstVi = first;
}

/* The front end that steps the core one video frame at a time (BizHawk's) gives each frame's movie frame: every read in
   it takes that entry, as BizHawk does. -1: count video frames instead. */
EXPORT void CALL pollinput_frame(int f) {
    frame = f;
}

EXPORT void CALL pollinput_set(const uint32_t *t, int count, const uint32_t *w, const uint32_t *woff, int wcount,
                               const uint32_t *wr, int wrcount, uint32_t *out, uint32_t *viOut, int cap) {
    table = t;
    tableCount = count;
    watch = w;
    watchOffset = woff;
    watchCount = wcount;
    writes = wr;
    writeCount = wrcount;
    log = out;
    viLog = viOut;
    logCap = cap;
    reads = 0;
}

EXPORT int CALL pollinput_reads(void) {
    return reads;
}


EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context, void (*debug)(void *, int, const char *)) {
    memPointer = (void *(*)(m64p_dbg_memptr_type))dlsym(core, "DebugMemGetPointer");
    viCount = (unsigned int (*)(void))dlsym(RTLD_DEFAULT, "vicount_value");
    return memPointer && viCount ? M64ERR_SUCCESS : M64ERR_INCOMPATIBLE;
}

EXPORT m64p_error CALL PluginShutdown(void) {
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name, int *caps) {
    if (type) *type = M64PLUGIN_INPUT;
    if (version) *version = 0x010000;
    if (api) *api = 0x020100;
    if (name) *name = "pollinput";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}

EXPORT void CALL InitiateControllers(CONTROL_INFO info) {
    int i;

    controls = info.Controls;
    for (i = 0; i < 4; i++) {
        controls[i].Present = i == 0;
        controls[i].RawData = 0;
        controls[i].Plugin = PLUGIN_NONE;
    }
}

/* RDRAM is kept as host-endian 32-bit words, so a word reads directly. */
static uint32_t word(uint32_t addr) {
    addr &= 0x7FFFFC;
    return *(uint32_t *)(rdram + addr);
}

/* A byte of the N64's big-endian view is at its address XOR 3, a halfword at its address XOR 2. */
static void write_value(uint32_t addr, uint32_t size, uint32_t value) {
    addr &= 0x7FFFFF;
    if (size == 1) {
        rdram[addr ^ 3] = (uint8_t)value;
    } else if (size == 2) {
        *(uint16_t *)(rdram + ((addr & ~1u) ^ 2)) = (uint16_t)value;
    } else {
        *(uint32_t *)(rdram + (addr & ~3u)) = value;
    }
}

EXPORT void CALL GetKeys(int ctrl, BUTTONS *keys) {
    int i;

    if (ctrl != 0) {
        keys->Value = 0;
        return;
    }
    if (rdram == NULL) {
        rdram = memPointer(M64P_DBG_PTR_RDRAM);
    }
    if (_mm_getcsr() & MXCSR_FLUSH) {
        _mm_setcsr(_mm_getcsr() & ~MXCSR_FLUSH);
        fpuResets++;
    }
    for (i = 0; i < writeCount; i++) {
        if (writes[i * 4] == (uint32_t)reads) {
            write_value(writes[i * 4 + 1], writes[i * 4 + 2], writes[i * 4 + 3]);
        }
    }
    if (reads < logCap) {
        for (i = 0; i < watchCount; i++) {
            uint32_t addr = watch[i];

            if (watchOffset[i] != 0xFFFFFFFFu) {
                uint32_t ptr = word(addr);

                /* A NULL pointer, or one outside RDRAM, records 0. */
                if (ptr < 0x80000000u || ptr >= 0x80800000u) {
                    log[reads * watchCount + i] = 0;
                    continue;
                }
                addr = ptr + watchOffset[i];
            }
            log[reads * watchCount + i] = word(addr);
        }
        viLog[reads] = viCount();
    }
    {
        int index = !byVi ? reads : frame >= 0 ? frame : (int)viCount() - firstVi;

        keys->Value = index >= 0 && index < tableCount ? table[index] : 0;
    }
    reads++;
}

EXPORT void CALL ControllerCommand(int ctrl, unsigned char *command) {}
EXPORT void CALL ReadController(int ctrl, unsigned char *command) {}
EXPORT int CALL RomOpen(void) {
    _mm_setcsr(_mm_getcsr() & ~MXCSR_FLUSH);
    return 1;
}
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL SDL_KeyDown(int keymod, int keysym) {}
EXPORT void CALL SDL_KeyUp(int keymod, int keysym) {}
