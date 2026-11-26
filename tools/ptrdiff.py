"""Compare the game's memory of the headless build with the displaced build's, word by word.

The displaced build (make headless-displaced, docs/port64.md) stores every pointer of the game's memory as the same
32-bit word as the normal headless build: PORT_PTR_BASE plus the offset into port_ram. Its port_ram lies 32 MB
higher. So the two builds' memory must be equal, word for word, at every frame. The only exception is a pointer to a
function: the normal build stores the function's address, the displaced build a handle (PORT_FN_BASE + 4 * index).

A word that differs is a pointer that the game stored without encoding (the displaced build has its host address
there, 0x0E000000 or higher), or a value that the game computed from such a pointer. Each one is named by the global
that holds it (from gen/ram.S, through the headless build's symbols).

The 64-bit headless build (make headless64) stores the same words as the displaced build, so --other compares it
the same way.

Usage: python3 tools/ptrdiff.py [--rom ROM] [--level N]... [--frame F]... [--input] [--audio] [--other EXE]
  defaults: levels 0, 5 and 13; frames 0, 10 and 300. It needs build/headless and build/headless-displaced.
  --input  play regress.py's input script of each level (walking, jumps, the ball, the pause menu); without it the
           controller is idle
  --audio  compare the builds with the real sound player instead (make headless-mus headless-displaced-mus). Its
           memory (audio_memory and alGlobals) is in the game's memory and is compared too
  --other  compare the headless build with this program instead of the displaced build (with --audio, the headless
           build with the real sound player)
It exits with 1 when a word differs.
"""
import argparse
import bisect
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import regress  # noqa: E402
# The image of the normal build, as the memory hash counts it (platform.c), and the function handles (ptr32.h).
IMAGE = (0x08048000, 0x0D80110C)
FN_BASE = 0x0BF00000


def symbols(exe):
    out = subprocess.run(["nm", str(exe)], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.split("\n"):
        p = line.split()
        if len(p) == 3:
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    base = next(v for v, n in syms if n == "port_ram")
    return base, syms


def dump(exe, rom, level, frame, path, script=None):
    cmd = [str(exe), "--rom", rom, "--headless", str(frame + 2), "--level", str(level), "--dump", str(frame), path]
    if script:
        cmd += ["--input", script]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if not os.path.exists(path):
        return None
    return pathlib.Path(path).read_bytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=regress.default_rom())
    ap.add_argument("--level", type=int, action="append")
    ap.add_argument("--frame", type=int, action="append")
    ap.add_argument("--input", action="store_true")
    ap.add_argument("--audio", action="store_true")
    ap.add_argument("--other")
    args = ap.parse_args()
    regress.require_rom(args.rom, "ptrdiff.py")
    suffix = "-mus" if args.audio else ""
    normal = ROOT / "build" / f"headless{suffix}" / "weemitt"
    displaced = args.other or ROOT / "build" / f"headless-displaced{suffix}" / "weemitt"
    base, syms = symbols(normal)
    addrs = [v for v, _ in syms]
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for level in args.level or [0, 5, 13]:
            script = None
            if args.input:
                script = os.path.join(tmp, f"level{level}.input")
                with open(script, "w") as f:
                    f.write(regress.input_script(level, max(args.frame or [300]) + 2))
            for frame in args.frame or [0, 10, 300]:
                a = dump(normal, args.rom, level, frame, os.path.join(tmp, "a.bin"), script)
                b = dump(displaced, args.rom, level, frame, os.path.join(tmp, "b.bin"), script)
                if a is None or b is None:
                    print(f"level {level} frame {frame}: {'the headless' if a is None else 'the other'} build "
                          "did not reach the frame")
                    total += 1
                    continue
                n = 0
                for i in range(0, min(len(a), len(b)), 4):
                    x, = struct.unpack_from("<I", a, i)
                    y, = struct.unpack_from("<I", b, i)
                    if x == y or (IMAGE[0] <= x < IMAGE[1] and FN_BASE <= y < FN_BASE + 0x100000):
                        continue
                    n += 1
                    if n <= 20:
                        k = bisect.bisect_right(addrs, base + i) - 1
                        print(f"  level {level} frame {frame}: {syms[k][1]}+{base + i - syms[k][0]:#x}: "
                              f"{x:08x} in the headless build, {y:08x} in the other build")
                for os_path in ("a.bin", "b.bin"):
                    os.remove(os.path.join(tmp, os_path))
                print(f"level {level} frame {frame}: {n} words differ")
                total += n
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
