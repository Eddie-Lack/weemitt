# Testing

## The ROM

The tests and tools need the Glover (USA) ROM. Set `GLOVER_ROM` to its path, or pass `--rom FILE`. Without either,
the tools look for `baserom.us.z64` in a gloveworld checkout next to this repository. A tool with no ROM stops with one
line that says so. Keep the ROM outside the repository.

## A first change

1. Build: `make -j4` and `make headless -j4` (building.md lists the packages).
2. Record the regression baseline once: `python3 tools/regress.py baseline`. It builds the baseline commit in a git
   worktree and takes about a minute.
3. Make the change.
4. `python3 tools/regress.py check` must say "same as the baseline" for every run. A change of timing also needs
   `python3 tools/dtcheck.py LEVEL` (delta-time.md). A change of the window or the menu needs
   `python3 tools/menutest.py` with a display, and a look at the window.
5. The table below names the tool for each part of the program.

## Every tool

| Tool | What it checks | Needs | Time |
| --- | --- | --- | --- |
| `tools/regress.py check` | the logic at the original timing, against the baseline | ROM | about 1 minute |
| `tools/dtcheck.py LEVEL...` | the same level at dt 1 and a shorter dt | ROM | seconds a level |
| `tools/tapcheck.py` | short presses of L on the ball | ROM | seconds |
| `tools/menutest.py` | where the menu's windows open, and settings.ini | ROM, a display (Xvfb works) | about 1 minute |
| `tools/hostworld/hwtest.py` | the host world (below) | ROM | a few minutes |
| `make libcheck` (`tools/libcheck.py`) | the library and its test host | ROM | several minutes |
| `make check` | the pointer slots and the displaced build (port64.md) | ROM, pyelftools | several minutes |
| `tools/ptrdiff.py` | the displaced build's memory against the headless build's | ROM | a minute |
| `tools/check_calls.py`, `tools/check_layouts.py`, `tools/check_ptr_slots.py`, `tools/list_pointers.py` | calls, structure layouts and pointers | pyelftools; clang for the layouts | a minute |
| `tools/cmpcode.py A B` | the machine code of two trees of object files, function by function | pyelftools | seconds |
| `tools/cfunc.py` | prints or replaces one C function's definition | nothing | seconds |
| `tools/inputtest/build.sh`, `tools/mustest/build.sh` | the input latch; the sound player's heap | nothing | seconds |
| `tools/audiotest/build.sh`, `tools/audiotest/render.sh` | the audio commands; rendered songs | ROM for the full test | under a minute |
| `tools/studio_states.py` | a studio state file saved and loaded in a new process plays as the straight run | ROM, the studio build | a minute |
| `WEEMITT_STUDIO_SELFTEST=1` with the studio (studio.md) | going to frames of the timeline | ROM, the studio build | seconds |
| `tools/n64ref/` | the port against the N64 in an emulator (n64-comparison.md) | ROM, gloveworld's emulator | 20 minutes |
| `bruteforce/tests/run.py` | the brute forcer's engine | ROM | a few minutes |
| `bruteforce/tests/sync.py` | the engine against the ROM in BizHawk | ROM, BizHawk | long |

## The headless mode

`weemitt --headless FRAMES` runs the game without a window, as fast as it can, and stops after FRAMES original
game frames. Time is simulated: each frame advances the clock by exactly one N64 frame, so a run gives the same
result every time.

