# The port compared with the N64

This file explains how to compare the port with the real game running in an emulator, what the comparison found
and what still differs. The tools are in `tools/n64ref/`.

## Summary

The attract mode was compared frame by frame for 21000 game frames: the menus, the presentation (level 43), the
fly-through (level 44), the five recorded demos (levels 10, 16, 20, 25 and 30) and the first four demos again.
The compared state is the port's `--trace` line: frameCount, mode, level, the random seed and the positions of
the glove, the ball and the camera, bit for bit.

- Most of the run matches bit for bit. The glove, ball and camera follow the same paths in every demo.
- Two bugs in the port change the game. A byte-order bug in the level parser garbles the vectors of some puzzle
  actions; it moves the camera to a wrong place at the end of the level 20 and level 30 demos. The order of the
  three steps in the port's frame makes a fade that the controller step starts (the end of a demo) one frame
  late, so each demo lasts one frame longer than on the N64.
- Five calls pass two calls of `Random` as arguments. x86 GCC evaluates them in the other order. Only sounds and
  effects differ, not the traced state.
- `guRotateF` in `src/port/gu.c` does not compute what the N64's does. The glove uses it while it swings
  (`HAND_SWISH`). The attract mode does not use it.
- What remains comes from the N64's thread timing: the screen fade in the first frames of each level, and the
  second fly-through, where the N64 itself does not repeat its first fly-through. The port repeats it exactly.

## How to run the comparison

The N64 side uses gloveworld's scripted emulator (`gloveworld/tools/emu`; run its `setup.sh` once). It needs a
display: `Xvfb :99 -screen 0 1024x768x24 &`. The scripts find gloveworld in `../gloveworld` (or `--gloveworld`).

```sh
# The N64: one line per game frame. About 100 video frames a second, five video frames a game frame:
# 21000 game frames take about 18 minutes.
python3 tools/n64ref/emu.py --frames 21000 --out n64.txt

# The port: the same lines, from a gdb breakpoint. A few seconds.
make RENDERER=null -j4
python3 tools/n64ref/native.py --frames 21000 --out native.txt

# Part by part: the first frame where a field differs.
python3 tools/n64ref/compare.py n64.txt native.txt [--extra] [--context 5]
```

`compare.py` also reads the port's own trace (`weemitt --headless 21000 --trace`). Keep the traces outside the
repository.

### What each tool does

| Tool | What it does |
| --- | --- |
| `fields.py` | the recorded fields: those of the `--trace` line, then frameCount2, dispFrameCount, the fade, the demo playback state, the controller, the pause flag, the frame rate, RunGame's message backlog, gfxIsDrawing and gfxTasks |
| `emu.py` | runs the ROM in mupen64plus and writes one line per game frame. Options: `--random-log`, `--snapshot` |
| `native.py` | runs the port under gdb and writes the same lines. Options: `--interleave`, `--random-log`, `--snapshot` |
| `compare.py` | cuts both traces into parts (a run of frames with the same mode and level) and pairs them; within a part it aligns frames by frameCount |
| `randdiff.py` | finds the first call of `Random` that differs between two logs |
| `memdiff.py` | compares the game's memory (data, bss and heap) at one frame |

### How emu.py finds a game frame

On the N64 three threads wait for the scheduler's message 9, which comes once per game frame: `ControllerProc`
(priority 115), `DrawGraphics` (20) and `RunGame` (10). RunGame does the game's work. No counter changes once per
RunGame frame in every mode. frameCount stops in some modes and when the game pauses. The emulator calls the
script once per video frame, and RunGame can run several frames in one video frame when it catches up.

So `emu.py` patches the game's code through the emulator's cheat engine. RunGame's call to `ClearText`
(0x8013DC38), which follows its `osRecvMesg`, calls a small routine at 0x80700000 instead. When the message is 9,
the routine copies the words of `fields.py` into a ring buffer at 0x80701000, then jumps to `ClearText`. The
routine and the buffer are in the upper 4 MB of RDRAM, which the emulator provides and the game does not use. The
assembly is in a comment in `emu.py`.

The record taken when RunGame starts frame k+1 is the line "frame k". At that moment DrawGraphics and
ControllerProc have already run for frame k+1, because their priority is higher. `native.py` records at the start
of `GameStep`, which is the same moment in the port. The port's `--trace` line comes after `GraphicsStep`. It is the
same for these fields while the port runs its steps in its current order (see divergence 2).

