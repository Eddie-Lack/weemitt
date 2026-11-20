# The 64-bit port

The game's structures hold 32-bit pointers (see [architecture.md](architecture.md)), so the port was first a 32-bit
program only. The same sources now also build as 64-bit programs, for use inside other programs, such as game
engines and Steam games. This file describes how: the game as C++, the pointer words (`Ptr32`), the 64-bit builds
and the game as a library. `make linux64`, `make windows64` and `make headless64` build the 64-bit programs
([building.md](building.md)). `make lib64` and `make lib-windows64` build the game as a library
([library.md](library.md)), also as 32-bit (`make lib32`, `make lib-windows32`).

## The approach

A class template `Ptr32<T>` is 4 bytes wide and holds a word for a place in `port_ram`. It converts to and from
`T *`, so `p->field`, `p[i]` and `*p` keep working. The pointer fields of the structures that keep the N64's layout
use it. Their layout then stays the same on a 64-bit host, which `tools/check_layouts.py` proves.

1. The game is compiled as C++, also on 32-bit. With `Ptr32<T>` as a plain pointer the logic is the same as in C.
2. Every pointer of the game's memory is stored as a 32-bit word, `Ptr32<T>`.
3. The 64-bit builds (`-m64`, and the 64-bit Windows target) cast between pointers and integers only through the
   port's macros. The loaders and the display-list readers in `src/gfx/` read words.
4. The game also builds as a library with no window.

## The game as C++

### The build

The Makefile compiles `src/game/*.c` with `g++` (`i686-w64-mingw32-g++` for Windows). `GAME_FLAGS` holds the flags
that both languages share. The C++ files add `GAME_CXXFLAGS`:

| Flag | Why |
| --- | --- |
| `-x c++` | the files keep their `.c` names |
| `-std=gnu++14` | the same standard as the menu (`HOST_CXXFLAGS`) |
| `-fno-exceptions -fno-rtti` | the game throws nothing; the objects need nothing from libstdc++ |
| `-fno-finite-loops` | C++ lets GCC assume that a loop without side effects ends. `eeprom.c` waits with `while (eepromMessageQueue[0] != 0) {}`, which `g++` would remove |

The rest of the runtime (`src/port`) and `gen/` stay C, with `GAME_CFLAGS` (`-std=gnu89` and `GAME_FLAGS`). The
runtime's files that read the game's structures (`PORT_CXX_SRCS`) and the sound player (`src/audio`, below) are C++
too. The brute forcer's Makefile reads `GAME_CXXFLAGS` from the main Makefile and compiles the same
files the same way. The headless build and the brute forcer still link with `gcc`: the game's objects use no libstdc++
symbol.

### C linkage

Every symbol keeps its C name. The port's C code, `gen/ram.S`, `gen/ramtab.c`, the brute forcer's `--wrap` hooks
(`bruteforce/engine/hooks.def`) and the tools all use these names.

- `port_pre.h` defines `PORT_C_BEGIN` and `PORT_C_END`: `extern "C" {` and `}` in C++, nothing in C. Each game file
  starts with `PORT_C_BEGIN` before its first `#include` and ends with `PORT_C_END`. So the game's headers need no
  change, and each function and global gets C linkage.
- `port_pre.h` wraps its own declarations and `port_fixups.h` in the same pair.
- C gives a global `const` object external linkage; C++ gives it internal linkage. The nine global `const` arrays
  (`D_80105B9C` and the others: words that follow other data in the ROM) are declared `extern`.
- In GNU C89, a function defined `inline` without `static` or `extern` is also emitted as an ordinary global
  function. In C++ an `inline` function is emitted only where a file needs a copy, as a weak symbol. The 55 such
  functions are not `inline` in the port. Other files call many of them, and `gen/ramtab.c` takes the address of `doCopyMode`.

A static variable at file scope gets a C++ name in the object's local symbols (`_ZL10sphereHits` for `sphereHits`).
Language linkage does not apply to it. Debuggers show the plain name. The global symbols are the same as in a C
build.

### Math

libstdc++'s `<math.h>` and `<stdlib.h>` put the C++ overloads in the global namespace. With them, `sqrt(f)` and
`fmod(f, g)` on a `float` would compute in `float`, and `abs(f)` would return a `float` where C converts to `int`
(`object.c` does `abs(f[0][0] - f[1][0])` on floats). `port_pre.h` defines `_GLIBCXX_INCLUDE_NEXT_C_HEADERS` before
it includes the C library. libstdc++'s wrappers then include the C library's own headers, which declare only the C
functions. `static_assert`s in `port_pre.h` check the types of `sqrt`, `fmod`, `pow`, `fabs` and `abs` for `float`
arguments, and `abs` for `long`.

