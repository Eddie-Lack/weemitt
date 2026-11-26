#!/usr/bin/env python3
"""The tests of the host world (docs/host-world.md) on the synthetic scenes of this folder, through weemitt's headless
mode (--host-scene). tools/libcheck.py runs the same scenes through the library and compares.

Usage: python3 tools/hostworld/hwtest.py [--rom ROM] [--exe build/headless/weemitt]
           [--checked build/headless-displaced/weemitt] [--baseline-exe EXE]

--baseline-exe takes a headless build of an earlier commit (from a git worktree). Every scene of this folder that it
can read must give the same trace with both programs, except the scenes in CHANGED, whose behaviour was meant to
change.

The ROM comes from --rom, $GLOVER_ROM or glover.z64. Scenes made at run time go to build/hostworld/.
Prints one line per check, "ok" or "FAIL", and exits 1 when a check fails.
"""
import argparse
import math
import os
import pathlib
import re
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build" / "hostworld"
LEVEL = 12   # a level whose enemies leave the glove alone at the scenes' places
CHANGED = {"turntable.scene"}   # a turning body turns the glove and the ball since the body's PlatformMotion
failures = 0


def report(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def f32(h):
    return struct.unpack(">f", bytes.fromhex(h))[0]


class Run:
    """One headless run of a scene: the trace per original frame and the scene's messages."""

    def __init__(self, exe, rom, scene, frames, level=LEVEL, extra=()):
        cmd = [str(exe), "--rom", str(rom), "--headless", str(frames), "--level", str(level), "--host-scene",
               str(scene), "--trace", *map(str, extra)]
        t = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.seconds = time.time() - t
        self.code = r.returncode
        self.err = r.stderr
        self.frames = {}
        self.bodies = {}
        self.lines = []
        for line in r.stdout.splitlines():
            w = line.split()
            if not w:
                continue
            if w[0] == "frame":
                self.lines.append(line)
                n = int(w[1])
                i, j, k = w.index("hand"), w.index("ball"), w.index("cam")
                self.frames[n] = {
                    "mode": int(w[5]),
                    "hand": [f32(x) for x in w[i + 1:i + 4]],
                    "ball": [f32(x) for x in w[j + 1:j + 4]],
                    "cam": [f32(x) for x in w[k + 1:k + 4]],
                }
            elif w[0] == "bodies":
                self.lines.append(line)
                d = dict(zip(w[2::2], w[3::2]))
                self.bodies[int(w[1])] = {
                    "yaw": d["yaw"], "action": int(d["action"]), "held": int(d["held"]), "ready": int(d["ready"]),
                    "restarts": int(d["restarts"]), "ground": int(w[w.index("ground") + 1]),
                    "terrain": (int(w[w.index("terrain") + 1]), int(w[w.index("terrain") + 2])),
                    "void": (int(w[w.index("void") + 1]), int(w[w.index("void") + 2])),
                    "voids": (int(w[w.index("voids") + 1]), int(w[w.index("voids") + 2])),
                    "safeKnown": int(w[w.index("safe") + 1]),
                    "water": (int(w[w.index("water") + 1]), int(w[w.index("water") + 2])) if "water" in w else None,
                    "safe": [f32(x) for x in w[w.index("safe") + 2:w.index("safe") + 8]],
                }
        self.stats = next((l for l in self.err.splitlines() if l.startswith("hostscene: ") and "static triangles" in l),
                          "")
        self.fatal = "fatal" in self.err.lower() or self.code != 0

    def stat(self, name):
        """A number of the final stats line, by the word after it."""
        w = self.stats.replace(",", "").replace("(", "").replace(")", "").replace(";", "").split()
        for i, x in enumerate(w):
            if x == name and i > 0 and w[i - 1].isdigit():
                return int(w[i - 1])
        return None


def lift_top(t):
    """The top of lift.scene's box at time t: 40 + 5 + 30 sin(2 pi t / 120)."""
    return 45.0 + 30.0 * math.sin(2.0 * math.pi * t / 120.0)


def grid_scene(path, n, size):
    """A generated terrain of 2 n n triangles, size units square, with gentle hills. Not committed."""
    lines = ["options maxtris 1024"]
    s = size / n

    def h(i, j):
        x, z = i * s, j * s
        return round(12.0 * math.sin(x / 300.0) * math.cos(z / 260.0), 3)

    for j in range(n):
        for i in range(n):
            x0, z0 = i * s - size / 2, j * s - size / 2
            a = (x0, h(i, j), z0)
            b = (x0, h(i, j + 1), z0 + s)
            c = (x0 + s, h(i + 1, j + 1), z0 + s)
            d = (x0 + s, h(i + 1, j), z0)
            lines.append("tri %g %g %g  %g %g %g  %g %g %g" % (*a, *b, *c))
            lines.append("tri %g %g %g  %g %g %g  %g %g %g" % (*a, *c, *d))
    lines.append("place 0 hand 0 60 0 ball 40 60 40 ballvel 3 0 2 camera")
    path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=os.environ.get("GLOVER_ROM", "glover.z64"))
    ap.add_argument("--exe", default=str(ROOT / "build" / "headless" / "weemitt"))
    ap.add_argument("--checked", default=str(ROOT / "build" / "headless-displaced" / "weemitt"),
                    help="the build with PORT_PTR_CHECK; skipped when it is missing")
    ap.add_argument("--baseline-exe", help="a headless build of an earlier commit to compare every scene with")
    a = ap.parse_args()
    if not pathlib.Path(a.rom).exists():
        print("hwtest: no ROM at %s" % a.rom)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    exe, rom = a.exe, a.rom
    sc = lambda name: HERE / name  # noqa: E731

    # Rest heights.
    r = Run(exe, rom, sc("flat.scene"), 400)
    b, h = r.frames[399]["ball"], r.frames[399]["hand"]
    report(not r.fatal and abs(b[1] - 9.0) <= 0.01 and abs(h[1] - 13.0) <= 0.01,
           "flat: the ball rests at %.3f (9) and the glove at %.3f (13)" % (b[1], h[1]))
    report(r.bodies[399]["ready"] == 1 and r.bodies[399]["terrain"] == (0, 0),
           "flat: ready, and both stand on terrain 0")

    # Chunks change nothing: many chunks against one, with the same faces in the same order.
    one = OUT / "seams-one.scene"
    one.write_text((HERE / "seams.scene").read_text().replace("options cell 64 ", "options cell 1000000 "))
    r1 = Run(exe, rom, sc("seams.scene"), 400)
    r2 = Run(exe, rom, one, 400)
    report(r1.stat("chunks") > 100 and r2.stat("chunks") == 1 and r1.lines == r2.lines,
           "seams: %s chunks and %s chunk give the same trace (%d lines)" % (r1.stat("chunks"), r2.stat("chunks"),
                                                                            len(r1.lines)))
    # Linking only the chunks near the glove, the ball and the camera changes nothing either.
    near = OUT / "seams-link.scene"
    near.write_text((HERE / "seams.scene").read_text().replace("options cell 64 maxtris 4096",
                                                                "options cell 64 maxtris 4096 link 100"))
    r3 = Run(exe, rom, near, 400)
    report(r3.stat("linked") < r3.stat("chunks") and r3.lines == r1.lines,
           "seams with link 100: %s of %s chunks linked at the end, the same trace" % (r3.stat("linked"),
                                                                                       r3.stat("chunks")))
    b = r1.frames[399]["ball"]
    report(math.hypot(b[0] - 700, b[2] - 700) > 200, "seams: the ball rolled %.0f units across the chunks" %
           math.hypot(b[0] - 700, b[2] - 700))

    # Terrains.
    r = Run(exe, rom, sc("terrains.scene"), 900)
    d0 = r.frames[199]["ball"][2] + 150
    d8 = r.frames[399]["ball"][2] + 150
    report(d8 > d0 + 10, "terrains: the ball stops after %.1f units on terrain 0 and %.1f on 8" % (d0, d8))
    report(any(r.bodies[f]["terrain"][1] == 12 for f in range(400, 600)), "terrains: the ball touches terrain 12")
    hurt = [f for f in range(600, 700) if r.bodies[f]["action"] == 28]
    report(hurt and r.bodies[699]["restarts"] == r.bodies[599]["restarts"]
           and all(r.bodies[f]["terrain"][0] == 9 for f in hurt),
           "terrains: on terrain 9 the glove is hurt (from frame %s) and no restart begins" % (hurt[0] if hurt else "-"))
    report(r.bodies[899]["restarts"] == r.bodies[699]["restarts"] + 1,
           "terrains: the ball on terrain 6 bursts and the glove restarts (restarts %d to %d)" % (
               r.bodies[699]["restarts"], r.bodies[899]["restarts"]))

    # A void: the ball and then the glove fall into a pit whose floor is a void. Each is reported, nothing is hurt or
    # burst, and no restart begins. The same pit without the void flag costs restarts.
    r = Run(exe, rom, sc("void.scene"), 700)
    bo = r.bodies
    report(any(bo[f]["void"][1] == 1 for f in range(100, 200)) and bo[299]["void"][1] == 2,
           "void: the ball is over the void while it falls, and touches it at frame 299")
    report(bo[99]["voids"] == (0, 0) and bo[299]["voids"][0] == 0 and bo[299]["voids"][1] >= 1
           and bo[499]["voids"][0] == 1,
           "void: the touches are reported: ball %d by frame 299, glove %d by frame 499" % (bo[299]["voids"][1],
                                                                                         bo[499]["voids"][0]))
    report(bo[699]["restarts"] == bo[99]["restarts"] and not any(bo[f]["action"] == 28 for f in range(100, 700)),
           "void: no restart and no hurt from the falls (restarts %d to %d)" % (bo[99]["restarts"], bo[699]["restarts"]))
    sf = bo[299]["safe"]
    report(bo[299]["safeKnown"] == 3 and abs(sf[0] + 2000) <= 1 and abs(sf[1] - 13) <= 0.01 and abs(sf[4] - 9) <= 0.01,
           "void: the safe places are on the ledge: glove %.1f %.1f %.1f, ball at height %.2f" % (sf[0], sf[1], sf[2],
                                                                                                 sf[4]))
    h, b = r.frames[699]["hand"], r.frames[699]["ball"]
    report(abs(h[1] - 13) <= 0.01 and abs(b[1] - 9) <= 0.01, "void: placed back on the ledge, the glove rests at %.2f "
           "and the ball at %.2f" % (h[1], b[1]))
    plain = OUT / "void-off.scene"
    plain.write_text((HERE / "void.scene").read_text().replace("  0 2\n", "  0 0\n"))
    r = Run(exe, rom, plain, 700)
    report(r.bodies[699]["restarts"] > r.bodies[99]["restarts"] and r.bodies[699]["voids"] == (0, 0),
           "void: the same pit without the void flag costs %d restarts" % (r.bodies[699]["restarts"]
                                                                          - r.bodies[99]["restarts"]))

    # The glove holds the ball over the pit: the two fall together, and both report the void and its touches.
    lines = (HERE / "void.scene").read_text().splitlines()
    tris = [l for l in lines if l.startswith("tri")]
    held = OUT / "void-held.scene"
    held.write_text("\n".join(tris + ["place 0 hand -2000 40 0 ball -2000 70 0 yaw 0 camera", "place 200 hand -1300 40 0",
                                      "place 500 hand -2000 40 0 ball -2100 40 0 camera"]) + "\n")
    r = Run(exe, rom, held, 700)
    bo = r.bodies
    report(bo[199]["held"] == 1 and bo[499]["held"] == 1 and bo[499]["void"] == (2, 2) and bo[499]["voids"][0] >= 1
           and bo[499]["voids"][0] == bo[499]["voids"][1] and bo[699]["restarts"] == bo[199]["restarts"],
           "void: a held ball and the glove report the void together (touches %d and %d), with no restart" % (
               bo[499]["voids"][0], bo[499]["voids"][1]))

    # The void flag on a moving body's triangles works as on the static world.
    body = OUT / "void-body.scene"
    body.write_text("\n".join(tris[:2] + ["body 1 -1300 -1500 0  0 0 0 1  1 1 1",
                                           "tri -400 0 -400  -400 0 400  400 0 400  0 2",
                                           "tri -400 0 -400  400 0 400  400 0 -400  0 2", "end"]
                                + [l for l in lines if l.startswith("place")]) + "\n")
    r = Run(exe, rom, body, 700)
    bo = r.bodies
    report(bo[299]["void"][1] == 2 and bo[499]["voids"][0] >= 1 and bo[499]["voids"][1] >= 1
           and bo[699]["restarts"] == bo[99]["restarts"],
           "void: a body's void triangles are reported (touches %d and %d), with no restart" % (
               bo[499]["voids"][0], bo[499]["voids"][1]))

    # A held ball goes with the glove: a place of one of the two moves both, and the glove holds on. A place of both
    # apart makes the glove let go.
    r = Run(exe, rom, sc("hold.scene"), 500)
    want = {299: ((300, 0), (300, 0), 1), 399: ((-300, 0), (-300, 0), 1), 499: ((0, 300), (300, 300), 0)}
    ok = True
    for f, (hw, bw, held) in want.items():
        h, b = r.frames[f]["hand"], r.frames[f]["ball"]
        ok &= abs(h[0] - hw[0]) <= 0.5 and abs(h[2] - hw[1]) <= 0.5 and abs(b[0] - bw[0]) <= 0.5
        ok &= abs(b[2] - bw[1]) <= 0.5 and r.bodies[f]["held"] == held
    report(ok, "hold: the glove alone and the ball alone move the two together; both apart, the glove lets go")

    # The level's water boxes leave with its landscape: the ball does not float on level 12's water.
    r = Run(exe, rom, sc("water.scene"), 400)
    report(abs(r.frames[399]["ball"][1] + 291.0) <= 0.01,
           "water: the ball rests on the floor at %.2f (-291), under level 12's water" % r.frames[399]["ball"][1])

    # A host water: the glove swims at the bottom, the ball floats at the surface (-20), and sinks to the floor (-191)
    # when the water goes at frame 400. A current of 0.25 along x carries the floating ball. A reload of the level
    # builds the water again.
    hwsc = (HERE / "hostwater.scene").read_text()
    for dt in (1.0, 0.25):
        r = Run(exe, rom, sc("hostwater.scene"), 600, extra=("--dt", dt))
        ys = [r.frames[f]["ball"][1] for f in range(150, 400)]
        mean = sum(ys) / len(ys)
        report(not r.fatal and abs(mean + 20.0) <= 3.0 and max(abs(y + 20.0) for y in ys) <= 8.0
               and r.bodies[300]["water"] == (3, 4) and abs(r.frames[599]["ball"][1] + 191.0) <= 0.01
               and r.bodies[599]["water"] == (0, 0) and r.stat("waters") == 0,
               "hostwater, dt %g: the ball floats at %.2f on average (-20), from %.2f to %.2f; water states %s (3 the "
               "glove swims, 4 the ball floats); without the water the ball rests at %.2f" % (
                   dt, mean, min(ys), max(ys), r.bodies[300]["water"], r.frames[599]["ball"][1]))
    flow = OUT / "hostwater-current.scene"
    flow.write_text(hwsc.replace("1200 180 1200  -20", "1200 180 1200  -20  0.25 0 0")
                    .replace("ball 200 40 0", "ball -400 40 0").replace("water 400 off", ""))
    r = Run(exe, rom, flow, 400)
    speed = (r.frames[380]["ball"][0] - r.frames[200]["ball"][0]) / 180.0
    report(not r.fatal and 7.0 <= speed / 0.25 <= 9.5,
           "hostwater: a current of 0.25 carries the floating ball at %.3f per frame, %.2f times the current" % (
               speed, speed / 0.25))
    again = OUT / "hostwater-reload.scene"
    again.write_text(hwsc.replace("water 400 off", "goto 300 12\nplace 450 ball 200 40 0"))
    r = Run(exe, rom, again, 700)
    ys = [r.frames[f]["ball"][1] for f in range(550, 700)]
    report(not r.fatal and r.stat("rebuilds") == 1 and r.stat("waters") == 1 and max(abs(y + 20.0) for y in ys) <= 8.0,
           "hostwater: a reload builds the water again (%s rebuilds); the ball floats at %.2f to %.2f" % (
               r.stat("rebuilds"), min(ys), max(ys)))
    beside = OUT / "water-host.scene"
    beside.write_text((HERE / "water.scene").read_text() + "water -2600 -300 -200  400 200 400  -150\n")
    r = Run(exe, rom, beside, 400)
    report(abs(r.frames[399]["ball"][1] + 291.0) <= 0.01 and r.stat("waters") == 1,
           "hostwater: with a host water elsewhere, level 12's water stays empty: the ball rests at %.2f (-291)" %
           r.frames[399]["ball"][1])

    # The level's own enemies and collectables switched off: none is left after the restart that the death pad
    # causes, which brings the enemies back. Without the option they are there.
    for level in (12, 13, 26):
        r = Run(exe, rom, sc("off.scene"), 500, level=level)
        on = OUT / "off-none.scene"
        on.write_text((HERE / "off.scene").read_text().replace("options off 3", "options off 0"))
        q = Run(exe, rom, on, 500, level=level)
        report(not r.fatal and r.stat("enemies") == 0 and r.stat("collectables") == 0
               and r.bodies[499]["restarts"] == r.bodies[199]["restarts"] + 1
               and q.stat("enemies") + q.stat("collectables") > 0,
               "off, level %d: 0 enemies and 0 collectables after a restart (%s and %s without the option)" % (
                   level, q.stat("enemies"), q.stat("collectables")))

    # The lift, at dt 1 and at dt 0.25.
    for dt in (1.0, 0.25):
        r = Run(exe, rom, sc("lift.scene"), 600, extra=("--dt", dt))
        # The trace's frame f follows the update that ends at f + dt.
        errs = [r.frames[f]["hand"][1] - 13.0 - lift_top(f + dt) for f in range(150, 600)]
        worst = max(abs(e) for e in errs)
        report(worst <= 0.05, "lift, dt %g: the glove stays 13 above the lift's top within %.4f" % (dt, worst))
    # The slider's x is 120 sin(2 pi t / 160): the glove's place on it must not depend on dt.
    rs = {dt: Run(exe, rom, sc("slider.scene"), 600, extra=("--dt", dt)) for dt in (1.0, 0.25)}
    xs = {dt: [rs[dt].frames[f]["hand"][0] - 120.0 * math.sin(2.0 * math.pi * (f + dt) / 160.0)
               for f in range(150, 600)] for dt in rs}
    drift = {dt: max(abs(x - xs[dt][0]) for x in xs[dt]) for dt in xs}
    report(max(drift.values()) <= 0.05, "slider: the glove keeps its place on the slider within %.4f at dt 1 and "
           "%.4f at dt 0.25" % (drift[1.0], drift[0.25]))

    # The turntable turns the glove with it: the glove's offset from the axis and its yaw follow the body's turn. A
    # ball resting on it turns too.
    ball_on = OUT / "turntable-ball.scene"
    ball_on.write_text((HERE / "turntable.scene").read_text().replace("place 0 hand 25 60 0 ball 0 30 150",
                                                                       "place 0 hand 0 30 150 ball 20 60 0"))
    for dt in (1.0, 0.25):
        for what, scene in (("hand", sc("turntable.scene")), ("ball", ball_on)):
            r = Run(exe, rom, scene, 400, extra=("--dt", dt))
            errs, radii, yaws = [], [], []
            for f in range(150, 350):
                p = r.frames[f][what]
                turn = 2.0 * math.pi * (f + dt) / 200.0
                # A turn by t about y takes the angle atan2(z, x) to atan2(z, x) - t.
                errs.append(math.atan2(p[2], p[0]) + turn)
                radii.append(math.hypot(p[0], p[2]))
                if what == "hand":
                    yaws.append(f32(r.bodies[f]["yaw"]) - turn)
            spread = lambda xs: max(abs(math.remainder(x - xs[0], 2 * math.pi)) for x in xs)  # noqa: E731
            worst = math.degrees(spread(errs))
            yaw = math.degrees(spread(yaws)) if yaws else 0.0
            report(not r.fatal and worst <= 2.0 and max(radii) - min(radii) <= 0.5 and yaw <= 2.0,
                   "turntable, %s, dt %g: the offset turns with the body within %.3f degrees, its radius %.2f to %.2f"
                   "%s" % (what, dt, worst, min(radii), max(radii),
                           ", the yaw within %.3f degrees" % yaw if what == "hand" else ""))

    # A conveyor strip of the static world: the glove rides it at 1 unit per original frame and stops after its end. The
    # ball rides it too: the game gives a ball that lands on a conveyor the conveyor's speed on top of the carry, and the
    # rolling ball lands again at each inner vertex of the strip, so it goes faster. With a floor just under the strip
    # it does not. A ball rolled across the strip keeps its height over the seams of the strip's own chunk.
    conv = (HERE / "conveyor.scene").read_text()
    place = "place 0 hand -250 40 0 ball 0 30 200 yaw 0 camera"
    rides = {}
    for dt in (1.0, 0.25):
        r = Run(exe, rom, sc("conveyor.scene"), 900, extra=("--dt", dt))
        x = lambda f: r.frames[f]["hand"][0]  # noqa: E731
        rides[dt] = x(550) - x(150)
        report(not r.fatal and abs(rides[dt] / 400.0 - 1.0) <= 0.05 and abs(x(850) - x(700)) <= 0.01
               and r.stat("conveyor") == 1,
               "conveyor, dt %g: the glove rides %.3f units per frame, stops at x %.1f after the end (300); %s "
               "conveyor chunk" % (dt, rides[dt] / 400.0, x(850), r.stat("conveyor")))
    report(abs(rides[0.25] / rides[1.0] - 1.0) <= 0.01, "conveyor: the glove rides %.2f units at dt 0.25 and %.2f at "
           "dt 1" % (rides[0.25], rides[1.0]))
    # The same strip with conveyors along +z, -x and a diagonal, the glove placed where it rides the longest.
    for v, start in (((0, 0, 1), (0, -45)), ((-1, 0, 0), (250, 0)), ((0.6, 0, -0.8), (-250, 45))):
        d = OUT / "conveyor-dir.scene"
        d.write_text(conv.replace("conveyor 1 1 0 0", "conveyor 1 %g %g %g" % v)
                     .replace(place, "place 0 hand %g 40 %g ball 0 30 200 yaw 0 camera" % start))
        for dt in (1.0, 0.25):
            r = Run(exe, rom, d, 160, extra=("--dt", dt))
            p0, p1 = r.frames[120]["hand"], r.frames[150]["hand"]
            vel = [(p1[i] - p0[i]) / 30.0 for i in range(3)]
            report(not r.fatal and max(abs(vel[i] - v[i]) for i in range(3)) <= 0.01,
                   "conveyor %g %g %g, dt %g: the glove rides %.4f %.4f %.4f units per frame" % (*v, dt, *vel))
    on = OUT / "conveyor-ball.scene"
    on.write_text(conv.replace(place, "place 0 hand 0 40 200 ball -250 30 0 yaw 0 camera"))
    r = Run(exe, rom, on, 600)
    rate = (r.frames[399]["ball"][0] - r.frames[150]["ball"][0]) / 249.0
    report(not r.fatal and abs(rate - 1.42) <= 0.02 and r.frames[599]["ball"][0] > 300,
           "conveyor: the ball rides %.3f units per frame and leaves the strip at its end" % rate)
    under = OUT / "conveyor-under.scene"
    under.write_text(on.read_text() + "tri -600 -1 -600  -600 -1 600  600 -1 600  0\n"
                     "tri -600 -1 -600  600 -1 600  600 -1 -600  0\n")
    r = Run(exe, rom, under, 400)
    rate = (r.frames[399]["ball"][0] - r.frames[150]["ball"][0]) / 249.0
    report(not r.fatal and abs(rate - 1.0) <= 0.005,
           "conveyor: with a floor 1 unit under the strip the ball rides %.4f units per frame" % rate)
    across = OUT / "conveyor-across.scene"
    across.write_text(conv.replace(place, "place 0 hand 0 40 300 ball -100 9 -150 ballvel 0 0 20 yaw 0 camera"))
    r = Run(exe, rom, across, 300)
    start = next(f for f in range(300) if r.frames[f]["ball"][2] < -100)
    ys = [r.frames[f]["ball"][1] for f in range(start, 300)]
    zs = [r.frames[f]["ball"][2] for f in range(start, 300)]
    report(max(abs(y - 9.0) for y in ys) <= 0.05 and min(zs) < -50 and max(zs) > 50,
           "conveyor: a ball rolled across the strip keeps its height within %.4f" % max(abs(y - 9.0) for y in ys))

    # Conveyors on bodies, in the body's space: the ball rides the slider's conveyor along x while the slider moves
    # along z; the glove rides the turntable's along the turntable's own x, which turns with it.
    for dt in (1.0, 0.25):
        r = Run(exe, rom, sc("beltbody.scene"), 300, extra=("--dt", dt))
        # The landing gives the ball the conveyor's speed on top of the carry for a while (see above): from frame 130
        # on it rides at the conveyor's speed.
        bx = [r.frames[f]["ball"][0] - r.frames[130]["ball"][0] - 0.3 * (f - 130) for f in range(130, 200)]
        local = []
        for f in range(110, 220):
            p = r.frames[f]["hand"]
            t = 2.0 * math.pi * (f + dt) / 400.0
            dx, dz = p[0] - 200.0, p[2]
            local.append((dx * math.cos(t) - dz * math.sin(t), dx * math.sin(t) + dz * math.cos(t)))
        along = [lx - local[0][0] - 0.3 * k for k, (lx, lz) in enumerate(local)]
        side = [lz - local[0][1] for lx, lz in local]
        report(not r.fatal and max(bx) - min(bx) <= 0.1 and max(map(abs, along)) <= 0.3 and max(map(abs, side)) <= 1.0,
               "beltbody, dt %g: the ball rides the slider's conveyor within %.3f; on the turntable the glove rides "
               "0.3 per frame along its x within %.3f, and %.3f across" % (dt, max(bx) - min(bx),
                                                                          max(map(abs, along)), max(map(abs, side))))

    # The glove that walks on the ball (HAND_BALLWALK, the game's link without a hold) and is placed with the ball: each
    # goes to its place, and the glove lets go.
    walk = OUT / "ballwalk-place.scene"
    walk.write_text("\n".join(l for l in (HERE / "flat.scene").read_text().splitlines() if l.startswith("tri"))
                    + "\nplace 0 hand 0 60 0 ball 0 9 0 yaw 0 camera\nplace 300 hand -200 40 0 ball 200 9 0\n")
    r = Run(exe, rom, walk, 450)
    h, b = r.frames[449]["hand"], r.frames[449]["ball"]
    report(not r.fatal and r.bodies[299]["action"] == 20 and r.bodies[449]["held"] == 0 and abs(h[0] + 200) <= 0.01
           and abs(b[0] - 200) <= 0.01,
           "ballwalk place: action %d before, then the glove at x %.1f and the ball at x %.1f, held %d"
           % (r.bodies[299]["action"], h[0], b[0], r.bodies[449]["held"]))

    # A body removed under the glove: the glove drops to the floor.
    r = Run(exe, rom, sc("remove.scene"), 500)
    h = r.frames[499]["hand"]
    report(abs(h[1] - 13.0) <= 0.01, "remove: the glove stands on the floor at %.3f after the body goes" % h[1])

    # A reload of the level builds the world again.
    r = Run(exe, rom, sc("reload.scene"), 700)
    b = r.frames[699]["ball"]
    report(r.stat("rebuilds") == 1 and abs(b[1] - 9.0) <= 0.01,
           "reload: %s rebuild, and the ball rests at %.3f again" % (r.stat("rebuilds"), b[1]))
    r = Run(exe, rom, sc("respawn.scene"), 700)
    h = r.frames[699]["hand"]
    report(r.bodies[699]["restarts"] == r.bodies[499]["restarts"] + 1 and abs(h[0] + 300) <= 1 and abs(h[2] + 300) <= 1,
           "reload: the respawn stays; the restart after it puts the glove at %.1f %.1f" % (h[0], h[2]))

    # A drawing view moves nothing, but its far distance becomes the game's farPlaneDist (docs/host-world.md, "The
    # camera"). The game then updates another set of actors, which draw other random numbers: rseed may differ. At
    # the level's own far distance (1000 in level 12) the trace is the same. The mirror view changes nothing at all.
    base = Run(exe, rom, sc("flat.scene"), 300, extra=("--hash-every", 1))
    r = Run(exe, rom, sc("view.scene"), 300, extra=("--hash-every", 1))
    strip = lambda ls: [l.split(" hash")[0] for l in ls]  # noqa: E731
    no_seed = lambda ls: [re.sub(r" rseed \S+", "", l) for l in strip(ls)]  # noqa: E731
    report(no_seed(base.lines) == no_seed(r.lines) and base.lines != r.lines,
           "view: a drawing view gives the same positions and state but the random seed, and other memory")
    near = OUT / "view-level-far.scene"
    near.write_text(sc("view.scene").read_text().replace(" 10 4000\n", " 10 1000\n"))
    r = Run(exe, rom, near, 300, extra=("--hash-every", 1))
    report(strip(base.lines) == strip(r.lines),
           "view: at the level's own far distance a drawing view gives the same trace, the random seed too")
    for level in (5, 13, 26):
        p = Run(exe, rom, sc("mirror.scene"), 200, level=level, extra=("--hash-every", 1))
        q = subprocess.run([exe, "--rom", rom, "--headless", "200", "--level", str(level), "--trace", "--hash-every",
                            "1"], capture_output=True, text=True).stdout.splitlines()
        report([l for l in p.lines if l.startswith("frame")] == [l for l in q if l.startswith("frame")],
               "mirror view, level %d: the same trace and memory hash as no view" % level)

    # Steering: the stick up walks along the view.
    inp = OUT / "steer.input"
    inp.write_text("".join("%d:0:0:70\n" % f for f in range(150, 200)))
    r = Run(exe, rom, sc("steer.scene"), 220, extra=("--input", inp))
    p0, p1 = r.frames[149]["hand"], r.frames[219]["hand"]
    ang = math.degrees(math.atan2(p1[2] - p0[2], p1[0] - p0[0]))
    report(p1[0] - p0[0] > 20 and abs(ang) < 10, "steer: the glove walked %.1f units at %.1f degrees from +x" % (
        math.hypot(p1[0] - p0[0], p1[2] - p0[2]), ang))

    # A large generated terrain: 2 x 140 x 140 = 39,200 triangles.
    big = OUT / "big.scene"
    grid_scene(big, 140, 6000.0)
    plain = Run(exe, rom, sc("flat.scene"), 1000)
    r = Run(exe, rom, big, 1000)
    report(not r.fatal and r.stat("static") == 39200 and r.frames[999]["ball"][1] > -20,
           "big: %s triangles in %s chunks; 1000 frames in %.2f s (%.2f s with the flat floor)" % (
               r.stat("static"), r.stat("chunks"), r.seconds, plain.seconds))

    # The same traces as an earlier build. The water field of the bodies line is newer than some builds.
    if a.baseline_exe:
        def plain(lines):
            out = []
            for line in lines:
                w = line.split()
                if w[0] == "bodies" and "water" in w:
                    k = w.index("water")
                    del w[k:k + 3]
                out.append(" ".join(w))
            return out
        same, changed, new = [], [], []
        for name in sorted(p.name for p in HERE.glob("*.scene")):
            for dt in (1.0, 0.25):
                old = Run(a.baseline_exe, rom, sc(name), 400, extra=("--dt", dt))
                if old.code != 0 or not old.frames:
                    new.append(name)
                    break
                cur = Run(exe, rom, sc(name), 400, extra=("--dt", dt))
                if plain(cur.lines) != plain(old.lines):
                    changed.append("%s (dt %g)" % (name, dt))
                elif dt == 0.25:
                    same.append(name)
        expected = sorted("%s (dt %g)" % (n, dt) for n in CHANGED for dt in (1.0, 0.25))
        report(sorted(changed) == expected,
               "baseline: %d scenes give the same trace at dt 1 and 0.25; changed: %s (expected: %s); new: %s" % (
                   len(same), ", ".join(changed) or "none", ", ".join(sorted(CHANGED)), ", ".join(new) or "none"))

    # Every scene under the pointer check.
    if pathlib.Path(a.checked).exists():
        for name in sorted(p.name for p in HERE.glob("*.scene")):
            r = Run(a.checked, rom, sc(name), 400)
            report(not r.fatal and len(r.frames) == 400, "pointer check: %s" % name)
        # A turning body removed under the glove, and one freed by a reload of the level: its motion lies in its
        # block, which the game's KillPlatform must not free on its own.
        tt = (HERE / "turntable.scene").read_text()
        for name, extra in (("turntable-remove", "remove 250 1\n"), ("turntable-reload", "goto 250 12\n")):
            path = OUT / (name + ".scene")
            path.write_text(tt + extra)
            r = Run(a.checked, rom, path, 600)
            h = r.frames[599]["hand"]
            report(not r.fatal and len(r.frames) == 600 and abs(h[1] - 13.0) <= 0.01 if name.endswith("remove")
                   else not r.fatal and len(r.frames) == 600 and r.stat("rebuilds") == 1,
                   "pointer check: %s runs (the glove at height %.2f, %s rebuilds)" % (name, h[1], r.stat("rebuilds")))
        # The conveyor platforms freed under the glove: by a clear, and by a reload of the level.
        for name, extra in (("conveyor-clear", "clear 300\n"), ("conveyor-reload", "goto 300 12\n")):
            path = OUT / (name + ".scene")
            path.write_text(conv + extra)
            r = Run(a.checked, rom, path, 600)
            report(not r.fatal and len(r.frames) == 600 and (r.stat("rebuilds") == 1 if name.endswith("reload")
                                                              else r.stat("conveyor") == 0),
                   "pointer check: %s runs (%s rebuilds, %s conveyor platforms at the end)" % (
                       name, r.stat("rebuilds"), r.stat("conveyor")))
        # The turntable, a conveyor and a host water together in every level, with a reload of the level at frame 350:
        # FreeLevel2 then finds the conveyor platforms, the turning body and the host water in the game's lists.
        tris = [l for l in conv.splitlines() if l.startswith(("tri", "conveyor"))]
        body = [l for l in (HERE / "turntable.scene").read_text().splitlines()
                if not l.startswith(("#", "place"))][2:]
        body = [l.replace("body 1 0 40 0", "body 1 400 40 300") for l in body]
        bad = []
        for level in range(43):
            path = OUT / "all-three.scene"
            path.write_text("\n".join(tris + body + ["water -600 -100 200  400 120 400  20  0.1 0 0",
                                                     "place 0 hand -250 40 0 ball -400 60 400 camera",
                                                     "goto 350 %d" % level]) + "\n")
            r = Run(a.checked, rom, path, 600, level=level)
            if r.fatal or len(r.frames) != 600:
                bad.append(level)
        report(not bad, "pointer check: a turntable, a conveyor and a host water in levels 0 to 42, with a reload, "
               "run%s" % (" (not in levels %s)" % bad if bad else ""))
        # The switch in every level, under the pointer check.
        bad = []
        for level in range(43):
            r = Run(a.checked, rom, sc("off.scene"), 600, level=level)
            if r.fatal or len(r.frames) != 600 or r.stat("enemies") != 0 or r.stat("collectables") != 0:
                bad.append(level)
        report(not bad, "pointer check: off.scene in levels 0 to 42 runs, with nothing of the level left on%s" % (
            " (not in levels %s)" % bad if bad else ""))
    else:
        print("skip pointer check: no %s" % a.checked)

    print("hwtest: %d failed" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
