# The studio

The studio is the window program with the brute forcer (`bruteforce/`). It records every game frame you play as a
timeline. You can pause, scrub back and forth like a video, play on from any frame, and start a level directly. You
can save the frame on show to a state file and load it again later.
You mark a start point and the points to reach, and the brute forcer's search looks for a faster way from the
start to them, in the background. Its best result goes onto the timeline when you take it: you watch it, or play on
from any frame of it (the plan: [bruteforce/docs/plan.md](../bruteforce/docs/plan.md)).

```sh
make studio -j4             # build/studio/weemitt-studio
make studio-windows -j4     # build/studio-windows/weemitt-studio.exe (copy SDL2.dll next to it)
```

## Using it

The game plays as in the normal program, with these differences:

- The physics are always Original: whole game frames, as on the N64.
- The sound is off. The real sound player keeps state outside the game's memory, which the timeline cannot restore.
- The Enhancements and Cheats menus are gone. They change the game's logic in a way the timeline's recorded input
  cannot replay. The menu has a Studio menu instead.
- The save file is not written. The game's save stays in memory.

| Control | Default keys | What it does |
| --- | --- | --- |
| All four C buttons together | I, J, K and L | pause, or play on from the frame on show |
| While paused: D-pad left or right | F or H | a frame back or on (hold to repeat) |
| While paused: L or R | Q or E | a second (20 frames) back or on |
| While paused: A | Space | play on from here with your input; the frames after this one are dropped |
| While paused: B | X | play the recording from here; it pauses at its end |
| The studio bar (while paused, or with the menu open) | the mouse | drag the timeline, step, jump to the start or the end |
| Menu, Studio, Start a level | the mouse | starts a level directly; the timeline starts again from there |

The bar's help line names the keys as they are bound now. A step on at the end of the timeline adds a frame with no
buttons held. The A that plays on is the studio's: the game gets A again only after it has been released, so play
does not go on with a jump. Short key taps work while paused. Start a level keeps the pause: while paused, one frame
runs at once and the level's first frame shows.

The Studio menu shows or hides the studio's windows: Points and search, Input, Start a level and Save and load
states. `settings.ini` keeps the choice (`[studio]`). A window that is shown opens where no other window is, and
stays inside the game window.

The Input window shows the stick and the buttons of the update that led to the frame on show, and of the next one.
An input that a take changed is drawn in orange while the take can be undone. Step through a found result with it
to learn what to press.

Quitting the studio stops a search that runs.

## State files

A state file holds the game at one frame. Menu, Studio, **Save and load states** opens the States window.

- **Save state** writes the frame on show to a file. Type a name first. The name can hold letters, digits, spaces,
  `-`, `_` and `.`. It cannot start with `.` or a space, and spaces at its end are left out. Windows device names (CON, PRN, AUX, NUL, COM0 to COM9, LPT0 to LPT9) are refused. A state of
  the same name is replaced, and the window warns of that. On Windows the warning ignores the case of the name.
- **Load state** loads the state picked in the list. It becomes frame 0 of a new timeline, as **Start a level**
  does, and the game is paused there. Play on from it as from any frame.

A new timeline, from a loaded state, a route or **Start a level**, forgets the start point, the points, the search's
result and the take to undo. A search that runs is asked to stop. When there is such work, the studio asks first.

## Route files

A route holds the timeline's inputs from its first frame. It holds no game data, so it can be shared. In the States
window, type a name and press **Save the route**. The file is `NAME.route` in `studio/routes` of the preference
folder. Its first lines name where the timeline starts: `power-on`, `level N` (from `--level` or Start a level) or
`state HASH` (a loaded state file). Each `F` line is one update's input, as in a search's `base.events`
(`bruteforce/README.md`).

**Load the route** puts the route on the timeline from its first frame, and goes to frame 0, paused. Press B to watch
it. A route loads only on a timeline that starts where the route starts: start that level, or load that state, first.

The files are `NAME.state` in the folder `studio/states` of the program's preference folder (on Windows
`%APPDATA%\weemitt\Glover\studio\states`). The window shows the folder. A state file can also be put there by
hand. The list shows every file that ends in `.state`, whatever other characters its name holds. On Windows the
studio reads and writes these files with the system's UTF-16 file names, so a folder or a name with other letters
(such as `é`) works.

A state file loads only in the build of the studio that saved it. A state holds pointers into the program, so a
state from any other build is refused with a message, even one with a small change. Keep the program with its states.

A state file holds the game's memory, which holds data from the ROM. Do not share it, and do not commit it.

On the command line:

