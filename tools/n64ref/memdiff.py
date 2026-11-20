#!/usr/bin/env python3
"""Compare the game's memory on the N64 with the native port's at the same game frame.

    python3 tools/n64ref/memdiff.py N64_DUMP NATIVE_DUMP [--binary WEEMITT] [--gloveworld DIR] [--words N] [--all]
                                    [--halves]

N64_DUMP comes from `emu.py --snapshot FRAME --snapshot-out FILE`: the N64's data, bss and heap
(0x801D41D0-0x80400000). NATIVE_DUMP comes from `native.py --snapshot FRAME --snapshot-out FILE` (port_ram after
a header that gives its address), or from `weemitt --headless N --dump FRAME FILE` with --binary. Take both at
the same FRAME. Keep the dumps out of the repository: they hold data from the ROM.

The port keeps the N64's layout, so a variable at N64 address A is at port_ram + (A - 0x80000000). One thing
moves: the heap ends 16 MB later in the port (PORT_HEAP_TOP), and Jalloc puts static blocks at the end of the
heap, so a static block is that much higher in the port. The script reads both heaps' block lists
(jallocControl) and maps addresses and pointers with them.

The N64 is big-endian and the port little-endian, and a dump does not say which bytes form a value. A word
counts as equal when its bytes match as one 32-bit value, as two 16-bit values or as bytes (or a mix by
halfword), or when both are pointers to the same place. A pointer into the N64's code or read-only data
(function pointers, string constants) cannot be checked; it counts as equal when the port's word is not a
pointer into port_ram either.

Differences are grouped by variable (from gloveworld's build/NGVE.map) or by heap block (its Jalloc name).
"""
import argparse, bisect, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GLOVEWORLD = os.path.abspath(os.path.join(HERE, '..', '..', '..', 'gloveworld'))

SNAP_START, SNAP_END = 0x801D41D0, 0x80400000
BSS_START, HEAP_START = 0x801F5680, 0x802B0D10
CODE_START, CODE_END = 0x80100000, 0x801D41D0  # read-only data and code
JALLOC = 0x8025D690
NUM_BLOCKS = 0x500
BLOCK = 0x14

# Variables whose differences are expected: they hold timing, threads, the scheduler, display lists the port
# builds differently, or the audio driver's state (the port has stubs).
EXPECTED = {
    'dynamic': 'display lists and matrices for the RSP',
    'tlist': 'graphics tasks', 'gfxYieldBuf': 'RSP yield buffer', 'rdp_output': 'RDP output buffer',
    'dram_stack': 'RSP stack', 'overlay_dl': 'overlay display list', 'overlay_dl2': 'overlay display list',
    'sched': 'scheduler threads and queues', 'nnScStack': 'scheduler stack', 'nnScAudioStack': 'audio stack',
    'nnScGraphicsStack': 'graphics stack', 'mainThread': 'thread', 'mainThreadStack': 'stack',
    'ControllerThread': 'thread', 'ControllerThreadStack': 'stack', 'drawGraphicsThread': 'thread',
    'drawGraphicsThreadStack': 'stack', 'idleThread': 'thread', 'timers': 'CPU-count timers',
    'frameStart': 'CPU count', 'jallocControl': 'heap block list (compared separately)',
}


