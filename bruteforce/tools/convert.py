#!/usr/bin/env python3
"""Converts a TAS movie into the engine's form, and new inputs back into a movie.

    python3 tools/convert.py to-run MOVIE --rom ROM --out RUN [--check] [--writes FILE]
    python3 tools/convert.py to-movie RUN INPUTS OUT

to-run plays the movie (.bk2 or .m64) once in mupen64plus from power-on and writes the folder RUN:
- run.events: the events script for the engine (glover play --events, glover search): the reads of the controller
  with the movie's input, the draws and the logic frames in the order the game ran them (docs/sync.md);
- run.json: for each read, the movie frame it came in (a video frame of a .bk2, an input sample of an .m64), and
  whether the game thread waited between frames there; the movie's path and kind.
--check also plays run.events in the engine and compares the game state at every such read with the emulator's.

to-movie takes INPUTS (the engine's text format, one line per read, numbered by read; a window of reads is fine)
and writes OUT: the base movie with those reads' inputs. A read's input fills every movie frame from the one after
the previous read up to its own, so a read that comes a few frames earlier still gets it.

A BizHawk movie's frame is a video frame. BizHawk's core runs two video frames before the movie's first frame, so
the movie's frame f is the input of a read after f + 2 vertical interrupts (tools/sync/vicount.c counts them).

A run holds only inputs and timing, no game data.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sync'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))

import movie as movies  # noqa: E402

BUTTONS = [(0x8000, 'A'), (0x4000, 'B'), (0x2000, 'Z'), (0x1000, 'S'), (0x0020, 'L'), (0x0010, 'R'),
           (0x0800, 'du'), (0x0400, 'dd'), (0x0200, 'dl'), (0x0100, 'dr'),
           (0x0008, 'cu'), (0x0004, 'cd'), (0x0002, 'cl'), (0x0001, 'cr')]
NAMES = {name.lower(): bit for bit, name in BUTTONS}


def fmt_input(inp):
    buttons, x, y = inp
    return ' '.join([str(x), str(y)] + [name for bit, name in BUTTONS if buttons & bit])


def parse_inputs(path):
    """The engine's text format as {read: (buttons, x, y)}."""
    out = {}
    nxt = 0
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        if ':' in line:
            num, line = line.split(':', 1)
            nxt = int(num)
        parts = line.split()
        buttons = 0
        for name in parts[2:]:
            buttons |= NAMES[name.lower()]
        out[nxt] = (buttons, int(parts[0]), int(parts[1]))
        nxt += 1
    return out


def ranges(numbers):
    """Sorted numbers as a list of (first, last) stretches."""
    out = []
    for n in numbers:
        if out and out[-1][1] == n - 1:
            out[-1] = (out[-1][0], n)
        else:
            out.append((n, n))
    return out


