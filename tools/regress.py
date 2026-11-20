"""Regression test for the game's logic: runs the headless port through a fixed set of runs and compares each
frame's state with a baseline.

The runs are the attract mode (no input; it plays the presentation, the fly-through and the five recorded demos)
and a start of every level with generated controller input. Each run prints the state trace (port_game_trace)
every frame and a hash of the game's whole memory every 10 frames. In the original timing (dt = 1) the logic must
give exactly the same trace and memory as the baseline: a rewrite that is exact at dt = 1 changes nothing.

The memory hash reads values in port_ram's address range as offsets into port_ram, so it needs port_ram at the same
address in both builds. gen/ram.S puts port_ram at a 32 MB boundary on Linux, which gives it a fixed address. The
baseline's sources get the same alignment before they are built (fix_ram_alignment). They also get the value of
ball.phys that level 40 needs when it starts directly (fix_direct_start), and tongueRange's byte swap
(fix_float_swap).

Usage:
  python3 tools/regress.py baseline [--rom ROM]     build BASELINE_REF in a git worktree and record its runs
                                                    (build/regress/baseline); --exe PATH uses that binary instead
  python3 tools/regress.py check [--rom ROM]        run the current build and compare with the baseline
  options: --runs NAME... (only these runs), --frames N (frames per level run), --exe PATH, --jobs N
"""
import argparse
import concurrent.futures
import os
import pathlib
import random
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "regress"
LEVELS = list(range(0, 48))
# The commit whose logic is the reference. This repository starts from one commit without earlier history, and its
# logic was checked against the earlier reference before it was published. An empty value means the first commit.
BASELINE_REF = ""
ATTRACT_FRAMES = 30000


def default_rom():
    """The ROM: $GLOVER_ROM, or baserom.us.z64 in a gloveworld checkout next to this one. From a git worktree, the
    checkout next to the main working tree counts too."""
    if "GLOVER_ROM" in os.environ:
        return os.environ["GLOVER_ROM"]
    roots = [ROOT]
    r = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode == 0:
        roots.append(pathlib.Path(r.stdout.strip()).parent)
    for root in roots:
        rom = root.parent / "gloveworld" / "baserom.us.z64"
        if rom.exists():
            return str(rom)
    return str(ROOT.parent / "gloveworld" / "baserom.us.z64")

# N64 buttons.
A, B, Z, START, L, R = 0x8000, 0x4000, 0x2000, 0x1000, 0x0020, 0x0010
CUP, CDOWN, CLEFT, CRIGHT = 0x0008, 0x0004, 0x0002, 0x0001


def input_script(level, frames):
    """Deterministic varied input: walking, running, jumps, the ball buttons, camera buttons, now and then the
    pause menu (opened and closed again)."""
    rnd = random.Random(1000 + level)
    lines = []
    f = 30
    while f < frames:
        length = rnd.randint(8, 50)
        x = rnd.choice([0, 0, 80, -80, 40, -40, 25, 60])
        y = rnd.choice([0, 80, 80, -80, 40, 60, -40])
        buttons = 0
        for bit, chance in ((A, 0.3), (B, 0.2), (Z, 0.15), (R, 0.1), (L, 0.05), (CUP, 0.05), (CDOWN, 0.05),
                            (CLEFT, 0.08), (CRIGHT, 0.08)):
            if rnd.random() < chance:
                buttons |= bit
        lines.append(f"{f}:{buttons:x}:{x}:{y}")
        # Release the buttons for a few frames so that the next press is a new press.
        lines.append(f"{f + length}:0:{x}:{y}")
        f += length + rnd.randint(2, 6)
        if rnd.random() < 0.03 and f + 40 < frames:
            lines.append(f"{f}:{START:x}:0:0")
            lines.append(f"{f + 3}:0:0:0")
            lines.append(f"{f + 20}:{START:x}:0:0")
            lines.append(f"{f + 23}:0:0:0")
            f += 30
    return "\n".join(lines) + "\n"


def runs(frames):
    out = [("attract", [], ATTRACT_FRAMES)]
    for level in LEVELS:
        out.append((f"level{level:02d}", ["--level", str(level)], frames))
    return out


def run_one(exe, rom, name, args, frames, dest):
    cmd = [str(exe), "--rom", rom, "--headless", str(frames), "--trace", "--hash-every", "10", *args]
    if name.startswith("level"):
        script = dest / f"{name}.input"
        script.write_text(input_script(int(name[5:]), frames))
        cmd += ["--input", str(script)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.startswith("frame ")]
    (dest / f"{name}.trace").write_text("\n".join(lines) + "\n")
    status = "ok" if r.returncode == 0 else f"exit {r.returncode}"
    return name, status, len(lines)


