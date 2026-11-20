"""The call stack of the N64 game thread at a controller read, from its saved registers and stack.

    from stack import Unwinder
    u = Unwinder(rom_path, symbol_addrs_path)
    frames = u.unwind(regs, stack_words)

When the frame message comes in the middle of a logic frame, the game thread is stopped wherever it was, and its
registers are saved in mainThread.context. regs holds the saved registers by name (a0 to a3, s0 to s8, ra, sp, pc,
as unsigned words); stack_words holds the words from the saved stack pointer up.

Each frame of the result is (function name, program counter, stack pointer, registers, the function's start): the
registers hold the callee-saved values as the function saw them. Frame 0 is the innermost. The unwinder reads each
function's prologue from the player's ROM: GCC's code makes the frame with "addiu sp, sp, -N" and saves ra and the
s registers with "sw REG, OFF(sp)".
"""
import bisect
import re
import struct

# The main code segment: ROM offset 0x1000 holds address 0x80100000 (gloveworld's glover.yaml).
CODE_ROM_DELTA = 0x80100000 - 0x1000

REG_NAMES = {4: 'a0', 5: 'a1', 6: 'a2', 7: 'a3', 16: 's0', 17: 's1', 18: 's2', 19: 's3', 20: 's4', 21: 's5',
             22: 's6', 23: 's7', 30: 's8', 31: 'ra'}
