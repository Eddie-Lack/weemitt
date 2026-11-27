# Guide for agents

weemitt is a native PC port of the N64 game Glover (USA, NGVE). Its game code is the C of the matching
decompilation [gloveworld](https://github.com/Eddie-Lack/gloveworld), compiled for 32-bit x86 and changed for
the PC. Unlike gloveworld, nothing here has to match the ROM. Read this file first, then [docs/](docs/README.md).

## Rules

- **Never commit a ROM**, any file that holds data copied from it (memory dumps, savestates, recordings,
  extracted textures or sounds), or a save file. The program reads the player's own ROM at start-up.
- **Never use leaked original source code**, the N64 SDK's source (libultra, libaudio, libmus), or code from
  other ports, in any form. Write the code from the ROM's machine code, public documentation, or your own
  design. gloveworld's tools may be used.
- **Keep the game's memory layout.** The game's data lives at its N64 offsets inside `port_ram` (see
  [docs/architecture.md](docs/architecture.md)). Do not change the type or size of a global or of a structure
  that is stored there or loaded from the ROM. New state goes in new globals outside it.
- **The logic must stay exact at the original timing.** With `deltaTime` 1 and `deltaTick` 1 (`--fixed`, or
  headless without `--dt`), the game must behave bit for bit as before. `python3 tools/regress.py check` proves
  it: every run must say "same as the baseline". The baseline is the commit named in `tools/regress.py`
  (BASELINE_REF); an empty name means the repository's first commit.
- **At a high frame rate the game must play as it does at the original rate.** `python3 tools/dtcheck.py LEVEL`
  compares the two (see [docs/delta-time.md](docs/delta-time.md)).
- **Write all prose (docs, comments, commits, PR text) in plain language** (ISO 24495-1): short sentences, one
  idea each, terms defined on first use. No em dashes. No AI model names.
- **Do not end sentences with a purpose clause** that explains why the preceding detail matters, such as "so
  that X instead of Y". State the fact and stop.
- Comment code where the reason is not obvious, in plain sentences. Follow the style of the code around you.

## The brute forcer (`bruteforce/`)

`bruteforce/` holds the brute forcer: an engine that runs the game's logic with cheap savestates, and a search that
looks for faster ways through the game. It has its own Makefile, README and docs (`bruteforce/docs/`, start with
`plan.md`). Its build compiles the game's code from `src/` with this repository's flags; the normal build does not
change.

- **The Windows program comes first.** Test it under Wine (`bruteforce/tests/run.py --wine`).
- **The engine must play the real game.** `bruteforce/tests/sync.py` compares it with the ROM in BizHawk's
  Mupen64Plus core, read by read; it must pass after any change to the game's code or the engine. Found results need
  no emulator check.
- **Never commit a savestate** or a state file: it holds the game's memory, which holds data from the ROM.
- Its README has a Thanks section that names the developers of the tools and projects used or used as reference.
  Check each name against the tool's own pages before adding it.

The studio ([docs/studio.md](docs/studio.md)) is the window program with the brute forcer: `make studio` and
`make studio-windows` (`studio.mk`). Its code in `platform.c` and `menu.cpp` stands behind `PORT_STUDIO`.

```sh
make studio-windows -j4 && make studio -j4       # build/studio-windows/weemitt-studio.exe, build/studio/weemitt-studio
WEEMITT_STUDIO_SELFTEST=1 build/studio/weemitt-studio --rom ROM --level 13 --headless 1500 --input INPUT
cd bruteforce && make -j4 && make windows -j4    # build/linux/glover, build/windows/glover.exe
python3 tests/run.py --rom ROM [--wine]          # the engine tests
python3 tests/sync.py --rom ROM                  # the engine against the emulator (tools/sync/bizhawk/setup.sh once)
```

## Everyday work

```sh
make -j4                      # build/linux/weemitt (needs gcc-multilib and the 32-bit SDL2 runtime)
make headless -j4             # the test build (build/headless): no renderer, the stand-in sound player
make windows                  # build/windows/weemitt.exe (needs MinGW and deps/SDL2-mingw, see docs/building.md)
make libcheck                 # the game as a library (build/lib64/libglover.so) and its checks, docs/library.md
python3 tools/regress.py check            # logic unchanged at the original timing? (set GLOVER_ROM)
python3 tools/dtcheck.py 5 20 attract     # same behaviour at dt 0.25? (exits 1 today: testing.md says why)
python3 tools/menutest.py                 # the menu's windows and settings.ini, in a window (needs a display)
./build/headless/weemitt --rom ROM --headless 600 --level 5 --trace   # one level, state per frame
```

| Tool | What it does |
| --- | --- |
| `tools/regress.py` | runs the attract mode and every level, compares each frame's state and memory hash with the baseline |
| `tools/dtcheck.py` | runs a level at dt 1 and at a shorter dt with the same input; compares counters and speeds |
| `tools/check_calls.py` | lists calls whose arguments differ from the definition (they break on x86) |
| `tools/check_layouts.py` | compares every structure layout with the N64's |
| `tools/list_pointers.py` | lists the structures and globals in `port_ram` that hold pointers |
| `tools/cfunc.py` | prints or replaces one C function definition |
| `tools/libcheck.py` | checks the library (`make libcheck`, [docs/library.md](docs/library.md)): exports, imports, and the test host's runs |
| `tools/gen/` | regenerates `gen/` from a gloveworld checkout (`make gen`) |
| `tools/menutest.py` | where the menu's windows open, and that settings.ini keeps no one-run option |

The tools find the ROM through `GLOVER_ROM` or `--rom`. [docs/testing.md](docs/testing.md) lists every tool and what
it needs.
