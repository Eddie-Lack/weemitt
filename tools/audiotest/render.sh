#!/bin/sh
# Renders Glover's songs and sound effects with the port's sound player and checks them.
#
#   tools/audiotest/render.sh ROM OUTDIR [SECONDS]
#
# It builds the game's objects with the real sound player (make AUDIO=mus RENDERER=null) in OUTDIR/build, links
# render_song.c with them in place of the platform layer, writes the WAV files and logs to OUTDIR and runs
# analyze_audio.py on them. The WAV files hold the game's audio: give an OUTDIR outside the repository.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rom=$1
out=$2
seconds=${3:-20}
if [ -z "$rom" ] || [ -z "$out" ]; then
    echo "usage: $0 ROM OUTDIR [SECONDS]" >&2
    exit 2
fi
mkdir -p "$out"
out=$(cd "$out" && pwd)
build=$out/build
make -C "$root" AUDIO=mus RENDERER=null BUILD="$build" -j4 >/dev/null
game="-m32 -msse2 -mfpmath=sse -malign-double -std=gnu89 -O2 -funsigned-char -fno-strict-aliasing -fwrapv \
      -ffp-contract=off -fno-pie -Wall -DM2CTX -D_LANGUAGE_C -DF3DEX_GBI -D_FINALROM -DNDEBUG -DPORT \
      -I$root/gen -I$root/src/port -I$root/include -I$root/include/PR -I$root/src/audio/mus \
      -include $root/src/port/port_pre.h"
gcc $game -c "$here/render_song.c" -o "$build/render_song.o"
objs=$(find "$build/src" "$build/gen" -name '*.o' ! -path "$build/src/port/platform.o" | sort)
wrap="-Wl,--wrap=alSynStartVoice -Wl,--wrap=alSynStopVoice -Wl,--wrap=alSynSetVol -Wl,--wrap=alSynSetPan \
      -Wl,--wrap=alSynSetFXMix -Wl,--wrap=alSynSetPitch"
gcc -m32 -no-pie $wrap -o "$build/render_song" "$build/render_song.o" $objs -L/usr/lib/i386-linux-gnu \
    -l:libSDL2-2.0.so.0 -lm
"$build/render_song" "$rom" "$out" "$seconds" 2>"$out/render_song.err"
[ -f "$here/analyze_audio.py" ] && python3 "$here/analyze_audio.py" "$out"