MinGW chooses its own printf functions for C++11 and the C runtime's (msvcrt) for C89. They format some values
differently. `port_pre.h` sets `__USE_MINGW_ANSI_STDIO` to 0 in C++, which keeps the C runtime's.

### Changes to the game's code

| Kind | Count |
| --- | --- |
| A cast of a `void *` to the target's type (113 of them on the results of `JallocAlloc`) | 127 |
| A cast between pointers to `char`, `signed char` and `unsigned char` (19), or from an `s32` pointer to an `int` pointer (1) | 20 |
| A prototype that C did without: `RestoreDrawList`, `DestroyAndFreeActor`, `StartNewLevel`, `MainTitleWait`, `WaitOKScr`, `TimeTrialScr` | 6 |
| A call that casts an argument to the parameter's type (the bits pass unchanged) | 3 |
| An old-style definition rewritten with a prototype (`WaitOKScr`) | 1 |
| A `const` object that gets an initializer (`noEdges` in `puzzle.c`) | 1 |
| `inline` removed from an external function | 55 |
| `extern` added to a global `const` array | 9 |
| `PORT_C_BEGIN` and `PORT_C_END` added to a game file | 57 |

`port_fixups.h` has a C++ form of `PORT_UNIT`, which `PORT_COPY` uses in `parse.c` and `nmeprocess.c`. C++ has no
`_Generic`. The template `PortUnit<T>` gives the unit of each type, and a type without one does not compile.
`BulletAcquireTarget` has a full prototype in `port_pre.h`.

The other differences between C and C++ that could change the logic do not occur in the game's code: variables of
an enum type, `sizeof` of a character constant, `goto` over an initialization, keywords of C++ used as names.

## Ptr32

### The representation

`src/port/ptr32.h` defines the types. Every pointer that lives in the game's memory is a 32-bit word W:

- W = 0 for `NULL`;
- otherwise W = `PORT_PTR_BASE` + (p - `port_ram`), which is p - `PORT_PTR_DELTA`.

| Build | Defines | `PORT_PTR_BASE` | `PORT_PTR_DELTA` |
| --- | --- | --- | --- |
| The normal 32-bit builds (Linux, headless, Windows, studio, space demo, brute forcer) | none | the address of `port_ram` | 0 |
| Displaced (`make headless-displaced`) | `PORT_PTR_DISPLACED`, `PORT_PTR_CHECK` | `0x0C000000` | `0x02000000` |
| 64-bit | `__LP64__` or `_WIN64` | `0x0C000000` | `port_ptr_delta`, set by a C++ initializer |

On the normal builds W is the host pointer itself, and every word in memory is the same as with plain pointers. `PORT_PTR_ENCODED` is defined for the displaced and the 64-bit builds.

The types, all of one 32-bit word, trivially copyable:

- `Ptr32<T>` converts to `T *` on its own, and from `T *` by assignment. `->`, `*`, `[]`, `++`, `--`, `+=` and `-=`
  work on it. A cast to an integer (`(u32)p`) gives the word. A cast to another pointer type decodes it. Its
  constructor from `T *` is explicit: in `c ? ptr32 : p` the `Ptr32` becomes a `T *`.
- `Fn32<F>` holds a function pointer. On an encoded build its word is a handle, `PORT_FN_BASE` (`0x0BF00000`) + 4 *
  the index in `port_fn_table` (`gen/ramtab.c`). The sound player's functions follow, from `port_fn_count` on: the
  index in `port_audio_fn_table` (`mus-glue.c`; the stand-in in `audio_stub.c` has none). A call converts it to the
  function first.
- `PortMesg` holds a message (`OSMesg`): a number as it is, a pointer into the game's memory as its word.

The headers write `PTR32(T)`, `FN32(F)` and `MESG32`. In C, and with `-DPORT_PTR32_ALIAS`, these are plain pointers.
In C on an encoded build they are `u32`, and `->` through one does not compile. The MIPS compile of
`check_layouts.py` (`PORT_TYPES_ONLY`) sees plain pointers.

Rules:

- A `Ptr32`, `Fn32` or `PortMesg` is never a parameter or a return value. On i386 a structure is returned through
  memory, which would change the calling convention and break the brute forcer's hooks. `list_pointers.py --check`
  counts any.
- Locals stay `T *`.
- An integer that holds an address holds the word: `JallocBlock.addr`, `JallocControl.heapStart`,
  `PORT_HEAP_TOP`, `TextureBank`, the display-list commands, `Rom2Ram`'s base. `PORT_P2W(p)` and `PORT_W2P(T, w)`
  convert. On the normal builds both are casts.
