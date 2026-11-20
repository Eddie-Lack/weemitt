# Facts about the game

Facts learned while building the brute forcer, with how each was found. Names are gloveworld's. Glover-Practice-Rom's
`docs/game.md` holds many more, about input, timing and the game's code.

## Memory

- The heap of weemitt's game runs from `jallocHeapStart` (0x802B0D10) to 0x81800000 (`port_ram` offset 0x1800000,
  weemitt's `PORT_HEAP_TOP`: 16 MB past the end of weemitt's 8 MB copy of RDRAM). On the N64 it ends at
  0x80400000. In the Atlantis boss (level 13, started directly) the static blocks take about 0.95 MB at
  the top and the dynamic blocks a few KB at the bottom (the save and load test prints the sizes).
- A level load frees the old level's blocks and allocates the new ones within one game frame. Found when the
  engine read the allocator after each frame and missed written memory (docs/engine.md).
- `pbankBuffer` (0x80077070) holds raw sound sample data. Some of its words look like addresses in weemitt's
  `port_ram`. Found when a memory hash that turns such addresses into offsets differed between Linux and Windows.

## Loads

- `frameCount` goes back twice in one load. In the Atlantis boss, an exit to the hub from the pause menu sets it to
  0 in `UpdatePause` (`ExitLevel`), and again about 15 frames later, when the hub starts. The search counts a load
  only when the count goes back from 30 or more (`count_loads` in `src/search.c`).
- The engine runs a load in one game frame. A console takes seconds, at a time that does not depend on the input.
  From console videos of Fear (the results screens skipped with A and B): from the hub into Fear 1 the
  screen is dark for about 3.8 s. Between two levels there are two dark stretches, of about 3.0 s and 3.5 s, with
  the results screen between them. Black there is Y 16 (limited range).
- In BizHawk's Mupen64Plus core a load takes about 1.75 times as long as on a console (Carnival: 3.6 s on a console,
  6.3 to 6.6 s in BizHawk at 60 fps). The core runs the N64's CPU at about half speed: it charges 2 `Count` ticks
  for each instruction.

## Speed of the game's code

- One game frame of a level takes about 0.04 ms natively (Intel Xeon at 2.8 GHz), the display lists included:
  about 25,000 game frames a second on one core.

## Threads and frames

- The draw thread moves animations on only when `animateActors` is set. The game thread sets it at the end of each
  logic frame (`RunGame`, after `MainGameLoop` and `ProcessPersonalLoop`), and the draw clears it in
  `RenderObjects`. So a draw in the middle of a logic frame does not move animations on. Found when the engine drew
  after the whole frame instead and enemies drifted from the emulator's.
- The level select loads the level and runs the level's first `MainGameLoop` on the same frame message:
  `RunGame` calls `LevelSelectScreen`, which sets `gameInfo.mode` to 4, and then the `switch` on the mode runs
  `MainGameLoop`. In mupen64plus that frame spans about ten controller reads. Found in the space 3 boss scenario.
- At `frameCount` 1 the game thread waits in `StartDrawing` until the draw thread sets `gfxIsDrawing`. weemitt's
  `StartDrawing` sets it itself.
- `UpdateActor` keeps its argument in `s1`; the unwinder finds such arguments from each function's first
  instructions (`tools/sync/stack.py`).
