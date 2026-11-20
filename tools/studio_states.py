#!/usr/bin/env python3
"""The studio's state files (docs/studio.md, "State files"): save a state at frame N, load it in a new process and
play M frames with the same input, then compare with the straight run.

    python3 tools/studio_states.py --rom ROM [--exe build/studio/weemitt-studio] [--wine] [--level 13]
                                   [--input SCRIPT] [--frame N] [--frames M]

The straight run plays N + M + 1 frames and saves a state after frame N and after frame N + M. The second run loads
the first state and plays M frames with the rest of the input. The test checks:

- the state trace and the hash of the game's memory (--trace, --hash-every 1) of every frame;
- the game's data in the two states at the end (all of the state after its piece versions);
- the timeline's self-test (WEEMITT_STUDIO_SELFTEST) on the timeline that starts at the loaded state;
- that a state file with a wrong magic, format, build, ROM, size or checksum is refused with its message.

Without --input the game gets generated input: walking in turn in four directions, with jumps. The state files go to
a temporary folder, which is removed at the end: they hold the game's memory.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HEADER = 512
ENGINE_HEADER = 32


def generated_input(path, frames):
    """Walks in turn up, right, down and left for 40 frames each, and presses A every 25 frames."""
    dirs = [(0, 70), (70, 0), (0, -70), (-70, 0)]
    with open(path, "w") as f:
        for frame in range(frames):
            x, y = dirs[(frame // 40) % 4]
            buttons = 0x8000 if frame % 25 < 3 else 0
            f.write("%d:%x:%d:%d\n" % (frame, buttons, x, y))


def read_script(path):
    lines = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.strip().split(":")
            if len(parts) != 4:
                continue
            try:
                lines.append((int(parts[0]), parts[1], parts[2], parts[3]))
            except ValueError:
                continue
    return lines


def shifted_script(src, dst, start):
    """The script from update `start` on, as frames from 0. The line in effect at `start` becomes frame 0."""
    lines = read_script(src)
    out = []
    before = None
    for frame, b, x, y in lines:
        if frame <= start:
            before = (0, b, x, y)
        else:
            out.append((frame - start, b, x, y))
    if before is not None:
        out.insert(0, before)
    with open(dst, "w") as f:
        for frame, b, x, y in out:
            f.write("%d:%s:%s:%s\n" % (frame, b, x, y))


def run(cmd, env=None):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def trace(stdout):
    """{frame: the line after its number} for the lines of --trace with --hash-every."""
    out = {}
    for line in stdout.splitlines():
        if line.startswith("frame ") and " hash " in line:
            number, rest = line[6:].split(" ", 1)
            out[int(number)] = rest
    return out


def game_data(path):
    data = open(path, "rb").read()
    if data[:8] != b"WEEMSTAT":
        raise SystemExit("%s is not a state file" % path)
    data_offset = struct.unpack_from("<I", data, 320)[0]
    state = data[HEADER:]
    # The engine's header: the frame counter and the heap's marks (StateHeader in bruteforce/engine/engine.c).
    frame, mark_hi, mark_lo = struct.unpack_from("<III", state, 16)
    return (frame, mark_hi, mark_lo), state[data_offset:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=None)
    ap.add_argument("--wine", action="store_true", help="run the Windows build under Wine")
    ap.add_argument("--level", type=int, default=13)
    ap.add_argument("--input", default=None)
    ap.add_argument("--frame", type=int, default=600, help="N: the frame to save the state after")
    ap.add_argument("--frames", type=int, default=300, help="M: the frames to play from the state")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "build/studio-windows/weemitt-studio.exe" if args.wine
                                   else "build/studio/weemitt-studio")
    prefix = ["wine", exe] if args.wine else [exe]
    n, m = args.frame, args.frames
    tmp = tempfile.mkdtemp(prefix="studio-states-")
    failures = []
    try:
        script = args.input
        if script is None:
            script = os.path.join(tmp, "input.txt")
            generated_input(script, n + m + 1)
        a_state = os.path.join(tmp, "a.state")
        a_end = os.path.join(tmp, "a_end.state")
        b_end = os.path.join(tmp, "b_end.state")
        rest = os.path.join(tmp, "rest.txt")
        shifted_script(script, rest, n + 1)

        code, out, err = run(prefix + ["--rom", args.rom, "--level", str(args.level), "--headless", str(n + m + 1),
                                       "--input", script, "--trace", "--hash-every", "1",
                                       "--save-state", str(n), a_state, "--save-state", str(n + m), a_end])
        if code != 0:
            raise SystemExit("the straight run failed (%d):\n%s" % (code, err[-2000:]))
        straight = trace(out)

        env = dict(os.environ, WEEMITT_STUDIO_SELFTEST="1")
        if args.wine:
            env["WINEDEBUG"] = "-all"
        code, out, err = run(prefix + ["--rom", args.rom, "--headless", str(m), "--load-state", a_state,
                                       "--input", rest, "--trace", "--hash-every", "1",
                                       "--save-state", str(m - 1), b_end], env)
        loaded = trace(out)
        selftest = [l for l in out.splitlines() if l.startswith("studio selftest:")]
        print("\n".join(selftest))
        if code != 0:
            failures.append("the run from the state exited with %d" % code)

        if len(loaded) != m or len(straight) != n + m + 1:
            failures.append("the runs printed %d and %d trace lines, not %d and %d"
                            % (len(straight), len(loaded), n + m + 1, m))
        differ = [k for k in range(m) if loaded.get(k) != straight.get(n + 1 + k)]
        if differ:
            k = differ[0]
            failures.append("%d of %d frames differ; the first, frame %d:\n  straight:   %s\n  from state: %s"
                            % (len(differ), m, n + 1 + k, straight.get(n + 1 + k), loaded.get(k)))
        print("trace and memory hash: %d frames compared, %d differ" % (m, len(differ)))

        head_a, data_a = game_data(a_end)
        head_b, data_b = game_data(b_end)
        same = head_a == head_b and data_a == data_b
        print("state at frame %d: %s (%d bytes of game data)" % (n + m, "same" if same else "DIFFERS", len(data_a)))
        if not same:
            failures.append("the states at frame %d differ (header %s against %s)" % (n + m, head_a, head_b))

        # Refusals: each changed copy of the state must not load, with this message.
        good = open(a_state, "rb").read()

        def changed(offset, value):
            b = bytearray(good)
            b[offset:offset + len(value)] = value
            return bytes(b)

        cases = [
            ("magic", changed(0, b"XXXXXXXX"), "is not a studio state file"),
            ("format", changed(8, struct.pack("<I", 99)), "has state format 99"),
            ("build", changed(16, struct.pack("<Q", struct.unpack_from("<Q", good, 16)[0] ^ 1)),
             "was saved by another build"),
            ("layout", changed(24, struct.pack("<I", struct.unpack_from("<I", good, 24)[0] ^ 1)),
             "was saved by another build"),
            ("rom", changed(28, struct.pack("<I", struct.unpack_from("<I", good, 28)[0] ^ 1)),
             "was saved with another ROM"),
            ("a changed byte", changed(len(good) - 100, bytes([good[-100] ^ 0x55])), "is damaged"),
            ("cut short", good[:-4096], "is damaged"),
            ("too short", good[:100], "is not a studio state file"),
        ]
        for name, data, message in cases:
            path = os.path.join(tmp, "bad.state")
            with open(path, "wb") as f:
                f.write(data)
            code, out, err = run(prefix + ["--rom", args.rom, "--headless", "5", "--load-state", path],
                                 dict(os.environ, WINEDEBUG="-all"))
            ok = code == 1 and message in err
            print("refused (%s): %s" % (name, "yes" if ok else "NO"))
            if not ok:
                failures.append("the state with %s: exit %d, messages:\n%s" % (name, code, err[-600:]))
        if not selftest or not selftest[-1].endswith(" 0 failures"):
            failures.append("the timeline's self-test from the loaded state failed")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for f in failures:
        print("FAIL: " + f)
    print("studio states: %s" % ("ok" if not failures else "%d failures" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
