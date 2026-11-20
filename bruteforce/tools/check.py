#!/usr/bin/env python3
"""Checks a search result in the real game: plays it in mupen64plus and confirms the score.

    python3 tools/check.py CONFIG [--rom ROM] [--best FILE] [--movie OUT]

CONFIG is the search's config. Its "run" must be the run.events of a run folder that tools/convert.py made. The check:
1. writes the base movie with the inputs of FILE (default: best.txt in the config's "out" folder) as a new movie, OUT
   (default: the "out" folder's best.bk2 or best.m64);
2. plays the new movie in mupen64plus from power-on and converts it (convert.py to-run --check): the engine must give
   the same state as the emulator at every read where the game thread waits between frames;
3. evaluates the config's goal in the emulator at every read where the game thread waits between frames. The
   search says the goal holds after the frames of read F, so in the emulator it must first hold at the next such
   read. Without a goal, or when the search did not reach it, it compares the progress score at the read where the
   search's plays end.

The search plays its changed inputs with the base movie's timing. Other inputs can change the timing: a lag frame
can come or go. The check then shows the score the emulator gives.

Needs mupen64plus and a display, as tests/sync.py does.
"""
import argparse
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sync'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))

import convert  # noqa: E402
import movie as movies  # noqa: E402

OPS = {
    '==': lambda v, n, bits: v == n, '!=': lambda v, n, bits: v != n,
    '<': lambda v, n, bits: v < n, '<=': lambda v, n, bits: v <= n,
    '>': lambda v, n, bits: v > n, '>=': lambda v, n, bits: v >= n,
    '&': lambda v, n, bits: (bits & int(n)) != 0,
}


def load_config(path):
    """The search's config. Like the search, it takes // comments."""
    text = open(path).read()
    text = re.sub(r'("(?:[^"\\]|\\.)*")|//[^\n]*', lambda m: m.group(1) or '', text)
    return json.loads(text)


def parse_cond(text):
    """A condition as the search writes it (src/cond.h): a list of "or" groups, each a list of "and" terms
    (value, type, op, number)."""
    toks = text.split()
    groups = [[]]
    i = 0
    while i < len(toks):
        if groups[0] or len(groups) > 1:
            if toks[i] == 'or':
                groups.append([])
            elif toks[i] != 'and':
                raise ValueError('%s: expected "and" or "or" before %s' % (text, toks[i]))
            i += 1
        if i + 3 > len(toks):
            raise ValueError('%s: a term is VALUE OP NUMBER' % text)
        value, op, num = toks[i:i + 3]
        name, _, typ = value.rpartition(':')
        if op not in OPS or not name:
            raise ValueError('%s: cannot read the term %s %s %s' % (text, value, op, num))
        sign = -1 if num.startswith('-') else 1
        digits = num.lstrip('+-')
        number = sign * int(digits, 16) if digits.lower().startswith('0x') else float(num)
        groups[-1].append((name, typ, op, number))
        i += 3
    return groups


def term_values(groups):
    """The values the condition reads, as convert.movie_to_run's extra watches."""
    out = []
    for group in groups:
        for name, typ, _, _ in group:
            if (name, typ) not in out:
                out.append((name, typ))
    return out


def as_number(bits, typ):
    if typ == 'f32':
        return struct.unpack('>f', struct.pack('>I', bits & 0xFFFFFFFF))[0]
    if typ.startswith('s') and bits & 0x80000000:
        return bits - 0x100000000
    return bits


def cond_holds(groups, values):
    """values: {(name, type): bits, sign-extended for the signed types}."""
    for group in groups:
        if all(OPS[op](as_number(values[(name, typ)], typ), number, values[(name, typ)])
               for name, typ, op, number in group):
            return True
    return False


