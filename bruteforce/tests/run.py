#!/usr/bin/env python3
"""Runs the engine tests.

    python3 tests/run.py [--build build/linux] [--base build/linux] [--rom ROM] [--quick] [--wine]

The ROM is found as in the README (--rom, $GLOVER_ROM, or glover.z64 in the working directory). The tests:
- saveload: in a boss level, in Atlantis 1 and from power-on (src/saveload.c explains what it checks);
- states between processes: one process saves a state, another loads it, and both print the same hashes;
- the parts of a frame: a read, a draw and a logic frame (play --events) give the same states as whole frames;
- stops: a read and a draw at a stop inside a logic frame, and a stop that is not reached (src/run.h);
- a short search ends on time and writes its best inputs;
- the trimmed draw gives the same state trace as the full draw, frame by frame;
- the same game as the base build: when --build is not the base build, both builds play the same input and must
  print the same state trace for every frame;
- with --wine, the same for the Windows build (build/windows) under Wine, which needs a Windows build and Wine.
--quick plays shorter games.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUTTONS = [(0x8000, 'A'), (0x4000, 'B'), (0x2000, 'Z'), (0x0020, 'L'), (0x0010, 'R'),
           (0x0008, 'cu'), (0x0004, 'cd'), (0x0002, 'cl'), (0x0001, 'cr')]


def find_rom(arg):
    for path in (arg, os.environ.get('GLOVER_ROM'), 'glover.z64', os.path.join(ROOT, 'glover.z64')):
        if path and os.path.isfile(path):
            return path
    sys.exit('run.py: give the ROM with --rom or $GLOVER_ROM')


def run(cmd, wine=False):
    """Runs cmd and returns (exit status, standard output). A failure prints its standard error."""
    if wine:
        cmd = ['wine'] + cmd
    env = dict(os.environ, WINEDEBUG='-all')
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if p.returncode != 0:
        # A negative status is the signal that ended the program (-11 for SIGSEGV).
        print(f'  {" ".join(cmd)}: exit status {p.returncode}')
        if p.stderr.strip():
            print('  ' + p.stderr.strip()[-2000:].replace('\n', '\n  '))
    return p.returncode, p.stdout.replace('\r', '')


def input_lines(seed, count):
    """Generated play-like input in the engine's text format: stretches of stick and buttons, released between."""
    rnd = random.Random(seed)
    lines = []
    while len(lines) < count:
        x = rnd.choice([0, 0, 80, -80, 40, -40, 25, 60])
        y = rnd.choice([0, 80, 80, -80, 40, 60, -40])
        names = [name for bit, name in BUTTONS if rnd.random() < 0.15]
        lines += ['%d %d %s' % (x, y, ' '.join(names))] * rnd.randint(8, 50)
        lines += ['%d %d' % (x, y)] * rnd.randint(2, 6)
    return lines[:count]


