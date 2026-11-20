#!/usr/bin/env python3
"""Record the native port's state once per game frame, with the same fields and timing as emu.py.

    python3 tools/n64ref/native.py --frames N [--binary build/linux/weemitt] [--rom FILE] [--out FILE]
                                   [--level L] [--input SCRIPT] [--interleave N64_TRACE]
                                   [--random-log FILE] [--snapshot FRAME --snapshot-out FILE]

It runs the port headless under gdb, with a breakpoint at GameStep (src/game/main.c). GameStep is RunGame's
work for one frame. When GameStep starts in frame k+1, the graphics step that followed frame k and the
controller step of frame k+1 have run. That is the moment emu.py records on the N64, so a line "frame k" holds
the state after frame k in both traces. The first fields are those of the port's own --trace line; the rest are
the extra fields of fields.py.

--interleave makes the port run as many graphics steps (DrawGraphics' frames) between two game frames as the
N64 did in an emu.py trace. On the N64 the graphics thread runs once per scheduler message, and RunGame can fall
behind and then run several frames in a row; the 'disp' field (dispFrameCount) counts the graphics steps. The
graphics step advances animations and the screen fade, so its count changes the game. With --interleave the
script skips the port's graphics step where the N64 ran none, and calls ControllerStep and GraphicsStep again
where it ran more than one. This takes the N64's thread timing out of a comparison.

--random-log writes one line per call of Random (maths.c) with a lim other than 0: the frame, frameCount, mode,
level, the calling function, lim and rseed before the call. emu.py --random-log writes the same lines; randdiff.py compares them.
"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import fields  # noqa: E402

GDB_SCRIPT = r'''
import sys
sys.path.insert(0, %(here)r)
import gdb, fields, struct

cfg = %(cfg)r
out = open(cfg['out'], 'w', buffering=1)
rlog = open(cfg['random_log'], 'w', buffering=1) if cfg['random_log'] else None
inf = gdb.selected_inferior()
st = {'base': None, 'entries': 0, 'action': None}
graphics = cfg['graphics']   # frame label -> graphics steps the N64 ran before that frame's record
early = cfg['early']         # frame label -> graphics steps that ran while RunGame loaded a level
fixup = {}                   # frame label -> steps run before a load, to add to dispFrameCount after its reset
done = {}                    # frame label -> graphics steps run so far


def mem(addr, n):
    return bytes(inf.read_memory(st['base'] + (addr - 0x80000000), n))


def label():
    """The label of the next record: GameStep's first start is "frame -1"."""
    return st['entries'] - 1


def record():
    lab = label()
    rec = {name: fields.read_field(mem, addr, typ, False) for name, addr, typ in fields.FIELDS}
    out.write(fields.format_line(lab, rec) + '\n')
    if lab in cfg['snapshot']:
        # A header (magic, the address of port_ram, the size), then port_ram: RDRAM and the heap after it.
        size = 0x1800000
        with open(cfg['snapshot_out'].replace('{}', str(lab)), 'wb') as f:
            f.write(b'WMRAM\0\0\0' + struct.pack('<II', st['base'], size))
            f.write(bytes(inf.read_memory(st['base'], size)))
    st['entries'] += 1


class GameBreak(gdb.Breakpoint):
    def stop(self):
        if st['base'] is None:
            st['base'] = int(gdb.parse_and_eval('(unsigned int)&port_ram'))
        lab = label()
        want = graphics.get(lab)
        if want is not None and done.get(lab, 0) < want:
            st['action'] = ('more', want - done.get(lab, 0))
            return True
        record()
        if early.get(label(), 0) > 0:
            st['action'] = ('early', 0)
            return True
        return False


class GraphicsBreak(gdb.Breakpoint):
    def stop(self):
        lab = label()
        if fixup.get(lab):
            # The graphics steps that ran during the load have run before it; they count after its reset.
            gdb.execute('set var dispFrameCount = dispFrameCount + %%d' %% fixup[lab])
            fixup[lab] = 0
        want = graphics.get(lab)
        if want is not None and done.get(lab, 0) >= want:
            st['action'] = ('skip', 0)
            return True
        done[lab] = done.get(lab, 0) + 1
        return False


class RandomBreak(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('lim')) == 0:
            return False  # Random(0) returns 0 and leaves rseed alone; emu.py does not see these calls
        caller = gdb.newest_frame().older()
        rlog.write('frame %%d fc %%d mode %%d level %%d caller %%s lim %%d rseed %%08x\n' %% (
            label() - 1, int(gdb.parse_and_eval('frameCount')), int(gdb.parse_and_eval('gameInfo.mode')),
            int(gdb.parse_and_eval('gameInfo.level')), caller.name() if caller else '?',
            int(gdb.parse_and_eval('lim')), int(gdb.parse_and_eval('rseed')) & 0xFFFFFFFF))
        return False


gdb.execute('set pagination off')
gdb.execute('set confirm off')
GameBreak('GameStep')
gbreak = GraphicsBreak('GraphicsStep') if graphics else None
if rlog:
    RandomBreak('Random')