- `OS_K0_TO_PHYSICAL` works on the word. The address operands of the `gbi.h` commands go through `PORT_GBI_ADDR`:
  a number as it is, a pointer as its word.
- A function that stores through a `T **` and gets a `Ptr32` slot is called with `PORT_OUT(T, &slot)`. A function
  that stores through a `PTR32(T) *` and gets a plain pointer's slot is called with `PORT_SLOT(T, &local)`. Both
  are casts on the normal builds. On an encoded build they pass a temporary, written back at the end of the full
  expression. `FindTexture`, `FindObject`, `JallocFree`, `JallocFreeStatic` and `JallocFreeDynamic` have C++
  overloads that do the same. A callee that stored through the temporary and then read the slot another way would
  read the old value. None does: the callees are `Altitude`, `NMEInitInstruction`, `TileRectangle` and the three
  `font_` functions, and none of them reads the caller's slot.
- A `Ptr32` passed through `...` (to `sprintf`) is passed as its word. Such calls cast it to the pointer type. The
  audit compile (below) finds none left in the game's files and the runtime's C++ files.
- A declaration of a function must have the definition's pointer parameters. `Altitude`, for example, is declared
  once, in `collision.h`, with its `Platform **`. `check_calls.py` lists any parameter whose pointer depth differs
  from the definition's. A function in a table (`port_fn_table`) must take the parameter types that the table's
  calls pass: `HiscoresInit`, `HiscoresDone` and `DoneTitleWait` take `s32 *` and `Screen *` for the screen's
  pointers.

### The displaced build

`make headless-displaced` builds `build/headless-displaced/weemitt`: the headless build with `-DPORT_PTR_DISPLACED
-DPORT_PTR_CHECK`. `gen/ram.S` puts 32 MB of guard (`port_ram_guard`) at `0x0C000000` and `port_ram` at
`0x0E000000`. At start-up the program checks the two addresses and makes the guard unreadable. So:

- a word used as a pointer without decoding faults where it is used;
- a pointer stored without encoding is a `0x0E...` value, which the memory hash takes as it is;
- with `PORT_PTR_CHECK`, storing a pointer that lies outside `[port_ram, port_win_end)` is fatal, with the caller's
  address. This holds for a `Ptr32` and for `PORT_P2W`. So is `->` through a NULL `Ptr32`: with a delta, it would give an address near the delta, not near 0.

`tools/ptrdiff.py` dumps the game's memory of the headless and the displaced build at the same frames and compares
them word by word. Only the function words may differ. These two builds use the stand-in sound player. `make
headless-mus` and `make headless-displaced-mus` build the same pair with the real one (`AUDIO=mus`), and
`ptrdiff.py --audio` compares them. The sound player's memory (`audio_memory`, with its channels, voices, filters
and command lists, and `alGlobals`) is in the game's memory and is compared too. `make check` runs
`check_ptr_slots.py`, `ptrdiff.py`, three runs of `regress.py check` with the displaced build, and `ptrdiff.py
--audio`. The regression baseline holds the stand-in's memory, so `regress.py` cannot check the builds with the real
sound player.

`gen/ram.S` ends the game's memory with `port_hw_end` and then a block, `port_win` to `port_win_end`, that holds the
host objects that the sound player's memory points to: `musGlobals`, `musPlayer`, `musHeap`, `musDmaState`, the
output buffers and `musDmaZeros`. `WINDOW_OBJECTS` in `tools/gen/gen_ram.py` lists them with the bytes reserved for
each, and `gen/port_win.h` gives the sizes to the C code. `mus_internal.h` checks with `static_assert` that each
object fits. Both memory hashes stop at `port_hw_end`.

### What it changes

