"""Checks of the library (docs/library.md, docs/testing.md): its exports and imports, its header, and that the test
host (tools/libhost) plays the game through it exactly as weemitt's headless mode does.

Usage: python3 tools/libcheck.py [--rom ROM] [--bits 32|64] [--wine] [--gl]   (make libcheck builds what it needs first)
  --bits  check only the 32-bit or the 64-bit library (default: both)
  --wine  also check the Windows DLLs (make lib-windows64 lib-windows64-stub libhost-windows64, and the same with 32)
          under Wine
  --gl    also check the renderer's pictures (make libglshot64 linux64 libglshot32 linux; needs a display, such as
          Xvfb)

Each check prints one line, "ok" or "FAIL". It exits with 1 when one fails.

The game's memory holds the save, and the hash of the memory with it. The library and the headless build read the
same save: weemitt's own save file unless --save names one. Under Wine each run gets a new empty save file.
"""
import argparse
import math
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import regress  # noqa: E402

B = ROOT / "build"


class Build:
    """The library, its test host and the headless builds of one word size."""

    def __init__(self, bits):
        s = "" if bits == 32 else "64"
        self.bits = bits
        self.lib = B / f"lib{bits}" / "libglover.so"
        self.stub = B / f"lib{bits}-stub" / "libglover.so"
        self.hostgfx = B / f"lib{bits}-hostgfx" / "libglover.so"   # make lib64-hostgfx; 64-bit only
        self.host = B / f"libhost{bits}" / "glover-host"
        self.clash = B / f"libhost{bits}" / "glover-host-clash"
        self.headless = B / f"headless{s}" / "weemitt"
        self.headless_mus = B / f"headless{s}-mus" / "weemitt"
        self.window = B / f"linux{s}" / "weemitt"
        self.glshot = B / f"libglshot{bits}" / "glover-glshot"
        self.dll = B / f"lib-windows{bits}" / "glover.dll"
        self.dll_stub = B / f"lib-windows{bits}-stub" / "glover.dll"
        self.host_exe = B / f"libhost-windows{bits}" / "glover-host.exe"


SYMS = (ROOT / "src" / "lib" / "glover.syms").read_text().split()
# One frame of the synthesizer (mus-glue.c, musFrameSize).
SYNTH_FRAME = 1104

failures = 0


def report(ok, what):
    global failures
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures += 1


def run(cmd, env=None, **kw):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=e, **kw)


def traces(exe, rom, name, frames, extra=(), env=None, work=None):
    """The 'frame' lines of a run, with regress.py's input for a level."""
    cmd = [*(exe if isinstance(exe, list) else [exe]), "--rom", rom, "--headless", frames, "--trace", "--hash-every",
           "10", *extra]
    if name != "attract":
        script = pathlib.Path(work) / f"level{name}.input"
        script.write_text(regress.input_script(int(name), frames))
        cmd += ["--level", name, "--input", script]
    r = run(cmd, env)
    return [l for l in r.stdout.splitlines() if l.startswith("frame ")], r.returncode


def same_traces(what, a, b):
    la, ra = a
    lb, rb = b
    if ra != 0 or rb != 0:
        report(False, f"{what}: exit {ra} and {rb}")
        return
    if la == lb and la:
        report(True, f"{what}: {len(la)} frames equal")
        return
    for i, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            report(False, f"{what}: first difference at line {i}")
            return
    report(False, f"{what}: {len(la)} and {len(lb)} lines")


def exports(lib):
    out = run(["nm", "-D", "--defined-only", lib]).stdout
    return sorted(l.split()[-1] for l in out.splitlines() if l.split() and l.split()[-2] in "TtWw")


def check_elf(lib, bits):
    names = exports(lib)
    report(names == sorted(SYMS), f"{lib.parent.name}: the exports are glover.syms ({len(names)} names)")
    head = run(["readelf", "-h", lib]).stdout
    report(f"ELF{bits}" in head, f"{lib.parent.name}: a {bits}-bit ELF file")
    dyn = run(["readelf", "-d", lib]).stdout
    needed = re.findall(r"Shared library: \[([^\]]+)\]", dyn)
    report("TEXTREL" not in dyn, f"{lib.parent.name}: no TEXTREL")
    report(sorted(needed) == ["libc.so.6", "libm.so.6"], f"{lib.parent.name}: NEEDED is {', '.join(needed)}")
    und = run(["nm", "-D", "--undefined-only", lib]).stdout
    bad = [l.split()[-1] for l in und.splitlines()
           if re.match(r"(SDL_|gl[A-Z_])", l.split()[-1]) or "GLIBCXX" in l or "CXXABI" in l]
    report(not bad, f"{lib.parent.name}: no SDL, OpenGL or C++ runtime import{(': ' + ' '.join(bad)) if bad else ''}")
    versions = sorted(set(re.findall(r"GLIBC_([0-9.]+)", und)), key=lambda v: [int(x) for x in v.split(".")])
    print(f"info {lib.parent.name}: the newest glibc version it needs is {versions[-1] if versions else 'none'} "
          f"({' '.join(l.split()[-1] for l in und.splitlines() if versions and 'GLIBC_' + versions[-1] in l)})")


