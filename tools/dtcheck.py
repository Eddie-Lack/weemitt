"""Checks that the game behaves the same at a high frame rate as at the original one.

Runs a level twice with the same controller input (given per original frame): once with updates of one original
frame (dt = 1, the N64's timing) and once with shorter updates (dt = 0.25 by default, four updates per original
frame, as at 80 frames a second in a level). It compares the state trace at each original frame:

- frameCount (fc), the mode and the level must match exactly while the runs have not diverged: a per-frame counter
  that runs on every update instead of once per original frame makes fc run ahead.
- The glove, ball and camera positions should stay close. Over time small differences grow (collisions, rounding),
  so the tool reports the speeds instead: the average distance each moves per original frame in each run. A speed
  that is about 1/dt times the original one means a movement that does not scale with deltaTime.

Usage: python3 tools/dtcheck.py LEVEL... [--dt 0.25] [--frames 600] [--rom ROM] [--exe PATH] [--ballworld]
       python3 tools/dtcheck.py attract        the attract mode (menus and fly-through; demos run at dt = 1)
"""
import argparse
import math
import os
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import regress  # noqa: E402


def f32(h):
    return struct.unpack("<f", struct.pack("<I", int(h, 16)))[0]


EXTRA = []  # options given to every run: --ballworld adds the Ballworld mode


def run(exe, rom, level, frames, dt, script):
    cmd = [exe, "--rom", rom, "--headless", str(frames), "--trace", "--dt", str(dt)] + EXTRA
    if level is not None:
        cmd += ["--level", str(level), "--input", str(script)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    rows = []
    for line in r.stdout.splitlines():
        if not line.startswith("frame "):
            continue
        p = line.split()
        d = dict(zip(p[2::2], p[3::2]))
        row = {"fc": int(d["fc"]), "mode": int(d["mode"]), "level": int(d["level"]), "rseed": d["rseed"]}
        i = p.index("hand")
        row["hand"] = [f32(x) for x in p[i + 1:i + 4]]
        i = p.index("ball")
        row["ball"] = [f32(x) for x in p[i + 1:i + 4]]
        i = p.index("cam")
        row["cam"] = [f32(x) for x in p[i + 1:i + 4]]
        rows.append(row)
    return rows, r.returncode


def speeds(rows, key):
    out = []
    for a, b in zip(rows, rows[1:]):
        if a["mode"] != b["mode"] or a["level"] != b["level"]:
            continue
        out.append(math.dist(a[key], b[key]))
    return sum(out) / len(out) if out else 0.0


def check(exe, rom, level, frames, dt):
    work = ROOT / "build" / "dtcheck"
    work.mkdir(parents=True, exist_ok=True)
    script = work / f"level{level}.input"
    if level is not None:
        script.write_text(regress.input_script(level, frames))
    base, rc1 = run(exe, rom, level, frames, 1.0, script)
    fast, rc2 = run(exe, rom, level, frames, dt, script)
    name = "attract" if level is None else f"level {level}"
    print(f"{name}: {len(base)} frames at dt 1 (exit {rc1}), {len(fast)} at dt {dt} (exit {rc2})")
    first = None
    for i, (a, b) in enumerate(zip(base, fast)):
        if (a["fc"], a["mode"], a["level"]) != (b["fc"], b["mode"], b["level"]):
            first = (i, a, b)
            break
    if first:
        i, a, b = first
        print(f"  counters differ at frame {i}: fc {a['fc']} -> {b['fc']}, mode {a['mode']} -> {b['mode']}, "
              f"level {a['level']} -> {b['level']}")
    else:
        print("  frame counter, mode and level match at every frame")
    for key in ("hand", "ball", "cam"):
        s1 = speeds(base, key)
        s2 = speeds(fast, key)
        ratio = s2 / s1 if s1 > 1e-6 else float("nan")
        flag = "  <-- check" if s1 > 1e-3 and (ratio > 1.5 or ratio < 0.67) else ""
        print(f"  {key}: {s1:.3f} per frame at dt 1, {s2:.3f} at dt {dt} (ratio {ratio:.2f}){flag}")
    drift = []
    for key in ("hand", "ball", "cam"):
        for i, (a, b) in enumerate(zip(base, fast)):
            if math.dist(a[key], b[key]) > 5.0:
                drift.append(f"{key} after {i} frames")
                break
    if drift:
        print("  positions first differ by more than 5 units: " + ", ".join(drift))
    return first is None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="+")
    ap.add_argument("--dt", type=float, default=0.25)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--rom", default=regress.default_rom())
    ap.add_argument("--exe", default=str(ROOT / "build" / "headless" / "weemitt"))
    ap.add_argument("--ballworld", action="store_true", help="both runs in the Ballworld mode")
    args = ap.parse_args()
    if args.ballworld:
        EXTRA.append("--ballworld")
    if args.exe == str(ROOT / "build" / "headless" / "weemitt"):
        subprocess.run(["make", "headless", f"-j{os.cpu_count()}"], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    ok = True
    for lv in args.levels:
        level = None if lv == "attract" else int(lv)
        frames = 3000 if level is None else args.frames
        ok &= check(args.exe, args.rom, level, frames, args.dt)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
