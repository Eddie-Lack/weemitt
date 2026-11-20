#!/usr/bin/env python3
"""The sync test: the engine must give the same game state as the real ROM in mupen64plus.

    python3 tests/sync.py [--rom ROM] [--build build/linux] [--scenario NAME ...] [--reads N] [--cached]

Each scenario starts at power-on and gives the game one input per controller read, in mupen64plus and in the engine.
mupen64plus runs the ROM with tools/sync/pollinput.so, which records the timing of the game's threads and the state
to compare at every read. tools/sync/events.py turns the timing into an events script, and the engine plays it
(play --events). At every read where the game thread waits between two frames, the compared values must be equal.

Scenarios: attract (no input: the logos, the fly-through and the recorded demos), and boss-N for each boss level N:
the team's level select (opened by writing gameInfo.mode during the fly-through, as Glover-Practice-Rom's
docs/game.md describes), then generated input in the level.

Needs mupen64plus (gloveworld's tools/emu/setup.sh), a display (Xvfb :99) and the Linux build.
"""
import argparse
import os
import pickle
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLOVEWORLD = os.environ.get('GLOVEWORLD', os.path.join(ROOT, '..', '..', 'gloveworld'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sync'))

import emurun  # noqa: E402
import events  # noqa: E402

BOSSES = {'atlantis': 13, 'carnival': 18, 'pirates': 23, 'prehistoric': 28, 'fear': 33, 'space1': 38, 'space3': 40}

# The values compared at each read: (watch text as play takes it, type).
COMPARE = [
    ('frameCount', 's32'), ('frameCount2', 's32'), ('gameInfo', 's8'), ('gameInfo+1', 's8'), ('rseed', 'u32'),
    ('hand+0x34', 'f32'), ('hand+0x38', 'f32'), ('hand+0x3c', 'f32'),
    ('hand+0x58', 'f32'), ('hand+0x5c', 'f32'), ('hand+0x60', 'f32'), ('hand+0x7c', 'f32'),
    ('hand+0xd4->0x4', 's16'),
    ('ball+0x34', 'f32'), ('ball+0x38', 'f32'), ('ball+0x3c', 'f32'),
    ('ball+0x58', 'f32'), ('ball+0x5c', 'f32'), ('ball+0x60', 'f32'),
    ('camera+0x34', 'f32'), ('camera+0x38', 'f32'), ('camera+0x3c', 'f32'), ('camera+0x7c', 'f32'),
    ('player+0x8', 's32'), ('player+0xc', 's32'), ('player+0x10', 's32'), ('player+0x14', 's32'),
    ('player+0x18', 's32'),
    ('enemies+0x34', 'f32'), ('enemies+0x38', 'f32'), ('enemies+0x3c', 'f32'),
    ('enemies+0x334', 'f32'), ('enemies+0x338', 'f32'), ('enemies+0x33c', 'f32'),
    ('actorList+0xe4', 's32'),
]

BUTTONS = [(0x8000, 'A'), (0x4000, 'B'), (0x2000, 'Z'), (0x1000, 'S'), (0x0020, 'L'), (0x0010, 'R'),
           (0x0800, 'du'), (0x0400, 'dd'), (0x0200, 'dl'), (0x0100, 'dr'),
           (0x0008, 'cu'), (0x0004, 'cd'), (0x0002, 'cl'), (0x0001, 'cr')]


def fmt_input(inp):
    buttons, x, y = inp
    return ' '.join([str(x), str(y)] + [name for bit, name in BUTTONS if buttons & bit])


def load_symbols(build):
    syms = {}
    for line in open(os.path.join(build, 'symbols.c')):
        line = line.strip()
        if line.startswith('{"'):
            name = line.split('"')[1]
            syms[name] = int(line.split(',')[1].strip().rstrip('u},'), 16)
    return syms


def address(syms, text):
    """'NAME+OFF' or 'NAME+OFF->OFF2' as the plugin's watch entry and the final address's low bits."""
    deref = None
    if '->' in text:
        text, deref = text.split('->')
        deref = int(deref, 0)
    name, _, off = text.partition('+')
    base = int(name, 16) if name.lower().startswith('0x') else syms[name]
    addr = base + (int(off, 0) if off else 0)
    if deref is None:
        return addr, addr
    return (addr, deref), deref


def decode(word, low, typ):
    """The value of type typ at an address with low bits `low`, from the big-endian word that holds it, as the
    engine's raw output prints it."""
    if typ in ('s8', 'u8'):
        v = (word >> ((3 - (low & 3)) * 8)) & 0xFF
        if typ == 's8' and v >= 0x80:
            v -= 0x100
    elif typ in ('s16', 'u16'):
        v = (word >> ((2 - (low & 2)) * 8)) & 0xFFFF
        if typ == 's16' and v >= 0x8000:
            v -= 0x10000
    else:
        v = word
    return v & 0xFFFFFFFF


def generated_input(seed, count):
    """Play-like input: walking and running in random directions, the ball and camera buttons, presses released
    between stretches. No Start."""
    rnd = random.Random(seed)
    out = []
    while len(out) < count:
        x = rnd.choice([0, 0, 80, -80, 40, -40, 25, 60, -60, 70])
        y = rnd.choice([0, 80, 80, -80, 40, 60, -40, 70, -70, 20])
        buttons = 0
        for bit, chance in ((0x8000, 0.3), (0x4000, 0.2), (0x2000, 0.15), (0x0010, 0.1), (0x0020, 0.05),
                            (0x0008, 0.05), (0x0004, 0.05), (0x0002, 0.08), (0x0001, 0.08)):
            if rnd.random() < chance:
                buttons |= bit
        out += [(buttons, x, y)] * rnd.randint(8, 50)
        out += [(0, x, y)] * rnd.randint(2, 6)
    return out[:count]


def scenario(name, reads, syms):
    """Returns (inputs per read, writes per read)."""
    if name == 'attract':
        return [], {}
    level = BOSSES[name[5:]]
    # At read 900 the fly-through runs (the attract mode reaches it near read 800). Writing 2 to gameInfo.mode opens
    # the level select there. The fly-through plays a recorded demo (autoPlayback 2), which keeps the controller's
    # buttons from the game: that is turned off too. The cursor is set, and Start starts the level. The fly-through
    # leaves handControl.noControlTimer and gameInfo.cameoActive set, and then ReadController passes only Start.
    writes = {900: [('gameInfo:s8', 2), ('autoPlayback:s32', 0)], 920: [('selectedLevel:s32', level)]}
    inputs = [(0, 0, 0)] * 940 + [(0x1000, 0, 0)] * 6 + [(0, 0, 0)] * 54
    inputs += generated_input(level, max(0, reads - len(inputs)))
    return inputs, writes


def emu_writes(syms, writes):
    """{read: [(watch text, value)]} as the plugin's (read, address, size, value)."""
    out = []
    for k, items in writes.items():
        for text, value in items:
            w, _ = address(syms, text.split(':')[0])
            size = {'s8': 1, 'u8': 1, 's16': 2, 'u16': 2}.get(text.split(':')[1], 4)
            out.append((int(k), w, size, value))
    return out


def emulator_log(rom, inputs, reads, writes, syms, extra=(), by_vi=False, max_vi=None):
    """Runs the ROM in mupen64plus with the plugin and returns (columns, records, vis): for each read, the words of
    events.WATCHED, extra and COMPARE, then the game thread's registers and stack (only where it is not waiting
    between frames), and the video frame of the read. The log holds no game data beyond these values."""
    named = list(events.WATCHED) + list(extra) + list(COMPARE)
    watch = [address(syms, t)[0] for t, _ in named]
    thread = syms['mainThread']
    watch += [thread + off for off in events.REGS.values()]
    watch += [(thread + events.REGS['sp'], 4 * j) for j in range(events.STACK_WORDS)]
    records, vis = emurun.emu_run(rom, inputs, watch, reads, writes=emu_writes(syms, writes), by_vi=by_vi,
                                  max_vi=max_vi)
    n = len(named)
    sp_pc = (n + list(events.REGS).index('sp'), n + list(events.REGS).index('pc'))
    for j, rec in enumerate(records):
        if (rec[sp_pc[0]], rec[sp_pc[1]]) == events.LOOP_TOP:
            records[j] = rec[:n]
    return named, records, vis


def decode_rows(columns, records, syms):
    """The log as one dict per read: 'TEXT:TYPE' for each value, 'compare' (COMPARE's values as play --raw prints
    them), and 'regs' and 'stack' where the game thread was in the middle of a frame."""
    lows = [address(syms, t)[1] for t, _ in columns]
    ncompare = len(COMPARE)
    n = len(columns)
    rows = []
    for rec in records:
        row = {'%s:%s' % (t, ty): decode(w, low, ty) for w, low, (t, ty) in zip(rec[:n], lows, columns)}
        row['compare'] = [row['%s:%s' % c] for c in columns[n - ncompare:]]
        if len(rec) > n:
            row['regs'] = dict(zip(events.REGS, rec[n:n + len(events.REGS)]))
            row['stack'] = rec[n + len(events.REGS):]
        rows.append(row)
    return rows


def find_stops(rows, rom):
    """{row index: stop} for the reads in the middle of a game frame (events.stop_for)."""
    import stack
    unwinder = stack.Unwinder(rom, os.path.join(GLOVEWORLD, 'symbol_addrs.txt'))
    hooks = events.hooked_functions(ROOT)
    out = {}
    for k, row in enumerate(rows):
        if 'regs' in row:
            stop = events.stop_for(row, unwinder, hooks)
            if stop is not None:
                out[k] = stop
    return out


def run_scenario(name, reads, rom, build, syms, cached=False, trim=False):
    inputs, writes = scenario(name, reads, syms)
    logpath = os.path.join(build, 'sync-%s.log' % name)
    # The emulator's log, kept for --cached. It holds only timing words, the game thread's registers and stack at
    # some reads, and the compared values: no game data beyond these.
    columns = list(events.WATCHED) + list(COMPARE)
    saved = None
    if cached and os.path.exists(logpath):
        with open(logpath, 'rb') as f:
            saved = pickle.load(f)
        if saved[0] != (columns, events.REGS, events.STACK_WORDS, inputs, writes, emurun.EMULATOR):
            saved = None
    if saved is not None:
        records, vis = saved[1], saved[2]
    else:
        columns, records, vis = emulator_log(rom, inputs, reads, writes, syms)
        with open(logpath, 'wb') as f:
            key = (columns, events.REGS, events.STACK_WORDS, inputs, writes, emurun.EMULATOR)
            pickle.dump((key, records, vis), f)
    rows = decode_rows(columns, records, syms)
    stops = find_stops(rows, rom)
    lines, readIdx, tops = events.derive(rows, syms, inputs, writes, fmt_input, stops)
    script = os.path.join(build, 'sync-%s.events' % name)
    with open(script, 'w') as f:
        f.write('#eeprom-fill 0x%02x\n' % emurun.EEPROM_FILL)
        f.write('\n'.join(lines) + '\n')
    out = subprocess.run([os.path.join(build, 'glover'), 'play', '--rom', rom, '--events', script, '--raw',
                          '--watch', ','.join('%s:%s' % c for c in COMPARE)] + (['--trim-draw'] if trim else []),
                         capture_output=True, text=True)
    if out.returncode != 0:
        return False, 'play failed: ' + out.stderr.strip()
    if 'did not reach' in out.stderr:
        return False, out.stderr.strip()
    eng = [[int(v, 16) for v in line.split(':', 1)[1].split()] for line in out.stdout.splitlines()]
    checked = 0
    for i, k in enumerate(readIdx):
        if not tops[i]:
            continue
        checked += 1
        emu = rows[k]['compare']
        if eng[i] != emu:
            diff = ['%s emu %08x engine %08x' % (COMPARE[j][0], emu[j], eng[i][j])
                    for j in range(len(COMPARE)) if emu[j] != eng[i][j]]
            return False, 'read %d (video frame %d), first difference after %d equal reads:\n    %s' % (
                k, vis[k], checked - 1, '\n    '.join(diff))
    return True, '%d reads compared, all equal (%d reads ran at a stop inside a logic frame)' % (checked, len(stops))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.environ.get('GLOVER_ROM', 'glover.z64'))
    ap.add_argument('--build', default=os.path.join(ROOT, 'build', 'linux'))
    ap.add_argument('--scenario', nargs='*', default=['attract'] + ['boss-' + b for b in BOSSES])
    ap.add_argument('--reads', type=int, help='reads per scenario (default 6000 for attract, 3000 for the others)')
    ap.add_argument('--cached', action='store_true', help="reuse the emulator's log of the last run")
    ap.add_argument('--trim-draw', action='store_true', help='run the engine with the trimmed draw')
    args = ap.parse_args()
    if not os.path.isfile(args.rom):
        sys.exit('sync.py: give the ROM with --rom or $GLOVER_ROM')
    emurun.build_plugins()
    syms = load_symbols(args.build)
    failed = 0
    for name in args.scenario:
        reads = args.reads or (6000 if name == 'attract' else 3000)
        # Each emulator run is a new process: mupen64plus's core keeps global state.
        p = subprocess.run([sys.executable, __file__, '--one', name, '--rom', args.rom, '--build', args.build,
                            '--reads', str(reads)] + (['--cached'] if args.cached else []) +
                           (['--trim-draw'] if args.trim_draw else []),
                           capture_output=True, text=True)
        result = [line for line in p.stdout.splitlines() if line.startswith(('ok ', 'FAIL'))]
        ok = p.returncode == 0 and result and result[0].startswith('ok')
        failed += not ok
        print(result[0] if result else 'FAIL %s: %s' % (name, p.stderr.strip()[-2000:]))
        if not ok and result:
            print('\n'.join(p.stdout.splitlines()[1:]))
    print('sync: all scenarios equal' if not failed else 'sync: %d scenarios differ' % failed)
    return 1 if failed else 0


def one():
    ap = argparse.ArgumentParser()
    ap.add_argument('--one')
    ap.add_argument('--rom')
    ap.add_argument('--build')
    ap.add_argument('--reads', type=int)
    ap.add_argument('--cached', action='store_true')
    ap.add_argument('--trim-draw', action='store_true')
    args = ap.parse_args()
    ok, msg = run_scenario(args.one, args.reads, args.rom, args.build, load_symbols(args.build), args.cached,
                           args.trim_draw)
    print(('ok   %s: ' if ok else 'FAIL %s: ') % args.one + msg)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(one() if '--one' in sys.argv else main())
