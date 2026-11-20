# Sync with the real game

The engine must give the same game state as the ROM in an emulator, game frame by game frame. This file explains how
the N64 game orders its work, why that is not always one frame at a time, and how the sync test and the converter
handle it.

## The game's threads

The game's code is in gloveworld's `src/main.c` and `src/sched.c`.

- The scheduler sends a frame message (9) to its clients every `desiredFrameRate` video retraces (3 in the levels, 2
  in the menus), but only when no graphics task is pending. It sends with `OS_MESG_NOBLOCK`: a message to a full
  queue is lost. Every 3 retraces it also sends a retrace message, which only `RunGame` acts on (it calls
  `ClearText`).
- Three clients each have a queue of 8 messages:
  - the controller thread (`ControllerProc`, priority 115) reads the controller (`osContStartReadData`,
    `ReadController`);
  - the draw thread (`DrawGraphics`, priority 20) builds a frame's display lists and runs `UpdateFade`;
  - the game thread (`RunGame`, priority 10) runs a frame of the game's logic.
- So on each frame message the read comes first, then the draw, then the logic. weemitt's frame (`port_game_update`)
  runs them in this order, and so does `engine_step`.

## When it is not one frame at a time

The game thread can be busy for longer than a frame, mostly while it loads a level: the loads wait for the cartridge
and decompress data. The controller and draw threads go on. The frame messages wait in the game thread's queue (8 at
most; more are lost). When the load ends, the game thread runs the queued frames one after another, with no read and
no draw between them unless a new message comes.

Measured in mupen64plus from power-on with no input: at the first 73 reads of the controller thread the game thread is
still starting up, and its queue fills to 8. Between reads 73 and 74 it runs 6 logic frames in a row.

In mupen64plus a game frame takes 5 video frames in steady play (gloveworld's `tools/emu` notes). The game thread is
then always waiting between frames when the next message comes.

What the draw thread changes matters to the logic:

- `CalcCameraRot` sets `camera.rot`, which the stick's direction uses.
- `UpdateFade` moves `gameInfo.fade` and `starZ`; the game takes no input until both are 0 (`controller.c`).
- `dispFrameCount` decides whether `JallocAlloc` takes a static or a dynamic block.
- Drawing the world moves animations on, which enemy scripts wait for, and can call `Random`.

## The model

`tools/sync/pollinput.c` is an input plugin for mupen64plus. At each read of the controller it records, before it
answers: which thread runs (the first read comes from `osContInit` in the game thread), the game thread's saved
stack pointer and program counter, its queue (`main_msgQ`: count, first index, the 8 messages), `dispFrameCount`,
the draw thread's state, the game thread's registers and stack, and the values to compare. The game thread waits between frames (a "top" read) when its
saved stack pointer and program counter are those of the top of `RunGame`'s loop: 0x801FFC18 and 0x801C7494.

`tools/sync/events.py` turns the log into an events script for the engine (`glover play --events`):

- After each read, the script writes the draw thread's state from the log (`dispFrameCount`, `draw_buffer`,
  `gfxIsDrawing`, `camera.rot`, `cameraInfo.prevRotX` and `prevRotY`, the previous camera positions,
  `gameInfo.fade` and `starZ`, `fadeOverlay.alpha`). At a top read these are the engine's own values when it is in
  sync. The comparison comes before the writes.
- The logic frames between two reads are the frame messages that the game thread took from its queue. A frame runs
  where its message was taken.
- The draws between two reads are the increase of `dispFrameCount` (a level load sets it to 0).
- After a top read, or a read where the game thread works through queued short frames, the first draw runs in the
  engine before the logic frames. Drawing moves things on, so it must run.
- A draw while the game thread is inside a long logic frame (a load) is skipped. The frame is long when it already
  ran before the previous read (no message taken since then), or when it goes on past the next read (no message
  taken until then). The script writes the draw thread's state from the next read's log instead. Without the
  world, that state is all such a draw changes.
- A read that comes in the middle of a game frame runs at a stop inside the engine's logic frame, with its draw
  ("Reads in the middle of a frame" below).

## The emulators

