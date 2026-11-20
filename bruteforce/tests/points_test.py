#!/usr/bin/env python3
"""Test of a run of whole frames from a saved state, and of a search for points (the studio's hand-off).

    python3 tests/points_test.py [--rom ROM] [--build build/linux] [--wine] [--seconds S]

1. glover saveload saves the state at frame 500 of the boss level, after its opening.
2. A run of 300 whole frames ("F" lines) of generated input starts from that state ("#state").
3. glover play plays it and gives the glove's position before every read. The point is the glove's place before
   read 150, a sphere of radius 30.
4. glover search looks for inputs that reach the point sooner. It must reach it, no later than the base run does.
5. The best inputs, played again in a run of their own, reach the point at the read that the search gave, and not
   before.
6. The same search with the strict limits ("limits": "strict"): its best inputs break the limits no more often
   than the base run's inputs do, and they reach the point no later than the base run.
7. A run that quits to the hub from the pause menu, and a search whose goal holds after the load: its score counts
   one load, at 70 game frames ("loadFrames").
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tests'))

import run as engine_tests  # noqa: E402

RADIUS = 30.0
POINT_READ = 150


def positions(exe, rom, script, wine):
    code, out = engine_tests.run([exe, 'play', '--rom', rom, '--events', script, '--watch',
                                  'hand+0x34:f32,hand+0x38:f32,hand+0x3c:f32'], wine)
    if code != 0:
        sys.exit('points test: play failed:\n' + out[-2000:])
    pos = {}
    for line in out.splitlines():
        m = re.match(r'(\d+): (\S+) (\S+) (\S+)', line)
        if m:
            pos[int(m.group(1))] = tuple(float(m.group(i).split('/')[0]) for i in (2, 3, 4))
    return pos


def first_reach(pos, point):
    for k in sorted(pos):
        if math.dist(pos[k], point) <= RADIUS:
            return k
    return None


STRICT = {'minHold': 3, 'minRelease': 3, 'stickStep': 48}
SEARCH_BUTTONS = ['A', 'B', 'Z']


def violations(lines):
    """The search's count of limit breaks (src/search.c, violations) for inputs in the text format."""
    frames = []
    for line in lines:
        parts = line.split()
        frames.append((int(parts[0]), int(parts[1]), {p.upper() for p in parts[2:]}))
    count = 0
    n = len(frames)
    for name in SEARCH_BUTTONS:
        start = 0
        for k in range(1, n + 1):
            if k == n or ((name in frames[k][2]) != (name in frames[k - 1][2])):
                pressed = name in frames[start][2]
                if start > 0 and k < n and k - start < (STRICT['minHold'] if pressed else STRICT['minRelease']):
                    count += 1
                start = k
    for k in range(1, n):
        count += (abs(frames[k][0] - frames[k - 1][0]) > STRICT['stickStep']
                  or abs(frames[k][1] - frames[k - 1][1]) > STRICT['stickStep'])
    return count


def search(exe, rom, tmp, point, lines, seconds, wine, limits):
    """A search for the point; returns the read it reaches the point at and all the inputs it plays."""
    out = 'out-' + limits
    config = os.path.join(tmp, 'points-%s.json' % limits)
    with open(config, 'w') as f:
        json.dump({'rom': os.path.abspath(rom), 'run': 'base.events', 'window': [0, 140], 'lookahead': 160,
                   'points': [{'place': list(point), 'radius': RADIUS, 'of': 'glove'}], 'out': out,
                   'seconds': seconds, 'workers': 2, 'buttons': SEARCH_BUTTONS, 'limits': limits}, f)
    code, text = engine_tests.run([exe, 'search', config], wine)
    best = os.path.join(tmp, out, 'best.txt')
    if code != 0 or not os.path.exists(best):
        sys.exit('points test: the search failed:\n' + text[-2000:])
    text = open(best).read()
    m = re.search(r'goal at read (\d+)', text.splitlines()[0])
    if m is None:
        sys.exit('points test: FAILED: the search did not reach the point: ' + text.splitlines()[0])
    found = {}
    for line in text.splitlines():
        m2 = re.match(r'(\d+): (.*)', line)
        if m2:
            found[int(m2.group(1))] = m2.group(2)
    return int(m.group(1)), [found.get(k, lines[k]) for k in range(len(lines))]


