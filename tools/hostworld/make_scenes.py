#!/usr/bin/env python3
"""Writes the generated test scenes of this folder: seams.scene, terrains.scene, void.scene, off.scene, lift.scene,
slider.scene, remove.scene, turntable.scene, conveyor.scene, beltbody.scene and hostwater.scene
(docs/host-world.md). They are made from simple shapes here, not from
any game's data. Run it from anywhere; it writes next to itself."""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent


def fmt(p):
    return "%g %g %g" % p


def floor(size=600, y=0):
    s = size
    return [f"tri {-s} {y} {-s}  {-s} {y} {s}  {s} {y} {s}  0", f"tri {-s} {y} {-s}  {s} {y} {s}  {s} {y} {-s}  0"]


def box(hx, hy, hz):
    """The 12 triangles of a box around the origin, counter-clockwise seen from outside."""
    v = [(x, y, z) for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)]
    # v index: x bit 4, y bit 2, z bit 1.
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    out = []
    for a, b, c, d in quads:
        out.append("tri %s  %s  %s  0" % (fmt(v[a]), fmt(v[b]), fmt(v[c])))
        out.append("tri %s  %s  %s  0" % (fmt(v[a]), fmt(v[c]), fmt(v[d])))
    return out


def seams():
    n, s = 24, 32.0

    def h(i, j):
        x, z = i * s - n * s / 2, j * s - n * s / 2
        return round(max(0.0, 20.0 - abs(x + 100) * 0.2) + (8.0 if z > 128 and abs(x) < 200 else 0.0), 3)

    lines = ["# A grid of 24 x 24 squares of 32 units with flat parts, slopes and creases, from 0 to 768 in x and z",
             "# (make_scenes.py). The triangles are listed cell by cell for a cell of 64, the chunker's order: with",
             "# 'cell 1000000' the game gets the same faces in the same order in one chunk. The ball rolls across it.",
             "options cell 64 maxtris 4096"]
    for cj in range(n // 2):
        for ci in range(n // 2):
            for j in (2 * cj, 2 * cj + 1):
                for i in (2 * ci, 2 * ci + 1):
                    x0, z0 = i * s, j * s
                    a = (x0, h(i, j), z0)
                    b = (x0, h(i, j + 1), z0 + s)
                    c = (x0 + s, h(i + 1, j + 1), z0 + s)
                    d = (x0 + s, h(i + 1, j), z0)
                    lines.append("tri %s  %s  %s  0" % (fmt(a), fmt(b), fmt(c)))
                    lines.append("tri %s  %s  %s  0" % (fmt(a), fmt(c), fmt(d)))
    lines.append("place 0 hand 84 40 84 ball 700 40 700 ballvel -14 0 -14 camera")
    return lines


def terrains():
    """Five pads in a row along x, 400 wide and 1400 long along z: terrain 0, 8 (slippery), 12 (a water surface),
    9 (hurts the glove) and 6 (death). The ball rolls along each of the first three in turn; then the glove stands
    on the fourth, and the ball drops on the fifth."""
    kinds = [0, 8, 12, 9, 6]
    lines = ["# Pads of terrain 0, 8, 12, 9 and 6, centred at x = 0, 500, 1000, 1500 and 2000 (make_scenes.py).",
             "# The ball rolls along +z on the first three, from z = -150 at 8 units a frame. The glove then stands",
             "# on the hurting pad, and the ball drops on the death pad."]
    for k, t in enumerate(kinds):
        x0, x1, z0, z1 = 500 * k - 200, 500 * k + 200, -400, 1000
        lines.append(f"tri {x0} 0 {z0}  {x0} 0 {z1}  {x1} 0 {z1}  {t}")
        lines.append(f"tri {x0} 0 {z0}  {x1} 0 {z1}  {x1} 0 {z0}  {t}")
    lines += ["place 0 hand 100 40 -300 ball 0 40 -150 ballvel 0 0 8 camera",
              "place 200 ball 500 40 -150 ballvel 0 0 8",
              "place 400 ball 1000 40 -150 ballvel 0 0 8",
              "place 600 hand 1500 40 0 ball 1000 40 -300 camera",
              "place 700 hand 100 40 -300 ball 2000 40 0 camera"]
    return lines


def void():
    """A ledge and a pit 1500 deep whose floor is a void (flag 2), away from level 12's water. The ball, and later
    the glove, fall into it. The host places both back on the ledge."""
    lines = ["# A ledge from x = -2300 to -1700 at height 0, and a pit from x = -1700 to -900 whose floor, 1500 below,",
             "# is a void (make_scenes.py). The ball drops into the pit at frame 100 and the glove at frame 300. The",
             "# host places both back on the ledge at frame 500."]
    lines.append("tri -2300 0 -400  -2300 0 400  -1700 0 400  0")
    lines.append("tri -2300 0 -400  -1700 0 400  -1700 0 -400  0")
    lines.append("tri -1700 -1500 -400  -1700 -1500 400  -900 -1500 400  0 2")
    lines.append("tri -1700 -1500 -400  -900 -1500 400  -900 -1500 -400  0 2")
    lines += ["place 0 hand -2000 40 0 ball -2100 40 0 yaw 0 camera",
              "place 100 ball -1300 40 0",
              "place 300 hand -1300 40 100",
              "place 500 hand -2000 40 0 ball -2100 40 0 camera"]
    return lines


def off():
    """A floor and a death pad, with the level's enemies and collectables switched off (options off 3). The ball
    bursts on the pad, and the restart brings the enemies back for the switch to take out again."""
    lines = ["# A floor, and a death pad from x = 200 to 600 (make_scenes.py). The level's enemies, garibs and potions",
             "# are switched off. The ball drops on the pad at frame 200 and bursts; the glove restarts.",
             "options off 3",
             "tri -600 0 -600  -600 0 600  200 0 600  0",
             "tri -600 0 -600  200 0 600  200 0 -600  0",
             "tri 200 0 -600  200 0 600  600 0 600  6",
             "tri 200 0 -600  600 0 600  600 0 -600  6",
             "respawn 0 -300 20 -300 0",
             "place 0 hand 0 40 0 ball 0 40 60 yaw 0 camera",
             "place 200 ball 400 40 0"]
    return lines


def conveyor():
    """A floor of 50-unit squares, from -600 to 600 in x and z, with a conveyor strip from x = -300 to 300 and z = -50
    to 50 that moves what stands on it along +x at 1 unit per original frame. The strip crosses cells of the chunker:
    it is cut into several conveyor chunks."""
    lines = ["# A floor of 50-unit squares with a conveyor strip from x = -300 to 300, z = -50 to 50 (make_scenes.py).",
             "# Conveyor 1 moves what stands on it along +x at 1 unit per original frame. The glove stands on the",
             "# strip and rides it off its end; the ball rests beside it.",
             "conveyor 1 1 0 0"]
    for j in range(24):
        for i in range(24):
            x0, z0 = -600 + 50 * i, -600 + 50 * j
            belt = -300 <= x0 < 300 and -50 <= z0 < 50
            tail = "  0 0 1" if belt else "  0"
            lines.append(f"tri {x0} 0 {z0}  {x0} 0 {z0 + 50}  {x0 + 50} 0 {z0 + 50}{tail}")
            lines.append(f"tri {x0} 0 {z0}  {x0 + 50} 0 {z0 + 50}  {x0 + 50} 0 {z0}{tail}")
    lines.append("place 0 hand -250 40 0 ball 0 30 200 yaw 0 camera")
    return lines


def beltbody():
    """Two bodies with a conveyor of their own, in their own space: a slider that moves along z with a conveyor along
    its x, under the ball, and a turntable with a conveyor along its own x, under the glove."""
    lines = ["# A slider at x = -200 that moves 60 units to each side along z, once every 400 original frames, with a",
             "# conveyor of 0.3 along x; the ball rests on it. A turntable at x = 200 that turns once every 400",
             "# frames, with a conveyor of 0.3 along its own x; the glove stands on it (make_scenes.py)."]
    lines += floor()
    lines.append("body 1 -200 40 0  0 0 0 1  1 1 1")
    lines += box(40, 5, 40)
    lines.append("end")
    lines.append("body 2 200 40 0  0 0 0 1  1 1 1")
    lines += box(40, 5, 40)
    lines.append("end")
    lines += ["move 1 sine 400 0 0 60", "move 2 spin 400", "bodyconveyor 0 1 0.3 0 0", "bodyconveyor 0 2 0.3 0 0",
              "place 0 hand 0 13 -300 ball 0 9 -400 yaw 0 camera",
              "place 100 hand 200 60 0 ball -215 60 60"]
    return lines


def hostwater():
    """A floor at -200 under a host water whose surface is at -20. The glove swims in it, and the ball floats on it."""
    lines = ["# A floor at -200 under a host water from -600 to 600 in x and z, with its surface at -20",
             "# (make_scenes.py). The glove is placed in the water; the ball drops onto it and floats. The water goes at",
             "# frame 400: the ball sinks to the floor."]
    lines += floor(y=-200)
    lines += ["water -600 -200 -600  1200 180 1200  -20",
              "place 0 hand 0 -100 0 ball 200 40 0 yaw 0 camera",
              "water 400 off"]
    return lines


def body_scene(comment, pose, motion, place):
    lines = comment + floor()
    lines.append(f"body 1 {pose}")
    lines += box(40, 5, 40)
    lines.append("end")
    lines.append(motion)
    lines.append(place)
    return lines


def main():
    scenes = {
        "seams.scene": seams(),
        "terrains.scene": terrains(),
        "void.scene": void(),
        "off.scene": off(),
        "conveyor.scene": conveyor(),
        "beltbody.scene": beltbody(),
        "hostwater.scene": hostwater(),
        "lift.scene": body_scene(
            ["# A floor and a lift: a box 80 x 10 x 80 whose top rises and falls 30 units around height 45, once every",
             "# 120 original frames (make_scenes.py). The glove stands on it."],
            "0 40 0  0 0 0 1  1 1 1", "move 1 sine 120 0 30 0",
            "place 0 hand 0 60 0 ball 150 30 0 yaw 0 camera"),
        "slider.scene": body_scene(
            ["# A floor and a slider: a box 80 x 10 x 80 at height 40 that moves 120 units to each side along x,",
             "# once every 160 original frames (make_scenes.py). The glove stands on it."],
            "0 40 0  0 0 0 1  1 1 1", "move 1 sine 160 120 0 0",
            "place 0 hand 0 60 0 ball 0 30 150 yaw 0 camera"),
        "turntable.scene": body_scene(
            ["# A floor and a turntable: a box 80 x 10 x 80 at height 40 that turns about y once every 200 original",
             "# frames (make_scenes.py). The glove stands near its edge and turns with it."],
            "0 40 0  0 0 0 1  1 1 1", "move 1 spin 200",
            "place 0 hand 25 60 0 ball 0 30 150 yaw 0 camera"),
        "remove.scene": body_scene(
            ["# The slider, removed at frame 300 while the glove stands on it (make_scenes.py). The glove drops to the",
             "# floor."],
            "0 40 0  0 0 0 1  1 1 1", "move 1 sine 160 120 0 0",
            "place 0 hand 0 60 0 ball 0 30 150 yaw 0 camera") + ["remove 300 1"],
    }
    for name, lines in scenes.items():
        (HERE / name).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