| Option | What it does |
| --- | --- |
| `--load-state FILE` | starts from the state: its frame is frame 0 of the timeline. The window starts paused. |
| `--save-state FRAME FILE` | with `--headless`: writes the state after frame FRAME to FILE and goes on. Up to 8 times. |

FRAME counts as the headless mode's frames do: the line `frame FRAME` of `--trace` shows the state that is saved.
With `--headless`, the frames and the `--input` script start again from 0 at the loaded state. The script's line
`0:...` gives the input of the first update after the load.

```sh
build/studio/weemitt-studio --rom ROM --level 40 --headless 1198 --input INPUT --save-state 1197 arms.state
build/studio/weemitt-studio --rom ROM --load-state arms.state
```

## Points and the search

The Points and search window shows with the studio bar.

1. Go to the frame to start from and press **Start here**. A small green sphere marks the glove there.
2. Go to a later frame where the glove (or the ball) should be, set the radius, and press **Point: glove here**
   (or **Point: ball here**). **Point: the level is done** asks for the level's exit instead. Add points in the
   order they must be reached; the last one is the end point (a red sphere, the others yellow).
3. Press **Search**. The search changes the inputs from the start point up to the frame of the last point marked,
   and plays on with your inputs after them. The window shows the best: the frame at which it reaches the end
   point, against the first result (your route). A load that the inputs pass before the end point (the level is
   left) counts 70 frames: a console takes about 3.5 seconds for it, and the game here takes one frame. Workers: how many processes search (0: one per logical
   processor). Input limits: what a runner can play. **strict** (the default) keeps every press and every gap
   between presses at least 3 frames long and the stick's change to 48 a frame; **loose** 2 frames and 96;
   **none** allows any input on any frame. Time limit: the search stops by itself after that many minutes (none by
   default). **Stop the search** ends it. The window shows the time and the tries a second, and "Your route": the
   frame at which the inputs you played reach the end point. A point must lie after the start point. A "level is
   done" point needs a route that already ends the level: the window says so when the level is not done at its
   frame. When the search ends without a result, the window shows the reason from `search.log`.
4. **Take the result** puts the best inputs on the timeline from the start point to the frame they reach the end
   point, and goes to the start point. Press B to watch it, or go to any frame of it and play on from there.
   **Undo the take** brings back the timeline from before the first take of this search. After a take the window
   says which result is on the timeline. A later take of the same search keeps the undo. Playing on, or a new
   search, drops it.

A search lives in a folder of its own, `studio/search-NNNN` in the program's preference folder (on Windows
`%APPDATA%\weemitt\Glover\studio`). It holds the start state, the run, the config, the search's results
(`best.txt`, `log.txt`) and its output (`search.log`). The state holds the game's memory: do not share it. Each
folder holds a state of about 5 MB. **Delete old searches** removes every search folder but the latest one's. On
Windows the studio writes these files with the system's UTF-16 file names.

## Checking a strat on a console

The studio counts game frames. On a console, a game frame takes 3 video frames (at 60 Hz), and more when the
console cannot finish its work in time (a lag frame). Lag differs between routes, so check a found strat on a
console with Glover-Practice-Rom.

1. Take the result and learn it in the studio: press B to watch it, or step through it frame by frame.
2. On the console, turn on the practice ROM's frame counter and lag counter (HUD). Save a state at the start
   point.
3. Play your old route from the state and note both counters at the end point. Load the state, play the new route
   and note them again.
4. The time of each route in video frames is 3 times its game frames, plus its lag frames. The route with fewer
   video frames is faster.

## How it works

`src/port/studio.c` keeps the input of every update (`inputs[f]` is what update `f` got) and a saved state every 60
frames. Frame `f` is the game after `f` updates. Going to a frame loads the last saved state before it and replays
the recorded inputs from there. With more than 256 saved states, the spacing doubles and every other state goes.

- The savestates are the brute forcer's (`bruteforce/docs/engine.md`). The studio build links the game's objects
  into one object, `game.o`, whose writable data lies in sections of its own, as the engine's build does
  (`studio.mk`). The platform layer, the renderer and the menu stay outside it. `devview.c` too: it is game code
  that only the overlays read. A state saved by the studio loads in the same program's search workers.
- `platform.c` and `menu.cpp` call the studio where `PORT_STUDIO` is defined. The normal build is unchanged
  (`tools/regress.py check`).
- The game's clock (`port_time_us`) is fixed at 0 in the studio. It feeds only the profiler timers, and a fixed
  value keeps the game's memory the same in every run.
- Every update goes through `run_update`. `studio_before_update` puts the recorded input there on a frame the
  timeline holds, or records the input. `studio_after_update` counts the frame and saves a state when one is due.