def compare(name, base, cur):
    a = (base / f"{name}.trace").read_text().splitlines()
    b = (cur / f"{name}.trace").read_text().splitlines()
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            fx = x.split()
            fy = y.split()
            diff = [f"{fx[k]}: {fx[k + 1]} -> {fy[k + 1]}" for k in range(2, min(len(fx), len(fy)) - 1)
                    if fx[k + 1] != fy[k + 1] and not fx[k].startswith(("0", "1", "2", "3", "4", "5", "6", "7",
                                                                         "8", "9", "a", "b", "c", "d", "e", "f"))]
            return f"first difference at frame {i}: " + ("; ".join(diff[:6]) if diff else f"{x}\n    {y}")
    if len(a) != len(b):
        return f"length differs: {len(a)} frames in the baseline, {len(b)} now"
    return None


# gen/ram.S before port_ram had a fixed address, and after.
RAM_ALIGN_OLD = "\t.globl\tSYM(port_ram)\n\t.p2align\t12\n"
RAM_ALIGN_NEW = "\t.globl\tSYM(port_ram)\n#if defined(__ELF__)\n\t.p2align\t25\n#else\n\t.p2align\t12\n#endif\n"


def fix_ram_alignment(src):
    """Gives port_ram the current build's fixed address in an older source tree (gen/ram.S)."""
    ram = src / "gen" / "ram.S"
    text = ram.read_text()
    if RAM_ALIGN_OLD in text:
        ram.write_text(text.replace(RAM_ALIGN_OLD, RAM_ALIGN_NEW))


# port_game_init before and after it gave ball.phys a value before it starts a level.
BALL_PHYS_OLD = "    ballLeftBehind = -1;\n    StartNewLevel(level);\n"
BALL_PHYS_NEW = "    ballLeftBehind = -1;\n    ball.phys = &defaultBehaviour[0];\n    StartNewLevel(level);\n"


def fix_direct_start(src):
    """Gives ball.phys the value ResetBall gives it before port_game_init starts a level, in an older source tree
    (src/port/frame.c). Level 40 (SPACEBOSS3) writes through it before ResetBall runs. Every other level sets the
    same value before its first frame."""
    frame = src / "src" / "port" / "frame.c"
    text = frame.read_text()
    if BALL_PHYS_OLD in text:
        frame.write_text(text.replace(BALL_PHYS_OLD, BALL_PHYS_NEW))


# gen/ramtab.c before and after splat wrote tongueRange (0x801EEC08) as a float. splat used to write its bytes as a
# string, so they were not swapped. The game's C does not read it, but the memory hash does.
SWAP_OLD = "    0x1eebcc, 15,\n    0x1eec0c, 1,\n"
SWAP_NEW = "    0x1eebcc, 17,\n"


def fix_float_swap(src):
    """Swaps tongueRange as the word it is, in an older source tree (gen/ramtab.c)."""
    tab = src / "gen" / "ramtab.c"
    text = tab.read_text()
    if SWAP_OLD in text:
        tab.write_text(text.replace(SWAP_OLD, SWAP_NEW))


def build_baseline():
    """Builds BASELINE_REF in build/regress/baseline-src (a git worktree) and returns its binary."""
    src = OUT / "baseline-src"
    if not src.exists():
        OUT.mkdir(parents=True, exist_ok=True)
        ref = BASELINE_REF or subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], cwd=ROOT, check=True,
                                             capture_output=True, text=True).stdout.split()[0]
        subprocess.run(["git", "worktree", "add", "--detach", str(src), ref], cwd=ROOT, check=True)
    fix_ram_alignment(src)
    fix_direct_start(src)
    fix_float_swap(src)
    target = "headless" if "headless:" in (src / "Makefile").read_text() else "RENDERER=null"
    subprocess.run(["make", target, f"-j{os.cpu_count()}"], cwd=src, check=True, stdout=subprocess.DEVNULL)
    built = src / "build" / ("headless" if target == "headless" else "linux") / "weemitt"
    return str(built)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["baseline", "check"])
    ap.add_argument("--rom", default=default_rom())
    ap.add_argument("--exe", default=str(ROOT / "build" / "headless" / "weemitt"))
    ap.add_argument("--runs", nargs="*")
    ap.add_argument("--frames", type=int, default=900)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()
    if args.mode == "baseline" and args.exe == str(ROOT / "build" / "headless" / "weemitt"):
        args.exe = build_baseline()
    elif args.exe == str(ROOT / "build" / "headless" / "weemitt"):
        subprocess.run(["make", "headless", f"-j{os.cpu_count()}"], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    dest = OUT / ("baseline" if args.mode == "baseline" else "current")
    dest.mkdir(parents=True, exist_ok=True)
    todo = [r for r in runs(args.frames) if not args.runs or r[0] in args.runs]
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futures = [ex.submit(run_one, args.exe, args.rom, n, a, f, dest) for n, a, f in todo]
        results = sorted(f.result() for f in futures)
    for name, status, count in results:
        line = f"{name}: {status}, {count} frames"
        if args.mode == "check":
            if not (OUT / "baseline" / f"{name}.trace").exists():
                line += "; no baseline"
            else:
                d = compare(name, OUT / "baseline", dest)
                if d:
                    failed += 1
                    line += "; " + d
                else:
                    line += "; same as the baseline"
        print(line)
    if args.mode == "check":
        print(f"{failed} of {len(results)} runs differ from the baseline")
        sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
