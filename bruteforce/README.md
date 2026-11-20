# Glover brute forcer

Searches for faster ways through Glover (N64, USA). It runs the game's logic natively from a saved state, tries small
changes to the inputs of a stretch of play, and keeps the changes that score better. It can also improve BizHawk TAS
movies: results are written back as movies and can be checked in an emulator.

It is built from weemitt's game code, in the folder above, and lives in weemitt's `bruteforce/` folder.

You need your own copy of the Glover (USA) ROM. The program reads it at start-up.

## Building

Run `make` in this folder. The program is 32-bit. On Debian or Ubuntu:

```sh
sudo apt-get install gcc-multilib make python3
make -j4                       # Linux: build/linux/glover
```

The Windows program is cross-compiled with MinGW-w64:

```sh
sudo apt-get install gcc-mingw-w64-i686
make windows -j4               # Windows: build/windows/glover.exe
```

`make OPT=fast` builds with more optimisation (`-O3`, in `build/linux-fast`), and `make OPT=znver4` for AMD Zen 4
processors (`build/linux-znver4`). Add `TARGET=windows` for the Windows versions.

## Running

All the tools are commands of one program: `glover COMMAND [OPTIONS]`. `glover` alone lists the commands.

| Command | What it does |
| --- | --- |
| `bench` | measures the engine's speed on this computer |
| `play` | plays inputs in the engine and prints the game's state per game frame |
| `saveload` | the save and load test |
| `search` | changes the inputs of a stretch of a movie and keeps what reaches the goal sooner |

### The benchmark

```sh
build/linux/glover bench --rom glover.z64            # Linux
build\windows\glover.exe bench --rom glover.z64      # Windows (Command Prompt or PowerShell)
```

It measures game frames a second in one process (with the full and with the trimmed draw, which the search uses),
the time of a save and a load, and then groups of worker processes that run at the same time: one, one per core,
and one per logical processor. A candidate is one try of the search:
a load of a saved state, then a few game frames. Options: `--level N` (default 13, the Atlantis boss), `--seconds S`
for each test (default 10), `--workers 1,8,16`, `--candidate F` (the frames of a candidate, default 20).

For steady numbers on a laptop, plug it in and choose the best performance power mode.

### Playing inputs

```sh
build/linux/glover play --rom glover.z64 --input run.txt --watch "frameCount:s32,hand+0x34:f32"
```

`--input` takes the engine's text format: one line per game frame, with the stick's X and Y (-128 to 127) and the
buttons held: `A B Z S` (Start) `L R`, `du dd dl dr` (the D-pad) and `cu cd cl cr` (the C buttons). A line may start
with its frame number and a colon. Text after `#` is a comment.

```
# frame: stick X, stick Y, buttons
0: 0 80 A
1: 0 80 A B
2: -40 80
```

`--watch` names the values to print: `NAME[+OFFSET]:TYPE`, with a game global's name (gloveworld's names) and a type
(`u8 s8 u16 s16 u32 s32 f32 ptr`). `NAME->OFFSET` reads through a pointer: `hand+0xd4->0x4:s16` is the glove's action.
`--trace` prints a fixed line of the main values instead. `--trim-draw` plays with the trimmed draw (see the search).

### From a movie to the engine and back

The search works on a run: the engine's form of a movie. `tools/convert.py` makes one from a BizHawk movie (`.bk2`)
or a Mupen64 movie (`.m64`). It plays the movie once in BizHawk's Mupen64Plus core (see "Tests" for the set-up) and records when
the game read the controller, drew and ran its logic.

```sh
python3 tools/convert.py to-run boss.bk2 --rom glover.z64 --out runs/boss --check
python3 tools/convert.py to-movie runs/boss runs/boss/search/best.txt improved.bk2
```

`--check` plays the run in the engine and compares the game's state with the emulator's at every read where the game
waits between frames. Only movies that start from power-on with a new save work. `to-movie`
writes a copy of the movie with the inputs of the reads in the file (the text format above, numbered by read).

### Runs of whole frames

A run can also hold whole game frames, one line each: `F` and the frame's input in the text format above. A line
`#state FILE` starts the run from that saved state instead of power-on (the file is relative to the run's folder,
and must come from the same program and ROM). The studio writes such runs ([../docs/studio.md](../docs/studio.md)),
and `glover play --events` and the search play them.

```
#state start.state
F 0 80 A
F 0 80
```

### The search

```sh
build/linux/glover search boss.json
```

The config is a JSON file:

```json
{
  "rom": "glover.z64",
  "run": "runs/boss/run.events",
  "window": [1200, 1260],
  "lookahead": 400,
  "goal": "gameInfo+0x54:u8 & 2",
  "fail": "player+0x10:s32 < 4",
  "out": "runs/boss/search",
  "seconds": 600
}
```

| Key | Meaning |
| --- | --- |
| `run` | the run of the base movie |
| `window` | the reads whose inputs may change: from the first up to, not including, the second |
| `lookahead` | reads after the window that are played with the base movie's inputs (default 100) |
| `goal` | a condition; the score is the read at which it first holds, the sooner the better. `gameInfo+0x54:u8 & 2` holds from the frame on which the level's exit starts after the level is done |
| `fail` | a condition that ends a try as a failure (optional) |
| `progress` | `[{"value": "NAME:TYPE", "weight": W}]`: when no try reaches the goal, the larger weighted sum at the end wins (optional) |
| `points` | instead of a goal: places and conditions to reach in order, `[{"place": [x, y, z], "radius": R, "of": "glove"}, {"condition": "..."}]` (`"of": "ball"` for the ball). The score is the read at which the last one is reached; until then, the points reached and the distance to the next place count |
| `buttons` | the buttons that may change (default all but Start), and `"stick": false` keeps the stick |
| `limits` | what a runner can play: `"none"` (the default), `"loose"` (presses and gaps of at least 2 frames, the stick's X and Y change by at most 96 a frame), `"strict"` (3 frames, 48), or `{"minHold": H, "minRelease": R, "stickStep": S}`. A try may break them no more often than the current inputs do, so a route with a short tap can still be searched |
| `perturb` | weights of the kinds of change: `stick` (turn it a little), `stick-new`, `button` (press or release for a few reads), `shift` (move a press by a read), `copy` (from a neighbouring read), `revert` (back to the base) |
| `workers`, `seconds`, `seed`, `out` | worker processes (default one per logical processor), the time limit (0: until stopped), the random seed, the output folder |
| `loadFrames` | the game frames counted for each load that a play passes before the goal (default 70, 3.5 seconds on a console). The engine runs a load in one game frame; a console takes 3 to 4 seconds |
| `trimDraw` | `false` plays with the game's full draw. The default trimmed draw skips the work that only builds the picture; the game plays the same |

A condition is one or more terms `VALUE OP NUMBER` joined by `and` or `or`, with `OP` one of `== != < <= > >=` or
`&` (true when the value and the number have a bit in common).

The search writes `OUT/best.txt` (the best inputs of the window), `OUT/log.txt` (the score over time) and a file per
worker. To stop it early, press Ctrl+C or create the file `OUT/stop`.

Paths in the config are relative to the config file's folder. The window must start where the game waits between
frames, and must not hold a load. It may hold lag frames: the search plays them with the base movie's timing. In a
run of a movie, the plays after the window end at the next load. A run of whole frames (`F` lines) plays on through
loads. A load is counted when the game's `frameCount` goes back, as it does when the level is left (a quit, an exit
to the hub, the level's end).

### Checking a result

```sh
python3 tools/check.py boss.json
```

The check writes the base movie with the best inputs as `OUT/best.bk2` (or `.m64`), plays it in the emulator,
compares the engine with the emulator at every read where the game waits between frames, and confirms the read at
which the goal first holds, or the progress score. Changed inputs can change the game's timing (a lag frame comes or goes). Then the emulator gives
another score than the search, and the check says so. The converted movie in `OUT/check-run` can be the base of
the next search.

## Tests

```sh
make test GLOVER_ROM=glover.z64
```

[tests/README.md](tests/README.md) lists them.

## Thanks

- ethteck and contributors, for splat. AngheloAlf, for spimdisasm. Simon Lindholm (simonlindholm), for asm-differ
  and decomp-permuter. Matt Kempster (matt-kempster) and contributors, for m2c. The decompals group, for their build
  of GCC 2.7.2 for MIPS.
- The mupen64plus team, for mupen64plus, and Rosalie (Rosalie241), for Rosalie's Mupen GUI.
- The BizHawk team (TASEmulators), for BizHawk, its Mupen64Plus core (which the sync tools build and run) and its
  `.bk2` movie format.
- The developers of the Mupen64 rerecording movie format.
- The developers of ScatterShot and the other SM64 brute forcers, which showed the method.
- The MinGW-w64 project, for the compiler of the Windows programs, and the Wine project, for running them in the
  tests.
