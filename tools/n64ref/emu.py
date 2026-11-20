#!/usr/bin/env python3
"""Record the real game's state once per game frame in mupen64plus, for comparison with the native port.

    python3 tools/n64ref/emu.py [--frames N] [--vis N] [--until-level L] [--out FILE] [--gfx rice|angrylion]
                                [--gloveworld DIR] [--rom FILE]

It boots the ROM with no input and lets the attract mode run: the menus, the presentation (level 43), the
fly-through (level 44) and the recorded demos of levels 10, 16, 20, 25 and 30. It writes one line per game
frame in the format of the native port's --trace (src/port/platform.c), followed by extra fields
(fields.py). It stops after N game frames (--frames), N video frames (--vis), or when the level after L starts
(--until-level).

How a game frame is found. On the N64 the scheduler (sched.c) broadcasts message 9 to three threads once per
game frame: ControllerProc (priority 115), DrawGraphics (20) and RunGame (10). RunGame does the game's work. It
can fall behind (during a load, for example) and then runs the queued frames in a row. No counter changes once
per RunGame frame in every mode, and the emulator's frame callback comes once per video frame, not per game
frame. So the script patches the game's code: RunGame's call to ClearText (0x8013DC38), which follows its
osRecvMesg, calls a small routine at 0x80700000 instead. That routine copies the words of fields.py into a ring
buffer when the message is 9, then jumps to ClearText. The routine and the buffer are in the upper 4 MB of
RDRAM, which mupen64plus provides (the Expansion Pak) and the game does not use. The patch goes through the
core's cheat engine, which also clears the recompiler's copy of the code.

The record taken when RunGame starts frame k+1 holds the state after frame k: after RunGame's frame k and after
DrawGraphics' and ControllerProc's frame k+1, which run first because their priority is higher. The native port
runs ControllerStep, GameStep and GraphicsStep in that order, and prints its line after GraphicsStep. So the
line "frame k" means the same thing in both traces, while RunGame keeps up. The 'vi' field is the video frame
at which the script saw the record; when RunGame catches up, several records share a 'vi'.

Needs gloveworld's tools/emu to be set up (setup.sh) and a display (Xvfb :99).
"""
import argparse, bisect, importlib.util, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fields  # noqa: E402

GLOVEWORLD = os.environ.get('GLOVEWORLD', os.path.abspath(os.path.join(HERE, '..', '..', '..', 'gloveworld')))

HOOK = 0x80700000       # the routine
COUNT = 0x80700100      # records written so far
TABLE = 0x80700200      # addresses of the words to copy, ending with 0
RING = 0x80701000       # 64 records of 32 words
RING_SIZE = 64
RECORD = 128
CALL_SITE = 0x8013DC38  # RunGame: jal ClearText, after osRecvMesg(&main_msgQ, &msg, OS_MESG_BLOCK)
CLEARTEXT_CALL = 0x0C051762

