"""The game state that the N64 and native recorders write once per game frame.

Each field is (name, N64 address, type). The native port keeps the N64 memory layout, so the same address
names the same variable in both: N64 address A is at port_ram + (A - 0x80000000) in the native process.
The N64 stores values big-endian and the port little-endian; read_field() takes the byte order as an argument.

The first fields are those of the native --trace line (platform.c), in its order. The rest help to find the
cause of a difference.
"""
import struct

TYPES = {'s8': 'b', 'u8': 'B', 's16': 'h', 'u16': 'H', 's32': 'i', 'u32': 'I', 'f32': 'I'}
# f32 is read as its bits (u32), as the native trace prints it.

HAND = 0x802902D8
BALL = 0x8029F978
CAMERA = 0x8028F8E0
POS = 0x34  # Actor.pos

FIELDS = [
    ('fc', 0x802004E0, 's32'),             # frameCount
    ('mode', 0x801E7530, 's8'),            # gameInfo.mode
    ('level', 0x801E7531, 's8'),           # gameInfo.level
    ('rseed', 0x801ED3F0, 'u32'),          # rseed (maths.c)
    ('hand.x', HAND + POS, 'f32'),
    ('hand.y', HAND + POS + 4, 'f32'),
    ('hand.z', HAND + POS + 8, 'f32'),
    ('ball.x', BALL + POS, 'f32'),
    ('ball.y', BALL + POS + 4, 'f32'),
    ('ball.z', BALL + POS + 8, 'f32'),
    ('cam.x', CAMERA + POS, 'f32'),
    ('cam.y', CAMERA + POS + 4, 'f32'),
    ('cam.z', CAMERA + POS + 8, 'f32'),
    # Extra fields: not in the native --trace line.
    ('fc2', 0x80201004, 's32'),            # frameCount2
    ('disp', 0x801EC7CC, 's32'),           # dispFrameCount: DrawGraphics' frames
    ('fade', 0x801E753C, 's16'),           # gameInfo.fade
    ('auto', 0x801EC7F0, 's32'),           # autoPlayback
    ('nkf', 0x801EC772, 's16'),            # nextKeyFrame (demo playback)
    ('pbtn', 0x801FA9E4, 'u16'),           # playbackButtons
    ('btn', 0x801FB280, 'u16'),            # controllerdata[0].button
    ('paused', 0x801EC7D2, 'u8'),          # gamePaused
    ('rate', 0x801EFC91, 'u8'),            # desiredFrameRate
    ('backlog', 0x8025D148, 's32'),        # main_msgQ.validCount: messages RunGame has not read yet
    ('drawing', 0x801EC7A6, 'u8'),         # gfxIsDrawing
    ('tasks', 0x801EC7C8, 's32'),          # gfxTasks: graphics tasks the RSP has not finished
]

# The fields of the native --trace line, in its order, and how they are printed.
TRACE_FIELDS = [f[0] for f in FIELDS[:13]]


def words():
    """The aligned words that hold the fields, in order, without repeats."""
    out = []
    for _, addr, _ in FIELDS:
        w = addr & ~3
        if w not in out:
            out.append(w)
    return out


def read_field(mem, addr, typ, big_endian):
    """mem(addr, n) returns n bytes at N64 address addr."""
    fmt = TYPES[typ]
    return struct.unpack(('>' if big_endian else '<') + fmt, mem(addr, struct.calcsize(fmt)))[0]


def decode_words(values, big_endian=True):
    """Fields from a record of words (as words() lists them) read on the N64."""
    blob = {}
    for w, v in zip(words(), values):
        blob[w] = struct.pack('>I' if big_endian else '<I', v)

    def mem(addr, n):
        w = addr & ~3
        return blob[w][addr - w:addr - w + n]
    return {name: read_field(mem, addr, typ, big_endian) for name, addr, typ in FIELDS}


def format_line(frame, rec, extra=True):
    """A line in the format of the native --trace, with the extra fields after it."""
    def h(n):
        return '%08x' % (rec[n] & 0xFFFFFFFF)
    s = 'frame %d fc %d mode %d level %d rseed %s hand %s %s %s ball %s %s %s cam %s %s %s' % (
        frame, rec['fc'], rec['mode'], rec['level'], h('rseed'), h('hand.x'), h('hand.y'), h('hand.z'),
        h('ball.x'), h('ball.y'), h('ball.z'), h('cam.x'), h('cam.y'), h('cam.z'))
    if extra:
        s += ' |' + ''.join(' %s %d' % (name, rec[name]) for name, _, _ in FIELDS[13:])
    return s


def parse_line(line):
    """The fields of a trace line (native --trace, or a recorder's line). Floats stay as their bits."""
    t = line.split()
    if not t or t[0] != 'frame':
        return None
    rec = {'frame': int(t[1])}
    i = 2
    while i < len(t):
        key = t[i]
        if key == '|':
            i += 1
            continue
        if key in ('hand', 'ball', 'cam'):
            for k, c in enumerate('xyz'):
                rec[key + '.' + c] = int(t[i + 1 + k], 16)
            i += 4
        elif key == 'rseed':
            rec[key] = int(t[i + 1], 16)
            i += 2
        else:
            rec[key] = int(t[i + 1])
            i += 2
    return rec
