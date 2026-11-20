"""TAS movie files: BizHawk's .bk2 and the Mupen64 .m64.

    from movie import read_movie, write_movie
    m = read_movie('run.bk2')       # m.frames: one (buttons, x, y) per movie frame, controller 1
    m.frames[100] = (0x8000, 0, 80)
    write_movie(m, 'new.bk2')       # the same kind of file; everything but the input is kept

buttons is the N64's controller word (engine.h: BTN_A 0x8000 and so on).

- A .bk2 is a zip. Its "Input Log.txt" holds one line per video frame (BizHawk's frame). The LogKey line names the
  columns: groups that start with #, the stick's axes first, then the buttons. A stick value is a number padded to 5
  characters and a comma. A button is '.' when up and its letter when down (BizHawk's Bk2MnemonicLookup). The other
  files of the zip (the header, the sync settings, a SaveRam) are copied as they are.
- BizHawk 2.11.1 has four more columns, "P1 A Up", "A Down", "A Left" and "A Right": each replaces a stick axis with
  full deflection (N64Input.GetStickValues). A Left gives X -128, else A Right gives X 127; A Up gives Y 127, else
  A Down gives Y -128. The reader applies them; the writer clears them and writes the stick's values.
- Only movies that start from power-on (with a new save) work: the readers refuse others.
- An .m64 has a header of 0x400 bytes, then 4 bytes per controller for each input sample. The emulator takes one
  sample per read of the controller, so a frame of an .m64 is a read, not a video frame. The word is mupen64plus's
  BUTTONS: the N64's button word with its two bytes swapped, then the stick's X and Y.
"""
import struct
import zipfile

# BizHawk's names for controller 1's buttons, their N64 bits and their letters in the input log.
BK2_BUTTONS = [
    ('P1 DPad U', 0x0800, 'U'), ('P1 DPad D', 0x0400, 'D'), ('P1 DPad L', 0x0200, 'L'), ('P1 DPad R', 0x0100, 'R'),
    ('P1 Start', 0x1000, 'S'), ('P1 Z', 0x2000, 'Z'), ('P1 B', 0x4000, 'B'), ('P1 A', 0x8000, 'A'),
    ('P1 C Up', 0x0008, 'u'), ('P1 C Down', 0x0004, 'd'), ('P1 C Left', 0x0002, 'l'), ('P1 C Right', 0x0001, 'r'),
    ('P1 L', 0x0020, 'l'), ('P1 R', 0x0010, 'r'),
]
BK2_BIT = {name: bit for name, bit, _ in BK2_BUTTONS}
BK2_LETTER = {name: letter for name, _, letter in BK2_BUTTONS}
BK2_LETTER.update({'P1 A Up': 'U', 'P1 A Down': 'D', 'P1 A Left': 'L', 'P1 A Right': 'R'})
FAKE_ANALOG = ('P1 A Up', 'P1 A Down', 'P1 A Left', 'P1 A Right')


def stick(v):
    """The stick's (x, y) that the game gets from one input log line's values."""
    def sbyte(n):
        return ((n + 128) & 0xFF) - 128

    x = -128 if v.get('P1 A Left') else 127 if v.get('P1 A Right') else sbyte(v.get('P1 X Axis', 0))
    y = 127 if v.get('P1 A Up') else -128 if v.get('P1 A Down') else sbyte(v.get('P1 Y Axis', 0))
    return x, y
OTHER_LETTERS = {'Reset': 'r', 'Power': 'P'}


class Movie:
    def __init__(self, kind):
        self.kind = kind        # 'bk2' or 'm64'
        self.frames = []        # (buttons, x, y) of controller 1, per movie frame
        self.extra = {}         # what writing back needs


def parse_logkey(line):
    """'#Reset|Power|#P1 X Axis|...|' as a list of groups of column names."""
    groups = []
    for part in line.split('#')[1:]:
        groups.append([name for name in part.split('|') if name])
    return groups


def parse_bk2_line(line, groups):
    """One input log line as {column name: value}: an int for an axis, a bool for a button."""
    values = {}
    i = 0
    for group in groups:
        for name in group:
            while line[i] == '|':
                i += 1
            if name.endswith('Axis'):
                comma = line.index(',', i)
                values[name] = int(line[i:comma])
                i = comma + 1
            else:
                values[name] = line[i] != '.'
                i += 1
    return values


def format_bk2_line(values, groups):
    out = '|'
    for group in groups:
        for name in group:
            if name.endswith('Axis'):
                out += str(values.get(name, 0)).rjust(5) + ','
            elif values.get(name):
                out += BK2_LETTER.get(name, OTHER_LETTERS.get(name, name.split()[-1][0]))
            else:
                out += '.'
        out += '|'
    return out


