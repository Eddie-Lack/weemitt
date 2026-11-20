/* The engine: Glover's game logic (weemitt's headless game) behind a small C interface.

   One process holds one game: the game's variables are globals. engine_create may be called once. The game's memory,
   code and variables sit at the same addresses in every process of one program (one executable). So a state saved in
   one process loads in another process of the same program, with the same ROM (the workers share states this way).
   Another program places them elsewhere and refuses the state. All the tools are one program, glover (src/main.c). */
#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>

/* Controller 1 for one game frame: the N64's button word (libultra's CONT_ bits, below) and the stick. */
typedef struct EngineInput {
    uint16_t buttons;
    int8_t x;
    int8_t y;
} EngineInput;

enum {
    BTN_CR = 0x0001, /* C-right */
    BTN_CL = 0x0002, /* C-left */
    BTN_CD = 0x0004, /* C-down */
    BTN_CU = 0x0008, /* C-up */
    BTN_R = 0x0010,
    BTN_L = 0x0020,
    BTN_DR = 0x0100, /* D-pad right */
    BTN_DL = 0x0200,
    BTN_DD = 0x0400,
    BTN_DU = 0x0800,
    BTN_START = 0x1000,
    BTN_Z = 0x2000,
    BTN_B = 0x4000,
    BTN_A = 0x8000
};

typedef struct EngineOptions {
    /* -1: the game starts at power-on. 0 or more: it starts this level directly, as weemitt's --level does (save
       slot 6, medium difficulty). */
    int startLevel;
    /* The EEPROM at power-on: eeprom's 512 bytes when it is not NULL, or else 512 bytes of eepromFill. A new
       EEPROM in mupen64plus holds 0xFF. */
    const uint8_t *eeprom;
    int eepromFill;
    /* 1: print the game's messages on standard error. */
    int verbose;
} EngineOptions;

/* The defaults: power-on, a new EEPROM (0xFF), no messages. */
void engine_default_options(EngineOptions *opt);

typedef struct Engine Engine;

/* Loads the ROM (.z64, .v64 or .n64 byte order, Glover USA) and starts the game. Returns NULL with a message in err
   when it cannot. */
Engine *engine_create(const char *romPath, const EngineOptions *opt, char *err, size_t errSize);
void engine_free(Engine *e);

/* For a program that loads the ROM and starts the game itself (the studio, src/port/studio.c): sets up the
   savestates for that game. Call it before port_game_init, after any crash handler is installed. rom is the ROM in
   .z64 byte order, as port_rom holds it; it must stay in memory. NULL with a message in err when it cannot. The
   program then counts the game frames it runs with engine_count_logic. */
Engine *engine_attach(const uint8_t *rom, unsigned long romSize, char *err, size_t errSize);
void engine_count_logic(Engine *e);

/* One game frame with this controller input: the game reads the controller once, then builds the frame's display
   lists and runs its logic, as on the N64. */
void engine_step(Engine *e, const EngineInput *in);

/* The three parts of a game frame, for replaying the N64's order of events where it is not one frame at a time
   (docs/sync.md). On the N64 each frame message wakes three threads: the controller thread reads the controller
   (engine_read), the draw thread builds a frame (engine_draw) and the game thread runs the game's logic
   (engine_logic), in this order of priority. When the game thread is busy (a level load), the other two go on, and
   the game thread later runs its queued frames one after another. engine_step is the three in order. These work
   for a game started at power-on only (EngineOptions.startLevel -1). */
void engine_read(Engine *e, const EngineInput *in);
void engine_draw(Engine *e);
void engine_logic(Engine *e);

/* A logic frame with stops. On the N64 the read and the draw can come in the middle of a logic frame, wherever the
   game thread is (docs/sync.md). The engine can stop a logic frame where it starts or returns from a call of one of
   the functions in engine/hooks.def. A stop names the hooked calls in progress there, from the outermost in, as
   the game thread's stack shows them: for each, the function (engine_hook_index) and the N64 address of its
   pointer argument, or ENGINE_ANY. */
#define ENGINE_STOP_DEPTH 6
#define ENGINE_ANY 0xFFFFFFFFu
typedef struct EngineStop {
    int depth;
    int hook[ENGINE_STOP_DEPTH];
    uint32_t arg[ENGINE_STOP_DEPTH];
    int nth;   /* 0: the first such call in the frame, 1: the second, and so on */
    int leave; /* 0: where the innermost call starts; 1: where it returns */
} EngineStop;
int engine_hook_index(const char *name); /* -1 when the function is not hooked */
const char *engine_hook_name(int index);
typedef void (*EngineStopFn)(Engine *e, void *ctx, int stop);
/* Runs one logic frame (engine_logic) and calls fn at each stop, in the order of the list. Returns the number of
   stops that the frame did not reach. */
