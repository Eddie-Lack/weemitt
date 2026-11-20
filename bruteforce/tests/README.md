# Tests

`make test` (or `python3 tests/run.py`) runs them with the Linux build. `--wine` also runs the Windows build under
Wine. `--quick` plays shorter games. The ROM comes from `--rom`, `$GLOVER_ROM` or `glover.z64` in the working
directory.

| Test | What it proves |
| --- | --- |
| saveload, boss level, Atlantis 1, power-on | a loaded state plays on as the game did: the same hash of the whole state every 5 frames, after loads of older and newer states; a loaded state saves the same bytes; the heap's gap (the part of the heap that has never been written, see [docs/engine.md](../docs/engine.md)) is still zero |
| a state carries over to another process | a state saved in one process and loaded in another gives the same hashes |
| a read, a draw and a logic frame equal a whole frame | the parts of a game frame, run apart (`glover play --events`), give the same state trace as whole frames |
| a read and a draw at a stop inside a logic frame | a group of events at a stop at the start of `MainGameLoop` (src/run.h) gives the same states as the same events just before the frame |
| a stop that is not reached | a group whose stop the frame does not reach runs after the frame, and `glover play` warns |
| a short search ends and writes its best inputs | `glover search` with two workers and no buttons ends after its time and writes `best.txt` |
| the trimmed draw plays the same game | `glover play --trim-draw` gives the same state trace as the full draw, frame by frame: from power-on, in the boss level, and in a script of reads, draws and logic frames |
| plays the same game as the base build | with `--build` another build (`OPT=fast`, `OPT=znver4`) or with `--wine` the Windows build: the same input gives the same state trace in every frame as `--base` (default `build/linux`) |

These tests need BizHawk's Mupen64Plus core, built once with `tools/sync/bizhawk/setup.sh` (it needs git, gcc, g++
and the development files of SDL2, libpng and zlib). With `GLOVER_EMULATOR=mupen64plus` they use the system's
mupen64plus instead: run `gloveworld/tools/emu/setup.sh` once and start a display (`Xvfb :99 &`).

| Test | What it proves |
| --- | --- |
| `tests/sync.py` | the engine gives the same game state as the real ROM in mupen64plus, read by read: the attract mode and a stretch of each boss level ([docs/sync.md](../docs/sync.md)) |
| `tests/movie_test.py` | the `.bk2` and `.m64` files read and write back the same; a made-up BizHawk movie converts, and the engine matches the emulator on it; new inputs go back into the movie (`--quick`: the files only) |
| `tests/points_test.py` | a run of whole frames from a saved state plays, a search for a place reaches it no later than the base run, and its inputs played again reach it at the read the search gave; with the strict limits the result breaks them no more often than the base run (no emulator needed; `--wine` with `--build build/windows` for the Windows build) |
| `tests/search_test.py` | a movie converts, a short search improves it, and `tools/check.py` confirms the result in the emulator |