The hook starts working after RunGame has begun its first frame, which loads the title screen for several
seconds. The first record is numbered from frameCount, which counts RunGame's frames in the menus.

### Tools for finding a cause

- `native.py --interleave N64_TRACE` makes the port run as many graphics steps between two game frames as the N64
  did. The `disp` field (dispFrameCount, counted by DrawGraphics) gives the count. The script skips the port's
  graphics step where the N64 ran none, and calls `ControllerStep` and `GraphicsStep` again where it ran more. At
  a level start some steps ran during the load, before the game set the fade to 300; the fade at the next record
  tells how many, and the script runs those before the load. This takes the N64's thread timing out of a
  comparison.
- `--random-log FILE` (both recorders) writes one line per call of `Random` with a non-zero lim: the frame,
  frameCount, mode, level, the caller, lim and rseed before the call. On the N64 a second routine replaces two
  instructions of `Random` (0x80147010). `randdiff.py` shows the first call that differs, or lists them
  (`--list N`). The N64 log names a static or inlined function after the global function before it, so callers
  are not compared unless `--callers` is given.
- `--snapshot FRAME[,FRAME...] --snapshot-out FILE` (both recorders; `{}` in the name stands for the frame) saves
  the game's memory at that line. On the N64 the routine copies 0x801D41D0-0x80400000 to 0x80400000 when it
  reaches the record. The files hold game data from the ROM: keep them out of the repository.