int engine_logic_stops(Engine *e, const EngineStop *stops, int count, EngineStopFn fn, void *ctx);

/* The trimmed draw (on: 1). The draw then skips the work that only builds the picture (engine/hooks.def, SKIP0):
   the game's logic stays the same, frame by frame, and a frame takes less time. The picture and the display lists
   are wrong. Off by default. */
void engine_trim_draw(Engine *e, int on);

/* Game logic frames since the start (engine_step and engine_logic count them). Part of the state. */
uint32_t engine_frame(const Engine *e);

/* Savestates. A buffer of engine_state_size bytes holds any state. engine_save returns the bytes it used, which is
   less while the game's heap is small. engine_load returns 0, or -1 when the state comes from another program or
   was saved with another ROM. A state starts with a header of ENGINE_STATE_HEADER bytes. */
#define ENGINE_STATE_HEADER 32
size_t engine_state_size(const Engine *e);
size_t engine_save(Engine *e, void *buffer);
int engine_load(Engine *e, const void *buffer);
/* The bytes a saved state in buffer uses (from its header), or 0 when it is not a state of this program and ROM.
   buffer must hold at least ENGINE_STATE_HEADER bytes. */
size_t engine_state_used(const void *buffer);
/* Where the state's game data starts in buffer. Before it come the header and the versions of the state's pieces,
   which name the content of each page for cheap saves and loads: two saves of the same game can differ there. */
size_t engine_state_data(const void *buffer);
/* What a state's header names, as this process would write it: a value of the program's layout (where the game's
   memory, variables and code lie) and a hash of the ROM. */
void engine_identity(const Engine *e, uint32_t *layout, uint32_t *rom);

/* The game's memory, read-only: N64 address 0x80000000 + n is engine_ram(e)[n]. Values are in the PC's byte order,
   and a pointer in the game's data is a Ptr32 word (weemitt's src/port/ptr32.h): engine_n64_address turns it into
   an N64 address. engine_host_to_n64 does the same for a pointer of this process into the game's memory. Out of
   range: 0. */
const uint8_t *engine_ram(const Engine *e);
uint32_t engine_n64_address(const Engine *e, uint32_t pointer);
uint32_t engine_host_to_n64(const void *p);

/* Reads at an N64 address. Out of range: 0. */
uint32_t engine_read32(const Engine *e, uint32_t addr);
uint16_t engine_read16(const Engine *e, uint32_t addr);
uint8_t engine_read8(const Engine *e, uint32_t addr);
float engine_readf(const Engine *e, uint32_t addr);
/* Follows a pointer of the game's data at addr: the N64 address it points to, or 0 for NULL. */
uint32_t engine_readptr(const Engine *e, uint32_t addr);

/* Writes the game's memory at an N64 address, in the PC's byte order: size 1, 2 or 4 bytes. For values that depend
   on the emulator's timing (docs/sync.md). Returns 0, or -1 out of range. */
int engine_write(Engine *e, uint32_t addr, int size, uint32_t value);

/* The N64 address of a game global (gloveworld's names), or 0 when there is none. */
uint32_t engine_symbol(const char *name);

typedef struct EngineSymbol {
    const char *name;
    uint32_t addr;
} EngineSymbol;
extern const EngineSymbol engine_symbols[];
extern const unsigned engine_symbol_count;

/* A 64-bit hash of the whole game state, as a save would hold it. Slow: for tests. */
uint64_t engine_hash(Engine *e);

/* A hash of the game's memory that every build for one system (Linux or Windows) gives, whatever its options:
   pointers to the game's memory count as offsets and pointers to the program as one value. The game's variables outside its memory are left out:
   their layout differs between builds. Raw data that looks like such a pointer (sound samples) is changed too, so
   the hash differs between Linux and Windows, where the memory has other addresses. Slow: for tests. */
uint64_t engine_check_hash(const Engine *e);

/* weemitt's one-line state trace: frame counter, mode, level, random seed, the glove's, the ball's and the camera's
   positions (floats as bits). */
void engine_trace(const Engine *e, char *buf, size_t size);

/* For tests: the number of nonzero bytes in the part of the heap that a state does not hold. It must be 0. */
size_t engine_check_gap(const Engine *e);

/* Sizes of the parts of a state, for the benchmark. */
typedef struct EngineStateInfo {
    size_t gameVars;   /* the game's variables outside its memory */
    size_t lowRam;     /* the N64's data and bss, below the heap */
    size_t heapLow;    /* the heap's lower part in use (dynamic blocks) */
    size_t heapHigh;   /* the heap's upper part in use (static blocks) */
    size_t total;
} EngineStateInfo;
void engine_state_info(const Engine *e, EngineStateInfo *info);

#endif
