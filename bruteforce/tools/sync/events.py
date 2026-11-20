"""Turns a log of the real game's controller reads into an events script for the engine (glover play --events).

docs/sync.md explains the model. In short: on the N64 each frame message wakes the controller thread (a read), the
draw thread (a draw) and the game thread (a logic frame), in this order of priority. While the game thread is busy,
for example while a level loads, the other two go on, and its frame messages wait in its queue. The log has, at each
read of the controller thread, the values below. Between two reads:

- the draws are the increase of dispFrameCount (a logic frame that loads a level sets it to 0: then the count after
  the read is the number of draws after that frame);
- the logic frames are the frame messages that the game thread took from its queue (main_msgQ). A frame runs where
  its message was taken, before the draws that came while it ran.

The engine holds the state the emulator holds only at a read where the game thread waits between frames (a "top"
read). So:

- After each read the script writes the draw thread's state (DRAW_STATE) from the log. At a top read the values are
  the engine's own when it is in sync; the comparison runs before the writes.
- After a top read, the first draw runs in the engine as it is: the engine holds what the draw sees.
- After a read where the game thread works through queued short frames, the first draw runs as it is too.
  Drawing moves things on (animations), so it must run. The engine's world is then the one after the frame the
  game thread was in: the nearest it has.
- A draw while the game thread is inside a long frame (a load: the frame ran before the previous read, or goes on
  past the next one) is skipped. The engine ran that frame where it began, so it is ahead of the emulator. The
  engine writes the draw thread's state from the next read's log instead: the values the last of those draws left.
  Other extra draws of a stretch are handled the same way.
- A read in the middle of a game frame (MainGameLoop and the calls around it in RunGame) runs inside the engine's
  logic frame, at a stop (engine.h, engine_logic_stops). The log holds the game thread's registers and stack there;
  tools/sync/stack.py finds the calls in progress, and the stop names the hooked ones (engine/hooks.def). The read,
  the draw thread's state and the draw go into a group after the frame's "L" line (src/run.h).
"""
import os

# Words for the timing: (watch text, type).
TIMING = [
    ('frameCount', 's32'), ('dispFrameCount', 's32'),
    ('__osRunningThread', 'u32'),
    ('mainThread+0xf4', 'u32'),   # the game thread's saved stack pointer (low word of context.sp)
    ('mainThread+0x11c', 'u32'),  # its saved program counter (context.pc)
    ('main_msgQ+0x8', 's32'), ('main_msgQ+0xc', 's32'),  # validCount, first
    ('codeDrawingRequest', 'u8'),
] + [('main_msgbuf+0x%x' % (4 * i), 'u32') for i in range(8)]

# What the draw thread (DrawGraphics, main.c) writes and the game's logic may read.
DRAW_STATE = [
    ('dispFrameCount', 's32'), ('draw_buffer', 'u8'), ('gfxIsDrawing', 'u8'),
    ('camera+0x78', 'f32'), ('camera+0x7c', 'f32'),          # camera.rot (CalcCameraRot)
    ('cameraInfo+0x1bc', 'f32'), ('cameraInfo+0x1c0', 'f32'),  # cameraInfo.prevRotX, prevRotY
    ('camera+0x40', 'f32'), ('camera+0x44', 'f32'), ('camera+0x48', 'f32'),  # camera.prevPos
    ('cameraInfo+0x44', 'f32'), ('cameraInfo+0x48', 'f32'), ('cameraInfo+0x4c', 'f32'),  # cameraInfo.actor.prevPos
    ('gameInfo+0xc', 's16'), ('gameInfo+0x5c', 's16'),       # gameInfo.fade, starZ (UpdateFade)
    ('fadeOverlay+0x26', 's16'),                             # fadeOverlay.alpha
]

WATCHED = TIMING + [w for w in DRAW_STATE if w not in TIMING]

# The game thread's saved registers: the offsets of their low words in mainThread (OSThread.context holds 64-bit
# registers).
REGS = {'a0': 0x3c, 'a1': 0x44, 'a2': 0x4c, 'a3': 0x54, 's0': 0x9c, 's1': 0xa4, 's2': 0xac, 's3': 0xb4,
        's4': 0xbc, 's5': 0xc4, 's6': 0xcc, 's7': 0xd4, 's8': 0xfc, 'ra': 0x104, 'sp': 0xf4, 'pc': 0x11c}
# The words of the game thread's stack recorded from its saved stack pointer up.
STACK_WORDS = 384

# The calls of RunGame that make up a game frame (gloveworld's main.c).
FRAME_CALLS = ('StartDrawing', 'MainGameLoop', 'ProcessPersonalLoop', 'CheckForExits')


