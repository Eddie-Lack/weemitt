/* The engine (engine.h): starts weemitt's game, steps it one game frame at a time, and saves and loads its state.

   What a state holds:
   - The game's variables outside its memory: the writable data of every game and runtime file. The Makefile puts
     them in two sections of their own, gstate_data and gstate_bss.
   - The game's memory, port_ram (weemitt's copy of RDRAM, which also holds the heap). The part below the heap (the
     N64's low memory and the main segment as copied from the ROM: its code, rodata, data and bss) is always
     held. Of the heap, only the parts that have been written since the start: the
     game's allocator (jalloc.c) takes dynamic blocks from the bottom up and static blocks from the top down, so
     the heap is in use below one mark and above another. The pages between the marks (the gap) are read-only. A
     write there stops at a fault, and the fault handler moves the nearer mark past the page and makes it writable
     (track.c). Memory in the gap has never been written, so it is zero. A load clears what lies in the state's gap
     but not in the engine's.
   - The memory after the heap that ram.S also places (port_hw, the N64's hardware registers).
   - The engine's frame counter. */
#include "engine.h"
/* PORT_PTR_BASE: how the game's memory stores a pointer (weemitt's src/port/ptr32.h). */
#include "../../src/port/ptr32.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's side (weemitt's src/port/frame.h, which uses plain C types). */
extern unsigned char *port_rom;
extern unsigned int port_rom_size;
extern unsigned short port_buttons;
extern signed char port_stick_x, port_stick_y;
extern unsigned char port_eeprom[512];
typedef struct PortFrameTask {
    unsigned int addr;
    unsigned int size;
    int sprite2d;
} PortFrameTask;
void port_game_init(int level);
int port_game_frame(PortFrameTask *tasks, int max);
void port_game_trace(char *buf, int size);

/* The sections that the Makefile makes (see the comment at the top). */
extern unsigned char __start_gstate_data[], __stop_gstate_data[];
extern unsigned char __start_gstate_bss[], __stop_gstate_bss[];
extern unsigned char __start_gram_bss[], __stop_gram_bss[];
extern unsigned char port_ram[];
/* The end of port_hw, the last part of the game's memory (gen/ram.S). The hash stops there. */
extern unsigned char port_hw_end[];

#define RAM_BASE 0x80000000u
#define HEAP_TOP 0x1800000u /* the end of the heap, as an offset in port_ram (weemitt's PORT_HEAP_TOP) */

/* track.c: the state's pieces and their versions. The heap is in use below gap_hi and from gap_lo up (port_ram
   offsets, multiples of the page size). */
extern volatile uint32_t gap_hi, gap_lo;
int track_init(unsigned char *data, unsigned char *dataStop, unsigned char *bss, unsigned char *bssStop,
               unsigned char *hw, uint32_t hwSize, uint32_t heapStart, uint32_t top, int full);
uint32_t track_count(uint32_t hi, uint32_t lo);
size_t track_body_size(uint32_t hi, uint32_t lo);
void track_save(uint8_t *table, uint8_t *body, int delta);
void track_load(const uint8_t *table, const uint8_t *body, uint32_t hi, uint32_t lo, void (*fix)(void));

#define STATE_MAGIC 0x54534C47u /* "GLST" */
#define STATE_VERSION 2

/* The state's header. Then come the versions of the state's pieces (track.c), 8 bytes each, and the parts in this
   order: the game's variables (data, bss), the memory after the heap, port_ram below markHi, and port_ram from
   markLo to the top of the heap. */
typedef struct StateHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t build;  /* a value that differs between builds whose states do not fit each other */
    uint32_t used;   /* bytes, header included */
    uint32_t frame;
    uint32_t markHi; /* port_ram offsets */
    uint32_t markLo;
    uint32_t rom;    /* a hash of the ROM */
} StateHeader;

_Static_assert(sizeof(StateHeader) == ENGINE_STATE_HEADER, "the state header's size");

struct Engine {
    uint8_t *rom;
    unsigned long romSize;
    uint32_t romHash;
    uint32_t frame;
    uint32_t heapStart; /* port_ram offset of the heap's start */
    int verbose;
};