- A seek replays with the full draw. The brute forcer's trimmed draw would leave picture data that lasts: a level
  computes its landscape's static matrices once, when it starts, and the trimmed draw skips writing them.
- After a seek the renderer starts a new interpolation (`gfx_interp_record` with cut 1).
- Each saved state of the timeline keeps the display lists of its frame (`Key.tasks`). They lie in the game's memory,
  which the state holds. A seek that loads a state and replays nothing shows that frame's picture from them.
- A state file (`studio_save_state`, `studio_load_state`) is a header of 512 bytes, then the engine's state as
  `engine_save` writes it. The header holds, in the PC's byte order:

  | Offset | Size | Field |
  | --- | --- | --- |
  | 0 | 8 | the magic `WEEMSTAT` |
  | 8 | 4 | the format version, 1 |
  | 12 | 4 | the header's size, 512 |
  | 16 | 8 | the build: the first 8 bytes of the SHA-256 hash of `game.o`, read as a hexadecimal number (`studio.mk`) |
  | 24 | 4 | the layout: where the game's memory, variables and code lie (`engine_identity`) |
  | 28 | 4 | a hash of the ROM (`engine_identity`) |
  | 32 | 8 | the size of the engine's state |
  | 40 | 8 | a check value of the engine's state (64-bit FNV-1a) |
  | 48 | 4 | the frame of the timeline it was saved at |
  | 52 | 8 | the level and the mode (`port_game_level`, `port_game_mode`), 4 bytes each |
  | 60 | 4 | the number of display lists |
  | 64 | 192 | 16 display lists (address, size, Sprite2D flag) |
  | 256 | 64 | the program's version (`git describe`), ending in a 0 byte |
  | 320 | 4 | where the game's data starts in the engine's state (`engine_state_data`) |
  | 324 | 188 | zero |

  A load checks the magic, the format, the build, the layout, the ROM, the size and the check value, in this order.
  The first that does not fit gives the message. The version only names the build in the message. A load replaces
  the timeline as a level start does: the first saved state of the new timeline is the loaded one.
- The engine's states in the timeline and in a search folder (`start.state`) have no such header. A state file is
  not one of them, and the search does not read state files.
- Starting a level puts the jump in the first state of a new timeline. The jump is due at the next update, so a
  replay from that state repeats it.
- `weemitt-studio COMMAND` runs the brute forcer's commands (`search`, `bench`, `play`, `saveload`) in the same
  program, without a window. The search's workers are such processes, so a state that the studio saves loads in
  them. On Windows they start without a console window (`proc_spawn`, `proc_start` in `bruteforce/src/procs.c`).
- **Search** writes the start state (`start.state`), a run of whole frames from it (`base.events`: `#state
  start.state`, then one `F INPUT` line per frame of the timeline from the start point) and `search.json` with the
  points and `"loadFrames": 70` (bruteforce/README.md, "The search"). It starts `weemitt-studio search search.json` in the background and
  reads `best.txt` twice a second: the best inputs, the frame they reach the end point at and the loads they pass.

## Tests

`WEEMITT_STUDIO_SELFTEST=1` with `--headless` records the run, then goes to 400 frames of the timeline (random
jumps, steps on and back, frames with a saved hash) and compares each with the state the game had there: the state
trace, and every 50 frames the hash of the whole state. It prints the number of failures and exits with 1 when
there are any.

`tools/studio_states.py` tests the state files. A run saves a state after frame N (default 600) and after frame
N + M (M default 300). A second process loads the first state and plays M frames with the rest of the input, with
the self-test on. The test compares the state trace and the hash of the game's memory of every frame, and the game's
data in the two states at frame N + M. Then it changes copies of the state (the magic, the format, the build, the
layout, the ROM, one byte, the length) and checks that each is refused with its message. Without `--input` it
makes walking input; `--level -1` starts at power-on. `--wine` runs the Windows build.

```sh
WEEMITT_STUDIO_SELFTEST=1 build/studio/weemitt-studio --rom ROM --level 13 --headless 1500 --input INPUT
build/studio/weemitt-studio saveload --rom ROM --level 13 --frames 600
python3 tools/studio_states.py --rom ROM [--level 40 --input INPUT --frame 1197 --frames 400]
```

Under Wine, the Windows build passes all three (`tools/studio_states.py --wine`), and its window opens on a virtual
display (Xvfb). Wine needs the folder `/run/user/UID` of the user who runs it: without it, Wine 9 on Ubuntu crashes
at the start of every program with "free(): invalid pointer".