def load_symbols(gw):
    """Variables in the data and bss: gloveworld's symbol lists, then its link map for the rest."""
    syms = {}
    for name in ('symbol_addrs.txt', 'undefined_syms_auto.txt'):
        for line in open(os.path.join(gw, name)):
            m = re.match(r'\s*([A-Za-z_]\w*)\s*=\s*0x([0-9A-Fa-f]{8})\s*;', line)
            if m and '_data_' not in m.group(1):
                a = int(m.group(2), 16)
                if SNAP_START <= a < HEAP_START:
                    syms.setdefault(a, m.group(1))
    for line in open(os.path.join(gw, 'build', 'NGVE.map')):
        m = re.match(r'\s+0x([0-9a-f]{8})\s+([A-Za-z_]\w*)\s*$', line)
        if m and '_data_' not in m.group(2) and not m.group(2).endswith('_end'):
            a = int(m.group(1), 16)
            if SNAP_START <= a < HEAP_START:
                syms.setdefault(a, m.group(2))
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def blocks(read32, readbytes, little):
    """The used Jalloc blocks: [(addr, size, name, static)]."""
    out = []
    last_static = read32(JALLOC + NUM_BLOCKS * BLOCK + 8)
    first_dynamic = read32(JALLOC + NUM_BLOCKS * BLOCK + 12)
    for i in range(NUM_BLOCKS):
        b = JALLOC + i * BLOCK
        used = readbytes(b + 0x13, 1)[0]
        if not used:
            continue
        addr, size = read32(b), read32(b + 4)
        # Jalloc copies at most 11 characters and no terminator: a name of 10 or 11 characters ends in whatever
        # the block's slot held before.
        name = re.match(rb'[\x20-\x7e]*', readbytes(b + 8, 11).split(b'\0')[0]).group(0)[:10].decode()
        if i <= last_static or i >= first_dynamic:
            out.append((addr, size, name, i <= last_static))
    return out, last_static, first_dynamic


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('n64')
    ap.add_argument('native')
    ap.add_argument('--gloveworld', default=GLOVEWORLD)
    ap.add_argument('--binary', help='for a weemitt --dump file: the weemitt that wrote it (gives port_ram\'s address)')
    ap.add_argument('--base', help='for a weemitt --dump file: the address of port_ram')
    ap.add_argument('--words', type=int, default=4, help='differing words to show per variable or block')
    ap.add_argument('--all', action='store_true', help='also show the variables whose differences are expected')
    ap.add_argument('--halves', action='store_true',
                    help='also report words that match only as two 16-bit values when the N64 word reads as a '
                         'plausible float (a float converted as two halves looks like that); expect some noise')
    args = ap.parse_args()

    n64 = open(args.n64, 'rb').read()
    raw = open(args.native, 'rb').read()
    if raw[:8] == b'WMRAM\0\0\0':
        base, size = struct.unpack('<II', raw[8:16])
        nat = raw[16:16 + size]
    else:
        # A dump from weemitt --dump: port_ram alone. Its address comes from the binary's symbols.
        if args.base is None and args.binary is None:
            sys.exit('%s has no header: give --binary (the weemitt that wrote it) or --base' % args.native)
        if args.base is not None:
            base = int(args.base, 0)
        else:
            nm = subprocess.run(['nm', args.binary], capture_output=True, text=True, check=True).stdout
            base = int(re.search(r'^([0-9a-f]+) \w port_ram$', nm, re.M).group(1), 16)
        nat = raw
        size = len(raw)

    def n_bytes(a, n):
        return n64[a - SNAP_START:a - SNAP_START + n]

    def n_read32(a):
        return struct.unpack('>I', n_bytes(a, 4))[0]

    def p_bytes(a, n):
        return nat[a - 0x80000000:a - 0x80000000 + n]

    def p_read32(a):
        return struct.unpack('<I', p_bytes(a, 4))[0]

    nb, n_last, n_first = blocks(n_read32, n_bytes, False)
    pb, p_last, p_first = blocks(p_read32, p_bytes, True)
    n_heap_size = n_read32(JALLOC + NUM_BLOCKS * BLOCK + 4)
    p_heap_size = p_read32(JALLOC + NUM_BLOCKS * BLOCK + 4)
    delta = p_heap_size - n_heap_size
    static_low = min([a for a, s, _, st in nb if st] or [SNAP_END])
    print('heap: N64 %d static and %d dynamic blocks; native %d and %d; static blocks move by 0x%X' % (
        sum(1 for b in nb if b[3]), sum(1 for b in nb if not b[3]), sum(1 for b in pb if b[3]),
        sum(1 for b in pb if not b[3]), delta))

    def host(a):
        """Where the port keeps N64 address a, as an offset in port_ram."""
        off = a - 0x80000000
        if static_low <= a < SNAP_END:
            off += delta
        return off

    # The block lists: the port's should be the N64's, with host pointers and static blocks moved.
    pmap = {(a - base, s, name) for a, s, name, _ in pb}
    missing = [(a, s, name) for a, s, name, st in nb if (host(a), s, name) not in pmap]
    if missing:
        print('heap blocks of the N64 that the port does not have at the same place: %d' % len(missing))
        for a, s, name in missing[:20]:
            print('    %08X size %6d %s' % (a, s, name))
    nmap = {(host(a), s, name) for a, s, name, _ in nb}
    extra = [(a, s, name) for a, s, name, st in pb if (a - base, s, name) not in nmap]
    if extra:
        print('heap blocks of the port that the N64 does not have: %d' % len(extra))
        for a, s, name in extra[:20]:
            print('    port_ram+%07X size %6d %s' % (a - base, s, name))

    def same_word(p, q):
        if p == q[::-1]:
            return True
        wn = struct.unpack('>I', p)[0]
        wq = struct.unpack('<I', q)[0]
        if 0x80000000 <= wn < 0x80400000:
            if wq == base + host(wn):
                return True
            if CODE_START <= wn < CODE_END and not (base <= wq < base + size):
                return True
        for h in (0, 2):
            ph, qh = p[h:h + 2], q[h:h + 2]
            if ph != qh[::-1] and ph != qh:
                return False
        if halves_here and p[0:2] == q[1::-1] and p[2:4] == q[3:1:-1] and 0x70 <= (wn >> 23) & 0xFF <= 0x90:
            # The same as two 16-bit values but not as one 32-bit value, and a float on the N64 between about
            # 1e-5 and 1e5: perhaps a float the port converted as two 16-bit halves.
            return False
        return True

    sym_addrs, sym_names = load_symbols(args.gloveworld)
    regions = {}  # key -> [count, [(addr, p, q)]]

    def owner(a):
        if a < HEAP_START:
            i = bisect.bisect_right(sym_addrs, a) - 1
            return ('var', sym_names[i] if i >= 0 else '?', sym_addrs[i] if i >= 0 else SNAP_START)
        for b_addr, b_size, name, st in nb:
            if b_addr <= a < b_addr + b_size:
                return ('heap', name, b_addr)
        return ('free', 'unused heap', 0)

    total = 0
    for a in range(SNAP_START, SNAP_END, 4):
        off = host(a)
        if off + 4 > len(nat):
            continue
        p = n_bytes(a, 4)
        q = nat[off:off + 4]
        if p == q:
            continue
        key = owner(a)
        # Only heap data (loaded from the ROM) is checked. Texels and vertices are 16-bit: --halves would list them.
        halves_here = args.halves and key[0] == 'heap' and key[1] not in ('TexBank', 'ObjBank')
        if same_word(p, q):
            continue
        if n_bytes(a & ~7, 8) == nat[host(a & ~7):host(a & ~7) + 8][::-1]:
            continue  # a double or a 64-bit integer
        if p == b'\x80\0\0\0' and a < BSS_START:
            # libmus's effect data (sound.c) starts with a word that the music driver relocates; the port has no
            # music driver and its data generator put another value there.
            continue
        if key[0] == 'free':
            continue
        total += 1
        r = regions.setdefault(key, [0, []])
        r[0] += 1
        if len(r[1]) < args.words:
            r[1].append((a, p, q))

    print('%d words differ' % total)
    for (kind, name, start), (count, words) in sorted(regions.items(), key=lambda kv: kv[0][2]):
        expected = kind == 'var' and name in EXPECTED
        if expected and not args.all:
            continue
        print('%s %s at %08X: %d words%s' % ('variable' if kind == 'var' else 'heap block', name, start, count,
                                          ' (expected: %s)' % EXPECTED[name] if expected else ''))
        for a, p, q in words:
            fp = struct.unpack('>f', p)[0]
            fq = struct.unpack('<f', q)[0]
            print('    %08X +0x%X: N64 %s  port %s   (as floats %.6g / %.6g)' % (
                a, a - start, p.hex(), q[::-1].hex(), fp, fq))
    hidden = sum(c for (k, n, _), (c, _) in regions.items() if k == 'var' and n in EXPECTED)
    if hidden and not args.all:
        print('(%d more words in variables whose differences are expected; --all shows them)' % hidden)


if __name__ == '__main__':
    main()