def hooked_functions(root):
    """The functions the engine hooks, from engine/hooks.def: {name: 1 when it takes a pointer argument}."""
    import re
    out = {}
    for line in open(os.path.join(root, 'engine', 'hooks.def')):
        m = re.match(r'HOOK([01A])\((\w+)[),]', line)
        if m:
            out[m.group(2)] = int(m.group(1) == '1')
    return out


def stop_for(row, unwinder, hooks):
    """The stop (src/run.h) at which the engine runs a read in the middle of a game frame, or None when the read is
    not inside one. row holds the game thread's registers ('regs') and stack ('stack').

    The engine can stop only where a hooked call starts or returns. The stop is the one nearest to the N64's point
    with no hooked call between, in this order:
    1. where the hooked call that last finished before the point returns, when that call is known for sure;
    2. where the innermost hooked call in progress returns, when no hooked call can come between;
    3. in MainGameLoop's own code or an unhooked call of it: where the next hooked call starts;
    4. else where the innermost hooked call in progress starts."""
    chain = unwinder.unwind(row['regs'], row['stack'])
    names = [f[0] for f in chain]
    if len(names) < 2 or names[-1] != 'RunGame' or names[-2] not in FRAME_CALLS:
        return None
    if names[-2] == 'StartDrawing':
        # The N64 waits there for the draw thread to start drawing; weemitt's StartDrawing does not wait.
        return 'StartDrawing leave'
    sp0 = row['regs']['sp']
    path = []
    inner = None  # the index in chain of the innermost hooked call in progress
    for j in range(len(chain) - 2, -1, -1):
        f = chain[j]
        if f[0] in hooks:
            arg = None
            if hooks[f[0]]:
                # The argument as the caller computed it, or as the function keeps it.
                arg = unwinder.call_arg0(chain[j + 1], row['stack'], sp0)
                if arg is None:
                    arg = unwinder.arg0(f, row['stack'], sp0)
            path.append(f[0] if arg is None else '%s=0x%x' % (f[0], arg))
            inner = j
    hooked = set(hooks)
    # The frames inside the innermost hooked call, or inside the game frame's call of RunGame.
    last = inner if inner is not None else len(chain) - 2

    # 1. The hooked call that last finished before the point. The frames are searched from the innermost out: a
    # frame's calls before its point ran after its caller's.
    for x in range(0, last + 1):
        name, pc, sp, regs, fstart = chain[x]
        found = unwinder.last_call_before(name, pc, hooked)
        if found is None:
            continue
        if found == 'unknown' or found[1] not in hooked:
            break
        addr, callee = found
        if hooks[callee]:
            arg = unwinder.call_arg0((name, addr, sp, regs, fstart), row['stack'], sp0)
            if arg is None:
                break
            return ' > '.join(path + ['%s=0x%x' % (callee, arg)]) + ' leave'
        # A call without an argument is known when it is the function's only call of it.
        if sum(1 for _, n in unwinder.calls(name) if n == callee) == 1:
            return ' > '.join(path + [callee]) + ' leave'
        break

    # 2. Where the innermost hooked call returns, when no hooked call can come before that.
    if inner is not None and not any(unwinder.call_may_follow(chain[x][0], chain[x][1], hooked)
                                     for x in range(0, inner + 1)):
        return ' > '.join(path) + ' leave'

    if path:
        return ' > '.join(path)
    if names[-2] != 'MainGameLoop':
        return None
    # 3. In MainGameLoop's own code, or in a call that is not hooked: stop at the next hooked call.
    site = chain[-2][1]
    calls = unwinder.calls('MainGameLoop')
    for addr, name in calls:
        if addr > site and name in hooks:
            nth = sum(1 for a, n in calls if n == name and a < addr)
            return name + (' nth=%d' % nth if nth else '')
    return None


# The game thread waits for its next frame message at the top of RunGame's loop with this stack pointer and program
# counter (inside osRecvMesg). Found from the log: at 5385 of 6000 reads of the attract mode the thread was here.
LOOP_TOP = (0x801FFC18, 0x801C7494)

MASK = {'u8': 0xFF, 's8': 0xFF, 'u16': 0xFFFF, 's16': 0xFFFF}


def s32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


def text(w):
    return '%s:%s' % w


def value(row, w):
    return row[text(w)] & MASK.get(w[1], 0xFFFFFFFF)