def check_header(work):
    src = pathlib.Path(work) / "h.c"
    src.write_text('#include "glover.h"\nint main(void) { return glover_api_version() != GLOVER_API_VERSION; }\n')
    inc = ROOT / "src" / "lib"
    r1 = run(["gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-Werror", "-fsyntax-only", f"-I{inc}", src])
    report(r1.returncode == 0, "glover.h compiles with gcc -std=c99 -Wall -Wextra -pedantic -Werror" + r1.stderr[:200])
    r2 = run(["g++", "-x", "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", f"-I{inc}", src])
    report(r2.returncode == 0, "glover.h compiles with g++ -std=c++11" + r2.stderr[:200])


def check_host(b, rom, work):
    stub = {"GLOVER_LIB": str(b.stub)}
    host, bits = b.host, b.bits
    for name in ("5", "13", "attract"):
        ref = traces(b.headless, rom, name, 900, work=work)
        same_traces(f"{bits}-bit clash host, {name}: the same as {b.headless.parent.name}",
                    traces(b.clash, rom, name, 900, env=stub, work=work), ref)
        if name != "attract":
            same_traces(f"{bits}-bit host with MXCSR 0xFFC0, level {name}: the same as {b.headless.parent.name}",
                        traces(host, rom, name, 900, ["--host-mxcsr", "0xFFC0"], stub, work), ref)
            r = run([host, "--rom", rom, "--headless", 100, "--level", name, "--host-mxcsr", "0xFFC0"], stub)
            report(r.returncode == 0 and "0 calls returned" in r.stdout, f"{bits}-bit: MXCSR 0xFFC0 kept after each "
                   f"call, level {name}: {r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.returncode}")
    same_traces(f"{bits}-bit host on a thread with a 512 KB stack, level 13",
                traces(host, rom, "13", 900, ["--thread"], stub, work), traces(b.headless, rom, "13", 900, work=work))
    # The Ballworld mode (GloverConfig.ballworld), at dt 1 and at shorter updates.
    for name, extra in (("5", []), ("13", []), ("13", ["--dt", "0.25"])):
        same_traces(f"{bits}-bit host, Ballworld, level {name} {' '.join(extra)}: the same as {b.headless.parent.name}",
                    traces(host, rom, name, 900, ["--ballworld", *extra], stub, work),
                    traces(b.headless, rom, name, 900, ["--ballworld", *extra], work=work))
    # A second game after the library is loaded again.
    r = run([host, "--rom", rom, "--headless", 300, "--level", 5, "--trace", "--hash-every", 10, "--reload"], stub)
    halves = r.stdout.split("reload\n")
    lines = [[l for l in h.splitlines() if l.startswith("frame ")] for h in halves]
    ref = [l for l in run([b.headless, "--rom", rom, "--headless", 300, "--level", 5, "--trace", "--hash-every",
                           10]).stdout.splitlines() if l.startswith("frame ")]
    report(r.returncode == 0 and len(lines) == 2 and lines[0] == lines[1] == ref and ref,
           f"{bits}-bit: unloaded and loaded again, the library plays the same game a second time ({len(ref)} frames)")
    # The fatal path.
    r = run([host, "--rom", rom, "--headless", 300, "--level", 5, "--fatal-at", 100, "--host-mxcsr", "0xFFC0"], stub)
    line = next((l for l in r.stdout.splitlines() if l.startswith("fatal:")), r.stdout.strip())
    report(r.returncode == 0 and "update 100: glover_update returned -5: fatal: test" in r.stdout
           and "0 calls returned" in r.stdout, f"{bits}-bit: fatal at update 100: exit {r.returncode}; {line}")
    r = run([host, "--rom", rom, "--headless", 10, "--level", 5, "--fatal-at-start"], stub)
    report(r.returncode == 0, f"{bits}-bit: {r.stdout.strip() or r.returncode}")
    r = run([host, "--rom", rom, "--headless", 10, "--level", 5, "--reenter"], stub)
    report(r.returncode == 0, f"{bits}-bit: re-entry: "
           f"{r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.returncode}")
    r = run([host, "--rom", rom, "--headless", 200, "--level", 5, "--lifecycle"], stub)
    for line in r.stdout.splitlines():
        if line.startswith("lifecycle:"):
            print(f"     {line}")
    report(r.returncode == 0, f"{bits}-bit: the argument checks, glover_destroy and the calls after it: exit "
           f"{r.returncode}")
    # The ROM's byte orders and errors.
    data = pathlib.Path(rom).read_bytes()
    v64 = bytearray(data)
    v64[0::2], v64[1::2] = data[1::2], data[0::2]
    n64 = bytearray(len(data))
    for k in range(4):
        n64[k::4] = data[3 - k::4]
    wrong = bytearray(data)
    wrong[0x3B:0x3F] = b"NGVP"
    files = {"g.v64": v64, "g.n64": n64, "wrong.z64": wrong, "short.z64": data[:0x80000],
             "truncated.z64": data[:0x400000]}
    for name, content in files.items():
        (pathlib.Path(work) / name).write_bytes(content)
    ref = traces(host, rom, "5", 300, env=stub, work=work)
    for name in ("g.v64", "g.n64"):
        same_traces(f"{bits}-bit: the {name[2:]} ROM gives the .z64's trace",
                    traces(host, pathlib.Path(work) / name, "5", 300, env=stub, work=work), ref)
    for name in ("wrong.z64", "short.z64", "truncated.z64"):
        r = run([host, "--rom", pathlib.Path(work) / name, "--headless", 10], stub)
        report(r.returncode == 3 and "returned -3" in r.stderr, f"{bits}-bit: {name}: {r.stderr.strip()}")


