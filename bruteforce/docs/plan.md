# Plan

This is the design of the brute forcer: what it is for, how it works, and what is built and what is not.

## The goal

The brute forcer looks for real-time speedrun strategies, routes and timesaves. The Windows program comes first.

- The studio plays the game in a window, records the play as a timeline of states, and hands control between the
  player and the search ([docs/studio.md](../../docs/studio.md)).
- Time is counted in game frames. Loads are added to the time where a strat changes them: each costs `loadFrames`
  (70 game frames, measured on a console, [game.md](game.md)). A model of lag in play is not part of the count.
- Found strats are checked on a console. A runner checks them with Glover-Practice-Rom's frame and lag counters
  ([docs/studio.md](../../docs/studio.md), "Checking a strat on a console").
- Inputs that the search finds must be playable by a person. The input limits are adjustable, from strict (a
  runner can do it) to loose (ideas).
- Results need no emulator check. The sync test ([sync.md](sync.md)) is the proof that the engine plays the real
  game.

## The method

The search starts from base inputs: one input for every game frame, from a movie or from a play in the studio. It:

1. Runs the base inputs to the start of a chosen window of frames and saves the game state there.
2. Makes a candidate: a copy of the window's inputs with small random changes (perturbations).
3. Loads the state, plays the candidate, and stops at the window's end or when a stop rule fires.
4. Scores the result with a fitness function: for example where the glove is, how fast it moves, the frame on which
   it reached a place, or a state such as a pick-up collected.
5. Keeps the candidate when it scores better (or by a looser rule, see Search), and goes on from it.
6. Runs many workers at once, each with its own copy of the game, and shares the best candidates between them.

## Facts to start from

- **gloveworld** is the matching decompilation of the USA ROM: all game code is C, and it builds the exact ROM.
  Its `tools/emu` runs the real ROM in mupen64plus with scripted input and reads RDRAM (the N64's main memory).
- **weemitt** is the native PC port of the decompilation. Its headless build (`make headless`) runs the game's
  logic with no window and no sound, game frame by game frame: about 25,000 game frames a second on one core of the
  build machine.
- In weemitt, `port_ram` is one block that stands for the N64's RDRAM. Every game global sits at its N64 offset in
  it, and so does the heap. Most of the game's state is therefore in one block. Some port-side state lives outside
  it (static variables in `src/port` and a few in `src/game` added for the frame-rate work); a savestate must hold
  those too.
- weemitt is a 32-bit program (the game's structures hold 32-bit pointers).
- weemitt's frame-rate code is exact at the original timing: with update length 1 (`deltaTime` 1) it runs the
  game's logic as the decompilation does (`tools/regress.py`). The brute forcer only ever uses length 1.
- The game draws through `GraphicsStep`, and drawing calls `Random`. The display lists must be built every game
  frame (with no renderer), or the random sequence differs from the N64.
- The game reads the controller once per game frame (20 a second in levels). Facts about input and timing are in
  Glover-Practice-Rom's `docs/game.md`.
- Glover-Practice-Rom has savestates, an input recording format, a frame log and many state viewers on the N64.
  Its `docs/game.md` and `docs/build.md` describe what a savestate must hold on the console.

## Engine

The engine is in `engine/` and is described in [engine.md](engine.md). It is a library: the game's logic built
natively, behind a small C interface, `engine/engine.h`: `engine_create`, `engine_step` (one game frame with one
input), `engine_save` and `engine_load`, reads of the game's memory by N64 address and `engine_symbol` (the N64
address of a game global). `engine_read`, `engine_draw` and `engine_logic` run the three parts of a game frame
apart, for the sync test ([sync.md](sync.md)).

The design:

- It is built from weemitt's game and runtime code, with the headless parts only. Prefer changes inside
  `bruteforce/` over changes to the game's code in `src/`.
- The library is static (`libglover.a`). All the tools are one program, `glover`, with a command for each tool. The
  game's writable data is grouped by renaming the sections of one partly linked object (`gstate_data`,
  `gstate_bss`), which works on Linux and Windows alike. A shared library would load at a different address in each
  process, and separate programs would place the game at different addresses.