gdb.execute('run')
def graphics_steps(n):
    """n more scheduler frames: on the N64 each runs ControllerProc and then DrawGraphics."""
    gbreak.enabled = False
    lab = label()
    for i in range(n):
        gdb.execute('call ControllerStep()')
        gdb.execute('set var currentTask = 1')
        gdb.execute('call GraphicsStep()')
        done[lab] = done.get(lab, 0) + 1
    gbreak.enabled = True


def run_early():
    # Graphics steps that ran on the N64 while RunGame loaded a level (after it reset dispFrameCount and before
    # it set the fade) run here, before the load.
    lab = label()
    if early.get(lab, 0) > 0 and lab not in fixup:
        graphics_steps(early[lab])
        fixup[lab] = early[lab]


while inf.pid:
    action, n = st['action']
    st['action'] = None
    if action == 'skip':
        gdb.execute('return')
    elif action == 'early':
        run_early()
    else:
        graphics_steps(n)
        record()
        run_early()
    gdb.execute('continue')
out.close()
if rlog:
    rlog.close()
'''


def graphics_counts(path):
    """Graphics steps between two records of an emu.py trace: {frame label: count}, and the labels whose steps
    ran during a level load."""
    counts = {}
    early = {}
    prev = None
    for line in open(path):
        r = fields.parse_line(line)
        if r is None:
            continue
        if prev is not None and r['frame'] == prev['frame'] + 1:
            if r['disp'] >= prev['disp'] and (r['level'], r['mode']) == (prev['level'], prev['mode']):
                counts[r['frame']] = r['disp'] - prev['disp']
            elif r['disp'] < prev['disp']:
                # The game frame between the records reset dispFrameCount: it started a level. Some graphics steps
                # ran while it loaded, before it set gameInfo.fade to 300; the rest ran after it. The fade at the
                # record tells how many ran after: each step moves the fade a sixth of the way to 0 (UpdateFade).
                after = fade_steps(r['fade'])
                counts[r['frame']] = r['disp']
                if after is not None and after <= r['disp']:
                    early[r['frame']] = r['disp'] - after
        prev = r
    return counts, early


def fade_steps(fade):
    """How many UpdateFade steps take the fade from 300 to this value, fading in (None if none do)."""
    f = 300
    for n in range(40):
        if f == fade:
            return n
        f = int(f + (0 - f) / 6.0)
        if f < 10:
            f = 0
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--frames', type=int, required=True, help='game frames to run')
    ap.add_argument('--binary', default=os.path.join(ROOT, 'build', 'linux', 'weemitt'))
    ap.add_argument('--rom', default=os.path.join(ROOT, '..', 'gloveworld', 'baserom.us.z64'))
    ap.add_argument('--out', default='-')
    ap.add_argument('--level', type=int, help='start this level directly (weemitt --level)')
    ap.add_argument('--input', help='input script (weemitt --input)')
    ap.add_argument('--interleave', metavar='N64_TRACE',
                    help='run as many graphics steps between game frames as the N64 did in this emu.py trace')
    ap.add_argument('--random-log', metavar='FILE', help='log every call of Random')
    ap.add_argument('--snapshot', metavar='FRAME[,FRAME...]',
                    help='save port_ram as it is at the line "frame FRAME" (for memdiff.py)')
    ap.add_argument('--snapshot-out', metavar='FILE',
                    help='where --snapshot writes; {} in the name stands for the frame (keep it out of the repository)')
    args = ap.parse_args()
    if args.snapshot and not args.snapshot_out:
        ap.error('--snapshot needs --snapshot-out')
    if args.snapshot and ',' in args.snapshot and '{}' not in args.snapshot_out:
        ap.error('with several snapshots, put {} in the --snapshot-out name')
    fd, tmp = tempfile.mkstemp(suffix='.txt')
    os.close(fd)
    cfg = {'out': tmp, 'snapshot': [int(f) for f in args.snapshot.split(',')] if args.snapshot else [],
           'snapshot_out': os.path.abspath(args.snapshot_out or ''),
           'random_log': os.path.abspath(args.random_log) if args.random_log else '',
           'graphics': {}, 'early': {}}
    if args.interleave:
        counts, early = graphics_counts(args.interleave)
        cfg['graphics'] = counts
        cfg['early'] = early
    with tempfile.NamedTemporaryFile('w', suffix='.py', delete=False) as f:
        f.write(GDB_SCRIPT % {'here': HERE, 'cfg': cfg})
        gdbpy = f.name
    cmd = [args.binary, '--rom', os.path.abspath(args.rom), '--headless', str(args.frames)]
    if args.level is not None:
        cmd += ['--level', str(args.level)]
    if args.input:
        cmd += ['--input', os.path.abspath(args.input)]
    try:
        subprocess.run(['gdb', '-batch', '-nx', '-x', gdbpy, '--args'] + cmd, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        data = open(tmp).read()
    finally:
        os.unlink(gdbpy)
        os.unlink(tmp)
    if args.out == '-':
        sys.stdout.write(data)
    else:
        open(args.out, 'w').write(data)


if __name__ == '__main__':
    main()