def load_check(exe, rom, tmp, wine):
    """Step 7: the boss level's pause menu has Exit to the hub as its third item; A twice says yes."""
    lines = (['0 0'] * 20 + ['0 0 S'] * 2 + ['0 0'] * 30 + (['0 -80'] * 3 + ['0 0'] * 3) * 3 + ['0 0 A'] * 2 +
             ['0 0'] * 10 + ['0 0 A'] * 2 + ['0 0'] * 300)
    with open(os.path.join(tmp, 'quit.events'), 'w') as f:
        f.write('#state start.state\n' + ''.join('F %s\n' % line for line in lines))
    config = os.path.join(tmp, 'load.json')
    with open(config, 'w') as f:
        json.dump({'rom': os.path.abspath(rom), 'run': 'quit.events', 'window': [0, 15], 'lookahead': 350,
                   'goal': 'frameCount:u32 >= 50 and frameCount:u32 < 100', 'out': 'out-load', 'seconds': 2,
                   'workers': 1, 'buttons': SEARCH_BUTTONS}, f)
    code, text = engine_tests.run([exe, 'search', config], wine)
    best = os.path.join(tmp, 'out-load', 'best.txt')
    if code != 0 or not os.path.exists(best):
        sys.exit('points test: the load search failed:\n' + text[-2000:])
    first = open(best).read().splitlines()[0]
    m = re.search(r'goal at read (\d+) after (\d+) loads \((\d+) game frames', first)
    ok = m is not None and int(m.group(2)) == 1 and int(m.group(3)) == int(m.group(1)) + 70
    print('points test: through a load: ' + (first[2:] if m else 'FAILED: ' + first))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom')
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux'))
    ap.add_argument('--wine', action='store_true')
    ap.add_argument('--seconds', type=float, default=8)
    args = ap.parse_args()
    rom = engine_tests.find_rom(args.rom)
    exe = os.path.join(args.build, 'glover' + ('.exe' if args.wine else ''))
    tmp = tempfile.mkdtemp()
    state = os.path.join(tmp, 'start.state')
    code, out = engine_tests.run([exe, 'saveload', '--rom', rom, '--level', '13', '--frames', '500', '--save', state,
                                  '--at', '500'], args.wine)
    if code != 0 or not os.path.exists(state):
        sys.exit('points test: saveload --save failed:\n' + out[-2000:])
    lines = engine_tests.input_lines(3, 300)
    base = os.path.join(tmp, 'base.events')
    with open(base, 'w') as f:
        f.write('#state start.state\n' + ''.join('F %s\n' % line for line in lines))
    pos = positions(exe, rom, base, args.wine)
    point = pos[POINT_READ]
    base_reach = first_reach(pos, point)
    print('points test: the base run reaches the point before read %d' % base_reach)

    goal, played = search(exe, rom, tmp, point, lines, args.seconds, args.wine, 'none')
    replay = os.path.join(tmp, 'best.events')
    with open(replay, 'w') as f:
        f.write('#state start.state\n' + ''.join('F %s\n' % line for line in played))
    reach = first_reach(positions(exe, rom, replay, args.wine), point)
    ok = goal <= base_reach and reach == goal + 1
    print('points test: the search reached the point at read %d (the base run: %d); its inputs played again reach '
          'it before read %s' % (goal, base_reach - 1, reach))
    window = 140 + 160
    strictGoal, strictPlayed = search(exe, rom, tmp, point, lines, args.seconds, args.wine, 'strict')
    before, after = violations(lines[:window]), violations(strictPlayed[:window])
    print('points test: with the strict limits: the point at read %d; limit breaks %d (the base run: %d)'
          % (strictGoal, after, before))
    ok = ok and strictGoal <= base_reach and after <= before
    ok = load_check(exe, rom, tmp, args.wine) and ok
    print('points test: ' + ('passed' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