static Engine engine;
static int created;
int engine_verbose; /* host.c prints the game's messages when it is set */

void engine_default_options(EngineOptions *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->startLevel = -1;
    opt->eepromFill = 0xFF;
}

static size_t data_size(void) {
    return __stop_gstate_data - __start_gstate_data;
}

static size_t bss_size(void) {
    return __stop_gstate_bss - __start_gstate_bss;
}

/* The memory that ram.S places after the heap. */
static unsigned char *hw_start(void) {
    return port_ram + HEAP_TOP;
}

static size_t hw_size(void) {
    return __stop_gram_bss - hw_start();
}

/* Where port_rom and port_rom_size lie in the game's bss. They describe this process's copy of the ROM: a state holds
   neither (engine_save stores 0). */
static size_t rom_slot(void) {
    return (unsigned char *)&port_rom - __start_gstate_bss;
}

static size_t rom_size_slot(void) {
    return (unsigned char *)&port_rom_size - __start_gstate_bss;
}

/* The end of the program's code (both GNU ld's ELF script and MinGW's PE script provide it). */
extern char etext[];

/* A value that differs when the program's layout differs: a state holds pointers to the program's code and data. */
static uint32_t build_value(void) {
    uint32_t h = 2166136261u;
    uint32_t v[7];
    int i;

    v[0] = (uint32_t)(uintptr_t)port_ram;
    v[1] = (uint32_t)(uintptr_t)__start_gstate_data;
    v[2] = (uint32_t)data_size();
    v[3] = (uint32_t)(uintptr_t)__start_gstate_bss;
    v[4] = (uint32_t)bss_size();
    v[5] = (uint32_t)(uintptr_t)&engine_step;
    v[6] = (uint32_t)(uintptr_t)etext;
    for (i = 0; i < 7; i++) {
        h = (h ^ v[i]) * 16777619u;
    }
    return h;
}

static uint32_t hash_rom(const uint8_t *rom, unsigned long size) {
    uint32_t h = 2166136261u;
    unsigned long i;

    for (i = 0; i < size; i++) {
        h = (h ^ rom[i]) * 16777619u;
    }
    return h;
}

static int load_rom(Engine *e, const char *path, char *err, size_t errSize) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *rom;
    long i;

    if (f == NULL) {
        snprintf(err, errSize, "cannot open the ROM %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x100000 || size > 0x4000000) {
        fclose(f);
        snprintf(err, errSize, "%s is not an N64 ROM", path);
        return -1;
    }
    rom = malloc(size);
    if (rom == NULL || fread(rom, 1, size, f) != (size_t)size) {
        fclose(f);
        free(rom);
        snprintf(err, errSize, "cannot read the ROM %s", path);
        return -1;
    }
    fclose(f);
    /* The game wants big-endian (.z64) byte order: .v64 swaps each pair of bytes, .n64 each word. */
    if (rom[0] == 0x37 && rom[1] == 0x80) {
        for (i = 0; i + 1 < size; i += 2) {
            uint8_t t = rom[i];
            rom[i] = rom[i + 1];
            rom[i + 1] = t;
        }
    } else if (rom[0] == 0x40 && rom[1] == 0x12) {
        for (i = 0; i + 3 < size; i += 4) {
            uint8_t t = rom[i];
            rom[i] = rom[i + 3];
            rom[i + 3] = t;
            t = rom[i + 1];
            rom[i + 1] = rom[i + 2];
            rom[i + 2] = t;
        }
    }
    if (rom[0] != 0x80 || rom[1] != 0x37 || memcmp(rom + 0x3B, "NGVE", 4) != 0) {
        free(rom);
        snprintf(err, errSize, "%s is not Glover (USA, game code NGVE)", path);
        return -1;
    }
    e->rom = rom;
    e->romSize = size;
    e->romHash = hash_rom(rom, size);
    return 0;
}