- `memdiff.py N64_DUMP NATIVE_DUMP` compares the two. It also reads a `weemitt --dump FRAME FILE` file (give
  `--binary` with the weemitt that wrote it). That dump comes after the frame, like the `--trace` line. It maps the port's heap (static blocks are 16 MB higher in
  the port) and pointers, and accepts a word that matches as one 32-bit value, two 16-bit values, bytes, or a
  double. It groups the rest by variable (from gloveworld's symbol lists) or by heap block (its Jalloc name).
  `--halves` also reports heap words that match only as two 16-bit values while the N64 word reads as a float
  between about 1e-5 and 1e5. That is what a float converted as two 16-bit halves looks like. Many differences
  are expected: libultra's and the scheduler's state, timers, display lists, the matrix stack, the double buffer's
  parity and the audio driver. Look for game data.

## Results

The N64 trace and the port's trace start in the same state. Frame numbers below are the N64's.

### The current port

| Part | N64 frames | Port | First difference |
| --- | --- | --- | --- |
| Menu | 0-139 | 141 frames (the port also has frame -1) | none |
| Presentation (43) | 140-565 (426) | 427 frames | none in the positions; the fade differs in 33 frames |
| Language screen (mode 14) | 566 | 1 frame | none |
| Fly-through (44) | 567-2927 (2361) | 2361 frames | none in the positions |
| Menu | 2928-3557 | 630 frames | none |
| Demo, level 10 | 3558-4778 (1221) | 1222 frames | none in the positions |
| Demo, level 16 | 5382-6171 (790) | 791 frames | none |
| Demo, level 20 | 6775-7878 (1104) | 1105 frames | frameCount 1083: the camera jumps to a garbled position (divergence 1) |
| Menu after level 20 | 7879-8481 | 603 frames | the random seed, from the first frame |
| Demo, level 25 | 8482-9492 (1011) | 1012 frames | none |
| Demo, level 30 | 10096-10907 (812) | 813 frames | frameCount 687: the camera, as in level 20 |
| Fly-through again | 11511-13870 (2360) | 2360 frames | frameCount 29: the random seed (see "What still differs") |
| Demos again | 14474- | | as in the first round |

Every part after the presentation starts one frame later in the port than on the N64, and one more frame later
after each demo. Each demo lasts one frame longer in the port (divergence 2). The menus between the demos last
603 frames in both.

### With the fixes for divergences 1 and 2

Every part has the same frames as on the N64: the same first frame, the same length. The positions, the random
seed and frameCount are the same in every frame, except in the second fly-through and the menu after it. With
`--extra`, the fade differs in about 20 frames at the start of each level (divergence 4).

With the fixes and `--interleave`, the extra fields match too: the fade, the demo's next key frame and buttons,
the pause flag and autoPlayback. All 21000 frames match except the second fly-through and the menu after it. The
`Random` logs match for the first 118438 calls, up to the thunder in level 30 (divergence 3).

The memory of the port and the N64 was compared at the start of each level (frames 141, 568, 3558, 5383, 6776,
8483, 10097, 11520) and in level 10 (3570 to 4000), with `--interleave`. The heap blocks have the same addresses
and sizes, except one overlay block in level 25 and five in level 30 (divergence 4). The data loaded from the ROM
matches; with `--halves`, no float converted as two halves remains. The differences that remain are in data the
graphics step advances: animation times, footprint ages, garib sprite frames, collision spheres placed from
animated objects. They come from the N64's thread timing (divergence 4).

## Divergences and their causes

### 1. Puzzle actions keep a vector in an s16 array (a port bug)

`PuzzleAction.data` is `s16 data[6]` (include/puzzle.h). Actions of types 8, 14, 15, 16, 17, 18 and 19
(`ACTION_SET_CONVEYOR`, `DROP_PICKUP`, `ADD_NME`, `MOVE_PLAT`, `LOOK_AT_PLAT`, `LOOK_AT_POINT`,
`SET_CAMERA_POS`) keep a `Vec3` of three floats in it, and `PuzzleExecuteAction` reads it as `*(Vec3 *)action->data`
(src/game/puzzle.c lines 292, 334, 343, 346, 364, 481, 489, 598-604). The parser reads these 12 bytes with
`PORT_COPY(parseAction->data, data, 12)` (src/game/parse.c:886). `PORT_COPY` converts by the type of the
destination, `s16`, so it swaps each half of each float on its own. Every such float has its two halves in the
wrong order: 2895.0 (0x4534F000) becomes 0xF0004534, about -1.6e29.

Evidence: in the level 20 demo at frameCount 1083, `SetCameraOverride` (called from `PuzzleExecuteAction`,
`ACTION_SET_CAMERA_POS`) puts the camera at (-1.6e29, 7e-41, 7e-41); the N64 puts it at (2895, -219, -160). The
target point of `ACTION_LOOK_AT_POINT` is garbled the same way. The game then takes a different number of random
numbers, and the menu after the demo differs in the random seed. Level 30 shows the same at frameCount 687.
`memdiff.py --halves` on the unfixed port lists 19 `Action` heap blocks in level 20, each with three such words
at offset 0x14. The same bug moves platforms (`MOVE_PLAT`, conveyors), enemies (`ADD_NME`) and dropped pickups
in levels the attract mode does not show.

Fix: in src/game/parse.c line 886, read the 12 bytes as three 32-bit values:

```c
                if (type == 17 || type == 18 || type == 19 || type == 14 || type == 15 || type == 16 || type == 8) {
                    /* These actions keep a Vec3 in data[]. */
                    port_copy_from_data(parseAction->data, data, 12, 4);
```

Line 888 (`type == 34`, `ACTION_EARTHQUAKE`, four s16 values) is right as it is. With this change levels 20 and
30 match the N64 to the end of the demo.

### 2. The port runs the graphics step after the game step (a port bug)

On the N64, when the scheduler sends a frame's message, ControllerProc runs first, then DrawGraphics, then RunGame
(their priorities are 115, 20 and 10). The port's `port_game_update` (src/port/frame.c lines 97-103) runs
ControllerStep, GameStep, then GraphicsStep. Over many frames both orders alternate one game step and one graphics
step, so most state comes out the same. The difference is what the graphics step sees from the controller step
of the same frame. When a demo ends, `ReadControllerFromData` (in the controller step) calls `ExitLevel`, which
starts the fade out. On the N64 the graphics step of that frame applies the first fade step (`UpdateFade`)
before the game step. In the port it runs one game step later. The fade reaches 276, where `ScreenFadedOut`
ends the level, one frame later. So the presentation and every demo last one frame longer in the port, and
every later part starts later.

Evidence: at the end of the level 10 demo the N64's fade at frameCount 1207 is 50; the port's is 0, and 50 one
frame later. With the order changed, all 25 parts of the 21000 frames have the same first frame and length as
on the N64.

Fix: in `port_game_update` (src/port/frame.c), run the graphics step before the game step:

```c
    if (deltaTick) {
        desiredFrameRate = newDesiredFrameRate ? newDesiredFrameRate : desiredFrameRate;
        ControllerStep();
    }
    /* The N64 runs DrawGraphics before RunGame: its priority is higher. */
    currentTask = 1;
    GraphicsStep();
    GameStep();
```

The display lists of an update then show the state before that update's game step, as on the N64. This changes
the logic, so `tools/regress.py` needs a new baseline. Run `tools/dtcheck.py` again too: the graphics step also
advances animations. After the change the `--trace` line, printed after the update, no longer shows the camera
that the graphics step sets in levels 43 and 44; `native.py` records at the start of `GameStep` and still matches
`emu.py`.

### 3. Two calls of Random in one call's arguments (a port bug, effects only)

C does not fix the order in which a call's arguments are evaluated. GCC 2.7.2 for MIPS evaluates these left to
right; GCC for x86 evaluates them right to left. The random seed comes out the same, but the two numbers go to
the other arguments. Checked in the ROM's code and the port's code:

| Place | Call | Effect |
| --- | --- | --- |
| src/game/weather.c:32 | `PlaySampleNot3D(248, Random(40) + 80, 128, Random(56) + 100)` | thunder volume and pitch |
| src/game/boss.c:817 | `PlaySampleNot3D(248, Random(20) - 106, 128, Random(26) - 116)` | a boss sound's volume and pitch |
| src/game/bullet.c:509 | `AddLightning(..., Random(96) - 96, Random(96) - 96, Random(96) - 96, ...)` | lightning colour (the first and third are exchanged) |
| src/game/game.c:1803 | `AddStarBurst(..., eolItems[1].y + Random(11) - 5.0f, Random(3) + 3, ...)` | end-of-level stars |
| src/game/game.c:1893 | `AddStarBurst(temp += 160.0f, Random(11) + 65, Random(3) + 3, ...)` | end-of-world stars |

`src/game/water.c:277` (two calls in one array index expression) is evaluated in the N64's order by x86 GCC too.
The `Random` logs found the first: the thunder in the level 30 demo (call 118438) takes `Random(56)` before
`Random(40)`.

Fix: evaluate the calls in the N64's order into locals before the call, for example in weather.c:

```c
        vol = Random(40) + 80;
        PlaySampleNot3D(248, vol, 128, Random(56) + 100);
```

and in bullet.c take the three values into locals in argument order. The scan was for statements with two or
more `Random(` calls; other side effects in arguments were not searched.

### 4. The N64's thread timing (not a port bug)

On the N64 RunGame can fall behind the scheduler. Then it runs the frames it missed in a row, and DrawGraphics
runs fewer times than RunGame for a while. A long frame (a level load) also lets DrawGraphics run in the middle of
RunGame's frame. In the emulator this happens at the start of every level: RunGame runs about six frames in a row
after the load. The port runs exactly one graphics step per game step.

The graphics step changes some game state. It applies the screen fade (`UpdateFade`), advances animations
(`DrawActor` calls `UpdateAnimations` when `animateActors` is set), ages footprints (`DrawFootprints`), and places
the collision spheres of animated objects (`TransformAndDrawObject`). So:

- The fade differs for about 20 frames at the start of each level. dispFrameCount shows the cause: on the N64 it
  lags frameCount by 4 or 5 after the load. With `--interleave` the fade matches.
- Animation times, footprint ages and garib frames differ by one or two steps in memory from a few frames into
  each level, even with `--interleave`. `StartDrawing` and `StopDrawing` (gloveworld src/banks.c) wait in a loop
  until DrawGraphics changes `gfxIsDrawing`; DrawGraphics then runs inside RunGame's frame. The port sets
  `gfxIsDrawing` at once (src/game/banks.c:1171, 1181). Where such a step falls inside a frame is not recorded, so
  `--interleave` cannot place it.
- `JallocAlloc` chooses the static or the dynamic heap from `frameCount != 0 && dispFrameCount >= 2`
  (src/game/jalloc.c). In levels 25 and 30 the overlays set up in the first frame (`bit-N` blocks, overlay.c) go
  to the dynamic heap on the N64 and to the static heap in the port. The N64 has probably already run a second
  graphics step inside that frame (in `StartDrawing`). Nothing traced depends on it.
- A test build that ran `ControllerStep` and `GraphicsStep` inside `StartDrawing` and `StopDrawing`, as the
  N64's wait does, did not remove the fade differences at the level starts. It is not recommended.

The emulator's timing is not the console's either: here a game frame lasts five video frames, because the
emulated RSP takes four to finish a frame's tasks (see gloveworld's tools/emu/README.md). A few more
instructions in the logging routine change the timing. Runs with different routines gave the same positions and
seeds up to the second fly-through; some timing fields (the backlog, the video frame) differed earlier.

