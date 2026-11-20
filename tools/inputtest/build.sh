#!/bin/sh
# Builds tools/inputtest into DIR (default build/inputtest) and runs it. It checks the input latch and the mapping
# of SDL events to buttons. It needs no ROM and no window.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/inputtest}
mkdir -p "$out/sdl-include/SDL2"
if [ -d /usr/include/x86_64-linux-gnu/SDL2 ]; then cp /usr/include/x86_64-linux-gnu/SDL2/* "$out/sdl-include/SDL2/"; fi
gcc -m32 -std=gnu99 -O2 -Wall -I"$out/sdl-include" -I/usr/include/SDL2 -I"$root/src/port" \
    -o "$out/inputtest" "$here/inputtest.c" "$root/src/port/inputevent.c" "$root/src/port/inputlatch.c" \
    "$root/src/port/settings.c" -L/usr/lib/i386-linux-gnu -l:libSDL2-2.0.so.0 -lm
"$out/inputtest"