def report(ok, name, failed):
    print(f"{'ok  ' if ok else 'FAIL'} {name}")
    failed[0] += not ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux'))
    ap.add_argument('--base', default=os.path.join(ROOT, 'build', 'linux'))
    ap.add_argument('--rom')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--wine', action='store_true')
    args = ap.parse_args()
    rom = find_rom(args.rom)
    builds = [(args.build, '', False)]
    if args.wine:
        builds.append((os.path.join(ROOT, 'build', 'windows'), '.exe', True))
    failed = [0]
    tmp = tempfile.mkdtemp()
    frames = 300 if args.quick else 600
    inputs = os.path.join(tmp, 'input.txt')
    with open(inputs, 'w') as f:
        f.write('\n'.join(input_lines(1, 3000)) + '\n')
    base_traces = {}
    for build, ext, wine in builds:
        exe = os.path.join(build, 'glover' + ext)
        name = os.path.basename(build)
        cases = [('boss level', ['--level', '13', '--frames', str(frames)]),
                 ('Atlantis 1', ['--level', '10', '--frames', str(frames)]),
                 ('power-on', ['--frames', '600' if args.quick else '3000'])]
        for what, extra in cases:
            code, out = run([exe, 'saveload', '--rom', rom] + extra, wine)
            ok = code == 0 and 'saveload: ok' in out
            report(ok, f'{name} saveload, {what}', failed)
            if not ok:
                print(out)
        state = os.path.join(tmp, 'state.bin')
        base = [exe, 'saveload', '--rom', rom, '--level', '13', '--frames', '400']
        code1, out1 = run(base + ['--save', state, '--at', '200'], wine)
        code2, out2 = run(base + ['--load', state], wine)
        ok = code1 == 0 and code2 == 0 and out1 == out2 and out1.count('hash') == 20
        report(ok, f'{name} a state carries over to another process', failed)
        if not ok:
            print(out1, out2)

        # The parts of a frame give the same states as whole frames.
        n = 400 if args.quick else 1500
        script = os.path.join(tmp, 'parts.events')
        with open(script, 'w') as f:
            for line in input_lines(1, n):
                f.write('R %s\nD\nL\n' % line)
        code1, whole = run([exe, 'play', '--rom', rom, '--input', inputs, '--frames', str(n), '--trace'], wine)
        code2, parts = run([exe, 'play', '--rom', rom, '--events', script, '--trace'], wine)
        report(code1 == 0 and code2 == 0 and whole == parts and whole.count('\n') == n + 1,
               f'{name} a read, a draw and a logic frame equal a whole frame', failed)

        # A read and a draw at a stop at the start of MainGameLoop (a group, src/run.h) give the same states as the
        # same read and draw just before the frame. The game is in a level from about frame 200 of this input.
        lines = input_lines(1, 400)
        stop_script = os.path.join(tmp, 'stop.events')
        before_script = os.path.join(tmp, 'before.events')
        with open(stop_script, 'w') as fs, open(before_script, 'w') as fb:
            for i, line in enumerate(lines):
                if i == 301:
                    continue
                fs.write('R %s\nD\nL\n' % line)
                fb.write('R %s\nD\n' % line + ('R %s\nD\nL\n' % lines[301] if i == 300 else '') + 'L\n')
                if i == 300:
                    fs.write('@ UpdateAmbientSounds\nR %s\nD\n@end\nL\n' % lines[301])
        code1, at_stop = run([exe, 'play', '--rom', rom, '--events', stop_script, '--trace'], wine)
        code2, before = run([exe, 'play', '--rom', rom, '--events', before_script, '--trace'], wine)
        report(code1 == 0 and code2 == 0 and at_stop == before and at_stop.count('\n') == 401,
               f'{name} a read and a draw at a stop inside a logic frame', failed)
        # A short search on the read, draw and logic script: it must end on time and write its best inputs.
        config = os.path.join(tmp, 'search.json')
        with open(config, 'w') as f:
            json.dump({'rom': os.path.abspath(rom), 'run': 'stop.events', 'window': [250, 270], 'lookahead': 30,
                       'progress': [{'value': 'hand+0x34:f32', 'weight': 1}], 'out': 'search-out',
                       'seconds': 3, 'workers': 2, 'buttons': []}, f)
        code, out = run([exe, 'search', config], wine)
        best = os.path.join(tmp, 'search-out', 'best.txt')
        ok = code == 0 and os.path.exists(best) and 'the plays end at read 300' in open(best).readline()
        report(ok, f'{name} a short search ends and writes its best inputs', failed)
        if not ok:
            print(out[-2000:])

        with open(stop_script, 'w') as fs:
            fs.write('R 0 0\nD\nL\n@ UpdateActor=0x1\nR 0 0\n@end\n')
        p = subprocess.run((['wine'] if wine else []) + [exe, 'play', '--rom', rom, '--events', stop_script],
                           capture_output=True, text=True, env=dict(os.environ, WINEDEBUG='-all'))
        report(p.returncode == 0 and 'did not reach' in p.stderr, f'{name} a stop that is not reached', failed)

        # The trimmed draw (engine_trim_draw) gives the same game as the full draw: the state trace of every frame,
        # from power-on and in the boss level, and in a script of reads, draws and logic frames.
        n = 600 if args.quick else 3000
        for what, extra in [('power-on', ['--input', inputs, '--frames', str(n)]),
                            ('boss level', ['--input', inputs, '--frames', str(n), '--level', '13']),
                            ('parts of frames', ['--events', script])]:
            code1, full = run([exe, 'play', '--rom', rom, '--trace'] + extra, wine)
            code2, trimmed = run([exe, 'play', '--rom', rom, '--trace', '--trim-draw'] + extra, wine)
            report(code1 == 0 and code2 == 0 and full == trimmed and full.count('\n') > 100,
                   f'{name} the trimmed draw plays the same game, {what}', failed)

        # The same game as the base build: the state trace of every frame.
        n = 600 if args.quick else 3000
        runs = {'power-on': [], 'boss level': ['--level', '13']}
        for what, extra in runs.items():
            code, trace = run([exe, 'play', '--rom', rom, '--input', inputs, '--frames', str(n), '--trace'] + extra,
                              wine)
            if os.path.abspath(build) == os.path.abspath(args.base):
                base_traces[what] = trace
            else:
                if what not in base_traces:
                    bexe = os.path.join(args.base, 'glover')
                    _, base_traces[what] = run([bexe, 'play', '--rom', rom, '--input', inputs, '--frames', str(n),
                                                '--trace'] + extra)
                same = code == 0 and trace == base_traces[what] and trace.count('\n') == n + 1
                report(same, f'{name} plays the same game as {os.path.basename(args.base)}, {what}', failed)
                if not same:
                    a, b = trace.splitlines(), base_traces[what].splitlines()
                    for i, (x, y) in enumerate(zip(a, b)):
                        if x != y:
                            print(f'  first difference, frame {i}:\n    {x}\n    {y}')
                            break
    print('all tests passed' if not failed[0] else f'{failed[0]} tests FAILED')
    return 1 if failed[0] else 0



if __name__ == '__main__':
    sys.exit(main())