def movie_to_run(movie_path, rom, out, build, writes=None, extra=(), check=False):
    """Plays a movie in mupen64plus and writes the run folder `out`. writes: {read: [(watch text, value)]}, memory
    writes before those reads (for tests). extra: more values to record at each read, as (watch text, type).
    Returns (run info, the emulator's rows, for each read of the controller thread its row index, the extra values
    of each such read)."""
    import emurun
    import events
    import sync

    writes = writes or {}
    syms = sync.load_symbols(build)
    m = movies.read_movie(movie_path)
    by_vi = m.kind == 'bk2'
    # A .bk2 gives one input per video frame: the run goes on until the movie's last frame. Its frame f is the input
    # of the reads after f + first video frames (emurun.BIZHAWK_FIRST_VI). An .m64 gives one input per read.
    first = emurun.BIZHAWK_FIRST_VI if by_vi else 0
    reads = len(m.frames) if not by_vi else len(m.frames) // 2 + 10
    columns, records, vis = sync.emulator_log(rom, m.frames, reads, writes, syms, extra=extra, by_vi=by_vi,
                                              max_vi=len(m.frames) + first if by_vi else None)
    rows = sync.decode_rows(columns, records, syms)
    if by_vi:
        rows = [row for row, vi in zip(rows, vis) if vi - first < len(m.frames)]
        frames = [vi - first for vi in vis[:len(rows)]]
        inputs = [m.frames[f] if f >= 0 else (0, 0, 0) for f in frames]
    else:
        frames = list(range(len(rows)))
        inputs = [m.frames[k] for k in frames]
    stops = sync.find_stops(rows, rom)
    lines, read_idx, tops = events.derive(rows, syms, inputs, writes, fmt_input, stops)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'run.events'), 'w') as f:
        f.write('# The events of %s (tools/convert.py, emulator %s)\n' % (os.path.basename(movie_path),
                                                                         emurun.EMULATOR))
        f.write('#eeprom-fill 0x%02x\n' % emurun.EEPROM_FILL)
        f.write('\n'.join(lines) + '\n')
    info = {
        'movie': os.path.abspath(movie_path), 'kind': m.kind,
        'writes': {str(k): v for k, v in writes.items()},
        'reads': [{'frame': frames[k], 'top': top} for k, top in zip(read_idx, tops)],
    }
    with open(os.path.join(out, 'run.json'), 'w') as f:
        json.dump(info, f)
    print('to-run: %d movie frames, %d reads of the controller thread, %d where the game thread waited between '
          'frames' % (len(m.frames), len(read_idx), sum(tops)))
    busy = ranges([i for i, top in enumerate(tops) if not top])
    if busy:
        shown = ', '.join('%d' % a if a == b else '%d-%d' % (a, b) for a, b in busy[:30])
        print('to-run: reads in the middle of a frame (loads and lag frames): %s%s' % (
            shown, ' and %d more stretches' % (len(busy) - 30) if len(busy) > 30 else ''))
        print('to-run: %d of these reads run at a stop inside the engine\'s logic frame (docs/sync.md)' % len(stops))
    if check:
        out_p = subprocess.run([os.path.join(build, 'glover'), 'play', '--rom', rom, '--events',
                                os.path.join(out, 'run.events'), '--raw', '--watch',
                                ','.join('%s:%s' % c for c in sync.COMPARE)], capture_output=True, text=True)
        if out_p.returncode != 0:
            sys.exit('to-run: the engine failed: ' + out_p.stderr.strip())
        eng = [[int(v, 16) for v in line.split(':', 1)[1].split()] for line in out_p.stdout.splitlines()]
        for i, k in enumerate(read_idx):
            if tops[i] and eng[i] != rows[k]['compare']:
                diff = [sync.COMPARE[j][0] for j in range(len(sync.COMPARE)) if eng[i][j] != rows[k]['compare'][j]]
                sys.exit('to-run: the engine differs from the emulator at read %d (movie frame %d): %s' % (
                    i, info['reads'][i]['frame'], ', '.join(diff)))
        print('to-run: the engine gives the same state as the emulator at every compared read')
    extra_values = [[rows[k]['%s:%s' % w] for w in extra] for k in read_idx]
    return info, rows, read_idx, extra_values


def to_run(args):
    writes = {}
    if args.writes:
        with open(args.writes) as f:
            writes = {int(k): [tuple(w) for w in v] for k, v in json.load(f).items()}
    movie_to_run(args.movie, args.rom, args.out, args.build, writes, check=args.check)


def to_movie(args):
    with open(os.path.join(args.run, 'run.json')) as f:
        info = json.load(f)
    m = movies.read_movie(info['movie'])
    new = parse_inputs(args.inputs)
    frames = [r['frame'] for r in info['reads']]
    for k, inp in sorted(new.items()):
        if k >= len(frames):
            sys.exit('to-movie: read %d is past the end of the run (%d reads)' % (k, len(frames)))
        start = frames[k - 1] + 1 if k > 0 else frames[k]
        for f in range(max(0, start), frames[k] + 1):
            while f >= len(m.frames):
                m.frames.append((0, 0, 0))
            m.frames[f] = inp
    movies.write_movie(m, args.out)
    print('to-movie: %d reads written to %s' % (len(new), args.out))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('to-run')
    a.add_argument('movie')
    a.add_argument('--rom', default=os.environ.get('GLOVER_ROM', 'glover.z64'))
    a.add_argument('--out', required=True)
    a.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux'))
    a.add_argument('--check', action='store_true')
    a.add_argument('--writes', help='memory writes before some reads, for tests: {"READ": [["NAME:TYPE", VALUE]]}')
    b = sub.add_parser('to-movie')
    b.add_argument('run')
    b.add_argument('inputs')
    b.add_argument('out')
    args = ap.parse_args()
    if args.cmd == 'to-run':
        to_run(args)
    else:
        to_movie(args)


if __name__ == '__main__':
    main()
