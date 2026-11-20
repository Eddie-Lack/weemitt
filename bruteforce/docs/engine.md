# The engine

The engine is weemitt's headless game behind a small C interface (`engine/engine.h`). This file explains how it is
built and how its savestates work.

## The build

- The Makefile reads weemitt's flags and source lists from weemitt's own Makefile (`make -pn`), for weemitt's
  headless build: no renderer and the silent stand-in sound player. The engine adds no flags that change the
  game's result. `devview.c` is left out: it only draws the Developer menu's views.
- All game and runtime objects are linked into one relocatable object, `game.o`. `objcopy` renames its `.data` and
  `.bss` to `gstate_data` and `gstate_bss`. The linker then provides `__start_` and `__stop_` symbols for both, on
  Linux (ELF) and on Windows (PE). `tools/checkgame.py` fails the build when `game.o` has another writable section.
- The partial link uses `--wrap` for each function in `engine/hooks.def`: the game's calls of `NAME` go to
  `__wrap_NAME` in `engine/hooks.c`, which calls `NAME`. `--wrap` changes only calls from another object file, so
  the list holds only functions that the game calls from another source file. The hooks let a logic frame stop at
  such a call (`engine_logic_stops`, see [sync.md](sync.md)). Without stops a hook costs a few instructions. The
  same file turns off weemitt's PC additions that the controller can reach and the N64 game does not have: the
  cheat list that R or Z opens in the pause menu (`OFF0(CheatMenuAvailable)`).
- `gen/ram.S` (the game's memory, `port_ram`, and the N64's hardware registers after it) is assembled apart, and its
  `.bss` becomes `gram_bss`.
- `game.o`, `ram.o`, the engine and the symbol table go into `libglover.a`. The one program, `glover`, links it
  statically, with a command for each tool (`src/main.c`).
- The program is 32-bit, as weemitt is. On Linux it is not position-independent and loads at 0x08048000. On Windows
  it is linked with `--disable-dynamicbase` and `--disable-reloc-section` at 0x08000000: it loads there or not at
  all. So every process of the program has the game's memory, code and variables at the same addresses, and a state
  saved in one process loads in another. A separately linked program would place the game's code and variables
  elsewhere: the state header's build value refuses its states.
- `OPT=fast` adds `-O3` and turns off the stack protector; `OPT=znver4` also adds `-march=znver4`. The game is still
  compiled with `-ffp-contract=off` and SSE floating point. The benchmark's check line is the same for all of them
  (see "Checks").

## Savestates

A state holds:

1. The game's variables outside `port_ram`: `gstate_data` and `gstate_bss`, about 1 MB. Most of it is
   `converted` in weemitt's `loaders.c` (1 MB), a bitmap used only while a bank loads. `port_rom` and
   `port_rom_size` (the ROM buffer of the process) are stored as 0. A load puts the engine's own values back.
2. The memory after the heap that `ram.S` places (`port_hw`): 880 bytes.
3. `port_ram` below the heap, 2.69 MB (0x2B0D10 bytes): the N64's low memory (the z-buffer, the RDP output and the
   music heap), then the main segment as copied from the ROM: its rodata and its MIPS code (which the port never
   runs or writes), then its data and bss.
