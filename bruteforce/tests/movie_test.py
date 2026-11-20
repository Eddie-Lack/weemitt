#!/usr/bin/env python3
"""Tests of the movie files and the converter.

    python3 tests/movie_test.py [--rom ROM] [--quick]

- The .bk2 and .m64 readers and writers: a written movie reads back the same, and a .bk2 input line has BizHawk's
  layout.
- The converter (tools/convert.py): a made-up BizHawk movie of random play from power-on converts to the engine's
  form, and the engine then gives the same state as mupen64plus at every compared read (to-run --check). New inputs
  for some reads go back into the movie (to-movie), and that movie converts and checks too. Needs mupen64plus and a
  display, as tests/sync.py does; --quick skips this part.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import movie as movies  # noqa: E402

LOGKEY = ('#Reset|Power|#P1 X Axis|P1 Y Axis|P1 DPad U|P1 DPad D|P1 DPad L|P1 DPad R|P1 Start|P1 Z|P1 B|P1 A|'
          'P1 C Up|P1 C Down|P1 C Left|P1 C Right|P1 L|P1 R|')


def make_bk2(path, frames):
    """A movie as BizHawk writes it for controller 1 of its Mupen64Plus core."""
    groups = movies.parse_logkey(LOGKEY)
    lines = ['[Input]', 'LogKey:' + LOGKEY]
    for buttons, x, y in frames:
        v = {name: bool(buttons & bit) for name, bit in movies.BK2_BIT.items()}
        v['P1 X Axis'] = x
        v['P1 Y Axis'] = y
        lines.append(movies.format_bk2_line(v, groups))
    lines.append('[/Input]')
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('Header.txt', 'MovieVersion BizHawk v2.0.0\nPlatform N64\nCore Mupen64Plus\nGameName Glover\n')
        z.writestr('Input Log.txt', '\n'.join(lines) + '\n')
        z.writestr('SyncSettings.json', '{}\n')
        z.writestr('Comments.txt', '')


def random_frames(seed, count):
    rnd = random.Random(seed)
    out = []
    while len(out) < count:
        x = rnd.choice([0, 0, 80, -80, 40, -40, 60])
        y = rnd.choice([0, 80, -80, 40, 60, -40])
        buttons = 0
        for bit, chance in ((0x8000, 0.3), (0x4000, 0.2), (0x2000, 0.15), (0x1000, 0.05), (0x0020, 0.05),
                            (0x0008, 0.05), (0x0002, 0.05)):
            if rnd.random() < chance:
                buttons |= bit
        out += [(buttons, x, y)] * rnd.randint(10, 120)
        out += [(0, x, y)] * rnd.randint(6, 20)
    return out[:count]


def check(name, ok, failed):
    print(('ok   ' if ok else 'FAIL ') + name)
    failed[0] += not ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.environ.get('GLOVER_ROM', 'glover.z64'))
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    failed = [0]
    tmp = tempfile.mkdtemp()

    # The formats.
    frames = random_frames(1, 500)
    path = os.path.join(tmp, 'a.bk2')
    make_bk2(path, frames)
    m = movies.read_movie(path)
    check('bk2 reads its frames', m.frames == frames, failed)
    line = movies.format_bk2_line({'P1 X Axis': -12, 'P1 Y Axis': 80, 'P1 A': True, 'P1 C Left': True,
                                   'P1 L': True}, movies.parse_logkey(LOGKEY))
    check('bk2 line layout', line == '|..|  -12,   80,.......A..l.l.|', failed)
    m.frames[10] = (0x8000 | 0x0001, 5, -5)
    out = os.path.join(tmp, 'b.bk2')
    movies.write_movie(m, out)
    m2 = movies.read_movie(out)
    check('bk2 writes and reads back', m2.frames == m.frames and m2.extra['logkey'] == LOGKEY, failed)
    with zipfile.ZipFile(out) as z:
        check('bk2 keeps the other files', sorted(z.namelist()) == sorted(['Header.txt', 'Input Log.txt',
                                                                           'SyncSettings.json', 'Comments.txt']),
              failed)
    # BizHawk 2.11.1's stick buttons: a pressed one replaces its axis with full deflection.
    key211 = ('#Reset|Power|#P1 X Axis|P1 Y Axis|P1 A Up|P1 A Down|P1 A Left|P1 A Right|P1 DPad U|P1 DPad D|'
              'P1 DPad L|P1 DPad R|P1 Start|P1 Z|P1 B|P1 A|P1 C Up|P1 C Down|P1 C Left|P1 C Right|P1 L|P1 R|')
    p211 = os.path.join(tmp, 'v211.bk2')
    with zipfile.ZipFile(p211, 'w') as z:
        z.writestr('Header.txt', 'MovieVersion BizHawk v2.0.0\r\nCore Mupen64Plus\r\n')
        z.writestr('Input Log.txt', '\r\n'.join([
            '[Input]', 'LogKey:' + key211,
            '|..|   10,   20,..L........A......|',
            '|..|   10,   20,U.LR..............|',
            '|..|  200,   -5,..................|', '[/Input]']) + '\r\n')
    m = movies.read_movie(p211)
    check('bk2 stick buttons', m.frames == [(0x8000, -128, 20), (0, -128, 127), (0, -56, -5)], failed)
    movies.write_movie(m, os.path.join(tmp, 'v211b.bk2'))
    check('bk2 stick buttons write back as axes',
          movies.read_movie(os.path.join(tmp, 'v211b.bk2')).frames == m.frames, failed)

    header = bytearray(0x400)
    header[0:4] = b'M64\x1a'
    header[4] = 3
    header[0x15] = 1
    mp = os.path.join(tmp, 'a.m64')
    open(mp, 'wb').write(bytes(header))
    m = movies.read_movie(mp)
    m.frames = frames[:100]
    m.extra['others'] = []
    movies.write_movie(m, mp)
    check('m64 writes and reads back', movies.read_movie(mp).frames == frames[:100], failed)

    if not args.quick:
        if not os.path.isfile(args.rom):
            sys.exit('movie_test.py: give the ROM with --rom or $GLOVER_ROM')
        conv = [sys.executable, os.path.join(ROOT, 'tools', 'convert.py')]
        bk2 = os.path.join(tmp, 'random.bk2')
        make_bk2(bk2, random_frames(2, 9000))
        run = os.path.join(tmp, 'run')
        p = subprocess.run(conv + ['to-run', bk2, '--rom', args.rom, '--out', run, '--check'],
                           capture_output=True, text=True)
        check('a BizHawk movie converts, and the engine matches the emulator', p.returncode == 0, failed)
        print('  ' + (p.stdout + p.stderr).strip().replace('\n', '\n  ')[-1500:])
        # New input for some reads, back into the movie, and the new movie checks too.
        new = os.path.join(tmp, 'new.txt')
        with open(new, 'w') as f:
            for k in range(1200, 1260):
                f.write('%d: 0 80 A\n' % k)
        bk2b = os.path.join(tmp, 'random2.bk2')
        p = subprocess.run(conv + ['to-movie', run, new, bk2b], capture_output=True, text=True)
        p2 = subprocess.run(conv + ['to-run', bk2b, '--rom', args.rom, '--out', os.path.join(tmp, 'run2'), '--check'],
                            capture_output=True, text=True)
        check('new inputs go back into the movie, and it matches too', p.returncode == 0 and p2.returncode == 0,
              failed)
        print('  ' + (p2.stdout + p2.stderr).strip().replace('\n', '\n  ')[-1500:])
    print('movie tests: all passed' if not failed[0] else 'movie tests: %d FAILED' % failed[0])
    return 1 if failed[0] else 0


sys.exit(main())
