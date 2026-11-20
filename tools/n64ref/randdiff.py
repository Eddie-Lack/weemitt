#!/usr/bin/env python3
"""Find the first call of Random that differs between two logs (emu.py and native.py --random-log).

    python3 tools/n64ref/randdiff.py N64_LOG NATIVE_LOG [--from-frame F] [--context N]

Random (maths.c) is the game's only random number generator, and many game decisions call it. The first call
that differs, in its caller, its lim, the frame or rseed, shows which part of the game went another way first.
Each log line is: frame F fc FC mode M level L caller NAME lim N rseed X (rseed before the call). Calls with
lim 0 are not in the logs: they leave rseed alone.
"""
import argparse


def load(path, start):
    out = []
    for line in open(path):
        t = line.split()
        if len(t) < 14 or t[0] != 'frame':
            continue
        rec = dict(zip(t[0::2], t[1::2]))
        if int(rec['frame']) >= start:
            out.append(rec)
    return out


def show(r):
    return 'frame %s fc %s mode %s level %s %s(%s) rseed %s' % (r['frame'], r['fc'], r['mode'], r['level'],
                                                               r['caller'], r['lim'], r['rseed'])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('n64')
    ap.add_argument('native')
    ap.add_argument('--from-frame', type=int, default=0, help='ignore calls before this frame')
    ap.add_argument('--context', type=int, default=8)
    ap.add_argument('--list', type=int, metavar='N', help='list the first N differing calls, one line each')
    ap.add_argument('--callers', action='store_true',
                    help='also compare the callers (they can differ in name only: the N64 log names a static or '
                         'inlined function after the global function before it)')
    args = ap.parse_args()
    a, b = load(args.n64, args.from_frame), load(args.native, args.from_frame)
    keys = ('frame', 'fc', 'mode', 'level', 'lim', 'rseed') + (('caller',) if args.callers else ())
    n = min(len(a), len(b))
    if args.list:
        shown = 0
        for i in range(n):
            if any(a[i][k] != b[i][k] for k in keys):
                print('call %d: N64 %s | native %s' % (i, show(a[i]), show(b[i])))
                shown += 1
                if shown >= args.list:
                    break
        return
    for i in range(n):
        if any(a[i][k] != b[i][k] for k in keys):
            print('call %d differs (%d calls before it are the same):' % (i, i))
            for j in range(max(0, i - args.context), min(n, i + args.context + 1)):
                mark = '>' if j == i else ' '
                print('%s N64    %s' % (mark, show(a[j])))
                print('%s native %s' % (mark, show(b[j])))
            return
    print('the first %d calls are the same (N64 log: %d calls, native log: %d)' % (n, len(a), len(b)))


if __name__ == '__main__':
    main()