# The routine (assembled with mips-linux-gnu-as at 0x80700000). RunGame keeps msg at 24($fp). When the record
# count equals the word at 0x80700104, it also copies the words from [*0x80700108, *0x8070010C) to 0x80400000.
#     lw    $t0, 24($fp)          # msg
#     lh    $t1, 0($t0)
#     addiu $t2, $zero, 9
#     bne   $t1, $t2, out         # not a game frame
#     lui   $t3, 0x8070
#     lw    $t4, 0x100($t3)       # record count
#     andi  $t5, $t4, 0x3F
#     sll   $t5, $t5, 7
#     addiu $t6, $t3, 0x1000
#     addu  $t6, $t6, $t5         # the record
#     addiu $t7, $t3, 0x200       # the address table
# loop:
#     lw    $t8, 0($t7)
#     beq   $t8, $zero, done
#     addiu $t7, $t7, 4
#     lw    $t9, 0($t8)
#     sw    $t9, 0($t6)
#     beq   $zero, $zero, loop
#     addiu $t6, $t6, 4
# done:
#     lw    $t5, 0x104($t3)       # the record to take a snapshot at
#     bne   $t4, $t5, count
#     lw    $t6, 0x108($t3)       # snapshot start
#     lw    $t7, 0x10C($t3)       # snapshot end
#     lui   $t8, 0x8040
# copy:
#     lw    $t9, 0($t6)
#     addiu $t6, $t6, 4
#     sw    $t9, 0($t8)
#     bne   $t6, $t7, copy
#     addiu $t8, $t8, 4
# count:
#     addiu $t4, $t4, 1
#     sw    $t4, 0x100($t3)
# out:
#     j     0x80145D88            # ClearText, which returns to RunGame
#     nop
HOOK_CODE = [
    0x8FC80018, 0x85090000, 0x240A0009, 0x152A001A, 0x3C0B8070, 0x8D6C0100, 0x318D003F, 0x000D69C0,
    0x256E1000, 0x01CD7021, 0x256F0200, 0x8DF80000, 0x13000005, 0x25EF0004, 0x8F190000, 0xADD90000,
    0x1000FFFA, 0x25CE0004, 0x8D6D0104, 0x158D0008, 0x8D6E0108, 0x8D6F010C, 0x3C188040, 0x8DD90000,
    0x25CE0004, 0xAF190000, 0x15CFFFFC, 0x27180004, 0x258C0001, 0xAD6C0100, 0x08051762, 0x00000000,
]
# A second routine, at 0x80700400, logs each call of Random (maths.c). It replaces Random's instructions at
# 0x80147010 (lui $v1, 0x801F; lw $v1, rseed), which come after its check of lim, and runs them before it jumps
# back. Each entry is 8 words: the return address, lim, frameCount, rseed before the call, the record count,
# the word of gameInfo.mode and level, dispFrameCount, 0. The entries go to a ring at 0x80710000.
#     lui   $t3, 0x8070
#     lw    $t4, 0x110($t3)       # call count
#     andi  $t5, $t4, 0x7FF
#     sll   $t5, $t5, 5
#     lui   $t6, 0x8071
#     addu  $t6, $t6, $t5         # the entry
#     sw    $ra, 0($t6)
#     sw    $a0, 4($t6)
#     lui   $t7, 0x8020
#     lw    $t8, 0x04E0($t7)      # frameCount
#     sw    $t8, 8($t6)
#     lui   $t7, 0x801F
#     lw    $t8, -0x2C10($t7)     # rseed
#     sw    $t8, 12($t6)
#     lw    $t8, 0x100($t3)       # record count
#     sw    $t8, 16($t6)
#     lui   $t7, 0x801E
#     lw    $t8, 0x7530($t7)      # gameInfo.mode, level
#     sw    $t8, 20($t6)
#     lui   $t7, 0x801F
#     lw    $t8, -0x3834($t7)     # dispFrameCount
#     sw    $t8, 24($t6)
#     addiu $t4, $t4, 1
#     sw    $t4, 0x110($t3)
#     lui   $v1, 0x801F           # the two instructions this replaced
#     j     0x80147018
#     lw    $v1, -11280($v1)
RANDOM_HOOK = 0x80700400
RANDOM_CODE = [
    0x3C0B8070, 0x8D6C0110, 0x318D07FF, 0x000D6940, 0x3C0E8071, 0x01CD7021, 0xADDF0000, 0xADC40004,
    0x3C0F8020, 0x8DF804E0, 0xADD80008, 0x3C0F801F, 0x8DF8D3F0, 0xADD8000C, 0x8D780100, 0xADD80010,
    0x3C0F801E, 0x8DF87530, 0xADD80014, 0x3C0F801F, 0x8DF8C7CC, 0xADD80018, 0x258C0001, 0xAD6C0110,
    0x3C03801F, 0x08051C06, 0x8C63D3F0, 0x00000000,
]
RANDOM_SITE = 0x80147010
RANDOM_ORIG = (0x3C03801F, 0x8C63D3F0)
RANDOM_COUNT = 0x80700110
RANDOM_RING = 0x80710000
RANDOM_RING_SIZE = 2048
SNAP_TARGET = 0x80700104
SNAP_RANGE = 0x80700108
SNAP_DEST = 0x80400000
# The game's data, bss and heap (see build/NGVE.map in gloveworld): what memdiff.py compares.
SNAP_START, SNAP_END = 0x801D41D0, 0x80400000
JAL_HOOK = 0x0C000000 | ((HOOK >> 2) & 0x03FFFFFF)


def snapshot_name(pattern, frame):
    return pattern.replace('{}', str(frame))


def load_functions():
    """The game's functions from gloveworld's link map: (sorted addresses, names)."""
    funcs = {}
    for line in open(os.path.join(GLOVEWORLD, 'build', 'NGVE.map')):
        m = re.match(r'\s+0x([0-9a-f]{8})\s+([A-Za-z_]\w*)\s*$', line)
        if m:
            a = int(m.group(1), 16)
            if 0x80100000 <= a < 0x801D41D0:
                funcs.setdefault(a, m.group(2))
    addrs = sorted(funcs)
    return addrs, [funcs[a] for a in addrs]