def derive(rows, syms, inputs, writes, fmt_input, stops=None):
    """rows: one dict per read, with the WATCHED values as unsigned numbers (keys 'watch text:type'). inputs[k]: the
    read's input. writes: {read: [(watch text, value)]}. stops: {row index: stop} for the reads in the middle of a
    game frame (stop_for). Returns (script lines, the indices of the controller thread's reads, and for each of
    those whether the game thread was waiting between frames)."""
    stops = stops or {}
    nine = syms['gfxRetraceMsg']
    controller = syms['ControllerThread']
    reads = [k for k, r in enumerate(rows) if r['__osRunningThread:u32'] == controller]
    tops = [(rows[k]['mainThread+0xf4:u32'], rows[k]['mainThread+0x11c:u32']) == LOOP_TOP for k in reads]
    # The logic frames between each read and the next: the frame messages taken from the queue. At most 3 messages
    # come in between two reads, which fixes how many times the queue's index went round.
    taken_frames = []
    for i in range(len(reads) - 1):
        a, b = rows[reads[i]], rows[reads[i + 1]]
        valid_a, valid_b = a['main_msgQ+0x8:s32'], b['main_msgQ+0x8:s32']
        first_a, first_b = a['main_msgQ+0xc:s32'], b['main_msgQ+0xc:s32']
        queue = [a['main_msgbuf+0x%x:u32' % (4 * ((first_a + j) % 8))] for j in range(valid_a)]
        base = (first_b - first_a) % 8
        taken = [base + 8 * w for w in range(3) if valid_a - valid_b <= base + 8 * w <= valid_a + 3 - valid_b]
        taken = taken[0] if taken else base
        taken_frames.append(sum(1 for m in queue[:taken] if m == nine))
    # The script as items: a line, or a logic frame [stop, lines of its group, ...] that holds groups.
    items = []
    last_logic = None  # the logic frame the game thread is in: the last one since the last read outside a group
    # The last logic frame, and the index of the item after it, while every read since then came in the middle of
    # it (no frame message taken). A long frame can be a load followed by the level's first game frame.
    long_logic = None
    long_from = 0

    def logic(count):
        nonlocal last_logic, long_logic, long_from
        for _ in range(count):
            last_logic = []
            items.append(last_logic)
        if count:
            long_logic = last_logic
            long_from = len(items)

    for i, k in enumerate(reads):
        a = rows[k]
        b = rows[reads[i + 1]] if i + 1 < len(reads) else None
        head = ['W %s %d' % (t, v) for t, v in writes.get(k, [])]
        if not tops[i]:
            head.append('#busy')
        head.append('R ' + fmt_input(inputs[k] if k < len(inputs) else (0, 0, 0)))
        head += ['W %s 0x%x' % (text(w), value(a, w)) for w in DRAW_STATE]
        if b is None:
            items += head
            break
        later = ['W %s 0x%x' % (text(w), value(b, w)) for w in DRAW_STATE]
        frames = taken_frames[i]
        # The draws.
        dfc_a, dfc_b = s32(a['dispFrameCount:s32']), s32(b['dispFrameCount:s32'])
        draws = dfc_b - dfc_a if dfc_b >= dfc_a else 1 + dfc_b
        stop = stops.get(k)
        if stop is not None and last_logic is None and long_logic is not None and not tops[i]:
            # The reads since the frame began came in its first part. They run at the same stop, before this one.
            last_logic = long_logic
            moved = items[long_from:]
            del items[long_from:]
            last_logic += [stop, moved]
        if stop is not None and last_logic is not None:
            # The read and the draw come in the middle of the frame the game thread is in.
            group = head
            if draws > 0:
                group.append('D')
                draws -= 1
            last_logic += [stop, group]
            logic(frames)
            if draws > 0:
                items += later
            continue
        items += head
        last_logic = None
        if tops[i]:
            long_logic = None
        # Is the game thread inside a long logic frame (a load)? Then the engine, which ran that frame where it
        # began, is ahead of the emulator. The frame is long when it already ran before the previous read (no
        # message taken since then), or when it goes on past the next read (no message taken until then).
        long_frame = not tops[i] and ((i > 0 and not tops[i - 1] and taken_frames[i - 1] == 0) or frames == 0)
        if not long_frame:
            # The engine holds what the draw sees: the game thread waits between frames, or it works through
            # queued short frames. Drawing moves things on (animations), so the draw must run. Later draws of the
            # stretch are counted as one stretch below.
            if draws > 0:
                items.append('D')
                draws -= 1
            logic(frames)
            if draws > 0:
                items += later
        else:
            # The draws change the draw thread's state, which comes from the log. In a load the world is not
            # drawn, or it is the half-built world of the frame that loads, which the engine does not have.
            if draws > 0:
                items += later
            logic(frames)
    lines = []
    for item in items:
        if isinstance(item, str):
            lines.append(item)
            continue
        lines.append('L')
        for j in range(0, len(item), 2):
            lines.append('@ ' + item[j])
            lines += item[j + 1]
            lines.append('@end')
    return lines, reads, tops
