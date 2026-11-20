#!/bin/sh
# Builds tools/audiotest into DIR (default build/audiotest) and runs it. rsp_audio.c is compiled as C++ with the
# game's flags, as the Makefile builds src/audio, but with warnings on. Give the ROM (a .z64 file) to also compare the
# interpreter with the ROM's audio microcode:
#
#   tools/audiotest/build.sh [DIR] [ROM]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/audiotest}
rom=$2
mkdir -p "$out"
plain="-m32 -std=gnu99 -O2 -Wall -fno-pie -include $root/src/port/ptr32.h -I$root/src/audio/synth -I$root/include \
       -I$root/include/PR -D_LANGUAGE_C"
game="-x c++ -std=gnu++14 -fno-exceptions -fno-rtti -fno-finite-loops -m32 -msse2 -mfpmath=sse -malign-double -O2 \
      -funsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-aggressive-loop-optimizations \
      -fno-delete-null-pointer-checks -fno-pie -Wall -DM2CTX -D_LANGUAGE_C -DF3DEX_GBI -D_FINALROM -DNDEBUG -DPORT \
      -I$root/gen -I$root/src/port -I$root/include -I$root/include/PR -I$root/src/game \
      -include $root/src/port/port_pre.h"
g++ $game -c "$root/src/audio/synth/rsp_audio.c" -o "$out/rsp_audio.o"
gcc $plain -c "$here/rspsim.c" -o "$out/rspsim.o"
gcc $plain -c "$here/audiotest.c" -o "$out/audiotest.o"
gcc -m32 -no-pie -o "$out/audiotest" "$out/audiotest.o" "$out/rspsim.o" "$out/rsp_audio.o" -lm
"$out/audiotest" $rom