4. The parts of the heap that have been written. The heap runs from `jallocHeapStart` (0x802B0D10) to 0x81800000,
   offset 0x1800000 in `port_ram` (weemitt's `PORT_HEAP_TOP`). The game's allocator takes dynamic blocks from the bottom up and
   static blocks from the top down. The engine keeps two marks: the heap is in use below `gap_hi` and from `gap_lo`
   up. The pages between them (the gap) are read-only. A write there faults; the handler (`engine/track.c`, a
   signal handler on Linux, a vectored exception handler on Windows) moves the nearer mark past the page and lets
   the write go on. Memory in the gap has never been written, so it is zero.
5. The engine's frame counter.

The header names the program (its layout: where the game's memory, variables and code lie) and the ROM (a hash).
A state of another program or ROM does not load.

Reading the allocator's blocks after each frame is not enough: a level load frees and allocates blocks inside one
game frame, and the blocks it freed are not seen. The page tracker sees every write.

### Copying only what changed

A save or a load of the whole state copies about 4.6 MB. In 20 game frames of the boss level only about 55 of its
1,200 pages change. So the engine splits the state into pieces (`engine/track.c`): one per page of the game's
variables and of `port_ram`, and one for the memory after the heap. Each piece has a version, a 64-bit number that
names its content. A state holds the version of each piece after the header (`engine_state_data` gives where the
game data starts).

- After a save or a load (a sync), the pieces' pages are read-only. The first write to one faults. The handler makes
  the page writable and lists it as dirty. At the next sync each dirty piece gets a new version and its page becomes
  read-only again.
- A load copies only the pieces whose version differs from the memory's. Then it clears the pages that lie in the
  state's gap but not in the engine's, and makes the state's gap the engine's.
- A save into a buffer that holds a state with the same marks copies only the pieces whose version changed.
- A page that is dirty at two syncs in a row is hot. It stays writable and gets a new version at every sync, which
  saves its fault. Every 256 syncs the hot pages are made read-only again.
- Pieces that share a page with other memory (the first and last page of each section, and the memory after the
  heap) are never protected. They get a new version at every sync, so they are always copied.
- Versions start at a random number in each process, so versions from different processes do not match by chance.
- `GLOVER_FULL_STATES=1` turns this off: every save and load copies everything. It is the reference for tests.

The save and load test (`glover saveload`) ends with 300 random loads, plays and saves over eight buffers. Versions
of the tracker broken on purpose (hot pages without new versions, skipped copies in a load or a delta save, gap
pages not cleared, a fault not listed) all failed it.

On the build machine a load after a game frame takes 0.020 ms, against 0.354 ms for a full copy. A save after a game
frame into the same buffer takes 0.018 ms.

## The trimmed draw

The draw (`GraphicsStep`) took 53% of a game frame. It also changes the game's state: it moves animations on,
places collision spheres and the platforms that follow objects, calls `Random`, steps the fade, and changes the
text, sprite, footprint and shadow lists. So it cannot be dropped. Without it the game differs from the first frame.

`engine_trim_draw` turns on the trimmed draw. The draw still runs, but these functions return at once while it runs
(`SKIP0` and `SKIPP` in `engine/hooks.def`; the link's `--wrap` reaches them because they are called from other
source files):

| Function | What it writes |
| --- | --- |
| `guMtxF2L` | the matrices for the graphics chip (`dynamic`), and the static matrices of the landscape |
| `WriteObjectDisplayListFlat`, `WriteObjectDisplayListGouraud` | vertices (`objectsVtx`), display list commands, `vtxPtr`, `glistp` |
| `LoadTexture` | display list commands |
| `DeformTextureCoords` | texture coordinates of meshes, read only by the two functions above |
| `spDraw` | display list commands, the sprite's `rsp_dl_next`, a render mode cache |
| `SetupViewing`, `InitDisplayLists`, `ApplyGlobalTransformations` | matrices and display list commands |
| `ProcessBackgroundLights` | `lightArray`, `lightOffset`, `lastLitActorFlags`, display list commands |

A study of the draw's code (every function under `GraphicsStep`) found no reader of these outside the draw's
picture work. `overlay_dl_next` advances with `spDraw`; it is only compared with the end of its buffer, which
decides whether an overlay is drawn. The evidence that the game stays the same:

- From one state, 600 frames of the boss level with the full and with the trimmed draw: the state trace is the same
  every frame. The memory that differs at the end (29 KB) lies in display lists, vertices, matrices, sprites'
  `rsp_dl_next`, the light array and texture coordinates. Skipping one function at a time showed which function
  causes each difference.
- The same comparison over 3,000 frames of random input in each of the levels 0 to 40, with two seeds: the traces
  are the same.
- The sync test passes with the trimmed draw (`tests/sync.py --trim-draw`): all eight scenarios match the emulator.
- `tests/run.py` compares the traces of the full and the trimmed draw from power-on, in the boss level and in a
  script of reads, draws and logic frames.

A frame takes about 20% less time with the trimmed draw. The picture and the display lists are wrong, so
`glover play` and the tests use the full draw unless `--trim-draw` is given. The search uses the trimmed draw
(config `"trimDraw"`, default true).

## Checks

- `engine_hash` hashes the whole state as a save holds it, with the whole heap. Use it within one program.
- `engine_check_hash` hashes `port_ram` and `port_hw`, up to `port_hw_end` (`gen/ram.S`), with pointers into it as
  offsets and pointers into the program as one value. A pointer in the game's memory is a Ptr32 word, `PORT_PTR_BASE`
  plus its offset (weemitt's `src/port/ptr32.h`). On the 32-bit builds the base is the address of `port_ram`.
- `engine_n64_address` turns such a word into an N64 address. `engine_host_to_n64` does the same for a pointer of
  this process into the game's memory. The hooks use it for the arguments they record.
  It is the same for builds of one system with different options, but not between Linux and Windows: raw data
  (sound samples in `pbankBuffer`) can look like a pointer into `port_ram`, which has another address on each
  system.
- The benchmark's "Check" is a hash of `engine_trace` (weemitt's state trace) for 600 frames. It is the same for
  every build on both systems.
- `tests/run.py --build B --base A` plays the same input in two builds and compares the state trace of every frame.

## Speed

`glover bench` measures the speed on the computer it runs on (README, "The benchmark"). Build with `make OPT=fast`
for the fastest game code. It reports game frames a second in one process, with the full and with the trimmed
draw; the size of a state; the time of a save and of a load; and game frames and candidates a second for groups
of workers. A candidate is a load of a saved state and 20 game frames. For steady numbers on a laptop, plug it in
and choose the best performance power mode.

On the build machine (Intel Xeon at 2.8 GHz), the boss level (13) with generated input runs at about 25,000 game
frames a second on one core, with any of the three builds. weemitt's headless build is as fast: 600 frames of
level 5 take 0.07 s with the level load. A state is about 4.6 MB.

On a laptop with 8 cores and 16 logical processors, the `fast` build runs about 50,000 game frames a
second in one process, and about 57,000 with the trimmed draw. A save or a load after a game frame takes about
0.01 ms, because only the changed pages are copied ("Copying only what changed"). The search then tries about
22,000 candidates a second with 16 workers, and about 17,500 with 8. 16 workers give more than 8, so the search
keeps one worker per logical processor. The 20 game frames of a candidate take most of its time; the load and the
rest take about 13%.

The work that could make the engine faster is listed in [plan.md](plan.md), "Speed".