## What still differs

### The second fly-through

The fly-through runs twice in the attract mode. In the port both runs are the same. On the N64 the second run
differs from the first from frameCount 29 in the random seed, and from frameCount 318 in the camera. The port
matches the N64's first run, so it cannot match its second. With `--interleave` the second run still differs at
frameCount 29.

The N64 does not repeat itself here either. Two emulator runs that differ only in their logging routines
(`emu.py` with and without `--random-log` and `--snapshot`) give the same positions and seeds up to the second
fly-through, and differ from each other from its frameCount 30 on. A third run, with two more fields in the record, ran RunGame and DrawGraphics in a
very different pattern at the start of the second fly-through (dispFrameCount 1, 5, 11, 12, 18 at frameCounts
1 to 5, against 2, 4, 4, 5, 5).

The `Random` logs show the first difference at frameCount 28. Enemy 3 (type 19) waits for its animation to
finish (its instruction 1 ends when `finishedAnim` is set) and then picks a random point (`NMEActionRandomMove`).
In the port and in the N64's first fly-through it picks the point at frameCount 28; in the N64's second
fly-through (the run with the `Random` log) at frameCount 32. Animations advance in DrawGraphics, so their
progress depends on how DrawGraphics and RunGame interleave. In the third run this enemy's animation time at
frameCounts 2, 4 and 6 is 3.67, 6.33 and 9.0; in the port it is 2.33, 5.0 and 7.67, one step behind. The second
fly-through starts from the menu, the first from the language screen. This is the N64's timing, not a port bug.
The comparison cannot settle it further.