The sync tools run the real ROM in one of two emulators. `$GLOVER_EMULATOR` chooses.

- `bizhawk` (the default): BizHawk 2.11.1's own Mupen64Plus core. `tools/sync/bizhawk/setup.sh` fetches its core,
  its high-level RSP plugin and its audio plugin from BizHawk's source and builds them for Linux in
  `build/bizhawk`. Nothing of BizHawk's is committed. `tools/sync/emurun.py` drives the core as BizHawk does: the
  pure interpreter (BizHawk's default CPU emulator), a zero-filled EEPROM, the core in a thread of its own, two
  video frames before the movie, then one video frame per movie frame. `tools/sync/bizhawk/nullvideo.c` is a video
  plugin that draws nothing and counts the vertical interrupts. GLideN64, the video plugin of BizHawk's movies,
  adds no emulated time either.
- `mupen64plus`: the system's mupen64plus 2.5.9, set up with gloveworld's `tools/emu/setup.sh`. It needs a display
  (`Xvfb :99`).

BizHawk's core is a 2013 fork of mupen64plus's core 2.0. It times the hardware differently from mupen64plus 2.5.9:
the video period, the first vertical interrupt after reset, the RDRAM registers, and the delay of the RDP's
interrupt. A BizHawk movie therefore gets other lag frames in mupen64plus 2.5.9, and the game's `frameCount` at the
movie's end differs between the two emulators.

In both emulators:

- The CPU emulator is the pure interpreter (`R4300Emulator` 0). The read times are the same in all three modes.
  The floating-point results are not: the dynamic recompiler uses the x87 FPU.
- The SSE flags that flush denormals (MXCSR bits 0x8000 and 0x0040) are off. In mupen64plus 2.5.9 a library of
  the SDL audio plugin turns them on when it loads, and `pollinput.c` and `vicount.c` clear them. BizHawk's core
  never sets them. With them on, the camera in the carnival and space 3 bosses differs from the engine's: a result
  that should be a denormal (a number below the smallest normal float) becomes 0. The real N64 flushes such
  results to 0 (libultra sets FCSR.FS), so in such places BizHawk, the emulators here and the engine all differ
  from the console.
- The EEPROM starts blank. BizHawk's core fills it with zeros and mupen64plus 2.5.9 with 0xFF. The events script
  names the value (`#eeprom-fill`), and the engine starts with the same.

## The sync test

`tests/sync.py` runs scenarios from power-on in the emulator and in the engine and compares, at every top read: the
frame counters, the mode and level, the random seed, the glove's position, speed, turn and action, the ball's
position and speed, the camera's position and turn, the player's score, garibs, lives, health and level time, and
the first two enemies' positions.

- `attract`: no input for 6000 reads: the logos, the title, the fly-through and the five recorded demos.
- `boss-NAME`: the team's level select, opened by writing `gameInfo.mode` 2 and `autoPlayback` 0 during the
  fly-through, then generated input in the boss level. The fly-through leaves `handControl.noControlTimer` and
  `gameInfo.cameoActive` set, so the level select takes only Start.

`--cached` reuses the emulator's logs of the last run (`build/linux/sync-*.log`) and replays only the engine.

## The converter

`tools/convert.py to-run` plays a movie in the emulator with the same plugin and writes the events script of the
movie. A BizHawk movie gives one input per video frame: the plugin answers each read with the input of the video
frame it comes in. The video frames are the core's vertical interrupts. In BizHawk's core the driver sets
each frame's input at the frame's start, as BizHawk does. In mupen64plus 2.5.9 `tools/sync/vicount.c` counts them
(a video plugin that passes everything on to the real one; mupen64plus's frame callback counts graphics tasks
instead). The count equals the game's own `__osViIntrCount` at every read. BizHawk runs two video frames before the
movie's first frame, so movie frame f is the input after f + 2 vertical interrupts. An .m64 gives one input per
read. `to-movie` writes new inputs back: a read's input fills the
movie frames from the one after the previous read up to its own.

The search changes inputs only where every read is a whole frame (a read, a draw and a logic frame): there the game
does not depend on the emulator's timing.

`tools/check.py` checks a search result: it writes the movie with the new inputs, converts it with `--check`, and
evaluates the search's goal at each read in the emulator's log. The search tests the goal after the logic frame of
read F; the emulator's log holds it at read F + 1.

## Results

`python3 tests/sync.py --rom ROM` runs every scenario on BizHawk's core with the default number of reads: 6000 for
the attract mode and 3000 for each boss scenario, where the level starts near read 1000. Every scenario gives the
same state as the emulator at every top read. For each scenario the test prints how many reads it compared and how
many of them ran at a stop inside a logic frame. The attract mode compares about 5,300 reads, and each boss scenario
about 2,600, of which 18 to 34 run at a stop.

## Reads in the middle of a frame

When a logic frame takes longer than the time between two frame messages, the next message comes while the game
thread is still inside that frame. The controller thread and the draw thread have higher priority, so they run at
once: the read and the draw come in the middle of the logic frame. Two things then differ from a whole frame:

- The draw moves animations on only when `animateActors` is set, and the logic frame sets it only at its end. A draw
  in the middle of a frame therefore moves nothing on (docs/game.md).
- The read's input reaches `ProcessInput` in the same frame when the frame had not got there yet.

The engine runs such a read and draw inside its logic frame, at a stop:

1. The plugin records the game thread's saved registers and 1.5 KB of its stack at every read. The log keeps them
   only for the reads where the game thread is not waiting between frames.
2. `tools/sync/stack.py` unwinds the stack. It reads each function's first instructions from the player's ROM: the
   frame size and where `ra` and the `s` registers are saved. The result is the chain of calls in progress. A
   call's pointer argument comes from the caller: the few instructions before the call and in its delay slot
   compute it from the caller's `s` registers, which the unwinder knows. When they do not, it comes from the `s`
   register where the function keeps it, if nothing changes that register before the point.
3. `events.stop_for` turns the chain into a stop: the calls in it that the engine hooks (`engine/hooks.def`), from
   the outermost in, with the pointer argument where there is one. For example `UpdateEnemies >
   NMEProcessActorScript=0x80299ce8`. The stop is where one hooked call starts or returns, the nearest one to the
   N64's point with no hooked call between. `stack.py` reads each function's control flow from the ROM to find it:
   - where the hooked call that last finished before the point returns, when every path to the point passes that
     call and no loop holds both;
   - where the innermost hooked call in progress returns, when no other hooked call can come before that;
   - in `MainGameLoop`'s own code, or in a call that is not hooked: where the next hooked call starts;
   - else where the innermost hooked call in progress starts.

   In `StartDrawing`, where the N64 waits for the draw thread, the stop is where `StartDrawing` returns.
4. `events.derive` puts the read, the draw thread's state and the draw into a group after the logic frame's `L`
   line. The engine runs the group when the frame reaches the stop (`engine_logic_stops`). Reads that came in the
   first part of a long frame, before the read with the stop, go into a group at the same stop.

A stop can come a little before the point where the N64 stopped. When the scenarios showed such a stop to be too
early, the function called at the point was added to the hooks (for example `AddPuff`, which `RestartPlayer` calls
after it moves the camera). The
scenarios match all the same. A group whose stop the frame does not reach runs after the frame; `glover play` then
prints a warning, and the sync test fails. `GLOVER_STOP_TRACE=1` makes `glover play` print the hooked calls of
each logic frame with stops, and the stops it reaches and misses.

The search plays the run's own events with its inputs in replay mode (src/run.h): without the emulator's writes,
and with draws without values. Inside a level the writes are the engine's own values: without them the engine
still matches the emulator in the boss scenarios. A level's exit (`CheckForExits`) and the other loads need them,
so the search's plays end there. The search keeps the base movie's timing, but other inputs can change it: a lag
frame can come or go. In a test in the pirates boss a changed window removed a lag stretch, and the emulator's
score differed from the search's by about 0.2 %. `tools/check.py` reports such differences.

## Open questions

- Whether BizHawk's core, as built here, plays a movie exactly as the BizHawk program does. `tests/sync.py` compares
  the engine with the core, not with the BizHawk program.
