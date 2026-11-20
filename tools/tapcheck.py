"""Checks how fast the glove can step on and off the ball with the L button.

Runs level 35 (the glove holds the ball from about frame 90) with scripted L presses. A press lasts one original
frame and the spacing is the number of frames from one press to the next (spacing 1 is L held, so there is no press
after the first). Each press must change the glove's action between HAND_JOINED and HAND_BALLWALK. The tool counts
the changes in the update trace.

Usage: python3 tools/tapcheck.py [--spacings 2,3,4,6,8] [--presses 12] [--rom ROM] [HEADLESS OPTIONS]
       python3 tools/tapcheck.py --dt 0.333 --pace --dt-jitter 0.1

Other options go to the headless build (for example `--dt 0.1`).
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

BUTTON_L = 0x20
JOINED, BALLWALK = 3, 20
START_FRAME = 150
LINE = re.compile(r"^update fc (\d+) new (\d+) tick (\d+) .* mode (\d+) prev")


def script(spacing, presses):
    """Input lines for the presses. Returns the text and the frames where a press starts."""
    lines = ["0:0:0:0"]
    starts = []
    f = START_FRAME
    for _ in range(presses):
        starts.append(f)
        lines.append(f"{f}:{BUTTON_L:x}:0:0")
        if spacing > 1:
            lines.append(f"{f + 1}:0:0:0")
        f += spacing
    lines.append(f"{f + 40}:0:0:0")
    return "\n".join(lines) + "\n", starts, f + 40


def run(exe, rom, spacing, presses, extra):
    text, starts, last = script(spacing, presses)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as t:
        t.write(text)
    try:
        cmd = [exe, "--rom", rom, "--headless", str(last), "--level", "35", "--input", t.name, "--trace-updates"]
        r = subprocess.run(cmd + extra, capture_output=True, text=True)
    finally:
        os.unlink(t.name)
    modes = []
    for line in r.stdout.splitlines():
        m = LINE.match(line)
        if m:
            modes.append((int(m.group(1)), int(m.group(4))))
    return starts, modes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spacings", default="2,3,4,6,8")
    ap.add_argument("--presses", type=int, default=12)
    ap.add_argument("--rom", default=os.environ.get("ROM", "../gloveworld/baserom.us.z64"))
    ap.add_argument("--exe", default=str(ROOT / "build/headless/weemitt"))
    a, extra = ap.parse_known_args()
    extra = [x for x in extra if x != "--"]
    for k in [int(x) for x in a.spacings.split(",")]:
        starts, modes = run(a.exe, a.rom, k, a.presses, extra)
        flips = []
        prev = None
        for fc, mode in modes:
            if mode in (JOINED, BALLWALK):
                if prev is not None and mode != prev and fc >= START_FRAME - 2:
                    flips.append(fc)
                prev = mode
        before = [m for fc, m in modes if fc < START_FRAME - 2]
        first = before[-1] if before else None
        # One flip per press: count the flips that fall in each press's window of `k` frames.
        hit = 0
        for s in starts:
            if any(s - 1 <= f <= s + k for f in flips):
                hit += 1
        print(f"spacing {k}: {hit} of {len(starts)} presses changed the action, {len(flips)} changes in all"
              f" (action before the first press: {first}, 3 is joined)")


if __name__ == "__main__":
    main()