SCENES = sorted((ROOT / "tools" / "hostworld").glob("*.scene"))
SCENE_LEVEL = 12   # tools/hostworld/hwtest.py's level


def scene_traces(exe, rom, scene, frames, extra=(), env=None, option="--scene"):
    """The 'frame' and 'bodies' lines of a host scene's run (docs/host-world.md)."""
    cmd = [*(exe if isinstance(exe, list) else [exe]), "--rom", rom, "--headless", frames, "--level", SCENE_LEVEL,
           option, scene, "--trace", "--hash-every", "10", *extra]
    r = run(cmd, env)
    return [l for l in r.stdout.splitlines() if l.startswith(("frame ", "bodies "))], r.returncode


def check_scenes(b, rom, work):
    """The host world through the library: the same game as weemitt's headless mode for each synthetic scene."""
    stub = {"GLOVER_LIB": str(b.stub)}
    bits = b.bits
    for scene in SCENES:
        for extra in ([], ["--dt", "0.25", "--dt-jitter", "0.3"]):
            same_traces(f"{bits}-bit host, scene {scene.name} {' '.join(extra)}: the same as {b.headless.parent.name}",
                        scene_traces(b.host, rom, scene, 600, extra, stub),
                        scene_traces(b.headless, rom, scene, 600, extra, option="--host-scene"))
    r = run([b.host, "--rom", rom, "--headless", 10, "--level", SCENE_LEVEL, "--coll-args"], stub)
    report(r.returncode == 0, f"{bits}-bit: the host world's argument checks: "
           f"{r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.returncode}")


def check_marks(b, rom):
    """The draw marks (glover_draw_marks) of level 5's frame 100: the setup, the sky, the world's actors and scenery,
    and the HUD, in that order, at rising addresses."""
    r = run([b.host, "--rom", rom, "--headless", 101, "--level", 5, "--marks", 100], {"GLOVER_LIB": str(b.stub)})
    lines = {l.split(":")[0]: l.split(":", 1)[1].split() for l in r.stdout.splitlines() if l.startswith("mark")}
    layers = [int(x) for x in lines.get("mark layers 100", [])]
    pairs = [x.split(":") for x in lines.get("marks 100", [])[3:]]
    addrs = [int(a, 16) for a, _ in pairs]
    report(r.returncode == 0 and layers[:3] == [0, 4, 1] and 2 in layers and layers[-1] == 8
           and all(x < y for x, y in zip(addrs, addrs[1:])),
           f"{b.bits}-bit draw marks of level 5, frame 100: {len(addrs)} marks, layers "
           f"{' '.join(map(str, layers[:5]))} ... {layers[-1] if layers else '-'}, rising addresses")


HOSTGFX_LEVELS = (5, 12, 13, 20, 30)


def host_gfx_line(out):
    """The numbers of the test host's "hostgfx" line, and the first broken rule."""
    line = next((l for l in out.splitlines() if l.startswith("hostgfx ")), "")
    head, _, first = line.partition(" first: ")
    words = head.split()[1:]
    return dict(zip(words[0::2], words[1::2])), first


def check_host_gfx(b, rom):
    """glover_host_gfx (docs/library.md): over several levels, with the latest frame, with the blend, split in two
    lists, and with the host's fog (which every fog command, geometry mode and render mode keeps), no unknown
    command, every pointer even, the vertices inside the game's memory, and the triangles equal to those of the requested
    layers in the game's own lists. The memory hash stays as without the call. The stub returns GLOVER_ERR_UNSUPPORTED."""
    libs = [b.lib] + ([b.hostgfx] if b.bits == 64 else [])
    for lib in libs:
        bad = []
        means = []
        for level in HOSTGFX_LEVELS:
            for extra in ((), ("--hostgfx-t", "0.5"), ("--hostgfx-t", "0.5", "--hostgfx-split"),
                          ("--hostgfx-split", "--hostgfx-fog")):
                r = run([b.host, "--rom", rom, "--headless", 300, "--level", level, "--hostgfx", 1, *extra],
                        {"GLOVER_LIB": str(lib)})
                n, first = host_gfx_line(r.stdout)
                ok = (r.returncode == 0 and int(n.get("calls", 0)) > 0 and n.get("failed") == "0"
                      and n.get("bad") == "0" and n.get("mismatch") == "0" and n.get("unknown") == "0")
                if not ok:
                    bad.append(f"level {level} {' '.join(extra) or 'latest'}: {n} {first}")
                means.append(float(n.get("mean-ms", 0)))
        report(not bad, f"{lib.parent.name}: glover_host_gfx's lists over levels "
               f"{', '.join(map(str, HOSTGFX_LEVELS))}, latest, blended, split in two and with the host's fog (mean {max(means):.3f} ms a call at most)"
               f"{(': ' + '; '.join(bad)) if bad else ''}")
    lib = {"GLOVER_LIB": str(b.lib)}
    a = run([b.host, "--rom", rom, "--headless", 300, "--level", 5, "--trace", "--hash-every", 10], lib).stdout
    c = run([b.host, "--rom", rom, "--headless", 300, "--level", 5, "--trace", "--hash-every", 10, "--hostgfx", 1,
             "--hostgfx-t", "0.5"], lib).stdout
    keep = lambda out: [l for l in out.splitlines() if not l.startswith("hostgfx ")]
    report(keep(a) == keep(c) and len(keep(a)) >= 300,
           f"{b.bits}-bit: the traces and memory hashes of level 5 are the same with glover_host_gfx in use")
    r = run([b.host, "--rom", rom, "--headless", 10, "--level", 5, "--hostgfx", 1], {"GLOVER_LIB": str(b.stub)})
    n, first = host_gfx_line(r.stdout)
    report(n.get("calls") == n.get("failed") and "returned -8" in first,
           f"{b.bits}-bit stub: glover_host_gfx returns GLOVER_ERR_UNSUPPORTED")