Engine *engine_create(const char *romPath, const EngineOptions *opt, char *err, size_t errSize) {
    EngineOptions def;
    Engine *e = &engine;

    if (created) {
        snprintf(err, errSize, "one process holds one game");
        return NULL;
    }
    if (opt == NULL) {
        engine_default_options(&def);
        opt = &def;
    }
    memset(e, 0, sizeof(*e));
    if (load_rom(e, romPath, err, errSize) != 0) {
        return NULL;
    }
    created = 1;
    e->verbose = opt->verbose;
    engine_verbose = opt->verbose;
    if (opt->eeprom != NULL) {
        memcpy(port_eeprom, opt->eeprom, sizeof(port_eeprom));
    } else {
        memset(port_eeprom, opt->eepromFill, sizeof(port_eeprom));
    }
    port_rom = e->rom;
    port_rom_size = e->romSize;
    e->heapStart = engine_symbol("jallocHeapStart") - RAM_BASE;
    if (track_init(__start_gstate_data, __stop_gstate_data, __start_gstate_bss, __stop_gstate_bss, hw_start(),
                   (uint32_t)hw_size(), e->heapStart, HEAP_TOP, getenv("GLOVER_FULL_STATES") != NULL) != 0) {
        snprintf(err, errSize, "cannot protect the game's memory");
        return NULL;
    }
    port_game_init(opt->startLevel);
    return e;
}

Engine *engine_attach(const uint8_t *rom, unsigned long romSize, char *err, size_t errSize) {
    Engine *e = &engine;

    if (created) {
        snprintf(err, errSize, "one process holds one game");
        return NULL;
    }
    memset(e, 0, sizeof(*e));
    e->rom = (uint8_t *)rom;
    e->romSize = romSize;
    e->romHash = hash_rom(rom, romSize);
    created = 1;
    e->heapStart = engine_symbol("jallocHeapStart") - RAM_BASE;
    if (track_init(__start_gstate_data, __stop_gstate_data, __start_gstate_bss, __stop_gstate_bss, hw_start(),
                   (uint32_t)hw_size(), e->heapStart, HEAP_TOP, getenv("GLOVER_FULL_STATES") != NULL) != 0) {
        snprintf(err, errSize, "cannot protect the game's memory");
        return NULL;
    }
    return e;
}

void engine_free(Engine *e) {
    /* The game cannot be stopped and started again in one process. */
    (void)e;
}

void engine_step(Engine *e, const EngineInput *in) {
    PortFrameTask tasks[16];

    port_buttons = in->buttons;
    port_stick_x = in->x;
    port_stick_y = in->y;
    port_game_frame(tasks, 16);
    e->frame++;
}

void engine_count_logic(Engine *e) {
    e->frame++;
}

uint32_t engine_frame(const Engine *e) {
    return e->frame;
}

size_t engine_state_size(const Engine *e) {
    (void)e;
    return sizeof(StateHeader) + 8 * (size_t)track_count(HEAP_TOP, HEAP_TOP) + track_body_size(HEAP_TOP, HEAP_TOP);
}

static size_t used_for(uint32_t hi, uint32_t lo) {
    return sizeof(StateHeader) + 8 * (size_t)track_count(hi, lo) + track_body_size(hi, lo);
}

static size_t state_used(const Engine *e) {
    (void)e;
    return used_for(gap_hi, gap_lo);
}

static int header_ok(const Engine *e, const StateHeader *h);

size_t engine_save(Engine *e, void *buffer) {
    uint8_t *p = buffer;
    StateHeader h;
    StateHeader old;
    uint32_t hi = gap_hi;
    uint32_t lo = gap_lo;
    uint8_t *body = p + sizeof(h) + 8 * (size_t)track_count(hi, lo);
    int delta;

    /* A buffer that holds a state of the same layout gets only the pieces that changed. */
    memcpy(&old, p, sizeof(old));
    delta = header_ok(e, &old) && old.markHi == hi && old.markLo == lo;
    memset(&h, 0, sizeof(h));
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.build = build_value();
    h.used = (uint32_t)state_used(e);
    h.frame = e->frame;
    h.markHi = hi;
    h.markLo = lo;
    h.rom = e->romHash;
    track_save(p + sizeof(h), body, delta);
    memset(body + data_size() + rom_slot(), 0, sizeof(void *));
    memset(body + data_size() + rom_size_slot(), 0, sizeof(port_rom_size));
    memcpy(p, &h, sizeof(h));
    return h.used;
}