### Other parts of the port that the attract mode does not test

- `guRotateF` (src/port/gu.c:116) uses the C library's double `sin` and `cos` and its own formula. The ROM's
  (0x801C5B90) calls `guNormalize` (multiplies by `1.0f / sqrtf(x*x + y*y + z*z)`), multiplies the angle by the
  float 0.017453292 (π/180), takes libultra's `__sinf` and `__cosf` (the port's `port_sinf` and `port_cosf`),
  computes `ab = x*y*t`, `bc = y*z*t`, `ca = z*x*t` with `t = 1 - cos`, and sets `mf[0][0] = x*x + cos*(1 - x*x)`,
  `mf[0][1] = ab + z*sin`, `mf[0][2] = ca - y*sin`, `mf[1][0] = ab - z*sin`, `mf[1][1] = y*y + cos*(1 - y*y)`,
  `mf[1][2] = bc + x*sin`, `mf[2][0] = ca + y*sin`, `mf[2][1] = bc - x*sin`, `mf[2][2] = z*z + cos*(1 - z*z)`,
  the rest from `guMtxIdentF`, all in single precision in that order. `HandSwish` (src/game/object.c) builds
  `hand.rotQuat` with `guRotateF(rot, 180.0f, 0, 1, 0)`; there `sin` is about -8.7e-8 on the N64 and 1.2e-16 in the
  port, so the glove's swing on a rope comes out slightly different. Fix: write `guRotateF` as above.
- `guMtxCatF` (src/port/gu.c:58) adds the four products in the N64's order, but the ROM's starts each sum from
  0.0. They differ only when every product is -0.0: the N64 gives +0.0. Collision spheres and platform positions
  come from this function. Starting each sum from `0.0f` makes it exact.
- `guMtxXFMF`, `guTranslateF`, `guScaleF` and `guMtxIdentF` compute what the ROM's do. `guLookAtF`,
  `guPerspectiveF`, `guMtxF2L`, `guMtxL2F` and the sprite functions (src/port/sprite.c) only feed the display
  lists and the overlays' screen positions, and were not compared.
- `port_sinf` and `port_cosf` (src/port/libultra.c) gave the same results as the N64 in every frame compared.
- The music stubs (src/port/audio_stub.c) change nothing the trace shows in the attract mode: every menu and demo
  has the N64's length.
