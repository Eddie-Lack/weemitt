# How the port works

The port runs the decompiled game's C on a PC. Most of the game's code is unchanged. This file explains the parts
that are new: the memory, the data from the ROM, the frame loop and the replacements for the N64's libraries.

## The program

| Part | Where | What it does |
| --- | --- | --- |
| The game | `src/game/`, `include/` | the decompilation's C, with the changes the PC needs, compiled as C++ |
| The runtime | `src/port/` | the N64 library calls the game makes, the ROM loaders, the frame driver, the platform layer |
| The renderer | `src/gfx/` | draws the game's display lists with OpenGL 3.3 |
| The sound | `src/audio/` | plays the game's music and sound effects |
| Generated tables | `gen/` | the game's memory layout and the conversions of its data (from `tools/gen/`) |

The game's code is a 32-bit program, as on the N64. Its structures hold 32-bit pointers, and data from the ROM holds
them too. The port is built for 32-bit x86 by default: `gcc -m32` on Linux, `i686-w64-mingw32-gcc` for Windows. The
same sources also build for x86-64 (`make linux64`, `make windows64`, see [port64.md](port64.md)), where the pointers
in the game's memory stay 32-bit words. The game's files and the sound player are C, compiled as C++ with C linkage
(`g++`, `i686-w64-mingw32-g++` or `x86_64-w64-mingw32-g++`). They are compiled with N64 structure alignment
(`-malign-double` on 32-bit x86; x86-64 has that alignment already), unsigned `char`, wrapping arithmetic and SSE
floating point. The platform layer and the renderer use the system's own
alignment, and talk to the game only through `src/port/frame.h`, which uses plain C types.

## Memory

`port_ram` is one block of 24 MB that stands for the N64's RDRAM: the N64 address `0x80000000 + n` is
`port_ram + n`. Every global variable of the game sits at its N64 offset in it (`gen/ram.S`), and the game's heap
runs from its usual start to 16 MB past the end of RDRAM. The game's data therefore keeps the N64's layout, and
code that relies on that layout (a table read one entry past its end, for example) behaves as on the N64.

