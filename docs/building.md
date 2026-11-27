# Building

The port is a 32-bit x86 program by default (see [architecture.md](architecture.md)). The same sources also build
as a 64-bit program (see "64-bit builds" below and [port64.md](port64.md)). You need your own copy of the Glover
(USA) ROM to run it; the build itself does not need the ROM.

## Linux

On Debian or Ubuntu:

```sh
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install gcc-multilib g++-multilib libsdl2-dev libsdl2-2.0-0:i386 libgl1:i386 libgl1-mesa-dri:i386
make -j4
./build/linux/weemitt --rom /path/to/glover.z64
```

Debian and Ubuntu cannot install the 64-bit and 32-bit SDL2 development packages together. The 64-bit headers
work for the 32-bit build; the Makefile copies their one architecture-specific header next to the build and links
the 32-bit runtime library. On other distributions, install the 32-bit SDL2 library and set `SDL_CFLAGS` and
`SDL_LIBS` on the make command line.

`make headless` builds the test build without the renderer, in `build/headless`. `make RENDERER=null` builds without
the renderer into `build/linux`; a later `make` links the program with its renderer again.

The tools are Python 3 programs. `make gen`, `make check`, `check_layouts.py`, `check_ptr_slots.py`,
`list_pointers.py` and `cmpcode.py` also need pyelftools (`python3 -m venv .venv && .venv/bin/pip install
pyelftools`), and `check_layouts.py` needs clang. `PYTHON=.venv/bin/python` tells `make gen` and `make check` which
Python to use. By default they use gloveworld's virtual environment when it exists, and `python3` otherwise.
testing.md lists every tool and what it needs.

Every build needs a C++ compiler: `g++ -m32` on Linux and `i686-w64-mingw32-g++` for Windows. The game's files
(`src/game/*.c`), the sound player (`src/audio`) and the runtime's files in `PORT_CXX_SRCS` are C compiled as C++,
with the game's flags (`GAME_CXXFLAGS`, see [port64.md](port64.md)). The rest of the runtime and `gen/` are C
(`GAME_CFLAGS`).

The in-game menu is C++ too (Dear ImGui, in `third_party/imgui`). Its files get the host flags (`HOST_CXXFLAGS`),
never the game's flags, and the program is linked with the C++ compiler. The Windows build links libstdc++ and
libgcc statically, so it needs no extra DLLs. The headless build (`RENDERER=null`) links `src/port/menu_null.c`
instead, does not contain ImGui and is linked with the C compiler.

The build includes the real sound player: libmus, libaudio's synthesizer and the interpreter of its audio commands
(`src/audio/`). The program opens a sound device through SDL; `--no-sound` turns it off. `make AUDIO=stub` builds a
silent stand-in instead, which the tests use (`make headless`). The two kinds of build can share a build directory:
changing `AUDIO` rebuilds what depends on it.

## Windows

Cross-compile from Linux with MinGW:

```sh
sudo apt-get install mingw-w64 g++-mingw-w64-i686 cmake ninja-build
tools/build_sdl_mingw.sh        # builds SDL2 for 32-bit Windows into deps/SDL2-mingw (git clones SDL)
make windows -j4                # build/windows/weemitt.exe
cp deps/SDL2-mingw/i686-w64-mingw32/bin/SDL2.dll build/windows/
```

Without SDL2 for MinGW, `make windows` stops with a message that names the script. With MSYS2 on Windows, the
MINGW32 environment's `mingw-w64-i686-gcc` and `mingw-w64-i686-SDL2` packages provide the same; pass
`SDL_PREFIX=/mingw32` to `make windows`.

The Windows program is a window program. Started from a console, it writes its messages (`--help`, `--trace`) to
that console, unless they are redirected to a file. This has not been checked on Windows itself.

The Build workflow (`.github/workflows/build.yml`) builds the Linux and the Windows program on every push and keeps
them as build artifacts on the run's page.

## 64-bit builds

`make BITS=64` builds any target as an x86-64 program. Four targets name the usual ones:

| Target | Program |
| --- | --- |
| `make linux64` | `build/linux64/weemitt` |
| `make windows64` | `build/windows64/weemitt.exe` |
| `make headless64` | `build/headless64/weemitt`: the headless test build |
| `make headless64-mus` | `build/headless64-mus/weemitt`: the headless build with the real sound player |

The 64-bit Linux build uses the system's 64-bit SDL2 (`libsdl2-dev`) and needs no multilib packages. It is linked
as a position-independent program, so it loads above 4 GB. The 64-bit Windows build needs the x86-64 MinGW
compiler and SDL2 built with it:

```sh
sudo apt-get install g++-mingw-w64-x86-64
tools/build_sdl_mingw.sh x86_64   # builds SDL2 for 64-bit Windows into deps/SDL2-mingw
make windows64 -j4
cp deps/SDL2-mingw/x86_64-w64-mingw32/bin/SDL2.dll build/windows64/
```

To run it under Wine, install `wine64` and use a 64-bit prefix (`WINEPREFIX=~/.wine64 WINEARCH=win64`). The
studio, the Space demo and the brute forcer stay 32-bit.

## The library

`make lib64` builds the game as a 64-bit Linux library with no window, `build/lib64/libglover.so`, for a host
program such as a game mod. It has the renderer, which draws with the host's OpenGL context. `make lib64-stub`
builds it with the stand-in sound player, for the tests. `make libhost64` builds the test host and `make libcheck`
runs the library's checks. `make lib-windows64` builds `build/lib-windows64/glover.dll` with the 64-bit MinGW
compiler (no SDL needed), and `make libhost-windows64` its test host. The same targets with 32 in place of 64 build
the 32-bit library, DLL and test hosts. [library.md](library.md) describes the library and its interface.

## Running

[playing.md](playing.md) is the guide for players: the ROM, the keys and the save folder. The program looks for the
ROM next to itself and in the working directory: first under the names `baserom.us.z64`, `glover.z64`, `Glover.z64`
and a few others, then any file that ends in `.z64`, `.v64` or `.n64` and whose header is Glover (USA). It also takes
`--rom FILE`. It accepts the `.z64`, `.v64` and `.n64` byte orders. Saves go to the user's data folder (SDL's
preference path: `~/.local/share/weemitt/Glover/eeprom.bin` on Linux, `%APPDATA%\weemitt\Glover\eeprom.bin` on
Windows; Ballworld mode uses `eeprom-ballworld.bin`, see [menu.md](menu.md)). The menu's settings go to
`settings.ini` in the same folder (see [menu.md](menu.md)).

## The Space demo

`make space-demo` builds `build/space-demo/weemitt`, and `make space-demo-windows` builds
`build/space-demo-windows/weemitt.exe` (copy SDL2.dll next to it, as for the normal Windows build). Any build also
runs the demo with `--space-demo`. The code is in `src/port/spacedemo.c`. The demo:

- starts in Space 1, without the title screens;
- joins Space 1, 2 and 3 into one run. A level's exit portal works as a door: the glove that brings the ball to it
  goes straight into the next level, without the level-done spin and the score screen;
- shows the time of each level and their sum after Space 3. A level's time is the game's own level timer
  (`player.levelFrames`), from the level's start to the glove reaching its exit. It stops while the game is paused;
- uses medium difficulty, with the easy difficulty's ball controls: the controls are not reversed while the glove
  stands on the ball;
- does not run out of lives (the game's own cheat 3, INFINITE LIVES: `cheatActivated` holds the value 8). Quitting from the pause menu, or pressing A on the screen
  of times, starts the run again in Space 1.

## Regenerating the tables

`gen/` describes the game's memory layout. It changes only when the game's data declarations change. To rebuild
it you need a gloveworld checkout that has run `make setup` and `make` (its README explains them), clang, the MIPS
binutils and pyelftools:

```sh
make gen GLOVEWORLD=../gloveworld
```