def best_header(best_path):
    """From best.txt's first line: the read at which the search says the goal holds (or None), its progress score
    (or None) and the read where its plays end (or None)."""
    with open(best_path) as f:
        line = f.readline()
    goal = re.search(r'goal at read (\d+)', line)
    progress = re.search(r'progress (-?[0-9.e+]+)', line)
    end = re.search(r'plays end at read (\d+)', line)
    return (int(goal.group(1)) if goal else None, float(progress.group(1)) if progress else None,
            int(end.group(1)) if end else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('config')
    ap.add_argument('--rom')
    ap.add_argument('--best')
    ap.add_argument('--movie')
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux'))
    args = ap.parse_args()
    cfg = load_config(args.config)
    base = os.path.dirname(os.path.abspath(args.config))
    rom = args.rom or (os.path.join(base, cfg['rom']) if cfg.get('rom') else os.environ.get('GLOVER_ROM'))
    if not rom or not os.path.isfile(rom):
        sys.exit('check: give the ROM with --rom, the config\'s "rom" or $GLOVER_ROM')
    run_dir = os.path.dirname(os.path.join(base, cfg['run']))
    if not os.path.exists(os.path.join(run_dir, 'run.json')):
        sys.exit('check: %s has no run.json; make the run with tools/convert.py to-run' % run_dir)
    out_dir = os.path.join(base, cfg.get('out', 'search-out'))
    best = args.best or os.path.join(out_dir, 'best.txt')
    with open(os.path.join(run_dir, 'run.json')) as f:
        info = json.load(f)
    goal = (cfg.get('goal') or '').strip()
    try:
        groups = parse_cond(goal) if goal else None
    except ValueError as e:
        sys.exit('check: goal: %s' % e)
    progress = [(p['value'].rpartition(':')[0], p['value'].rpartition(':')[2], float(p.get('weight', 1)))
                for p in cfg.get('progress', [])]

    # 1. The new movie.
    new_movie = args.movie or os.path.join(out_dir, 'best.' + info['kind'])
    convert.to_movie(argparse.Namespace(run=run_dir, inputs=best, out=new_movie))

    # 2. Play it in the emulator, convert it and compare the engine with the emulator.
    extra = term_values(groups) if groups else []
    extra += [(name, typ) for name, typ, _ in progress if (name, typ) not in extra]
    writes = {int(k): [tuple(w) for w in v] for k, v in info.get('writes', {}).items()}
    new_info, rows, read_idx, extra_values = convert.movie_to_run(
        new_movie, rom, os.path.join(out_dir, 'check-run'), args.build, writes, extra=extra, check=True)
    tops = [r['top'] for r in new_info['reads']]
    claimed, claimed_progress, play_end = best_header(best)

    # 3. The score in the emulator.
    first = None
    if groups is not None:
        # The search tests the goal only after the window's first frame.
        start = int(cfg['window'][0]) + 1
        for i, vals in enumerate(extra_values):
            if i >= start and tops[i] and cond_holds(groups, dict(zip(extra, vals))):
                first = i
                break
        if first is not None:
            print('check: in the emulator the goal first holds at read %d (movie frame %d)' % (
                first, new_info['reads'][first]['frame']))
        if claimed is not None:
            if first is None or first != claimed + 1:
                print('check: FAIL: the search said the goal holds after the frames of read %d' % claimed)
                return 1
            print('check: ok: the same as the search (the goal holds after the frames of read %d)' % claimed)
            return 0
    if progress and play_end is not None and play_end < len(extra_values):
        values = dict(zip(extra, extra_values[play_end]))
        real = sum(w * as_number(values[(name, typ)], typ) for name, typ, w in progress)
        print('check: progress at read %d: %.4f in the emulator, %s in the search' % (
            play_end, real, '%.4f' % claimed_progress if claimed_progress is not None else 'unknown'))
        if claimed_progress is None or abs(real - claimed_progress) > 1e-3 * max(1.0, abs(real)):
            print('check: FAIL: the emulator gives another progress score (the timing changed?)')
            return 1
        print('check: ok: the same progress score')
    return 0


if __name__ == '__main__':
    sys.exit(main())