| Part | Change |
| --- | --- |
| `include/PR/ultratypes.h`, `gbi.h` | `u32` and `s32` are `int` types on a 64-bit host; the `long` members that are 4 bytes on the N64 use them |
| libultra and the scheduler | the members of `OSThread`, `OSMesgQueue`, `OSIoMesg`, `OSTimer`, `OSTask`, `NNSched` and the others; message buffers are `MESG32` |
| The game's structures | 249 pointer members in 78 structures, and one in an unnamed structure in `BulletInfo`; `Mesh.faces`, `uv` and `origUV` get the types `MeshFace` and `MeshUV` |
| Function pointers | `BulletInfo.hitFunc` and `maintainFunc`, `Screen.anims`, `ScreenSpot.func` are `Fn32` |
| Globals | 119 pointer globals and arrays: 232 declarations |
| The runtime | `loaders.c`, `os.c`, `sprite.c`, `frame.c`, `devview.c`, `spacedemo.c`, `debugpad.c`, `libultra.c` and `gu.c` are C++ with C linkage (`PORT_CXX_SRCS`) |
| Loader | `port_ptrs` slots get `PORT_N64_TO_W`, `port_fnptrs` slots `PORT_FN_WORD` of an index into `port_fn_table` |
| Hashes | `hash_ram` and `engine_check_hash` read words through `PORT_PTR_BASE`; the engine's hash ends at `port_hw_end` |
| Renderer | its range comes from `PORT_PTR_BASE` and its base from `PORT_PTR_DELTA`; the base is a `uintptr_t` |
| Brute forcer | `engine_n64_address` reads words; `engine_host_to_n64` converts the hooks' arguments |
| Casts that the displaced build showed to be needed | the heap (`JallocInit`, `JallocAlloc`), the texture and object banks (`Rom2Ram`), the font's bitmaps, the face indices of five loops, `sprintf` of a `Ptr32` string, `OS_K0_TO_PHYSICAL`, the address operands of `gbi.h`, `MusConfig` in `dma.c`, the colour images and dynamic buffers that `frame.c` passes to the renderer |

`MusConfig.wbk` in `dma.c` holds a cartridge address, not a pointer, and is a `u32`.

The heap's block names are not pointers: `JallocAlloc` copies each name into `char name[11]`.

### The sound player

The sound player (`src/audio`) keeps its structures in the libmus heap, `audio_memory` in `port_ram`, and its pbanks
in `port_ram` too. Its parts:

| Part | Change |
| --- | --- |
| C++ | the 15 files are C compiled as C++ with `GAME_CXXFLAGS`, between `PORT_C_BEGIN` and `PORT_C_END`; 81 casts of a `void *`; the layout checks use `static_assert` in C++. The files still compile as C, which `tools/mustest` does |
| Host objects | `musGlobals`, `musPlayer`, `musHeap`, `musDmaState`, the output buffers `musBuffer` and `musDropBuffer`, and `musDmaZeros` are in `port_win` (above) |
| Structures | 87 members in 30 structures of `libaudio.h`, `synth_internal.h` and `mus_internal.h`, and `alGlobals`, are `PTR32` or `FN32`; each function pointer type got a function type for `FN32` (`ALFilterHandlerFn` and the others); `ALSynConfig.dmaproc` is an `FN32(ALDMANewFn)` |
| Numbers | `ALWaveTable.base` and `MusConfig.wbk` hold cartridge addresses and are `u32`; `musBankInit` takes the wbank as a `u32` |
| Functions | `port_audio_fn_table` (`mus-glue.c`) lists the 19 functions that the sound player's structures point to. On an encoded build their handles follow `port_fn_table`'s |
| Reverb presets | `ALSynConfig.params` points to a preset, so libmus reads the ROM's copies of its six presets in `port_ram` (`D_801F4190` and the others), as the synthesizer does for `alFxNew`'s. They hold the same values as the host copies did |
| Command lists | an address in an audio command is a word: `PORT_P2W` or the cast of a `PTR32` member makes it, and the interpreter (`rsp_audio.c`) reads it with `PORT_W2P`. So are `ALSave.dramOut` and the address that the DMA routine returns. The reverb computes its delay line positions on the words |
| Places that need care | `alHeapDBAlloc` stores a word in `ALHeap.cur`. `SYN_SET_PITCH` sends the bits of an `f32` through `SynValue.p`, so `synParamFromBits` and `synParamBits` pass the bits as they are, without encoding. `amDmaNew` stores an encoded pointer in the load filter's `dmaState` |

The command lists are in `port_ram`, so their addresses follow the rule for an integer that holds an address, and
`ptrdiff.py --audio` compares them. `musRamToHost` in `mus-glue.c` needs no `PORT_W2P`: it adds an offset to
`port_ram`, which gives the host pointer on every build. Each object in `port_win` is pointed to from the game's
memory: `musGlobals` and `musPlayer`, and the others by `ALSynth.heap`, by the load filters' `dmaState` and by the
addresses in the command lists.

### The tools that measure it

