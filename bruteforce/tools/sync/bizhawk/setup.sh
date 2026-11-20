#!/bin/bash
# Builds BizHawk's mupen64plus core for Linux: the emulator that the sync tools run (docs/sync.md, "The emulators").
#
#     tools/sync/bizhawk/setup.sh [BIZHAWK_SOURCE]
#
# BizHawk 2.11.1 runs N64 movies on its own fork of mupen64plus's core 2.0, with its own high-level RSP plugin and
# audio plugin. Their timing differs from the system's mupen64plus. This script fetches those three from BizHawk's
# repository at tag 2.11.1 (or takes a checkout of it as BIZHAWK_SOURCE), and builds them with a small shim for the
# Win32 calls (Windows.h here). It also builds nullvideo.so and the input plugin pollinput.so against the core's own
# headers. Everything goes to build/bizhawk. Nothing of BizHawk's is committed.
#
# Needs git, gcc, g++ and the development files of SDL2, libpng and zlib.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=$ROOT/build/bizhawk
TAG=2.11.1
mkdir -p "$OUT"

if [ -n "$1" ]; then
    SRC=$(cd "$1" && pwd)/libmupen64plus
else
    SRC=$OUT/src/libmupen64plus
    if [ ! -d "$SRC/mupen64plus-core" ]; then
        rm -rf "$OUT/src"
        git clone --quiet --depth 1 --branch "$TAG" --filter=blob:none --sparse \
            https://github.com/TASEmulators/BizHawk.git "$OUT/src"
        git -C "$OUT/src" sparse-checkout set libmupen64plus/mupen64plus-core libmupen64plus/mupen64plus-rsp-hle \
            libmupen64plus/mupen64plus-audio-bkm
    fi
fi
API=$SRC/mupen64plus-core/src/api
CFLAGS_COMMON="-O2 -fPIC -ffp-contract=off -fno-strict-aliasing -w"

echo "setup: the core"
OBJ=$OUT/obj/core
mkdir -p "$OBJ"
CORE_SRCS="api/callbacks.c api/common.c api/config.c api/debugger.c api/frontend.c api/vidext.c
main/main.c main/util.c main/cheat.c main/eventloop.c main/md5.c main/rom.c main/savestates.c main/workqueue.c
main/sdl_key_converter.c memory/dma.c memory/flashram.c memory/memory.c memory/n64_cic_nus_6105.c memory/pif.c
memory/tlb.c plugin/plugin.c plugin/dummy_video.c plugin/dummy_audio.c plugin/dummy_input.c plugin/dummy_rsp.c
r4300/r4300.c r4300/exception.c r4300/interupt.c r4300/profile.c r4300/pure_interp.c r4300/recomp.c r4300/reset.c
r4300/empty_dynarec.c osal/dynamiclib_unix.c osal/files_unix.c main/zip/ioapi.c main/zip/zip.c main/zip/unzip.c
debugger/debugger.c debugger/dbg_memory.c debugger/dbg_breakpoints.c debugger/dbg_decoder.c"
# -fcommon: the 2013 sources define globals in headers. The pure interpreter is the only CPU emulator that BizHawk
# offers besides the cached one; the dynamic recompiler is left out (empty_dynarec.c).
CORE_FLAGS="$CFLAGS_COMMON -fcommon -fvisibility=hidden -include Windows.h -I$HERE -I$SRC/mupen64plus-core/src
-DNO_ASM -DDBG -DNOCRYPT -DNOUNCRYPT -DIOAPI_NO_64 -DM64P_PARALLEL $(sdl2-config --cflags)"
for f in $CORE_SRCS; do
    gcc $CORE_FLAGS -c "$SRC/mupen64plus-core/src/$f" -o "$OBJ/$(echo "$f" | tr / _).o"
done
g++ $CFLAGS_COMMON $(sdl2-config --cflags) -I$HERE -I$SRC/mupen64plus-core/src -DNO_ASM -DDBG \
    -c "$SRC/mupen64plus-core/src/osd/screenshot.cpp" -o "$OBJ/screenshot.o"
g++ -shared -o "$OUT/libbhcore.so" "$OBJ"/*.o $(sdl2-config --libs) -lpng -lz -lpthread -ldl -lm

echo "setup: the RSP plugin"
RSP=$SRC/mupen64plus-rsp-hle/src
gcc $CFLAGS_COMMON -fvisibility=hidden -I"$API" -shared -o "$OUT/libbhrsp.so" \
    $(ls "$RSP"/*.c | grep -v osal_dynamiclib_win32.c) -ldl -lm

echo "setup: the audio plugin"
AUD=$SRC/mupen64plus-audio-bkm
# The plugin uses MSVC's min and max macros, and stdarg.h without including it.
gcc $CFLAGS_COMMON -fvisibility=hidden -I"$API" -include stdarg.h "-Dmin(a,b)=((a)<(b)?(a):(b))" \
    "-Dmax(a,b)=((a)>(b)?(a):(b))" -shared -o "$OUT/libbhaudio.so" "$AUD/main.c" "$AUD/osal_dynamiclib_unix.c" \
    -ldl -lm

echo "setup: the video and input plugins"
gcc $CFLAGS_COMMON -I"$API" -shared -o "$OUT/nullvideo.so" "$HERE/nullvideo.c"
gcc $CFLAGS_COMMON -I"$API" -shared -o "$OUT/pollinput.so" "$ROOT/tools/sync/pollinput.c" -ldl
echo "setup: done, in $OUT"