/* 1 when the header belongs to a state of this program and ROM. */
static int header_ok(const Engine *e, const StateHeader *h) {
    return h->magic == STATE_MAGIC && h->version == STATE_VERSION && h->build == build_value() &&
           (e == NULL || h->rom == e->romHash);
}

size_t engine_state_used(const void *buffer) {
    StateHeader h;

    memcpy(&h, buffer, sizeof(h));
    return header_ok(created ? &engine : NULL, &h) ? h.used : 0;
}

/* After a load: this process's ROM, which a state does not hold. Written only when it differs: the page may be
   read-only again (track.c). */
static void restore_rom(void) {
    if (port_rom != engine.rom) {
        port_rom = engine.rom;
    }
    if (port_rom_size != engine.romSize) {
        port_rom_size = engine.romSize;
    }
}

size_t engine_state_data(const void *buffer) {
    StateHeader h;

    memcpy(&h, buffer, sizeof(h));
    return sizeof(h) + 8 * (size_t)track_count(h.markHi, h.markLo);
}

void engine_identity(const Engine *e, uint32_t *layout, uint32_t *rom) {
    *layout = build_value();
    *rom = e->romHash;
}

int engine_load(Engine *e, const void *buffer) {
    const uint8_t *p = buffer;
    StateHeader h;

    memcpy(&h, p, sizeof(h));
    if (!header_ok(e, &h) || h.used != used_for(h.markHi, h.markLo)) {
        return -1;
    }
    track_load(p + sizeof(h), p + sizeof(h) + 8 * (size_t)track_count(h.markHi, h.markLo), h.markHi, h.markLo,
               restore_rom);
    e->frame = h.frame;
    return 0;
}

const uint8_t *engine_ram(const Engine *e) {
    (void)e;
    return port_ram;
}

uint32_t engine_n64_address(const Engine *e, uint32_t pointer) {
    uint32_t base = PORT_PTR_BASE;

    (void)e;
    if (pointer < base || pointer >= base + HEAP_TOP) {
        return 0;
    }
    return pointer - base + RAM_BASE;
}

uint32_t engine_host_to_n64(const void *p) {
    const unsigned char *b = (const unsigned char *)p;

    if (b < port_ram || b >= port_ram + HEAP_TOP) {
        return 0;
    }
    return (uint32_t)(b - port_ram) + RAM_BASE;
}

static const uint8_t *at(uint32_t addr, uint32_t size) {
    if (addr < RAM_BASE || addr - RAM_BASE > HEAP_TOP - size) {
        return NULL;
    }
    return port_ram + (addr - RAM_BASE);
}

uint32_t engine_read32(const Engine *e, uint32_t addr) {
    const uint8_t *p = at(addr, 4);
    uint32_t v = 0;

    (void)e;
    if (p != NULL) {
        memcpy(&v, p, 4);
    }
    return v;
}

uint16_t engine_read16(const Engine *e, uint32_t addr) {
    const uint8_t *p = at(addr, 2);
    uint16_t v = 0;

    (void)e;
    if (p != NULL) {
        memcpy(&v, p, 2);
    }
    return v;
}

uint8_t engine_read8(const Engine *e, uint32_t addr) {
    const uint8_t *p = at(addr, 1);

    (void)e;
    return p != NULL ? *p : 0;
}

float engine_readf(const Engine *e, uint32_t addr) {
    uint32_t v = engine_read32(e, addr);
    float f;

    memcpy(&f, &v, 4);
    return f;
}

int engine_write(Engine *e, uint32_t addr, int size, uint32_t value) {
    uint8_t *p = (uint8_t *)at(addr, (uint32_t)size);

    (void)e;
    if (p == NULL || (size != 1 && size != 2 && size != 4)) {
        return -1;
    }
    if (size == 1) {
        *p = (uint8_t)value;
    } else if (size == 2) {
        uint16_t v = (uint16_t)value;

        memcpy(p, &v, 2);
    } else {
        memcpy(p, &value, 4);
    }
    return 0;
}