| Tool | What it counts | On the current code |
| --- | --- | --- |
| `tools/list_pointers.py --check` | raw pointer members of the game's structures, globals in `port_ram` that hold raw pointers, functions that pass a `Ptr32` | 0, also with `--audio` |
| `tools/check_layouts.py --m64` | structures whose layout differs from the N64's in a 64-bit compile; globals whose size differs from the 32-bit build's | 0 structures, 0 globals, also with `--audio` |
| `make check64` | errors of a 64-bit compile of the game's files, the runtime's C++ files and the sound player; then, as a second count, of the program's other C files (platform layer, renderer, studio, brute forcer) | 0 and 0 |
| `tools/check_ptr_slots.py` | the slots that the loader relocates, by the type that holds them | 0 raw pointers; 1,759 of 1,783 data slots and 65 of 65 function slots are `Ptr32`, `Fn32` or `PortMesg` (657 of them in the tails of unsized arrays); 24 data slots lie in data that no file declares |
| `tools/cmpcode.py A B` | functions whose machine code differs between two trees of objects | |
| `tools/ptrdiff.py` | words that differ between the headless and the displaced build's memory | 0, also with `--audio` |

`list_pointers.py` reads the objects built with the game's flags and counts only the structures that the game's
headers, `src/game/` and `src/audio/` declare. `check_layouts.py --audio` also compares the sound player's files.
One of them (`mus-voice.c`) does not compile for MIPS: clang reads the host's `<math.h>`. `PortFn` is declared for
that compile too (`PORT_TYPES_ONLY`), for `port_audio_fn_table` in `mus-glue.c`.

`-DPORT_PTR_AUDIT` is a compile for review, not a build. It removes the casts of a `Ptr32` to an integer and makes a
`Ptr32` passed through `...` a warning (with `-Wconditionally-supported`). Each cast it finds either gives a number
that the game uses as a number (a ROM offset before a bank is converted, a display-list word) or needs `PORT_W2P`.

## The 64-bit build

### The builds

`BITS=64` on the make command line builds any target as an x86-64 program. Four targets name the usual ones:
`linux64`, `windows64`, `headless64` and `headless64-mus` ([building.md](building.md)). The studio, the Space demo
and the brute forcer stay 32-bit.

| Flag | 32-bit | 64-bit |
| --- | --- | --- |
| `ARCH_FLAGS` | `-m32 -msse2 -mfpmath=sse` | `-m64` (SSE arithmetic is the default) |
| `-malign-double` | yes | no: x86-64 aligns doubles and 64-bit integers on 8 bytes, as the N64 does |
| Linux | `-fno-pie`, linked `-no-pie` | `-fpie`, linked `-pie` |
| Windows | `i686-w64-mingw32-g++` | `x86_64-w64-mingw32-g++` |

Both 64-bit programs load above 4 GB. The Linux program is position-independent, and the kernel puts it near
`0x555555554000` (higher, at a random place, when address randomization is on). MinGW's 64-bit image base is `0x140000000`, and the image allows a high-entropy address. A
pointer cut to 32 bits then never points into the program: it faults where it is used. No other image base is set.

`port_ptr_delta` is `(uintptr_t)port_ram - PORT_PTR_BASE`, set by a C++ initializer in `loaders.c`. It holds before
any of the program's code runs, also when the game is linked into another program (see "The game as a library"). `gen/ram.S` aligns
`port_ram` on 32 MB on Linux and on 4096 bytes on Windows, so the delta is a multiple of 16, which `alHeapInit`
needs. The words stay the same as on the displaced build: `0x0C000000` plus the offset into `port_ram`.

### What it changes

| Part | Change |
| --- | --- |
| `ultratypes.h` | `u32` and `s32` are `int` types on every host, also on i386. On i386 `int` and `long` have the same size and calling convention |
| `frame.h` and its users | the interface between the game and the platform layer uses `int` and `unsigned int` where it used `long`: `port_rom_size`, the counters, `port_game_*`, `port_debug_*`. So do `devview.c`, the sound callback in `platform.c` and the brute forcer's declaration of `port_rom_size` |
| ROM addresses | `PORT_ROM_ADDR(p)` (`ptr32.h`) gives the number of a ROM or PI address that the game keeps in a pointer: 150 casts in `banks.c`, 3 in `dma.c`, 2 in `loaders.c`, and one each in `main.c` (`boot`), `sound.c` (`PrepareSong`) and `mus-init.c` (`MusBankInitialize`). `port_set_rom_symbols` stores the symbols through `uintptr_t` |
| Host addresses | the size of the RSP boot code is the difference of two `PORT_P2W` words. `LoadTextureBank` adds a palette's offset to the texture's `u8` pointer, not to its address as a `u32` |
| Calls | `hand.c` declares `SetupLight` and `puzzle.c` declares `AddAmbientSfxAtPoint` with their `f32` parameters, as the definitions have them. Both calls pass 0. On i386 the bits of 0 and 0.0f are the same on the stack. On x86-64 an integer goes in a general register and a float in an SSE register: with an `s32` in the declaration the callee would read whatever the SSE register held |
| Formats | the state trace (`port_game_trace`) prints `s32` and `u32` values with `%d` and `%x`, and `credits.c` a score with `%u`, not with the `long` formats |
| Crash handlers | `log.c` reads `REG_RIP` on x86-64 Linux and the registers `Rax` to `Rsp` on Win64, and prints addresses at their full width. `track.c` (the brute forcer) reads `REG_RIP` too |
| Memory hash | `hash_ram` counts a word in the program's image as a marker. On the encoded builds no word of the game's memory is a host address, and the image's range is the baseline build's, two constants |
| Libm | `port_pow`, `port_log1p` and `port_expm1` (`portmath.c`) take the place of the C library's (below) |
| Tools | `ptrdiff.py --other EXE` compares the headless build's memory with any build's. `tools/build_sdl_mingw.sh x86_64` builds SDL2 for 64-bit Windows. A recording names its program `linux64` or `windows64` |