def read_bk2(path):
    m = Movie('bk2')
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        log = z.read('Input Log.txt').decode('utf-8-sig')
        m.extra['newline'] = '\r\n' if '\r\n' in log else '\n'
        m.extra['files'] = {n: z.read(n) for n in names if n != 'Input Log.txt'}
        m.extra['order'] = names
    groups = None
    lines = []
    for line in log.splitlines():
        if line.startswith('LogKey:'):
            groups = parse_logkey(line[len('LogKey:'):])
            m.extra['logkey'] = line[len('LogKey:'):]
        elif line.startswith('|') and groups is not None:
            lines.append(line)
    if groups is None:
        raise ValueError('%s: no LogKey line in the input log' % path)
    m.extra['groups'] = groups
    m.extra['values'] = []
    for line in lines:
        v = parse_bk2_line(line, groups)
        m.extra['values'].append(v)
        buttons = 0
        for name, bit in BK2_BIT.items():
            if v.get(name):
                buttons |= bit
        m.frames.append((buttons,) + stick(v))
    header = m.extra['files'].get('Header.txt', b'').decode('utf-8-sig')
    m.extra['header'] = dict(l.split(' ', 1) for l in header.splitlines() if ' ' in l)
    for key in ('StartsFromSavestate', 'StartsFromSaveRam'):
        if m.extra['header'].get(key, '').strip().lower() in ('true', '1'):
            raise ValueError('%s: the movie does not start from power-on (%s); only such movies work' % (path, key))
    if 'Core.bin' in names:
        raise ValueError('%s: the movie holds a savestate; only movies that start from power-on work' % path)
    return m


def write_bk2(m, path):
    groups = m.extra['groups']
    lines = ['[Input]', 'LogKey:' + m.extra['logkey']]
    old = m.extra['values']
    for f, (buttons, x, y) in enumerate(m.frames):
        # Columns other than controller 1's (the console's, other controllers') keep their values.
        v = dict(old[f]) if f < len(old) else {}
        for name, bit in BK2_BIT.items():
            v[name] = bool(buttons & bit)
        for name in FAKE_ANALOG:
            v[name] = False
        v['P1 X Axis'] = x
        v['P1 Y Axis'] = y
        lines.append(format_bk2_line(v, groups))
    lines.append('[/Input]')
    newline = m.extra.get('newline', '\n')
    data = (newline.join(lines) + newline).encode()
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in m.extra['order']:
            if name == 'Input Log.txt':
                z.writestr(name, data)
            else:
                z.writestr(name, m.extra['files'][name])


M64_HEADER = 0x400


def read_m64(path):
    m = Movie('m64')
    data = open(path, 'rb').read()
    if data[:4] != b'M64\x1a':
        raise ValueError('%s is not an .m64 movie' % path)
    controllers = data[0x15]
    samples = struct.unpack_from('<I', data, 0x18)[0]
    # The start type: 1 from a savestate, 2 from power-on, 4 from an EEPROM.
    start = struct.unpack_from('<H', data, 0x1C)[0]
    if start not in (0, 2):
        raise ValueError('%s: the movie does not start from power-on (start type %d); only such movies work' % (
            path, start))
    m.extra['header'] = bytearray(data[:M64_HEADER])
    m.extra['controllers'] = controllers
    m.extra['others'] = []
    pos = M64_HEADER
    for _ in range(samples):
        words = struct.unpack_from('<%dI' % controllers, data, pos)
        pos += 4 * controllers
        w = words[0]
        buttons = ((w & 0xFF) << 8) | ((w >> 8) & 0xFF)
        x = (w >> 16) & 0xFF
        y = (w >> 24) & 0xFF
        m.frames.append((buttons, x - 256 if x >= 128 else x, y - 256 if y >= 128 else y))
        m.extra['others'].append(words[1:])
    return m


def write_m64(m, path):
    header = bytearray(m.extra['header'])
    controllers = m.extra['controllers']
    struct.pack_into('<I', header, 0x18, len(m.frames))
    out = bytearray(header)
    for f, (buttons, x, y) in enumerate(m.frames):
        w = ((buttons >> 8) & 0xFF) | ((buttons & 0xFF) << 8) | (x & 0xFF) << 16 | (y & 0xFF) << 24
        others = m.extra['others'][f] if f < len(m.extra['others']) else (0,) * (controllers - 1)
        out += struct.pack('<%dI' % controllers, w, *others)
    open(path, 'wb').write(out)


def read_movie(path):
    if path.lower().endswith('.bk2'):
        return read_bk2(path)
    if path.lower().endswith('.m64'):
        return read_m64(path)
    raise ValueError('%s: a movie is a .bk2 or an .m64' % path)


def write_movie(m, path):
    (write_bk2 if m.kind == 'bk2' else write_m64)(m, path)