def check_sound(b, rom, work):
    mus = {"GLOVER_LIB": str(b.lib)}
    bits = b.bits
    for name in ("5", "13"):
        same_traces(f"{bits}-bit host with the sound player, level {name}: the same as {b.headless_mus.parent.name}",
                    traces(b.host, rom, name, 900, env=mus, work=work),
                    traces(b.headless_mus, rom, name, 900, work=work))
    results = []
    for extra in ([], [], ["--audio-thread"]):
        r = run([b.host, "--rom", rom, "--headless", 900, "--level", 13, "--audio-hash", *extra], mus)
        m = re.search(r"audio rate (\d+) frames (\d+) hash (\w+) dropped (\d+) game time ([0-9.]+)", r.stdout)
        results.append(m.groups() if m else None)
    ok = all(results) and results[0] == results[1] == results[2]
    report(ok, f"{bits}-bit sound: two runs and a run read on a second thread give the same hash ({results[0]})")
    if results[0]:
        rate, frames, _, dropped, seconds = results[0]
        expect = int(rate) * float(seconds)
        report(abs(int(frames) - expect) <= SYNTH_FRAME and dropped == "0" and rate == "22047",
               f"{bits}-bit sound: {frames} frames at {rate} Hz for {seconds} s of game time (expected {expect:.0f}), "
               f"{dropped} dropped")
    # The samples themselves, against the headless build's (--audio-out).
    for name in ("13", "attract"):
        level = [] if name == "attract" else ["--level", name]
        mine = pathlib.Path(work) / f"lib{bits}-{name}.raw"
        ref = pathlib.Path(work) / f"headless{bits}-{name}.raw"
        r1 = run([b.host, "--rom", rom, "--headless", 900, *level, "--audio-out", mine], mus)
        r2 = run([b.headless_mus, "--rom", rom, "--headless", 900, *level, "--audio-out", ref])
        a = mine.read_bytes() if mine.exists() else b""
        c = ref.read_bytes() if ref.exists() else b""
        report(r1.returncode == 0 and r2.returncode == 0 and a == c and a,
               f"{bits}-bit sound, {name}: the library's samples equal {b.headless_mus.parent.name}'s --audio-out, "
               f"byte for byte ({len(a) // 4} and {len(c) // 4} frames)")


def rms(path):
    """The root mean square of a file of 16-bit stereo frames, 0 for an empty one."""
    data = pathlib.Path(path).read_bytes() if pathlib.Path(path).exists() else b""
    n = len(data) // 2
    if n == 0:
        return 0.0
    v = struct.unpack(f"<{n}h", data[:n * 2])
    return math.sqrt(sum(x * x for x in v) / n)


def check_volume(b, rom, work):
    """GLOVER_OPT_MUSIC_VOLUME and GLOVER_OPT_EFFECTS_VOLUME: level 13 with its song and its sound effects, 900 frames.
    The song and the effects are two parts of the sound: with one at 0 the energy falls, the energies of the two parts
    add up to that of the whole, and both at 0 give silence. The game's state in the trace stays the same."""
    mus = {"GLOVER_LIB": str(b.lib)}
    bits = b.bits
    level = "13"
    energy = {}
    for m, e in ((100, 100), (0, 100), (100, 0), (0, 0)):
        out = pathlib.Path(work) / f"volume{bits}-{m}-{e}.raw"
        r = run([b.host, "--rom", rom, "--headless", 900, "--level", level, "--audio-out", out, "--volume", m, e],
                mus)
        line = next((l for l in r.stdout.splitlines() if l.startswith("volume:")), "")
        report(r.returncode == 0 and line == f"volume: set 0 0, 101 gives -1, get {m} {e}",
               f"{bits}-bit volume options {m} {e}: '{line}'")
        energy[(m, e)] = rms(out) ** 2
    full, effects, music, silent = energy[(100, 100)], energy[(0, 100)], energy[(100, 0)], energy[(0, 0)]
    report(full > 1e4 and effects < 0.9 * full and music < 0.9 * full and abs(effects + music - full) < 0.05 * full
           and silent == 0,
           f"{bits}-bit volume: energy {full:.0f} at 100 100, {effects:.0f} with the music at 0, {music:.0f} with the "
           f"effects at 0 (together {(effects + music) / max(full, 1):.3f} of the whole), {silent:.0f} with both at 0")
    # The synthesizer's voices in the game's memory get the scaled volume, so the memory hash differs: the
    # comparison leaves it out.
    nohash = lambda t: ([re.sub(r" hash \w+$", "", l) for l in t[0]], t[1])
    same_traces(f"{bits}-bit volume 0 0, level {level}: the same game state as without the options",
                nohash(traces(b.host, rom, level, 900, extra=["--volume", 0, 0], env=mus, work=work)),
                nohash(traces(b.host, rom, level, 900, env=mus, work=work)))