The renderer, the loaders and the display-list readers need nothing of their own: they read words through
`PORT_PTR_DELTA` (see "Ptr32"), and the 64-bit build draws the same picture as the 32-bit one.

A link with `-flto` checks the declarations of every function and global against the definition across files
(`-Wlto-type-mismatch`, `-Wodr`). The mismatches it reports are of other kinds than the two calls above: an
`s16` or `u8` parameter declared as `s32` or the other way (the callee reads the low bits on both hosts), a pointer
declared with another pointed-to type, and the C declarations in `gen/ramtab.c` that only take addresses. None
passes a value in another register class or at another width.

### Libm

The game's code calls five functions of the C library whose results could differ between libraries: `pow`,
`log1p` and `expm1` in the frame-rate helpers (`delta.c`, `CameraFollowLerp` in `camera.c`, the credits in
`credits.c`), `tan` in `guPerspectiveF` (`gu.c`), and `sincos` in the Developer view (`devview.c`). The others
(`sqrt`, `sqrtf`, `fmod`, `remainder`, `lround`) are exact by definition. The sound player calls only `sqrt`.

What the regression traces show: none of the five runs at the original timing. The frame-rate helpers return their
argument when `deltaTime` is 1, the camera follow speed 1 skips `CameraFollowLerp`, and the credits' `pow` runs only
when `deltaTime` is not 1. `tan` gives the same float for every float field of view from 0.01 to 179.99 degrees, on i386 and x86-64
glibc (all 118 million of them were compared). `sincos` only draws.

What a direct test shows: the functions do give other bits. Over two million arguments of the kind the helpers pass
(k in [0, 1], deltaTime in (0, 1.5)), x86-64 glibc's double differs from i386 glibc's in 0.1% of the `pow` calls,
7% of the `log1p` calls and 12% of the `expm1(dt * log1p(-k))` results. i386 glibc computes them on the x87 unit,
x86-64 glibc in SSE with other algorithms, and MinGW with its own. Rounded to a float, as most callers do, none of
four million results differed. But `DeltaPowD`, `DeltaLerpD` and `DeltaDivD` return doubles. The 32-bit Linux and
Windows builds already differed from each other in the same way: MinGW's `DeltaLerpD` and `DeltaDivD` differed
from i386 glibc's in 20% and 18% of the same arguments (under Wine).

