#!/usr/bin/env python3
"""End-to-end test of the search: a movie, its run, a short search and the emulator check of its result.

    python3 tests/search_test.py [--rom ROM] [--seconds S]

1. An .m64 movie: the boss-atlantis scenario of tests/sync.py (the level select, then generated play).
2. tools/convert.py to-run --check: the run, and the engine matches the emulator.
3. glover search: a short search that brings the glove's X position above 0 sooner.
4. tools/check.py: the best inputs, played in mupen64plus as a movie, sync with the engine and reach the goal at
   the read the search gave.

Needs mupen64plus and a display, as tests/sync.py does.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))

import movie as movies  # noqa: E402
import sync  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.environ.get('GLOVER_ROM', 'glover.z64'))
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux'))
    ap.add_argument('--seconds', type=int, default=30)
    args = ap.parse_args()
    if not os.path.isfile(args.rom):
        sys.exit('search_test.py: give the ROM with --rom or $GLOVER_ROM')
    rom = os.path.abspath(args.rom)
    args.build = os.path.abspath(args.build)
    tmp = tempfile.mkdtemp()

    # 1. The movie and the writes that open the level select.
    syms = sync.load_symbols(args.build)
    inputs, writes = sync.scenario('boss-atlantis', 2200, syms)
    header = bytearray(0x400)
    header[0:4] = b'M64\x1a'
    header[4] = 3
    header[0x15] = 1
    m = movies.Movie('m64')
    m.extra = {'header': header, 'controllers': 1, 'others': []}
    m.frames = list(inputs)
    movie = os.path.join(tmp, 'base.m64')
    movies.write_movie(m, movie)
    with open(os.path.join(tmp, 'writes.json'), 'w') as f:
        json.dump({str(k): v for k, v in writes.items()}, f)

    # 2. The run.
    run = os.path.join(tmp, 'run')
    p = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'convert.py'), 'to-run', movie, '--rom', rom,
                        '--out', run, '--check', '--writes', os.path.join(tmp, 'writes.json'),
                        '--build', args.build], capture_output=True, text=True)
    print((p.stdout + p.stderr).strip())
    if p.returncode != 0:
        print('search test: FAILED (to-run)')
        return 1

    # 3. The search. In the base run the glove's X position first goes above 0 near read 1600.
    cfg = os.path.join(tmp, 'search.json')
    with open(cfg, 'w') as f:
        json.dump({'rom': rom, 'run': 'run/run.events', 'window': [1480, 1580], 'lookahead': 400,
                   'goal': 'hand+0x34:f32 > 0', 'out': 'out', 'seconds': args.seconds, 'workers': 2}, f)
    p = subprocess.run([os.path.join(args.build, 'glover'), 'search', cfg], capture_output=True, text=True,
                       cwd=tmp)
    print('\n'.join((p.stdout + p.stderr).strip().splitlines()[-5:]))
    if p.returncode != 0 or not os.path.exists(os.path.join(tmp, 'out', 'best.txt')):
        print('search test: FAILED (search)')
        return 1

    # 4. The check in the emulator.
    p = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'check.py'), cfg, '--build', args.build],
                       capture_output=True, text=True)
    print((p.stdout + p.stderr).strip())
    print('search test: passed' if p.returncode == 0 else 'search test: FAILED (check)')
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