def check_camera_scale(b, rom, work):
    """GLOVER_OPT_CAMERA_DISTANCE_SCALE: level 13 with its sound effects alone, 900 frames. The option is 1 at the start
    and refuses 0 and 101. At 1 the sound and the game's state with its memory hash are those of a run without it. At
    0.5 the camera's distances are halved, so the sound effects are louder; the game's logic stays the same. In a
    host scene in level 12 at a scale of 1/3, GLOVER_OFF_AMBIENT takes out most of the effects' energy."""
    mus = {"GLOVER_LIB": str(b.lib)}
    bits = b.bits
    level = "13"
    energy = {}
    data = {}
    for scale in (None, 1, 0.5):
        out = pathlib.Path(work) / f"camscale{bits}-{scale}.raw"
        extra = ["--camera-scale", scale] if scale else []
        r = run([b.host, "--rom", rom, "--headless", 900, "--level", level, "--audio-out", out, "--volume", 0, 100,
                 *extra], mus)
        if scale:
            line = next((l for l in r.stdout.splitlines() if l.startswith("camera scale:")), "")
            report(r.returncode == 0 and line == f"camera scale: 1 at the start, set 0, 0 gives -1, 101 gives -1, "
                   f"get {scale:g}", f"{bits}-bit camera scale option {scale}: '{line}'")
        energy[scale] = rms(out) ** 2
        data[scale] = out.read_bytes() if out.exists() else b""
    report(data[1] == data[None] and len(data[1]) > 0,
           f"{bits}-bit camera scale 1: the same sound as without the option, byte for byte ({len(data[1]) // 4} frames)")
    report(energy[0.5] > 1.2 * energy[None],
           f"{bits}-bit camera scale 0.5: effects energy {energy[0.5]:.0f}, {energy[0.5] / max(energy[None], 1):.2f} "
           f"times that at 1 ({energy[None]:.0f})")
    same_traces(f"{bits}-bit camera scale 1, level {level}: the same game state and memory as without the option",
                traces(b.host, rom, level, 900, extra=["--camera-scale", 1], env=mus, work=work),
                traces(b.host, rom, level, 900, env=mus, work=work))
    # GLOVER_OFF_AMBIENT: a floor in level 12 near two of its ambient sounds at points, the camera's distances at a
    # third. With the bit the effects lose most of their energy.
    world = ("tri 667 0 63  667 0 2063  2667 0 2063  0\ntri 667 0 63  2667 0 2063  2667 0 63  0\n"
             "place 0 hand 1667 30 1063 ball 1767 30 1063 yaw 0 camera\n"
             "view 0 3  1367 100 1063  1667 20 1063  0 1 0  45 1.3333333 10 5000\n")
    amb = {}
    for off in (3, 7):
        sc = pathlib.Path(work) / f"ambient{off}.scene"
        sc.write_text(f"options off {off}\n" + world)
        out = pathlib.Path(work) / f"ambient{bits}-{off}.raw"
        r = run([b.host, "--rom", rom, "--headless", 300, "--level", SCENE_LEVEL, "--scene", sc, "--audio-out", out,
                 "--volume", 0, 100, "--camera-scale", 1 / 3], mus)
        amb[off] = rms(out) ** 2 if r.returncode == 0 else -1
    report(amb[3] > 1e5 and 0 <= amb[7] < amb[3] / 4,
           f"{bits}-bit GLOVER_OFF_AMBIENT, level {SCENE_LEVEL} at a camera scale of 1/3: effects energy {amb[3]:.0f} "
           f"with the level's ambient sounds, {amb[7]:.0f} without them")
    nohash = lambda t: ([re.sub(r" hash \w+$", "", l) for l in t[0]], t[1])
    same_traces(f"{bits}-bit camera scale 0.5, level {level}: the same game state as without the option",
                nohash(traces(b.host, rom, level, 900, extra=["--camera-scale", 0.5], env=mus, work=work)),
                nohash(traces(b.host, rom, level, 900, env=mus, work=work)))


def wine(bits):
    """Wine and its environment for a word size: $WINE64 or $WINE32, else wine64 or wine (on PATH or in /usr/lib/wine),
    with $WINEPREFIX64 or $WINEPREFIX32, else ~/.wine64 (a 64-bit prefix) or ~/.wine (docs/building.md)."""
    if bits == 64:
        exe = os.environ.get("WINE64") or shutil.which("wine64") or "/usr/lib/wine/wine64"
        prefix = os.environ.get("WINEPREFIX64", os.path.expanduser("~/.wine64"))
    else:
        exe = os.environ.get("WINE32") or shutil.which("wine") or "/usr/lib/wine/wine"
        prefix = os.environ.get("WINEPREFIX32", os.path.expanduser("~/.wine"))
    return exe, {"WINEDEBUG": "-all", "WINEPREFIX": prefix}


