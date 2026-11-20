#!/bin/sh
# Builds tools/mustest into DIR (default build/mustest) and runs it. Each libmus and synthesizer file is compiled
# twice: as C with the plain flags below, and as C++ with the game's flags (as the Makefile builds src/audio, but with
# warnings on). The program links the plain objects. In C on the 32-bit host, the PTR32 members of the structures
# (src/port/ptr32.h) are plain pointers.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/mustest}
mkdir -p "$out/plain" "$out/game"
plain="-m32 -std=gnu99 -O2 -Wall -include $root/src/port/ptr32.h -DPORT_C_BEGIN= -DPORT_C_END= \
       -I$root/gen -I$root/src/audio/mus -I$root/include -I$root/include/PR -D_LANGUAGE_C"
game="-x c++ -std=gnu++14 -fno-exceptions -fno-rtti -fno-finite-loops -m32 -msse2 -mfpmath=sse -malign-double -O2 \
      -funsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-aggressive-loop-optimizations \
      -fno-delete-null-pointer-checks -fno-pie -Wall -DM2CTX -D_LANGUAGE_C -DF3DEX_GBI -D_FINALROM -DNDEBUG -DPORT \
      -I$root/gen -I$root/src/port -I$root/include -I$root/include/PR -I$root/src/game \
      -include $root/src/port/port_pre.h"
objs=
for src in "$root"/src/audio/mus/*.c "$root"/src/audio/synth/*.c; do
    name=$(basename "$src" .c)
    gcc $plain -c "$src" -o "$out/plain/$name.o"
    g++ $game -c "$src" -o "$out/game/$name.o"
    objs="$objs $out/plain/$name.o"
done
gcc $plain -c "$here/mustest.c" -o "$out/plain/mustest.o"
gcc -m32 -o "$out/mustest" $objs "$out/plain/mustest.o" -lm
"$out/mustest"
