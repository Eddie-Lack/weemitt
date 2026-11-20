#!/usr/bin/env python3
"""Compare a trace of the real game (emu.py) with a trace of the native port, part by part.

    python3 tools/n64ref/compare.py N64_TRACE NATIVE_TRACE [--fields a,b,...] [--extra] [--context N]

Both traces have one line per game frame: the native port's --trace line, or the lines of emu.py and
native.py, which add more fields. The script cuts each trace into parts: a part is a run of frames with the same
mode and level, such as the menu (mode 1), a level being played (mode 4) or the language screen (mode 14). It
pairs the k-th part with a given mode and level in one trace with the k-th in the other. Within a pair it aligns
frames by frameCount (fc); frames with the same fc, as while a level loads or the game is paused, pair in order.
For each pair it prints where the part starts in each trace, how many frames it has, and the first frame where a
compared field differs, with the values on both sides.

By default it compares the fields of the native --trace line. --extra adds the extra fields that both traces
have, except those that depend on the N64's thread timing (vi, backlog, disp, fc2, rate, tasks, drawing).
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fields  # noqa: E402

TIMING_FIELDS = {'vi', 'backlog', 'disp', 'fc2', 'rate', 'frame', 'tasks', 'drawing'}
FLOAT_FIELDS = {n for n, _, t in fields.FIELDS if t == 'f32'}


def load(path):
    recs = []
    for line in open(path):
        r = fields.parse_line(line)
        if r is not None:
            recs.append(r)
    return recs


def parts(recs):
    """[(mode, level, occurrence, [records])]"""
    out = []
    seen = {}
    for r in recs:
        key = (r['mode'], r['level'])
        if out and (out[-1][0], out[-1][1]) == key:
            out[-1][3].append(r)
        else:
            seen[key] = seen.get(key, 0) + 1
            out.append((key[0], key[1], seen[key], [r]))
    return out


def align(a, b):
    """Pairs of records of two parts with the same fc; repeated fc values pair in order."""
    def keyed(recs):
        count = {}
        out = {}
        order = []
        for r in recs:
            k = (r['fc'], count.get(r['fc'], 0))
            count[r['fc']] = k[1] + 1
            out[k] = r
            order.append(k)
        return out, order
    ka, oa = keyed(a)
    kb, _ = keyed(b)
    return [(ka[k], kb[k]) for k in oa if k in kb]


def show(name, v):
    if name in FLOAT_FIELDS:
        return '%08x (%.9g)' % (v, struct.unpack('<f', struct.pack('<I', v))[0])
    if name == 'rseed':
        return '%08x' % v
    return str(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('n64')
    ap.add_argument('native')
    ap.add_argument('--fields', help='comma-separated fields to compare (default: the --trace fields)')
    ap.add_argument('--extra', action='store_true', help='also compare the extra fields both traces have')
    ap.add_argument('--context', type=int, default=0, help='also print N aligned frames after a difference')
    ap.add_argument('--all', action='store_true', help='list every part, also those only one trace has')
    args = ap.parse_args()
    a, b = load(args.n64), load(args.native)
    if args.fields:
        names = args.fields.split(',')
    else:
        names = list(fields.TRACE_FIELDS)
        if args.extra:
            common = set(a[0]) & set(b[0]) if a and b else set()
            names += [n for n, _, _ in fields.FIELDS[13:] if n in common and n not in TIMING_FIELDS]
    names = [n for n in names if n not in ('fc', 'mode', 'level')]
    pa, pb = parts(a), parts(b)
    index_b = {(m, l, k): recs for m, l, k, recs in pb}
    print('fields: %s' % ' '.join(names))
    print('N64 trace: %d frames, %d parts; native trace: %d frames, %d parts' % (len(a), len(pa), len(b), len(pb)))
    for mode, level, k, ra in pa:
        rb = index_b.get((mode, level, k))
        title = 'mode %d level %d (#%d)' % (mode, level, k)
        if rb is None:
            if args.all:
                print('%s: N64 frames %d-%d, not in the native trace' % (title, ra[0]['frame'], ra[-1]['frame']))
            continue
        pairs = align(ra, rb)
        head = '%s: N64 frames %d-%d (%d), native frames %d-%d (%d), %d aligned' % (
            title, ra[0]['frame'], ra[-1]['frame'], len(ra), rb[0]['frame'], rb[-1]['frame'], len(rb), len(pairs))
        first = None
        ndiff = 0
        for i, (x, y) in enumerate(pairs):
            diff = [n for n in names if n in x and n in y and x[n] != y[n]]
            if diff:
                ndiff += 1
                if first is None:
                    first = (i, diff)
        if first is None:
            print(head + ': same')
            continue
        i, diff = first
        x, y = pairs[i]
        print(head + ': %d differ; first at fc %d (N64 frame %d, native frame %d): %s' % (
            ndiff, x['fc'], x['frame'], y['frame'], ' '.join(diff)))
        for n in diff:
            print('    %-8s N64 %s  native %s' % (n, show(n, x[n]), show(n, y[n])))
        for x, y in pairs[i + 1:i + 1 + args.context]:
            diff = [n for n in names if n in x and n in y and x[n] != y[n]]
            print('    fc %d: %s' % (x['fc'], ' '.join('%s %s/%s' % (n, show(n, x[n]), show(n, y[n])) for n in diff)
                                      or 'same'))
    if args.all:
        index_a = {(m, l, k) for m, l, k, _ in pa}
        for mode, level, k, rb in pb:
            if (mode, level, k) not in index_a:
                print('mode %d level %d (#%d): native frames %d-%d, not in the N64 trace' % (
                    mode, level, k, rb[0]['frame'], rb[-1]['frame']))


if __name__ == '__main__':
    main()