CALLEE_SAVED = ['s0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 's8']


class Unwinder:
    def __init__(self, rom_path, symbol_addrs):
        self.rom = open(rom_path, 'rb').read()
        funcs = []
        for line in open(symbol_addrs):
            m = re.match(r'\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+);(.*)', line)
            if m and 'type:func' in m.group(3):
                funcs.append((int(m.group(2), 16), m.group(1)))
        funcs.sort()
        self.starts = [a for a, _ in funcs]
        self.names = [n for _, n in funcs]

    def word(self, addr):
        off = addr - CODE_ROM_DELTA
        return struct.unpack_from('>I', self.rom, off)[0]

    def function(self, pc):
        i = bisect.bisect_right(self.starts, pc) - 1
        return (self.starts[i], self.names[i]) if i >= 0 else (None, None)

    def prologue(self, start, pc):
        """The frame size and the saved registers {name: offset} of the function at start, counting only the
        instructions before pc (those that ran)."""
        size = 0
        saved = {}
        addr = start
        while addr < pc and addr < start + 0x100:
            ins = self.word(addr)
            op, rs, rt, imm = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, ins & 0xFFFF
            if op == 9 and rs == 29 and rt == 29 and imm & 0x8000:     # addiu sp, sp, -N
                size = 0x10000 - imm
            elif op == 0x2B and rs == 29 and rt in REG_NAMES:          # sw REG, OFF(sp)
                saved.setdefault(REG_NAMES[rt], imm)
            elif op in (2, 3) or (op == 0 and (ins & 0x3F) in (8, 9)):  # j, jal, jr, jalr: the prologue is over
                break
            addr += 4
        return size, saved

    def arg0(self, frame, stack_words, sp0):
        """The first argument (a0) of the call that frame is in, when the function keeps it: copied into an s
        register ("move sX, a0") that no later instruction before the frame's pc changes, or stored on the stack
        ("sw a0, OFF(sp)"). Else None."""
        name, pc, sp, regs, start = frame
        addr = start
        while addr < pc and addr < start + 0x100:
            ins = self.word(addr)
            op, rs, rt, rd, fn = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31, ins & 0x3F
            if op == 0 and fn in (0x21, 0x25) and rs == 4 and rt == 0 and rd in REG_NAMES:
                if any(written(self.word(a)) == rd for a in range(addr + 4, pc, 4)):
                    return None
                return regs.get(REG_NAMES[rd])
            if op == 0x2B and rs == 29 and rt == 4:
                i = (sp + (ins & 0xFFFF) - sp0) // 4
                return stack_words[i] if 0 <= i < len(stack_words) else None
            addr += 4
        return None

    def call_arg0(self, caller, stack_words, sp0):
        """The first argument (a0) that the caller passed to the call it is in, from the few instructions before
        the call and in its delay slot, when they compute it from the caller's s registers, sp, constants and
        stack words. Else None."""
        name, pc, sp, regs, start = caller
        first = pc
        while first - 4 >= max(start, pc - 32) and not is_branch(self.word(first - 4)):
            first -= 4
        code = [self.word(a) for a in range(first, pc + 8, 4) if a != pc]
        known = {0: 0, 29: sp}
        for num, rname in REG_NAMES.items():
            if 16 <= num <= 30 and regs.get(rname) is not None:
                known[num] = regs[rname]
        # An s register written in the window had another value before that write.
        for ins in code:
            r = written(ins)
            if r is not None:
                known.pop(r, None)
        for ins in code:
            r = written(ins)
            if r is None:
                continue
            known[r] = evaluate(ins, known, lambda off: stack_word(stack_words, sp0, sp + off))
            if known[r] is None:
                del known[r]
        return known.get(4)

    def calls(self, name):
        """The calls (jal) in a function, in order: (address, the called function's name)."""
        i = self.names.index(name)
        start = self.starts[i]
        end = self.starts[i + 1] if i + 1 < len(self.starts) else start + 0x1000
        out = []
        for addr in range(start, end, 4):
            ins = self.word(addr)
            if ins >> 26 == 3:
                target = 0x80000000 | ((ins & 0x3FFFFFF) << 2)
                tstart, tname = self.function(target)
                if tstart == target:
                    out.append((addr, tname))
        return out

    def bounds(self, name):
        """The function's first address and the address after its end."""
        i = self.names.index(name)
        return self.starts[i], self.starts[i + 1] if i + 1 < len(self.starts) else self.starts[i] + 0x1000

    def loop_start(self, name, pc):
        """The lowest address that a backward jump after pc reaches, when that is at or before pc (pc is inside a
        loop of the function), else None."""
        start, end = self.bounds(name)
        low = None
        for addr in range(pc + 4, end, 4):
            ins = self.word(addr)
            op = ins >> 26
            target = None
            if op in (1, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
                off = ins & 0xFFFF
                target = addr + 4 + ((off - 0x10000 if off & 0x8000 else off) << 2)
            elif op == 2:
                target = (addr & 0xF0000000) | ((ins & 0x3FFFFFF) << 2)
            if target is not None and start <= target <= pc:
                low = target if low is None else min(low, target)
        return low

    def graph(self, name):
        """The function's control flow: (blocks, successors), or None when a jump through a register other than ra
        (a switch table) hides some edges. blocks maps a block's first address to the address after its last
        instruction (a branch's delay slot is in the branch's block); successors maps it to the first addresses
        of the blocks that can follow."""
        memo = self.__dict__.setdefault('_graphs', {})
        if name in memo:
            return memo[name]
        start, end = self.bounds(name)
        leaders = {start}
        jumps = {}  # the address of a branch or jump: (its targets, whether it can fall through)
        for addr in range(start, end, 4):
            ins = self.word(addr)
            op, rs, rt, fn = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, ins & 0x3F
            off = ins & 0xFFFF
            rel = addr + 4 + ((off - 0x10000 if off & 0x8000 else off) << 2)
            if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or op == 1 or (op == 0x11 and rs == 8):
                uncond = op == 4 and rs == 0 and rt == 0
                jumps[addr] = ([rel], not uncond)
            elif op == 2:
                jumps[addr] = ([(addr & 0xF0000000) | ((ins & 0x3FFFFFF) << 2)], False)
            elif op == 0 and fn == 8:
                if rs != 31:
                    memo[name] = None
                    return None
                jumps[addr] = ([], False)
            else:
                continue
            for t in jumps[addr][0]:
                if start <= t < end:
                    leaders.add(t)
            if addr + 8 < end:
                leaders.add(addr + 8)
        order = sorted(leaders)
        blocks = {}
        succ = {}
        for i, b in enumerate(order):
            e = order[i + 1] if i + 1 < len(order) else end
            blocks[b] = e
            last = [a for a in jumps if b <= a < e]
            if last:
                targets, falls = jumps[last[-1]]
                succ[b] = [t for t in targets if start <= t < end] + ([e] if falls and e < end else [])
            else:
                succ[b] = [e] if e < end else []
        memo[name] = (blocks, succ)
        return memo[name]

    @staticmethod
    def block_of(blocks, addr):
        for b, e in blocks.items():
            if b <= addr < e:
                return b
        return None

    @staticmethod
    def reach(succ, frm, back=False):
        """The blocks reachable from the blocks frm (following edges backward with back)."""
        edges = succ
        if back:
            edges = {}
            for b, ts in succ.items():
                for t in ts:
                    edges.setdefault(t, []).append(b)
        seen = set()
        todo = list(frm)
        while todo:
            b = todo.pop()
            for t in edges.get(b, []):
                if t not in seen:
                    seen.add(t)
                    todo.append(t)
        return seen

    def last_call_before(self, name, pc, targets):
        """The call that ran last on every path from the function's start to pc, of the calls that may reach
        targets (reaches): (its address, the called function), or None when no such call lies on every path, or
        'unknown' (no control flow, or that call is in a loop that also holds pc). A call that only some paths
        pass may have run after it."""
        g = self.graph(name)
        if g is None:
            return 'unknown'
        blocks, succ = g
        start, _ = self.bounds(name)
        p = self.block_of(blocks, pc)
        before = self.reach(succ, [p], back=True) | {p}
        best = None
        for a, n in self.calls(name):
            c = self.block_of(blocks, a)
            if c not in before or (c == p and a >= pc) or not self.reaches(n, targets):
                continue
            # On every path: without c, pc's block cannot be reached from the start.
            if c != p:
                cut = {k: [t for t in v if t != c] for k, v in succ.items() if k != c}
                if start == p or p in self.reach(cut, [start]):
                    continue
            if best is None or a > best[0]:
                best = (a, n)
        if best is None:
            return None
        c = self.block_of(blocks, best[0])
        if c in self.reach(succ, [c]) and p in self.reach(succ, [c]) and c in self.reach(succ, [p]) | {p}:
            return 'unknown'
        return best

    def call_may_follow(self, name, pc, targets):
        """1 when a call that may reach targets can run in the function after pc, before it returns."""
        g = self.graph(name)
        if g is None:
            return True
        blocks, succ = g
        p = self.block_of(blocks, pc)
        after = self.reach(succ, [p])
        for a, n in self.calls(name):
            b = self.block_of(blocks, a)
            if (b in after or (b == p and a > pc)) and self.reaches(n, targets):
                return True
        return False

    def has_indirect_call(self, name):
        """1 when the function calls through a register (jalr): its callees are unknown."""
        start, end = self.bounds(name)
        return any(self.word(a) >> 26 == 0 and self.word(a) & 0x3F == 9 for a in range(start, end, 4))

    def reaches(self, name, targets, seen=None):
        """1 when the function is one of targets, or may call one, directly or through other calls. A call through
        a register counts as may."""
        memo = self.__dict__.setdefault('_reach', {})
        key = (name, frozenset(targets))
        if key in memo:
            return memo[key]
        top = seen is None
        seen = set() if top else seen
        if name in targets:
            return True
        if name in seen:
            return False
        seen.add(name)
        result = self.has_indirect_call(name) or any(self.reaches(callee, targets, seen)
                                                     for _, callee in self.calls(name))
        # Inside a cycle, a False can come from a call that was cut short: only the outermost answer is kept.
        if top or result:
            memo[key] = result
        return result

    def unwind(self, regs, stack_words, stop='RunGame'):
        sp0 = regs['sp']
        regs = dict(regs)
        frames = []
        pc = regs['pc']
        sp = sp0
        for _ in range(32):
            start, name = self.function(pc)
            if start is None:
                break
            frames.append((name, pc, sp, dict(regs), start))
            if name == stop:
                break
            size, saved = self.prologue(start, pc)

            def at(off):
                i = (sp + off - sp0) // 4
                return stack_words[i] if 0 <= i < len(stack_words) else None

            for reg, off in saved.items():
                v = at(off)
                if v is None:
                    return frames
                regs[reg] = v
            ra = regs['ra']
            if ra < 0x80000000 or ra >= 0x80800000:
                break
            sp += size
            regs['sp'] = sp
            # The call is the instruction before the delay slot.
            pc = ra - 8
            regs['pc'] = pc
        return frames


def written(ins):
    """The register an instruction writes, or None."""
    op, rt, rd, fn = ins >> 26, (ins >> 16) & 31, (ins >> 11) & 31, ins & 0x3F
    if op == 0:
        if fn in (8, 0x18, 0x19, 0x1A, 0x1B) or rd == 0:  # jr, mult and div write no register here
            return None
        return rd
    if op in (8, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x20, 0x21, 0x23, 0x24, 0x25) and rt != 0:
        return rt
    if op == 3:  # jal writes ra
        return 31
    return None


def is_branch(ins):
    op = ins >> 26
    return op in (1, 2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 0 and ins & 0x3F in (8, 9))


def stack_word(stack_words, sp0, addr):
    i = (addr - sp0) // 4
    return stack_words[i] if 0 <= i < len(stack_words) and addr % 4 == 0 else None


def evaluate(ins, known, load):
    """The value an instruction writes, from the known registers, or None."""
    op, rs, rt, fn, sa = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, ins & 0x3F, (ins >> 6) & 31
    imm = ins & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    a, b = known.get(rs), known.get(rt)
    m = 0xFFFFFFFF
    if op == 0:
        if fn == 0x21 and a is not None and b is not None:
            return (a + b) & m
        if fn == 0x23 and a is not None and b is not None:
            return (a - b) & m
        if fn == 0x25 and a is not None and b is not None:
            return a | b
        if fn == 0x24 and a is not None and b is not None:
            return a & b
        if fn == 0 and b is not None:
            return (b << sa) & m
        if fn == 2 and b is not None:
            return b >> sa
        return None
    if op == 9 and a is not None:
        return (a + simm) & m
    if op == 0xD and a is not None:
        return a | imm
    if op == 0xC and a is not None:
        return a & imm
    if op == 0xF:
        return imm << 16
    if op == 0x23 and rs == 29 and a is not None:
        return load(simm)
    return None