A pointer that the game stores in its memory is a 32-bit word: `Ptr32<T>`, `Fn32<F>` or `PortMesg` in the
structures and globals of `port_ram`, and the same word in the integers that hold addresses (display-list commands,
the heap's block table). The word is `PORT_PTR_BASE` plus the offset into `port_ram`. On the normal builds
`PORT_PTR_BASE` is the address of `port_ram`, so the word is the host pointer itself. A 64-bit build and the
displaced test build use a fixed base ([port64.md](port64.md)).

The 32-bit program is linked at a low address (not position-independent); the 64-bit program is
position-independent. libultra masks addresses with `0x1FFFFFFF` to make physical addresses for the graphics chip.
The words are all below `0x20000000`, so the mask leaves them unchanged, and the renderer adds `PORT_PTR_DELTA` (0 on
the normal builds) to reach the memory.

On Linux `port_ram` starts at a 32 MB boundary (`gen/ram.S`). The linker picks which one: it is `0x0C000000` in the
Linux and headless builds and `0x0A000000` in the studio. The memory hash of the regression test depends on that
address ([delta-time.md](delta-time.md#verification)).

## Data from the ROM

The player's ROM is loaded into memory at start-up. The N64 is big-endian and the PC little-endian, so data is
converted where it enters the game:

- The main segment's data: `port_load_ram` (`src/port/loaders.c`) copies it from the ROM, swaps each 2, 4 and 8
  byte field (`port_swap2`, `port_swap4`, `port_swap8` in `gen/ramtab.c`), turns N64 pointers into Ptr32 words
  (`port_ptrs`) and puts the words of the port's functions in the data that points to functions (`port_fnptrs`,
  an index into `port_fn_table`).
- Texture banks, object banks, level files and demo recordings are converted as they load (`loaders.c`). The
  level parser reads each field through `PORT_COPY`, which knows the field's size.
- ROM asset addresses (`xxxRomStart`) are read from the code in the player's ROM (`gen/romsyms.c`).

`tools/gen/` writes the tables from a built gloveworld checkout. The types come from compiling the port's own
game sources for big-endian MIPS with debug information. `tools/gen/extra_types.c` gives the types of data that
only the port's code reads. `tools/check_layouts.py` checks that every structure has the N64's layout on the PC.

## The frame

On the N64, `main.c` runs three threads that wait for the scheduler's message: `ControllerProc` reads the pad,
`RunGame` updates the game and `DrawGraphics` builds the display lists. The scheduler sends the message once every
`desiredFrameRate` video retraces: every third in the levels (20 frames a second) and every second in the menus
(30 a second).

The port runs the same work on one thread. `main.c`'s thread bodies became `ControllerStep`, `GameStep` and
`GraphicsStep`. `port_game_update` (`src/port/frame.c`) runs the three in the N64 threads' priority order
(the controller, the graphics, then the game) and returns the frame's display lists, which the renderer draws. Waits for another thread became immediate: `StartDrawing` and `StopDrawing` set
`gfxIsDrawing` themselves, and an EEPROM request is served when it is posted.

The platform layer (`src/port/platform.c`) has two ways to call `port_game_update`. In the Smooth mode it calls it
once per drawn frame, for the time that has passed. [delta-time.md](delta-time.md) explains how the game's logic
handles updates of any length. In the Original mode it calls it once per original frame with a length of 1, and the
renderer blends the previous and the latest frame when it draws ([interpolation.md](interpolation.md)). Three rules of
the frame driver matter for the other files:

- The game's logic counts original frames. One original frame is a window of one or more updates, and the last of
  them is the tick. The first update after a change of game mode, or after `frameCount` restarts, is a whole frame
  (`deltaTime` 1, a tick), whatever time has passed.
- `ControllerStep` runs in the first update of a window, not in the tick. The frame's stick and buttons are then
  available to the whole window, as on the N64. The button presses wait for the tick: the other updates clear them.
- `GraphicsStep` runs before `GameStep` in every update. Outside a tick the driver restores the random seed after it.

## The N64's libraries

| Library | In the port |
| --- | --- |
| libultra (OS: threads, messages, DMA, controllers, EEPROM, timers) | `src/port/os.c`: no threads; DMA copies at once; the controller comes from the platform layer; the EEPROM is a 512-byte file |
| libultra (gu matrix functions) | `src/port/gu.c` |
| libultra (sine, cosine) | `src/port/libultra.c`: the N64's polynomial approximations, bit for bit |
| libultra (sp sprite functions) | `src/port/sprite.c` |
| F3DEX and Sprite2D microcode, the RDP | `src/gfx/` |
| libmus (music and sound effects) | `src/audio/mus/`, translated from the ROM; `src/port/audio_stub.c` is a silent stand-in that the default build uses |
| libaudio's synthesizer | `src/audio/synth/`, translated from the ROM; it builds the same audio command lists as the ROM |
| The audio microcode (aspMain) | `src/audio/synth/rsp_audio.c`: runs the command lists on the PC, bit for bit as the microcode |

## Differences from the N64 code

The game's C changed in these ways:

- The threads became step functions (`main.c`); the waits for other threads went (`banks.c`, `eeprom.c`).
- Functions that read big-endian data are in `src/port/loaders.c` instead of `banks.c` and `jalloc.c`.
- Two screen functions now pass on the arguments that the N64 passed on in registers (`title.c`, `frontend2.c`).
  `tools/check_calls.py` finds such calls.
- The game's own `memset` went; the C library's does the same.
- The game's files compile as C++ as well as C: casts of `void *` and between character pointers, a few prototypes,
  no external `inline` functions ([port64.md](port64.md)).
- Game functions with C library names (`atan`, `atan2`, `acos`, `strncmp`) are renamed by `port_pre.h`.
- The game's code is built without the compiler optimisations that assume no out-of-bounds read: the N64 code
  reads one entry past an array in places, which the original compiler compiled as written.
- The frame-rate rewrite ([delta-time.md](delta-time.md)).