- A state holds `port_ram` below the heap and the parts of the heap that have been written. The heap's unused
  middle is read-only, and a fault handler tracks the writes there. Reading the allocator's blocks after each frame
  would miss blocks freed within a frame (a level load).
- The Windows program loads at 0x08000000, without ASLR (address space layout randomisation) and without a
  relocation table. The Linux one is not position-independent and loads at 0x08048000.
- Development and the automated tests run on Linux. The Windows program is cross-compiled with MinGW-w64, as
  weemitt's Windows build is. Both are 32-bit programs. A 32-bit process on 64-bit Windows can use 4 GB when it is
  built large-address-aware, and the Windows program is.
- One process holds one game. Workers are separate processes of the one program, one per logical processor. The
  game's data holds pointers into its own memory and the program, so every worker places them at the same
  addresses. A state saved in one worker then loads in any other, and workers share states through shared memory
  (a file mapping on Windows). Windows has no `fork`: each worker starts, loads the ROM once (well under a second),
  and then only loads states.

## Speed

Simulations should be as fast as possible. `glover bench` measures game frames a second per core and for all
cores, and saves and loads a second. [engine.md](engine.md), "Speed", gives typical figures.

Measured on the build machine (callgrind, 600 game frames of Atlantis 1 in weemitt's headless build): the game's
logic (`GameStep`) is 46% of the work, building the display lists (`GraphicsStep`) 36%, the level load 14%. A game
frame is about 280,000 instructions.

Built:

- **Simulate only from the first changed frame.** The search keeps a state for every frame (or every few frames,
  the config's `cache`) of the current best inputs in the window. A candidate starts from the state at its first
  change.
- **Cheap loads.** A load copies back only the pages written since the state was saved, found with page protection
  on both systems ([engine.md](engine.md), "Copying only what changed"). The plain full copy stays as the
  reference: `GLOVER_FULL_STATES=1` turns the tracking off, and the tests pass both ways.
- **The trimmed draw** ([engine.md](engine.md), "The trimmed draw") skips ten functions that only build the
  picture. A frame takes about 20% less time, and the search uses it.
- **Compiler options**: `make OPT=fast` builds the game's code with `-O3`, and `make OPT=znver4` also targets AMD
  Zen 4 processors.
- **Workers**: one per logical processor by default (16 give more than 8 on an 8-core processor with 16 threads).
  Workers run at a lower process priority, so the computer stays usable.

Not built yet:

- **A logic-only draw.** The draw's other work stays: it moves animations on, places collision spheres and
  platforms, calls `Random` and changes lists that the logic reads. Cutting more needs a copy of the object drawing
  (`CreateObjects`, `DrawActor`, `TransformAndDrawObject`) that computes the matrices only for objects with a
  sphere, a platform or a sprite.
- **Stop early.** Stop a candidate when it can no longer beat the best, or when its state matches one already seen
  (a hash of the values the score and the archive use). A `fail` condition in the config already ends a try.
- **More compiler tuning**: link-time optimisation and profile-guided optimisation with a training run. Measure
  each.
- **Memory limits.** Each worker needs the game's memory (24 MB in weemitt) and its cache of states along the
  window. 16 workers with a 20-frame window need about 1.5 GB. A shared archive (see Search) would keep inputs for
  every block and full states only for a limited number of blocks, within a budget set in the config. A block
  without a state would be rebuilt by replaying its inputs from the nearest saved state.
- **Search methods that waste fewer frames** (see Search).

**32-bit, not 64-bit.** A 64-bit build would have twice the registers, perhaps 10 to 20% faster. But the game's
structures hold 32-bit pointers, as on the N64, and so does the data loaded from the ROM. A 64-bit engine would
need weemitt's 64-bit work ([docs/port64.md](../../docs/port64.md)) in the engine's build too, and the sync risk is
high. The engine stays 32-bit. Revisit this only if the engine is slow after all the other speed work.

**No GPU.** The game's code branches too much to run well on a GPU. The simulation stays on the CPU.

## Sync

The engine must match the real game. Without this nothing it finds is real.

1. Run scripted inputs that cover many levels and actions in the engine and in an emulator. Compare the game state
   each game frame: the glove's and the ball's position and speed, their actions, the camera, `frameCount`,
   `rseed`, the level.
2. Find and fix every difference. Likely causes: floating point (x86 and the VR4300 both use IEEE single, but
   library functions and the order of operations can differ), memory that the N64 leaves as it was and the port
   clears, the heap size, the timing of lag frames.
3. Keep this as an automated test (`tests/sync.py`) that runs on every change.

Lag frames: on the N64 a game frame can take more than its 3 retraces, and a movie records input per controller
read or per video frame. The converter maps movie frames to game frames exactly ([sync.md](sync.md)).

This is built ([sync.md](sync.md)). The emulator is BizHawk 2.11.1's own Mupen64Plus core, built from BizHawk's
source: it times the game's threads differently from mupen64plus 2.5.9. The engine matches it in the attract mode
and in all seven boss levels. Reads in the middle of a logic frame (lag frames, loads) run at a stop inside the
engine's logic frame. The open question is in [sync.md](sync.md), "Open questions".

## Input files

- **BizHawk movie (`.bk2`)**, made with BizHawk's Mupen64Plus core: read and written.
- **Mupen64 movie (`.m64`)**, used by Rosalie's Mupen GUI and mupen64plus: read and written.
- The engine's own text format for a window of inputs: one line per game frame, easy to read and edit.
- Movie files are read and written by a separate converter (`tools/convert.py`, with `tools/movie.py`). It turns a
  movie into the engine's form once, before a run: one input per game frame, with movie frames and lag already
  mapped. The search never parses a movie; it reads that array.
- Not read yet: Project64 recordings, Glover-Practice-Rom's recordings and the game's own `.moo` demos.

## Search

Built: hill climbing over a window with many workers, goal, fail and progress scores, the points of the studio, the
input limits, and these perturbations: turn the stick a little or change its length, set a new stick direction,
toggle a button for a few frames, move the start or the end of a press by one frame, copy a neighbouring frame's
input, and put a stretch back to the base inputs. Each has a weight set in the config. `glover search` describes the config (`src/search.c` and the README).

Not built yet:

- **Archive of states**: keep many distinct states, not one best. Each state goes into a block by a few rounded
  values (for example the glove's position on a grid, its action, the ball's state). Each block keeps the best
  inputs that reach it. A step picks a block (weighted towards promising ones), loads its state, plays a burst of
  changed inputs, and sorts every new state it reaches into its block. This explores many routes at once and keeps
  stepping stones that score worse for now. It should become the main search, with the block keys and weights
  from the config.
- **Other rules**: simulated annealing (sometimes keep worse), a population of candidates with crossover.
- **Adaptive changes**: pick a kind of perturbation more often while it keeps finding improvements. The stick's
  angle and length are continuous; small steps around a good value work better than random values there.
- **Moving windows**: a short window (tens of frames) at a time, moved along the movie.

## Scores

- Built in: the read on which a condition first holds, the points (places and conditions in order), and a weighted
  sum of state values as progress. Conditions and values are named by the game's symbols (gloveworld's names) and
  offsets into known structures (the glove, the ball, the camera, actors).
- Configs are JSON files that name the run, the window, the score and the search rules.
- Not built yet: a score that needs real code, as a small C file compiled into the run (a plug-in).

## Output

- Built: the best inputs so far (`best.txt`, which `tools/convert.py to-movie` turns into a movie) and a log of the
  score over time. `tools/check.py` can check a result in the emulator, but results do not need it.
- Not built yet: a short report of what changed (which frames and inputs), how the score changed, and the frames
  gained.

## Tools

- Built: `glover play` plays inputs in the engine and prints the state per game frame. `tests/run.py --build B
  --base A` compares the state trace of two builds.
- Not built yet: a tool that compares two runs and shows where they split, a path plot of the glove and the ball,
  a state viewer for one frame, and live progress in the terminal.