The choice: `portmath.c` holds the port's own `port_pow`, `port_log1p` and `port_expm1`, and every build uses them.
They use only double additions, multiplications and divisions, which SSE rounds the same way on i386 and x86-64,
and they call no library function. So every build, 32-bit or 64-bit, Linux or Windows, gives the same bits. The
test above, built for the four targets (Windows under Wine), gave the same doubles on all four.
Their error is within 10 units in the last place of a double (1 for `log1p` and `pow` of the helpers' arguments).

Two other choices do not work:

- Keep the C library: the 64-bit build would differ from the 32-bit one at a short update, rarely, and only in the
  frame-rate path. A recording made at a high frame rate on one build could then go out of sync on the other.
- Copy i386 glibc's results into the 64-bit build: they come from the x87 unit's `fyl2x` and `f2xm1` in extended
  precision, with glibc's own steps around them. The port would need glibc's code (LGPL) or a copy of its algorithm,
  and the Windows builds would still differ.

The 32-bit build is exact at the original timing: the functions do not run there. At a short update their last bits
could differ from the C library's. Rounded to a float, `port_pow`, `-port_expm1(dt * port_log1p(-k))` and `1 /
-port_expm1(dt * port_log1p(-1 / d))` give the same result as i386 glibc for all of four million arguments.

`tan` stays the C library's: glibc gives the same floats on i386 and x86-64, and so does the 64-bit Windows build
under Wine. A native Windows C library could differ in a last bit of the projection matrix. That changes a matrix
in the game's memory and the picture, not the game's state.

## The game as a library

### What it is

`make lib64` builds `build/lib64/libglover.so` and `make lib-windows64` builds `build/lib-windows64/glover.dll`:
the game, the sound player and the renderer, with no window, no `main` and no SDL. `make lib32` and `make
lib-windows32` build the same as 32-bit libraries. A host program calls it through `src/lib/glover.h`.
[library.md](library.md) is the guide for its users. It runs one game per process (a new game needs the library
loaded again) and has no savestates.

### What it adds

| Part | Change |
| --- | --- |
| `src/port/rom.c` | `port_rom_prepare`: the byte-order fix and the checks of the ROM, which `load_rom` in `platform.c` uses too, and a check that the ROM has all its 8 MB |
| `src/port/statehash.c` | `port_hash_ram`: the memory hash, which the program uses too |
| `libultra.c`, `port.h`, `frame.h` | `port_fatal_hook`: `port_fatal` calls it, when set, after the message and before `exit(1)` |
| `jalloc.c`, `banks.c` | the two out-of-memory paths of the heap and the unknown object bank call `port_fatal`; the game's code loops forever there |
| `mus-glue.c` | `port_audio_sink`: when set, it gets each block of frames that the sound player makes on the game's clock |
| `frame.c` | `port_game_bodies`: the positions that the trace prints; `port_level_count` and `port_goto_level_count` |
| `ptr32.h` | `PORT_LIB` turns the encoded words on, also on i386 |
| `platform.c` | headless `--audio-out FILE`: the sound player's samples, for the comparison with the library |
| `src/gfx` | `gfx_host.h`: the log, the environment, the clock and the OpenGL loader, without SDL; `gfx_init` returns -1 when OpenGL lacks a function; `gfx_set_target` chooses the framebuffer to draw into |
| `src/lib` | the library: `glover.c` (the interface and the guard of each call), `host.c` (the platform functions), `audio.c` (the sound queue), `gl.c` and `gl_null.c` (the renderer's calls), `coll.c` and `view.c` (the host world, [host-world.md](host-world.md)) |
| Makefile | `LIB=1`: `-fPIC` on Linux, `-fvisibility=hidden -DPORT_LIB`; the targets `lib64`, `lib64-stub`, `lib-windows64`, `lib-windows64-stub`, `libhost64`, `libhost-windows64`, `libglshot64`, the same with 32, and `libcheck` |
| Tools | `tools/libhost/libhost.c` (the test host), `tools/libhost/glshot.c` (the picture test), `tools/libcheck.py` |

The game's code needs seven functions from the program around it: `plog_printf`, `plog_vprintf`, `port_time_us` and,
with the real sound player, `port_audio_open`, `port_audio_lock`, `port_audio_unlock` and `port_audio_yield`.
`src/lib/host.c` gives them. Nothing else of the platform layer is linked. No object needs a symbol of libstdc++,
so the library links with `gcc -shared`, not `g++`.

Each call that runs the game's code goes through one guard (`enter` in `glover.c`). It refuses a second call while
one runs, sets MXCSR to `0x1F80` and puts the host's value back, and catches a fatal error. `port_fatal_hook` jumps
back to the guard with `__builtin_longjmp`, which needs no unwinding support from the system. The game is then dead,
and later calls return `GLOVER_ERR_DEAD`.

`gen/ram.S` and `port_ram`'s alignment are the same as in the programs. On Linux the library is loaded where the
system chooses, with `port_ram` on a 32 MB boundary; `port_ptr_delta` is set when the library loads.

The 32-bit library stores the game's pointers as offsets (`PORT_PTR_ENCODED`), as the 64-bit builds and the
displaced test build do. With plain addresses it would give other memory hashes than the 32-bit program: the hash
counts a word as a pointer when its value lies in the memory or in the program's image, and texture data and other
values fall into those ranges at other places when the library loads at another address. With the offsets the hash
uses fixed ranges and equals the 32-bit program's. `-fvisibility=hidden` does not hide
the `.globl` symbols of `gen/ram.S`; the version script `src/lib/glover.map` does. With `-fPIC` alone GCC would
treat every global function as replaceable at load time and inline fewer of them; hidden visibility keeps the code
as in the 64-bit program.

The headless mode's clock advances by each update's length times the frame's retraces. Only the host knows the
length before the demo's override, so the library takes the clock from the host (`clockUs`). The test host gives the
headless mode's clock, and the memory, with the profiler timers in it, is the same.

`glover.h` checks its structure sizes with `_Static_assert` only from C11 on: `gcc -std=c99 -pedantic -Werror`
rejects it. A log callback during `glover_create` gets `GLOVER_ERR_REENTER` for the game it gets, so `glover_create`
hands the host the game's pointer before the game starts, and takes it back if the start fails.

## Checks

These checks cover the C++ build, the pointer words, the 64-bit builds and the library:

- `make check` runs `check_ptr_slots.py`, then builds the headless and the displaced build and compares their
  memory (`ptrdiff.py`), runs three levels of `regress.py check` with the displaced build, and compares the pair
  with the real sound player (`ptrdiff.py --audio`).
- `make check64` counts the errors of a 64-bit compile. Both counts must be 0.
- `tools/check_layouts.py` (also with `--m64` and `--audio`), `tools/check_calls.py` and `tools/list_pointers.py
  --check --audio` must report no difference.
- `tools/regress.py check`, also with `--exe build/headless64/weemitt`, `--exe build/headless-displaced/weemitt` and
  `--exe build/libhost64/glover-host` (the library through the test host), must find every run the same as the
  baseline.
- `tools/ptrdiff.py --other EXE` compares the headless build's memory with any build's, word for word, and
  `--input` plays the input of `regress.py`. With `--audio --other build/headless64-mus/weemitt` it compares the
  builds with the real sound player.
- `tools/libcheck.py` checks the library ([testing.md](testing.md#the-library)).
- `tools/cmpcode.py A B` compares the machine code of two builds of the same sources, function by function. It
  shows what a change of flags or of the pointer type does to the code: most functions stay the same, and the
  others mostly differ in register choice, block order, or code that GCC duplicated, merged or inlined.

On the normal 32-bit Windows build the memory hash differs from Linux's: the hash counts the program's own image,
which lies elsewhere on Windows ([testing.md](testing.md)). The game's memory holds the save, so two runs give the
same hash only when they start from the same save. Give each run a new `--save` file, or point `XDG_DATA_HOME` at an
empty folder on Linux.

The speed of `glover bench` varies by 5 to 10% from run to run, even on one pinned CPU. The count of instructions is
exact: `valgrind --tool=cachegrind` over `glover play --level 13 --frames 3000` measures the cost of a change.

## What is left

The library leaves these for later:

1. **Savestates, a new game after `glover_destroy`, and more than one game in a process.** They need the brute
   forcer's renamed sections (bruteforce/docs/engine.md). In `-fPIC` objects these include `.data.rel`,
   `.data.rel.local` and `.data.rel.ro*`, which `checkgame.py` does not accept yet. They also need a snapshot taken
   before `port_game_init`. Later: `dlmopen`, or a copy of the DLL for each game.
2. **Collision with the host's world.** Done ([host-world.md](host-world.md)): `glover_coll_` gives the game the
   host's triangles, in chunks in `port_ram` from `JallocAlloc`, moving bodies as platforms that turn what stands on
   them, conveyors and water volumes (version 3). Left: the tilt of a body about x or z.
3. **A camera set by the host.** Done ([host-world.md](host-world.md#the-camera)): `glover_set_view`, through a hook
   after `SetupViewing` builds the view. A layer of the actors with alpha and depth, for a host to put over its own
   picture, is done too ([library.md](library.md#drawing-over-the-hosts-picture)).
4. **A pacer in the library** (`glover_pacer_`), tested bit for bit against the window loop's arithmetic.
5. **Sound pulled by a host thread**, with a real lock and a time limit on the `MusAsk` waits. The volume of the songs
   and of the effects is done: `GLOVER_OPT_MUSIC_VOLUME` and `GLOVER_OPT_EFFECTS_VOLUME` ([library.md](library.md#sound)).
6. **Interpolated drawing** (`gfx_interp_record` at each game frame), and saving and restoring the host's OpenGL
   state.
7. **A renderer that does not need OpenGL.** Done for a host with its own F3DEX2 interpreter: `glover_host_gfx`
   gives it display lists (version 4, [library.md](library.md#lists-for-the-hosts-interpreter)). Left: a Direct3D
   path in the library.
8. **The window program, the headless mode and the studio on the library**, and a static `libglover.a` with its
   symbols made local (`ld -r` and `--keep-global-symbols`).
9. **The Developer and Cheats features** (`glover_dev.h`), the trace of each update, and symbol lookup.
10. **The Steam Runtime:** `fmod` from an older glibc version, or a build in the Steam Runtime SDK.
11. **The brute forcer on `rom.c`**, without its own copy of the ROM checks.