def load_gloveworld_emu():
    path = os.path.join(GLOVEWORLD, 'tools', 'emu', 'emu.py')
    spec = importlib.util.spec_from_file_location('gloveworld_emu', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_script(args, out):
    words = fields.words()
    assert len(words) < RECORD // 4

    def run(emu):
        yield 1
        if emu.r32(CALL_SITE) != CLEARTEXT_CALL:
            raise RuntimeError('unexpected code at %08X: %08X (not Glover USA?)' % (CALL_SITE, emu.r32(CALL_SITE)))
        for i, w in enumerate(HOOK_CODE):
            emu.w32(HOOK + 4 * i, w)
        for i, w in enumerate(words):
            emu.w32(TABLE + 4 * i, w)
        emu.w32(TABLE + 4 * len(words), 0)
        emu.w32(COUNT, 0)
        emu.w32(SNAP_TARGET, 0xFFFFFFFF)
        emu.w32(SNAP_RANGE, SNAP_START)
        emu.w32(SNAP_RANGE + 4, SNAP_END)
        snap_index = None
        snaps = sorted(int(f) for f in args.snapshot.split(',')) if args.snapshot else []
        rlog = None
        if args.random_log:
            if tuple(emu.r32(RANDOM_SITE + 4 * i) for i in range(2)) != RANDOM_ORIG:
                raise RuntimeError('unexpected code in Random')
            for i, w in enumerate(RANDOM_CODE):
                emu.w32(RANDOM_HOOK + 4 * i, w)
            emu.w32(RANDOM_COUNT, 0)
            jump = 0x08000000 | ((RANDOM_HOOK >> 2) & 0x03FFFFFF)
            emu.cheat('n64ref random', [(0x81000000 | (RANDOM_SITE & 0xFFFFFF), jump >> 16),
                                        (0x81000000 | ((RANDOM_SITE + 2) & 0xFFFFFF), jump & 0xFFFF),
                                        (0x81000000 | ((RANDOM_SITE + 4) & 0xFFFFFF), 0),
                                        (0x81000000 | ((RANDOM_SITE + 6) & 0xFFFFFF), 0)])
            rlog = open(args.random_log, 'w', buffering=1)
            faddrs, fnames = load_functions()
            rseen = 0
            rpending = []
        # Two 16-bit GameShark writes (type 0x81): the cheat engine invalidates recompiled code.
        emu.cheat('n64ref hook', [(0x81000000 | (CALL_SITE & 0xFFFFFF), JAL_HOOK >> 16),
                                  (0x81000000 | ((CALL_SITE + 2) & 0xFFFFFF), JAL_HOOK & 0xFFFF)])
        seen = 0
        frame = -1
        levels_started = []  # levels in the order they started
        while True:
            yield 1
            count = emu.r32(COUNT)
            if count - seen > RING_SIZE:
                raise RuntimeError('the ring buffer overflowed at video frame %d' % emu.frame)
            while seen < count:
                base = RING + (seen % RING_SIZE) * RECORD
                raw = emu.read(base, 4 * len(words))
                vals = [int.from_bytes(raw[4 * i:4 * i + 4], 'big') for i in range(len(words))]
                rec = fields.decode_words(vals)
                seen += 1
                if seen == 1:
                    # The hook takes effect after RunGame's first frame has begun (it loads the title for several
                    # seconds). In the menus frameCount counts RunGame's frames from 0, so it numbers this record.
                    frame = rec['fc'] - 1 if rec['mode'] == 1 else -1
                    if snaps:
                        snap_index = snaps[0] - frame
                        emu.w32(SNAP_TARGET, snap_index)
                out.write(fields.format_line(frame, rec) + ' vi %d\n' % emu.frame)
                if rec['mode'] == 4 and (not levels_started or levels_started[-1] != rec['level']):
                    levels_started.append(rec['level'])
                    print('video frame %d game frame %d: level %d' % (emu.frame, frame, rec['level']),
                          file=sys.stderr)
                frame += 1
                if snap_index is not None and seen == snap_index + 1:
                    name = snapshot_name(args.snapshot_out, snaps[0])
                    with open(name, 'wb') as f:
                        f.write(emu.read(SNAP_DEST, SNAP_END - SNAP_START))
                    print('wrote the memory at frame %d to %s' % (snaps[0], name), file=sys.stderr)
                    snaps.pop(0)
                    snap_index = None
                    if snaps:
                        snap_index = snaps[0] - (frame - seen)
                        if snap_index < count:
                            print('frame %d passed before its snapshot was set up; take it in another run'
                                  % snaps[0], file=sys.stderr)
                        emu.w32(SNAP_TARGET, snap_index)
                if args.frames and frame >= args.frames:
                    return
                if args.until_level is not None and len(levels_started) >= 2 \
                        and levels_started[-2] == args.until_level and levels_started[-1] != args.until_level:
                    return
            if rlog is not None:
                rcount = emu.r32(RANDOM_COUNT)
                if rcount - rseen > RANDOM_RING_SIZE:
                    raise RuntimeError('the Random log overflowed at video frame %d' % emu.frame)
                while rseen < rcount:
                    raw = emu.read(RANDOM_RING + (rseen % RANDOM_RING_SIZE) * 32, 28)
                    rpending.append([int.from_bytes(raw[4 * i:4 * i + 4], 'big') for i in range(7)])
                    rseen += 1
                if seen:
                    frame0 = frame - seen  # the label of record 0
                    for ra, lim, fc, rseed, recs, ml, disp in rpending:
                        if recs == 0:
                            continue
                        i = bisect.bisect_right(faddrs, ra) - 1
                        mode, level = (ml >> 24) & 0xFF, (ml >> 16) & 0xFF
                        rlog.write('frame %d fc %d mode %d level %d caller %s lim %d rseed %08x\n' % (
                            recs - 1 + frame0, fc, mode - 256 * (mode > 127), level - 256 * (level > 127),
                            fnames[i] if i >= 0 else '%08x' % ra, lim - (1 << 32) * (lim >> 31), rseed))
                    rpending = []
            if args.vis and emu.frame >= args.vis:
                return
            if emu.frame > 2000 and seen == 0:
                raise RuntimeError('no game frame recorded: the hook did not take')
    return run


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--frames', type=int, default=0, help='stop after this many game frames')
    ap.add_argument('--vis', type=int, default=0, help='stop after this many video frames')
    ap.add_argument('--until-level', type=int, help='stop when a level other than L follows level L')
    ap.add_argument('--out', default='-', help='output file (default: standard output)')
    ap.add_argument('--random-log', metavar='FILE', help='also log every call of Random (compare with randdiff.py)')
    ap.add_argument('--snapshot', metavar='FRAME[,FRAME...]',
                    help='save the game memory (0x%08X-0x%08X) as it is at the line "frame FRAME"; frames at '
                         'least 10 apart' % (SNAP_START, SNAP_END))
    ap.add_argument('--snapshot-out', metavar='FILE',
                    help='where --snapshot writes; {} in the name stands for the frame (keep it out of the repository)')
    ap.add_argument('--gfx', default='rice', choices=['rice', 'angrylion'])
    ap.add_argument('--gloveworld', help='path of the gloveworld checkout (default: ../gloveworld)')
    ap.add_argument('--rom', help='the ROM (default: gloveworld/baserom.us.z64)')
    args = ap.parse_args()
    global GLOVEWORLD
    if args.gloveworld:
        GLOVEWORLD = os.path.abspath(args.gloveworld)
    if args.snapshot:
        if not args.snapshot_out:
            ap.error('--snapshot needs --snapshot-out')
        if ',' in args.snapshot and '{}' not in args.snapshot_out:
            ap.error('with several snapshots, put {} in the --snapshot-out name')
        if not (args.frames or args.vis or args.until_level is not None):
            args.frames = max(int(f) for f in args.snapshot.split(',')) + 1
    if not (args.frames or args.vis or args.until_level is not None):
        ap.error('give --frames, --vis or --until-level')
    rom = args.rom or os.path.join(GLOVEWORLD, 'baserom.us.z64')
    gw = load_gloveworld_emu()
    os.environ.setdefault('DISPLAY', ':99')
    os.environ.setdefault('SDL_AUDIODRIVER', 'dummy')
    out = sys.stdout if args.out == '-' else open(args.out, 'w', buffering=1)
    emu = gw.Emu(rom, args.gfx, os.path.join(gw.BUILD, 'out'))
    emu.run(make_script(args, out))
    if out is not sys.stdout:
        out.close()


if __name__ == '__main__':
    main()