| Option | Effect |
| --- | --- |
| `--level N` | start level N directly, as the level select does (save slot 6, medium difficulty); N is -1 (the normal start) to 47, see "Level numbers" |
| `--input FILE` | controller input: lines `FRAME:BUTTONS:X:Y` (buttons in hex, the N64's bits); each line holds until the next. Blank lines and `#` comments are skipped. A line that cannot be read, a value out of range (stick -128 to 127) or a frame out of order stops the run with the file and the line number |
| `--save FILE` | the save file to read (the headless mode never writes it). `tools/regress.py` gives each run a file that does not exist: an empty save |
| `--fixed`, `--original-physics`, `--smooth-physics` | the window's physics modes (menu.md); the headless mode runs whole frames unless `--dt` says otherwise |
| `--space-demo`, `--camera-follow N` | the Space demo; the camera's follow speed, 1 to 3 (menu.md) |
| `--easy-ball` | the easy difficulty's stick direction on the ball at every difficulty (see [menu.md](menu.md)) |
| `--ballworld` | Ballworld mode from the start: every level is played as the ball alone (see [menu.md](menu.md)). With it, `controlMethod` is 2 and the glove actor `hand` has alpha 0 |
| `--skip-intro`, `--infinite-lives`, `--infinite-health`, `--unlock-all`, `--level-select` | the options of the Enhancements and Cheats menus (see [menu.md](menu.md)) |
| `--goto-level FRAME LEVEL` | at original frame FRAME, start LEVEL as the menu's Go to level does |
| `--log FILE` | also write the program's messages to FILE, each line with the elapsed seconds |
| `--trace` | print the state after each frame: frame counter, mode, level, random seed, glove, ball and camera positions |
| `--hash-every N`, `--hash` | print a hash of the game's whole memory every N frames, or every frame |
| `--shot FRAME`, `--shot-prefix PATH`, `--shot-size W H` | save the picture after FRAME as a PNG file. Needs the window build (`build/linux/weemitt`) and a display; the headless build refuses it |
| `--dt D` | run updates of D original frames each (for example 0.25, four updates per frame); output follows the original frames |
| `--dt-jitter J` | vary each update's length by up to J times D (for example 0.3), the same way every run; with `--pace` the drawn frame may be longer than D and is split into updates as the window does at a frame rate limit (D 1 and J 0.05 imitate a limit of 20) |
| `--trace-updates` | print the glove and ball state after every update |
| `--record FILE` | write an input recording to FILE (any build; see "Recording and replay") |
| `--replay FILE` | run a recording: no FRAMES needed, it ends with the file (see "Recording and replay") |
| `--interp-shot FRAME` | with `--shot-prefix`: after FRAME, save five screenshots at 0, 0.25, 0.5, 0.75 and 1 of the way between the previous frame and FRAME (see [interpolation.md](interpolation.md)). Needs a display and a renderer |
| `--interp-check` | print how far each frame's picture at 0 is from the previous frame's picture. The same needs |
| `--render-scale N`, `--msaa N`, `--texture-filter MODE`, `--widescreen MODE`, `--shot-window` | the Graphics window's picture options for a run, and whole-window screenshots (see [menu.md](menu.md#test-option)) |
| `--debug-input FILE` | drive the game's debug pad `pad[4]` with a script in the format of `--input` (see [menu.md](menu.md#game-debug)) |
| `--debug-action FRAME NAME` | at original frame FRAME, start a debug action: `prev-platform`, `next-platform`, `mark-levels`, `timing`, `prev-song`, `next-song`, `inertia`, `quit`, `hurt` or `autoplay` (the title menu's Z). It can be given several times |
| `--dump FRAME FILE` | after FRAME, write the game's 24 MB of memory to FILE (keep such files out of the repository: they hold game data) |

A mistake on the command line (an unknown option, a value that is not a number or out of range) prints one line and
exits with 2. `--help` lists the options. A fatal error in the game exits with 1.

The `--trace` line holds the frame counter (`fc`), the mode, the level, the random seed (`rseed`) and the glove's,
the ball's and the camera's positions. The positions are the floats' bits in hex: `44160000` is 600.0.
`tools/dtcheck.py` (`f32`) decodes them.

Without `--level`, the game starts normally. With no input it runs its attract mode: the presentation, the
fly-through, and recorded demos of levels 10, 16, 20, 25 and 30, then again. The demos replay recorded buttons,
so they exercise a lot of real play.

### Level numbers

| N | Level | N | Level | N | Level |
| --- | --- | --- | --- | --- | --- |
| 0 to 7 | Hub 1 to Hub 8 | 20 to 24 | Pirates 1, 2, 3, boss, bonus | 38 | Space boss 1 |
| 8 | Cave | 25 to 29 | Prehistoric 1, 2, 3, boss, bonus | 39 | Space boss 2 (cut scene) |
| 9 | Assault course | 30 to 34 | Fear 1, 2, 3, boss, bonus | 40 | Space boss 3 |
| 10 to 14 | Atlantis 1, 2, 3, boss, bonus | 35 to 37 | Space 1, 2, 3 | 41 | Space bonus |
| 15 to 19 | Carnival 1, 2, 3, boss, bonus | | | 42 to 47 | the game's other scenes (42 is WAYROOM, 43 the presentation, 44 the fly-through, 47 the outro) |

Cheats, Go to level and `--goto-level` take 0 to 42. The menu's list holds 0 to 41.

## Recording and replay

A bug that shows only in the window, at the player's frame rate, cannot be found with a scripted headless run. The
recorder writes what reaches the game's logic. The headless build then replays it.

Record with `--record FILE`, or with the Developer menu's "Record input (from the next start)" (`record_input` in
`settings.ini`). With the menu option the program records from the start of every run to
`weemitt-YYYYMMDD-HHMMSS.rec` in the save folder. A window build shows a red REC mark in the top right corner and
puts `[REC]` in the window title. The file is flushed after every update, so a crash keeps it.

Replay with `weemitt --rom ROM --replay FILE [--trace] [--trace-updates] [--hash-every N]`. The replay runs the
recorded updates and ends with the file. It exits with 1 when the run departs from the recording. It takes the level
and the Space demo, easy ball and camera options from the file, and says so when other ones were given. A recording
that cannot be written stops a headless run with an error. The headless build and the window build both do this. `--headless FRAMES`
with `--replay` stops after that many original frames. A window build also prints `--trace` and `--trace-updates`
lines to standard output when it records, in the format of the headless build, so the two outputs can be compared.

What the file holds (`src/port/record.c` describes the format; version 1, little-endian):

- Header: the build id (git version, system, sound player), the `--level` and Space demo options, the settings that
  change the logic (easy ball controls, camera follow speed), a hash of the ROM, a hash of the save, and the save's
  512 bytes. The save holds no ROM data. The replay uses the file's save unless `--save` names another one, and
  warns when that save differs.
- One record for each call of `port_game_update`: its length `dt` (the float the game gets), the button word and
  the stick that `port_buttons`, `port_stick_x` and `port_stick_y` hold at that moment. The game's latching of
  presses between two reads (`port_buttons_latched`) follows from these. A change of a setting is written before
  the update it first affects.
- After an update in which the game read the controller: the word it received (`ControllerStep` reads once per
  frame window). The replay checks it.
- Every 30 updates, a check value: a hash of the `--trace` state (frame counter, mode, level, random seed, and the
  glove, ball and camera positions as bits). The replay checks it. It prints the first update where the run
  departs and says so at the end ("all checks equal" or "DEPARTED").
- At the end, the update count and a hash of the game's memory. A replay prints both hashes. They differ between a
  window run and a replay, always and only because the game's profiler timers (`timers`, `timer.c`) hold
  `osGetCount` values: real time in the window, simulated time in the headless build.

Nothing else reaches the logic. The game's time sources (`osGetCount`) feed only those profiler timers. The game has
no other clock, and no random source except `rseed`, which the updates determine. The settings that change logic
are in the header and in the setting records. The EEPROM is the save.

A recording made by another version of the program, on another system or with the other sound player is still
replayed. The replay prints a note. Where the game logic differs, the checks show the first update that departs.
Between Windows and Linux the trace agrees at updates of exactly one original frame. Shorter updates can differ in
the last bits, because the helpers use the C library's `pow` and `exp` (see [delta-time.md](delta-time.md)).

### Checking that a window run replays

On Linux the window build runs under a virtual display with the software renderer. There is no real keyboard. Four
test variables exist (window builds only, the fourth also in the headless build):

- `WEEMITT_INPUT_SCRIPT=FILE` feeds the window the buttons and stick of a `--input` script, one line per original
  frame.
- `WEEMITT_QUIT_FRAMES=N` quits after N original frames.
- `WEEMITT_TAPS=START,SPACING,COUNT` pushes a key down and a key up event for L at once, COUNT times, every SPACING
  original frames from frame START: a tap that the keyboard state never shows.
- `WEEMITT_DUMP_END=FILE` writes the game's memory to FILE when a recording or a replay ends.

```sh
LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy WEEMITT_INPUT_SCRIPT=in.txt WEEMITT_QUIT_FRAMES=400 \
  xvfb-run -a build/linux/weemitt --rom ROM --space-demo --max-fps 60 --record run.rec --trace --trace-updates > win.out
build/headless/weemitt --rom ROM --replay run.rec --trace --trace-updates > rep.out
grep -E '^(update|frame) ' win.out | cmp - <(grep -E '^(update|frame) ' rep.out)
```

Results: with the real renderer (software OpenGL) and the real sound player on SDL's dummy device, the window run
at `--max-fps` 60, 20 and Unlimited, in the Space demo and on level 5 with scripted input (400 original frames
each), gives a trace of every update that equals the replay's, line for line. A window build with the stand-in
sound player gives the same. Compared with `WEEMITT_DUMP_END`, the memory of a window run and of its replay differ
only in the profiler timers. The renderer and the audio thread do not change the game's logic state.

## The tools

The tools use the test build, `build/headless/weemitt` (`make headless`), and build it themselves. It has no
renderer and uses the stand-in sound player (`src/port/audio_stub.c`). The real sound player (`make AUDIO=mus`)
keeps its state in the game's memory. In the headless mode it runs on the game's clock and gives the same memory in
every run, but its memory differs from the baseline's, which was made with the stand-in.

- `tools/regress.py baseline` builds the baseline commit (BASELINE_REF in the script) in a git worktree and records the state of
  every run: the attract mode for 30000 frames and each of the levels 0 to 47 (level 40 included) for 900 frames with
  generated input, 49 runs in all. Before it builds
  the baseline, it gives the baseline's start-up code two later changes: the fixed address of `port_ram` and the
  value of `ball.phys` that level 40 needs when it starts directly.
  `tools/regress.py check` runs the current build and compares each frame. At the original timing the logic must
  give the same state and the same memory hash in every frame. The memory hash counts pointers to the program's
  own code and data as one value, so a build that only moved the code gives the same hash.
- `tools/regress.py check` fails when a run has no baseline, when a run exits with an error, and when `--runs` names
  a run that does not exist.
- `tools/dtcheck.py LEVEL...` runs each level at dt 1 and at a shorter dt (0.25 unless `--dt` says otherwise) with the
  same controller input, one entry per original frame (regress.py's input, seed 1000 + LEVEL; `--seed N` plays
  another), and compares the frame counter, the mode and the level per
  original frame and the average speeds of the glove, the ball and the camera. `attract` runs the attract mode. A speed
  ratio far from 1 can be a real frame-rate error or runs that took different paths after a small difference. Find the
  first frame where the runs depart and the reason. Running the level with other input seeds helps: a frame-rate error
  shows with every seed, a chaotic split does not. [delta-time.md](delta-time.md#verification) says more. The exit code
  is 1 when the counters differ or a run fails. A speed marked `<-- check` does not change it. Today
  `dtcheck.py 5 20 attract` exits with 1: in level 5 the run at dt 0.25 enters a portal at frame 445 and the run at
  dt 1 does not, and the ball's speed ratio in level 20 is marked. Both are splits after small differences (see
  delta-time.md, "Known limits").
  `--ballworld` runs both in the Ballworld mode. The mode's decisions and counters run only in ticks, and its float
  and climb are scaled by `deltaTime` (`TICK` and `SCALED` in `src/game/ballworld.c`).
  `CollideActorWithPlane` (`collision.c`) can read `bvel[numBounces - 1]` in a call that has not written it: an
  earlier call in the same pass of `CheckCollisions` wrote it. On the N64 the read gets that earlier bounce, because
  each call's frame is at the same address and no call between them reaches that deep. `bvel` is `static` in the port
  for that reason. Level 22 with regress.py's input in the Ballworld mode reaches this read at frame 142.
- `tools/tapcheck.py [--dt D ...]` runs level 35 with scripted L presses (one frame long, spaced 2, 3, 4, 6 and 8 frames
  apart) and counts how many change the glove between `HAND_JOINED` and `HAND_BALLWALK`. Other options go to the
  headless build. `tools/inputtest/build.sh` checks the input latch and the mapping of SDL events to buttons
  ([delta-time.md](delta-time.md#how-fast-the-glove-can-step-on-and-off-the-ball)). It needs no ROM.
- `tools/check_calls.py` lists calls whose arguments differ from the called function's definition. The N64 passed
  arguments in registers, which hid such mismatches; on x86 they break.
- `tools/check_layouts.py` compiles every game file for the N64's MIPS and compares each structure layout with the
  PC build's.
- `tools/list_pointers.py [--globals]` lists the structures with pointer members and the globals in `port_ram` that
  hold pointers, from the Linux build's debug information ([port64.md](port64.md#the-tools-that-measure-it)).
- `tools/mustest/build.sh` compiles the native libmus (`src/audio/mus`) and the synthesizer (`src/audio/synth`)
  with plain and with the game's flags, links them and runs `MusInitialize` with the game's configuration. It
  checks the heap use and the place of each libmus block in the heap against the N64's values. It needs no ROM.
- `tools/audiotest/build.sh [DIR] [ROM]` checks the interpreter of the audio command lists
  (`src/audio/synth/rsp_audio.c`, compiled as C++ with the game's flags). Without a ROM it decodes known ADPCM frames and
  compares them with a straightforward reference decoder, and runs a volume ramp. With the ROM it also loads the
  ROM's audio microcode into a model of the RSP (`tools/audiotest/rspsim.c`). It runs random commands, lists like the
  synthesizer's and every ADPCM wave of the game's sound banks through both. It requires the same DMEM, states and
  output, bit for bit. The model covers only the instructions that the audio microcode uses.
- `tools/audiotest/render.sh ROM OUTDIR [SECONDS]` builds the game with the real sound player in `OUTDIR/build` and
  links `tools/audiotest/render_song.c` with it. The program starts the player as the game does and renders songs
  and sound effects to WAV files in OUTDIR, with a log of every voice change. `analyze_audio.py` then checks the
  files: sound in every file, no clipping, stereo where the pans differ, a reverb tail, and the pitch of held notes
  against the pitch ratios that libmus sent. The files hold the game's audio: give an OUTDIR outside the
  repository. It takes about 20 seconds. [audio-synth-map.md](audio-synth-map.md), section 20, lists its results.

## The library

The library ([library.md](library.md)) runs the same game as the headless build. Its test host,
`build/libhost64/glover-host` (`make libhost64`, from `tools/libhost/libhost.c`; `make libhost32` builds the 32-bit
one in `build/libhost32`), loads it at run time and runs the
game as the headless mode does: the same options (`--rom --headless --level --input --trace --hash --hash-every --dt
--dt-jitter --pace --dump --save`), the same splitting of drawn frames into updates, the same clock and the same
output. The library is `$GLOVER_LIB`, or `libglover.so` next to the host. So the tools take the host in place of the
headless build:

```sh
GLOVER_LIB=build/lib64-stub/libglover.so python3 tools/regress.py check --exe build/libhost64/glover-host
GLOVER_LIB=build/lib64-stub/libglover.so python3 tools/dtcheck.py 5 20 attract --exe build/libhost64/glover-host
GLOVER_LIB=build/lib64-stub/libglover.so python3 tools/ptrdiff.py --other build/libhost64/glover-host --input
GLOVER_LIB=build/lib64/libglover.so python3 tools/ptrdiff.py --audio --other build/libhost64/glover-host --input
GLOVER_LIB=build/lib32-stub/libglover.so python3 tools/regress.py check --exe build/libhost32/glover-host
GLOVER_LIB=build/lib32-stub/libglover.so python3 tools/dtcheck.py 5 20 attract --exe build/libhost32/glover-host
```

The regression baseline holds the stand-in sound player's memory, so `regress.py` takes the stub library. Without
`--save` the host reads weemitt's own save file and never writes it, as the headless build does.

`make libcheck` (`tools/libcheck.py`) checks the rest, for the 64-bit and the 32-bit library (`--bits` chooses one).
Each check prints one line. The 32-bit library is compared with the 32-bit headless builds, the 64-bit one with the
64-bit builds.

- The exports of the Linux libraries are the names in `src/lib/glover.syms`. None has text relocations, and
  they need only `libc.so.6` and `libm.so.6`: no SDL, OpenGL or C++ runtime. It prints the newest glibc version they
  need, as information.
- `glover.h` compiles with `gcc -std=c99 -Wall -Wextra -pedantic -Werror` and with `g++ -std=c++11`.
- `glover_host_gfx` ([library.md](library.md#lists-for-the-hosts-interpreter)): the test host's `--hostgfx 1` over
  levels 5, 12, 13, 20 and 30, with the latest frame, with `--hostgfx-t 0.5`, split in two lists, and with the
  host's fog (`--hostgfx-fog`), for `lib64`, `lib64-hostgfx` and `lib32`. No call fails, no command is unknown, every
  pointer is even, the vertices lie in the game's memory (but for the four of a texture rectangle), every list ends,
  and the triangles equal those of the actors in the game's own lists. With the host's fog, every geometry mode keeps
  `G_FOG`, every render mode blends the fog in its first cycle, every fog colour and position is the host's, each
  entry list sets the fog position before its first triangle, the fog's argument checks hold, and every other
  command equals the one in the lists of a call without the fog. The trace and the memory hash of level 5 stay the same with the call in use, and the stub returns
  `GLOVER_ERR_UNSUPPORTED`. The line gives the mean time of a call.
- The clash host (`glover-host-clash`) defines `osRecvMesg`, `camera`, `hand`, `Random`, `port_ram`, `plog_printf`
  and `port_time_us` itself and exports them. Levels 5 and 13 and the attract mode still give the headless build's
  traces: the library keeps its own symbols.
- With `--host-mxcsr 0xFFC0` the host sets flush-to-zero, denormals-are-zero and rounding toward zero before each
  call. The traces stay the same, and each call returns with the host's MXCSR.
- `--fatal-at 100` makes update 100 stop the game through a hidden option (`glover_set_option(g, 0x7F000001, 100)`,
  documented only here and in `glover.c`). The update returns `GLOVER_ERR_FATAL` with `fatal: test`, later calls
  return `GLOVER_ERR_DEAD`, a second `glover_create` returns `GLOVER_ERR_BUSY`, and the process ends normally.
- `--fatal-at-start` makes the game stop inside `glover_create` (`glover_set_option(NULL, 0x7F000002, 1)`, also a
  hidden option). `glover_create` returns `GLOVER_ERR_FATAL` with `fatal: test` and no handle, and a second one
  returns `GLOVER_ERR_BUSY`.
- `--reenter`: a log callback that calls `glover_status` gets `GLOVER_ERR_REENTER`.
- `--lifecycle` checks the arguments and the end of a game: start levels 48 and -2, `ballworld` 2, a
  `GloverConfig` size that ends inside `clockUs`, `GloverBodies` without its size and `glover_goto_level(43)` give
  `GLOVER_ERR_ARG`; a `GloverConfig` size that ends before `ballworld` leaves the mode off; `glover_destroy` from the log callback gives
  `GLOVER_ERR_REENTER`; after `glover_destroy`, `glover_status` gives `GLOVER_ERR_DEAD`, while
  `glover_audio_rate`, `glover_gl_shutdown` and a second `glover_destroy` still work. On success `glover_create`
  leaves an empty `err`.
- `--marks 100` prints the draw marks (`glover_draw_marks`) of level 5's frame 100: the setup, the sky, the world's
  actors and scenery in turn, and the HUD, at rising addresses.
- `--reload` plays level 5, unloads the library, loads it again and plays it again. Both runs give the headless
  build's trace and hashes.
- `.v64` and `.n64` copies of the ROM give the `.z64`'s trace. A short ROM, a ROM cut to 4 MB and a wrong game code
  give `GLOVER_ERR_ROM`.
- `--thread` runs level 13 on a thread with a 512 KB stack: the same trace.
- `--ballworld` sets `GloverConfig.ballworld`. Levels 5 and 13, and level 13 at `--dt 0.25`, give the headless
  build's traces with `--ballworld`. Both read the mode's own save file, `eeprom-ballworld.bin`.
- The sound: with the real sound player, levels 5 and 13 give `headless64-mus`'s traces. `--audio-hash` reads the
  sound after each update and prints a hash of all frames. Two runs and a run with `--audio-thread` (the sound read
  on a second thread) give the same hash. The count of frames is within one synthesizer frame (1104) of 22047 times
  the seconds of game time, and none is dropped. With `--audio-out FILE` the host writes the samples it reads, and
  weemitt's headless mode writes the sound player's samples with the same option (builds with the real sound player
  only). For level 13 and the attract mode, 900 frames, the two files are the same, byte for byte.
- The volume options: `--volume M E` sets `GLOVER_OPT_MUSIC_VOLUME` and `GLOVER_OPT_EFFECTS_VOLUME` and reads them
  back; 101 gives `GLOVER_ERR_ARG`. Level 13, 900 frames, has a song and sound effects. The music at 0 and the effects
  at 0 each lower the energy of the samples (the mean of the squares), and the two energies add up to the whole within
  5%. Both at 0 give silence. The trace with both at 0, without its memory hash, is the same as without the option:
  the synthesizer's voices in the game's memory hold the scaled volume.
- The camera scale: `--camera-scale S` reads `GLOVER_OPT_CAMERA_DISTANCE_SCALE` (1 at the start), checks that 0 and
  101 give `GLOVER_ERR_ARG`, and sets S. Level 13, 900 frames, with the effects alone: at 1 the samples, the trace and
  the memory hash are the same as without the option. At 0.5 the effects are louder (more than 1.2 times the energy).
  The trace without its memory hash stays the same there, as nothing in that run makes debris or reads a sound's
  handle. Elsewhere a scale other than 1 can change the logic ([library.md](library.md#sound)). A floor in level 12 near two of its ambient sounds at
  points, at a scale of 1/3: with `GLOVER_OFF_AMBIENT` (`options off 7`) the effects have less than a quarter of the
  energy that they have without it (`off 3`).

`python3 tools/libcheck.py --wine` also checks the Windows DLLs under Wine: the 64-bit ones with `$WINE64` or
`wine64` and the prefix `$WINEPREFIX64` or `~/.wine64`, the 32-bit ones with `$WINE32` or `wine` and `$WINEPREFIX32`
or `~/.wine`. Their exports are `glover.syms`, they import only `KERNEL32.dll` and `msvcrt.dll`, and they allow a
random load address (the 64-bit ones also a high one). The Windows test host with each DLL gives the same traces and hashes as
the Linux host with the Linux library of the same build: levels 5, 13 and 36, level 13 at `--dt 0.25 --dt-jitter
0.3` and level 13 with `--ballworld`, 900 frames each, each run with a new empty save. The fatal path, the MXCSR check and `--lifecycle` pass there
too.

`python3 tools/libcheck.py --gl` checks the renderer. `build/libglshot64/glover-glshot` (`tools/libhost/glshot.c`)
opens a hidden OpenGL 3.3 window with SDL, draws frames 100 and 299 of level 5 into a 640x480 framebuffer through the
library, and reads them back. They must equal `build/linux64/weemitt --headless 300 --level 5 --shot 100 --shot 299`,
pixel for pixel; the 32-bit test (`make libglshot32`) is compared with `build/linux/weemitt`. After each call of the
renderer, the OpenGL state must be what `glover.h` promises (each value of [library.md](library.md#drawing)). A
second `glover_gl_init`, options outside their ranges and `glover_gl_read_frame` before a picture must give
`GLOVER_ERR_ARG`, and `glover_gl_shutdown` must work after `glover_destroy`. The stub library must return
`GLOVER_ERR_UNSUPPORTED`. `GLSHOT_LOG=1` prints the library's log. It needs a display (`Xvfb :99` and `DISPLAY=:99`).

The layer (`glover_gl_draw_layer`, `glover_gl_composite`, [library.md](library.md#drawing-over-the-hosts-picture)):
`glshot --layer MASK` draws a layer at each shot and writes it as `PREFIX_layer_FRAME.rgba`, and its depth as
`PREFIX_layerdepth_FRAME.f32`. `libcheck.py --gl` checks:

- before the first lists, with an unknown bit, with no part and with a short size, `glover_gl_draw_layer` gives
  `GLOVER_ERR_ARG`, and so does `glover_gl_composite` before a layer;
- level 5's frames 100 and 299 with all four parts: the layer's RGB equals `glover_gl_draw`'s picture at 640x480,
  pixel for pixel, and its alpha is 255 everywhere;
- a floor of the host world with a drawing view 129 units from the glove, and the actors alone: alpha 0 at the four
  corners and 255 on the glove; the depth there, made linear with `projZ`, is within 13 units (the glove's radius)
  of the glove's distance along the view (124.7 against 128.9);
- the composite over a host picture of red, nearer than the glove left of its pixel and farther to the right: red
  on the left, the layer's colour on the right;
- the composite again with `GLOVER_COMPOSITE_DEPTH`: the depth on the right becomes the glove's, the depth on the
  left stays the host's;
- after a `glover_gl_draw_layer` that fails, `glover_gl_composite` gives `GLOVER_ERR_ARG`;
- a large floor with a view 1549 units from the glove, with the near planes 1 and 50: the glove's depth made linear
  with `projZ` is the same within 0.1% (1544.77 and 1544.98). Made linear with `nearZ` and `farZ`, a near plane of 1
  gives 1537.7;
- the OpenGL state after each call.

## The host world

The host world ([host-world.md](host-world.md)) has its own tests on the synthetic scenes of `tools/hostworld/`:

```sh
python3 tools/hostworld/hwtest.py --rom ROM          # needs make headless headless-displaced
```

`hwtest.py` runs weemitt's headless mode with `--host-scene` and checks:

- `flat.scene`: the ball rests at 9.00 (its radius) and the glove at 13.00, both on terrain 0.
- `seams.scene` in 144 chunks and in one chunk (`cell 1000000`, with the faces in the same order) gives the same
  trace, bit for bit, while the ball rolls across the seams. Without the seam pass the traces differ. With `link
  100`, which links only the chunks near the glove, the ball and the camera, the trace is the same too.
- `terrains.scene`: the ball rolls further on terrain 8 than on 0; it touches terrain 12; on terrain 9 the glove is
  hurt (action 28) and no restart begins; on terrain 6 the ball bursts and a restart begins.
- `void.scene`: the ball and then the glove fall 1500 units into a pit whose floor is a void. The ball is over the
  void (state 1) while it falls and touches it (state 2); the touches of both are counted; no restart begins and
  the glove is not hurt. The safe places are on the ledge, and both rest there again after the host places them.
  The same pit without the void flag costs restarts: the ball bursts in the air. Two variants are made at run time:
  the glove holding the ball falls into the pit, and both report the void and the same count of touches; and the
  void floor is a moving body, whose touches are reported in the same way.
- `hold.scene`: while the glove holds the ball, a place of the glove alone and then of the ball alone moves both,
  and the glove still holds it; a place of both apart makes the glove let go.
- `water.scene`: a floor under level 12's water; the ball rests on it at -291 and does not float.
- `off.scene` in levels 12, 13 and 26: with the level's enemies and collectables switched off, none is in the game
  after the restart that the death pad causes; without the option some are. Under the pointer check the scene runs
  600 frames in every level, 0 to 42, with none left on.
- `lift.scene` at dt 1 and at dt 0.25: the glove stays 13 above the lift's top within 0.05. `slider.scene`: the
  glove keeps its place on the slider within 0.05 at both.
- `turntable.scene` at dt 1 and at dt 0.25: over 200 frames the glove's offset from the axis turns with the body
  within 2 degrees and keeps its radius within 0.5, and its yaw advances by the body's turn within 2 degrees. A
  variant with the ball resting on the turntable checks the ball's offset in the same way.
- `conveyor.scene` at dt 1 and at dt 0.25: the glove rides the conveyor strip at 1 unit per original frame within
  5%, and stops after its end; the distance at dt 0.25 is that at dt 1 within 1%. A variant puts the ball on the strip:
  it rides at 1.42 units per frame within 0.02 (see [host-world.md](host-world.md#conveyors)) and leaves the strip.
  With a floor 1 unit under the strip it rides at 1 within 0.005. Another rolls the ball across the strip: its
  height stays within 0.05. The strip with conveyors along +z, -x and a diagonal (0.6, 0, -0.8), at dt 1 and 0.25:
  the glove rides at the conveyor's velocity within 0.01 in each axis.
- `beltbody.scene` at dt 1 and at dt 0.25: the ball rides the slider's own conveyor at 0.3 per frame while the slider
  moves across, and the glove on the turntable rides 0.3 per frame along the turntable's own x, which turns.
- `hostwater.scene` at dt 1 and at dt 0.25: the glove swims (water state 3) and the ball floats (state 4), at -20
  on average within 3 units, and within 8 units at every frame. When the water goes, the ball rests on the floor at
  -191. Variants: a current of 0.25 carries the floating ball at 7 to 9.5 times the current; a reload of the level
  builds the water again (`rebuilds` 1) and the ball floats again; with a host water elsewhere, level 12's own water
  stays empty.
- A generated scene puts the glove onto the ball, where it walks on it (`HAND_BALLWALK`), and then places both apart:
  each goes to its place, and the glove lets go.
- `remove.scene`: the slider is removed while the glove stands on it; the glove then stands on the floor at 13.
- `reload.scene`: a start of the same level builds the world once again (`rebuilds` 1), and the ball rests again.
  `respawn.scene`: the host's respawn stays after the reload; a restart then puts the glove there.
- `view.scene`: a drawing view gives the same positions and state as without it, but `rseed` can differ, and the
  memory hash differs. The view's far distance (4000) becomes the game's `farPlaneDist`, which decides which actors
  the game updates, and those draw random numbers (host-world.md, "The camera"). The same scene with the level's own
  far distance (1000) gives the same trace, `rseed` too. `mirror.scene` in levels 5, 13 and 26: the same trace and
  memory hash as without a view.
- `steer.scene`: with a steering view along +x and the stick pushed up, the glove walks along +x within 10 degrees.
- A generated terrain of 39,200 triangles, written to `build/hostworld/` at run time: the time of 1000 frames.
- Every scene in the build with `PORT_PTR_CHECK` (`make headless-displaced`) without a fatal error. Two variants of
  the turntable run there too: the turning body removed under the glove, and freed by a reload of the level. So do
  the conveyor freed by a clear and by a reload, and a scene with the turntable, a conveyor and a host water in every
  level, 0 to 42, with a reload of the level at frame 350.
- With `--baseline-exe EXE`, a headless build of an earlier commit (built in a `git worktree`): every scene that the
  older build can read gives the same trace with both at dt 1 and at dt 0.25, except the scenes in `CHANGED`, whose
  behaviour was meant to change. The `water` field of the `bodies` line is left out of the comparison.

`libcheck.py` runs every scene through the library's test host (`--scene`), at dt 1 and at `--dt 0.25 --dt-jitter
0.3`: the trace, the hashes and the `bodies` lines must equal the headless build's, on 32 and 64 bits. With
`--wine`, `terrains`, `lift`, `seams`, `turntable`, `conveyor`, `beltbody` and `hostwater` must give the same through
the DLLs. The
host's `--coll-args` checks each error code, the 2 MB reserve of the heap, version 1's `GloverBodies` size, a
version 2 host (a `GloverMesh` that ends before `conveyor`, a 40-byte `GloverCollStats`, `apiVersion` 2 accepted), a
configuration of version 4, which is refused, and the waters' checks. It also
checks that a triangle with an edge under 0.025 is left out, that a failed `glover_coll_set_static` leaves the
world as it was, and that `glover_set_view` refuses an up vector along the view. With
`--gl`, the mirror view's (`GLOVER_VIEW_TEST_GAME`) picture must equal the game's own, pixel for pixel, a view of its own must draw another
picture, and `glover_gl_options` must refuse widescreen 2 while a drawing view is set.

## Linux and Windows

The Windows build gives the same state trace as the Linux build. This was checked under Wine for 3000 frames of the
attract mode, and for 600 frames of levels 5, 35, 36 and 37 with the dtcheck input. Compare traces, not memory hashes, between the two. The Windows program loads at 0x400000. The ROM
offsets in the game's data (up to 0x77DE20) fall in the program's address range there. The hash (`port_hash_ram` in
`src/port/statehash.c`) counts them as pointers to the program, and the Linux build does not. Linking the Windows build at a
higher address (`-Wl,--image-base=0x08000000`, as the studio and the brute forcer do) would remove this cause. Raw
sound data in `pbankBuffer` that looks like an address in `port_ram` would still differ.
The memory also holds the save, so two runs of the 64-bit builds, whose hashes do compare, must start from the
same save: give each run a new `--save` file.