def wine_env(work, n):
    save = pathlib.Path(work) / f"save{n}.bin"
    save.write_bytes(b"")
    return save


def check_wine(b, rom, work):
    """The Windows DLLs: exports and imports, and the same traces as the Linux library under Wine."""
    bits = b.bits
    objdump = "x86_64-w64-mingw32-objdump" if bits == 64 else "i686-w64-mingw32-objdump"
    for dll in (b.dll, b.dll_stub):
        build = dll.parent.name
        p = run([objdump, "-p", dll]).stdout
        names = sorted(re.findall(r"^\s*\[\s*\d+\]\s+(\w+)$", p, re.M))
        report(names == sorted(SYMS), f"{build}: the exports are glover.syms ({len(names)} names)")
        report(("pei-x86-64" if bits == 64 else "pei-i386") in p, f"{build}: a {bits}-bit DLL")
        dlls = sorted(set(re.findall(r"DLL Name: (\S+)", p)))
        report(all(d.upper() in ("KERNEL32.DLL", "MSVCRT.DLL") or d.lower().startswith("api-ms-win-crt")
                   for d in dlls), f"{build}: imports {' '.join(dlls)}")
        chars = re.search(r"DllCharacteristics\s+([0-9a-fA-F]+)", p)
        c = int(chars.group(1), 16) if chars else 0
        # HIGH_ENTROPY_VA (0x20) applies to 64-bit images only.
        report(c & 0x40 and (c & 0x20 or bits == 32), f"{build}: DYNAMIC_BASE{' and HIGH_ENTROPY_VA' if bits == 64 else ''} "
               f"({c:#x})")
    wexe, wenv = wine(bits)
    n = bits * 100
    for dll, linux, label in ((b.dll_stub, b.stub, "stub"), (b.dll, b.lib, "mus")):
        for name, extra in (("5", []), ("13", []), ("36", []), ("13", ["--dt", "0.25", "--dt-jitter", "0.3"]),
                            ("13", ["--ballworld"])):
            n += 1
            save = wine_env(work, n)
            w = traces([wexe, b.host_exe], rom, name, 900, ["--save", save, *extra], {**wenv, "GLOVER_LIB": str(dll)},
                       work)
            l = traces(b.host, rom, name, 900, ["--save", save, *extra], {"GLOVER_LIB": str(linux)}, work)
            same_traces(f"wine, {bits}-bit {label} DLL, level {name} {' '.join(extra)}: the same as the Linux library",
                        w, l)
    for name in ("terrains.scene", "lift.scene", "seams.scene", "turntable.scene", "conveyor.scene",
                 "beltbody.scene", "hostwater.scene"):
        scene = ROOT / "tools" / "hostworld" / name
        for extra in ([], ["--dt", "0.25", "--dt-jitter", "0.3"]):
            n += 1
            save = wine_env(work, n)
            same_traces(f"wine, {bits}-bit stub DLL, scene {name} {' '.join(extra)}: the same as the Linux library",
                        scene_traces([wexe, b.host_exe], rom, scene, 600, ["--save", save, *extra],
                                     {**wenv, "GLOVER_LIB": str(b.dll_stub)}),
                        scene_traces(b.host, rom, scene, 600, ["--save", save, *extra], {"GLOVER_LIB": str(b.stub)}))
    r = run([wexe, b.host_exe, "--rom", rom, "--headless", 10, "--level", SCENE_LEVEL, "--coll-args", "--save",
             wine_env(work, n + 3)], {**wenv, "GLOVER_LIB": str(b.dll_stub)})
    report(r.returncode == 0, f"wine, {bits}-bit: the host world's argument checks: exit {r.returncode}")
    r = run([wexe, b.host_exe, "--rom", rom, "--headless", 300, "--level", 5, "--fatal-at", 100, "--host-mxcsr",
             "0xFFC0", "--save", wine_env(work, n + 1)], {**wenv, "GLOVER_LIB": str(b.dll_stub)})
    report(r.returncode == 0 and "glover_update returned -5: fatal: test" in r.stdout and "0 calls returned" in r.stdout,
           f"wine, {bits}-bit: fatal at update 100 and MXCSR kept: exit {r.returncode}")
    r = run([wexe, b.host_exe, "--rom", rom, "--headless", 200, "--level", 5, "--lifecycle", "--save",
             wine_env(work, n + 2)], {**wenv, "GLOVER_LIB": str(b.dll)})
    report(r.returncode == 0, f"wine, {bits}-bit: the argument checks, glover_destroy and the calls after it: exit "
           f"{r.returncode}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=regress.default_rom())
    ap.add_argument("--bits", type=int, choices=(32, 64))
    ap.add_argument("--wine", action="store_true")
    ap.add_argument("--gl", action="store_true")
    args = ap.parse_args()
    builds = [Build(bits) for bits in ((args.bits,) if args.bits else (64, 32))]
    for b in builds:
        for lib in (b.lib, b.stub) + ((b.hostgfx,) if b.bits == 64 else ()):
            check_elf(lib, b.bits)
    with tempfile.TemporaryDirectory() as work:
        check_header(work)
        for b in builds:
            check_host(b, args.rom, work)
            check_scenes(b, args.rom, work)
            check_marks(b, args.rom)
            check_host_gfx(b, args.rom)
            check_sound(b, args.rom, work)
            check_volume(b, args.rom, work)
            check_camera_scale(b, args.rom, work)
            if args.wine:
                check_wine(b, args.rom, work)
            if args.gl:
                check_gl(b, args.rom, work)
    print(f"libcheck: {failures} checks failed")
    return 1 if failures else 0


def png_rgb(path):
    """The pixels of a PNG that weemitt wrote (write_png in platform.c): 8-bit RGB, rows of filter 0, from the top."""
    data = pathlib.Path(path).read_bytes()
    pos = 8
    idat = b""
    width = height = 0
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            width = int.from_bytes(chunk[0:4], "big")
            height = int.from_bytes(chunk[4:8], "big")
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = width * 3 + 1
    rows = [raw[y * stride:(y + 1) * stride] for y in range(height)]
    if any(r[0] != 0 for r in rows):
        return width, height, None
    return width, height, b"".join(r[1:] for r in rows)


def check_gl(b, rom, work):
    """The renderer in the library draws what weemitt's --shot draws, pixel for pixel."""
    shots = ("100", "299")
    bits = b.bits
    prefix = pathlib.Path(work) / f"gl{bits}"
    r = run([b.glshot, "--rom", rom, "--level", 5, "--headless", 300, *[a for f in shots for a in ("--shot", f)],
             "--prefix", prefix], {"GLOVER_LIB": str(b.lib)})
    for line in r.stdout.splitlines():
        if "GL state" in line:
            report("is the promised one" in line, f"{bits}-bit glshot: {line}")
        elif line.startswith(("a second glover_gl_init", "glover_destroy")):
            print(f"     {bits}-bit glshot: {line}")
    report(r.returncode == 0, f"{bits}-bit glshot: exit {r.returncode}")
    ref = pathlib.Path(work) / f"ref{bits}_"
    run([b.window, "--rom", rom, "--headless", 300, "--level", 5, *[a for f in shots for a in ("--shot", f)],
         "--shot-prefix", ref])
    for f in shots:
        w, h, rgb = png_rgb(f"{ref}{int(f):05d}.png")
        raw = pathlib.Path(f"{prefix}_{int(f):05d}.rgba")
        lib = raw.read_bytes() if raw.exists() else b""
        mine = b"".join(lib[i:i + 3] for i in range(0, len(lib), 4))
        flipped = rgb and b"".join(rgb[(h - 1 - y) * w * 3:(h - y) * w * 3] for y in range(h))
        report(rgb is not None and mine == rgb and mine != flipped,
               f"{bits}-bit frame {f}: the library's picture equals {b.window.parent.name}'s --shot, pixel for pixel "
               f"({w}x{h}, not the same upside down)")
    r = run([b.glshot, "--rom", rom, "--level", 5, "--headless", 10, "--prefix", prefix], {"GLOVER_LIB": str(b.stub)})
    report(r.returncode == 5 and "returned -8" in r.stdout, f"{bits}-bit, the stub library: {r.stdout.strip()}")
    # A camera set by the host (docs/host-world.md): the mirror view draws the game's own picture through the host's
    # path, pixel for pixel; a view of its own draws another picture.
    pics = {}
    for name in ("none", "mirror", "view"):
        scene = [] if name == "none" else ["--scene", ROOT / "tools" / "hostworld" / f"{name}.scene"]
        pre = pathlib.Path(work) / f"view{bits}{name}"
        r = run([b.glshot, "--rom", rom, "--level", SCENE_LEVEL, "--headless", 200, "--shot", 199, "--prefix", pre,
                 *scene], {"GLOVER_LIB": str(b.lib)})
        raw = pathlib.Path(f"{pre}_00199.rgba")
        pics[name] = raw.read_bytes() if raw.exists() else None
        report(r.returncode == 0 and pics[name] is not None, f"{bits}-bit glshot, scene {name}: exit {r.returncode}")
        if name == "view":
            report("widescreen 2 and a drawing view: -1" in r.stdout,
                   f"{bits}-bit glshot: glover_gl_options refuses widescreen 2 while a drawing view is set")
    report(pics["none"] is not None and pics["mirror"] == pics["none"],
           f"{bits}-bit: the mirror view's picture equals the game's own, pixel for pixel")
    report(pics["view"] is not None and pics["view"] != pics["none"], f"{bits}-bit: a host view draws another picture")
    check_layer(b, rom, work)


def check_layer(b, rom, work):
    """The layer (glover_gl_draw_layer, glover_gl_composite, docs/library.md)."""
    bits = b.bits
    env = {"GLOVER_LIB": str(b.lib)}
    # All four parts: the picture of glover_gl_draw at the same size, pixel for pixel, and opaque.
    pre = pathlib.Path(work) / f"layer{bits}"
    r = run([b.glshot, "--rom", rom, "--level", 5, "--headless", 300, "--shot", 100, "--shot", 299, "--prefix", pre,
             "--layer", 15], env)
    states = [l for l in r.stdout.splitlines() if "GL state" in l]
    report(r.returncode == 0 and states and all("is the promised one" in l for l in states),
           f"{bits}-bit layer: exit {r.returncode}, the GL state after each of {len(states)} calls is the promised one")
    checks = next((l for l in r.stdout.splitlines() if l.startswith("glover_gl_draw_layer before")), "")
    report(checks.count("-1") == 5, f"{bits}-bit layer: {checks}")
    for f in (100, 299):
        pic = pathlib.Path(f"{pre}_{f:05d}.rgba")
        lay = pathlib.Path(f"{pre}_layer_{f:05d}.rgba")
        a = pic.read_bytes() if pic.exists() else b""
        c = lay.read_bytes() if lay.exists() else b""
        same = len(a) == len(c) > 0 and all(a[i:i + 3] == c[i:i + 3] for i in range(0, len(a), 4))
        opaque = len(c) > 0 and all(c[i] == 255 for i in range(3, len(c), 4))
        report(same and opaque, f"{bits}-bit layer of level 5, frame {f}, all four parts: the RGB of glover_gl_draw "
               f"at 640x480, pixel for pixel, and alpha 255")
    # The actors alone, with a drawing view on the glove: transparent around it, opaque on it, its depth, and the host's
    # depth hiding or showing it in the composite.
    scene = pathlib.Path(work) / "layer.scene"
    tris = [l for l in (ROOT / "tools" / "hostworld" / "flat.scene").read_text().splitlines() if l.startswith("tri")]
    scene.write_text("\n".join(tris + ["place 0 hand 0 40 0 ball 200 40 200 yaw 0 camera",
                                       "view 0 1 0 60 -120 0 13 0 0 1 0 45 1.333333 10 3000"]) + "\n")
    r = run([b.glshot, "--rom", rom, "--level", SCENE_LEVEL, "--headless", 200, "--shot", 199, "--prefix", pre,
             "--layer", 1, "--scene", scene], env)
    out = r.stdout.splitlines()
    states = [l for l in out if "GL state" in l]
    report(r.returncode == 0 and states and all("is the promised one" in l for l in states),
           f"{bits}-bit layer of the actors: exit {r.returncode}, the GL state after each call is the promised one")
    corners = next((l for l in out if l.startswith("layer 199")), "")
    glove = next((l for l in out if l.startswith("glove 199")), "")
    comp = next((l for l in out if l.startswith("composite 199")), "")
    nums = lambda line: [float(x) for x in line.replace(",", " ").replace(";", " ").split()  # noqa: E731
                         if x.replace(".", "").replace("-", "").isdigit()]
    c = nums(corners.split("corners")[1]) if "corners" in corners else []
    report(c[:4] == [0, 0, 0, 0], f"{bits}-bit layer of the actors: alpha 0 at the corners ({corners})")
    g = nums(glove)
    ok = len(g) >= 6 and g[2] == 255 and abs(g[4] - g[5]) <= 13.0
    report(ok, f"{bits}-bit layer of the actors: on the glove alpha 255, and the depth made linear is the glove's "
               f"distance within its radius ({glove})")
    v = nums(comp)
    ok = len(v) >= 11 and v[0] == 0 and v[1:4] == [255, 0, 0] and v[4:7] == v[7:10] and v[4:7] != [255, 0, 0]
    report(ok, f"{bits}-bit composite: the host's red in front hides the glove, behind it shows it ({comp})")
    cd = next((l for l in out if l.startswith("composite depth 199")), "")
    v = nums(cd)
    ok = len(v) >= 6 and v[0] == 0 and abs(v[1] - v[3]) < 1e-5 and v[2] < v[3] - 1e-5 and abs(v[4] - v[5]) < 1e-5
    report(ok, f"{bits}-bit composite with GLOVER_COMPOSITE_DEPTH: the glove's depth replaces the host's behind it, "
               f"not in front ({cd})")
    stale = next((l for l in out if l.startswith("stale 199")), "")
    report(stale.endswith("-1, then glover_gl_composite -1"),
           f"{bits}-bit layer: after a layer that failed to draw, nothing to composite ({stale})")
    # The depth made linear with the projection's own terms (projZ) does not depend on the near plane. With the plain
    # planes a near plane of 1 puts the glove 7 units (0.45%) nearer at 1549 units.
    seen = []
    for near in (1, 50):
        scene = pathlib.Path(work) / f"layer-near{near}.scene"
        scene.write_text("tri -20000 0 -20000  -20000 0 20000  20000 0 20000  0\n"
                         "tri -20000 0 -20000  20000 0 20000  20000 0 -20000  0\n"
                         "place 0 hand 0 40 0 ball 60 40 30 yaw 0 camera\n"
                         f"view 0 1 0 400 -1500 0 13 0 0 1 0 30 1.333333 {near} 30000\n")
        r = run([b.glshot, "--rom", rom, "--level", SCENE_LEVEL, "--headless", 200, "--shot", 199, "--prefix",
                 pathlib.Path(work) / f"layer-near{near}", "--layer", 1, "--scene", scene], env)
        glove = next((l for l in r.stdout.splitlines() if l.startswith("glove 199")), "")
        g = nums(glove)
        seen.append(g[4] if r.returncode == 0 and len(g) >= 6 else None)
    ok = None not in seen and abs(seen[0] - seen[1]) <= 0.001 * seen[1]
    report(ok, f"{bits}-bit layer: the glove's depth made linear at 1549 units with the near planes 1 and 50: {seen}, "
               f"within 0.1%")


if __name__ == "__main__":
    sys.exit(main())