uint32_t engine_readptr(const Engine *e, uint32_t addr) {
    return engine_n64_address(e, engine_read32(e, addr));
}

static int compare_symbol(const void *key, const void *item) {
    return strcmp((const char *)key, ((const EngineSymbol *)item)->name);
}

uint32_t engine_symbol(const char *name) {
    const EngineSymbol *s = bsearch(name, engine_symbols, engine_symbol_count, sizeof(EngineSymbol), compare_symbol);

    return s != NULL ? s->addr : 0;
}

static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    size_t i;

    /* FNV-1a over 8-byte words. The shift mixes the product's high bits back down: without it, two differences in
       the high bits of different words can cancel out. */
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;

        memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 32;
    }
    for (; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ull;
        h ^= h >> 32;
    }
    return h;
}

uint64_t engine_hash(Engine *e) {
    uint64_t h = 1469598103934665603ull;

    size_t a = rom_slot() < rom_size_slot() ? rom_slot() : rom_size_slot();
    size_t b = rom_slot() < rom_size_slot() ? rom_size_slot() : rom_slot();

    /* The whole heap, not only the part a state holds: the marks depend on the game's history, the state does
       not. The ROM's address and size are left out, as a save leaves them out. */
    h = (h ^ e->frame) * 1099511628211ull;
    h = hash_words(h, __start_gstate_data, data_size());
    h = hash_words(h, __start_gstate_bss, a);
    h = hash_words(h, __start_gstate_bss + a + 4, b - a - 4);
    h = hash_words(h, __start_gstate_bss + b + 4, bss_size() - b - 4);
    h = hash_words(h, hw_start(), hw_size());
    h = hash_words(h, port_ram, HEAP_TOP);
    return h;
}

#if defined(_WIN32)
#include <windows.h>
extern IMAGE_DOS_HEADER __ImageBase;

/* The compiler sees __ImageBase as a single header and warns about reading the headers that follow it. */
#pragma GCC diagnostic ignored "-Warray-bounds"
static void image_range(uint32_t *start, uint32_t *end) {
    const char *base = (const char *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);

    *start = (uint32_t)(uintptr_t)base;
    *end = *start + nt->OptionalHeader.SizeOfImage;
}
#else
extern char __executable_start[];
extern char _end[];

static void image_range(uint32_t *start, uint32_t *end) {
    *start = (uint32_t)(uintptr_t)__executable_start;
    *end = (uint32_t)(uintptr_t)_end;
}
#endif

uint64_t engine_check_hash(const Engine *e) {
    uint32_t ramStart = PORT_PTR_BASE;
    uint32_t ramEnd = ramStart + (uint32_t)(port_hw_end - port_ram);
    uint32_t imageStart;
    uint32_t imageEnd;
    const uint32_t *p = (const uint32_t *)port_ram;
    const uint32_t *end = (const uint32_t *)port_hw_end;
    uint64_t h = 1469598103934665603ull;

    image_range(&imageStart, &imageEnd);
    h = (h ^ e->frame) * 1099511628211ull;
    for (; p < end; p++) {
        uint32_t w = *p;

        if (w >= ramStart && w < ramEnd) {
            w = (w - ramStart) ^ 0xA5000000u;
        } else if (w >= imageStart && w < imageEnd) {
            w = 0xC0DEC0DEu;
        }
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 32;
    }
    return h;
}

void engine_trace(const Engine *e, char *buf, size_t size) {
    (void)e;
    port_game_trace(buf, (int)size);
}

size_t engine_check_gap(const Engine *e) {
    const uint32_t *p = (const uint32_t *)(port_ram + gap_hi);
    const uint32_t *end = (const uint32_t *)(port_ram + gap_lo);
    size_t n = 0;

    (void)e;
    for (; p < end; p++) {
        n += *p != 0;
    }
    return n;
}

void engine_state_info(const Engine *e, EngineStateInfo *info) {
    info->gameVars = data_size() + bss_size() + hw_size();
    info->lowRam = e->heapStart;
    info->heapLow = gap_hi - e->heapStart;
    info->heapHigh = HEAP_TOP - gap_lo;
    info->total = state_used(e);
}
