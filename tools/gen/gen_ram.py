"""Generate the port's copy of the N64 memory layout and the tables that fill it from the player's ROM.

Writes to build/port/gen:
  ram.s           every data, rodata and bss symbol of the game at its N64 offset inside `port_ram`, an 8 MB block
                  that stands for RDRAM (0x80000000-0x80800000). The game's data keeps its original layout.
  port_win.h      the bytes reserved for each host object in port_win, at the end of ram.S.
  ramtab.c        what to do after the main segment is copied from the ROM into port_ram: the byte swaps (from the
                  C types in types.json, else from splat's data directives), the words that hold pointers, and the
                  functions that data points to.
  port_romsyms.h  ROM asset symbols (xxxRomStart) become pointers that are set at start-up. A ROM rebuilt by the
  romsyms.c       map editor moves the assets and patches the code that loads their addresses. The port reads
                  each address from those instructions in the player's ROM.
"""
import bisect
import json
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile

import portlib as P

ELF = P.ROOT / "build" / "NGVE.elf"

# Data that holds byte streams across many labels, where splat mistakes some byte runs for addresses: libmus's
# sound effect streams (D_801E7760 to the end of D_801E9C9C; docs/audio-libmus-map.md, section 12.3).
BYTE_STREAMS = [(0x801E7760, 0x801E9CB4)]


def elf_functions():
    """Address -> name of every function in the decomp's ELF."""
    funcs = {}
    with open(ELF, "rb") as f:
        elf = ELFFile(f)
        text = elf.get_section_by_name(".text")
        symtab = elf.get_section_by_name(".symtab")
        for s in symtab.iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC" and s.name:
                funcs.setdefault(s["st_value"], s.name)
    return funcs


def elf_data_symbols():
    """Name -> (address, size) of the ELF's global symbols that are not functions."""
    out = {}
    with open(ELF, "rb") as f:
        elf = ELFFile(f)
        for s in elf.get_section_by_name(".symtab").iter_symbols():
            data_like = s["st_info"]["type"] in ("STT_OBJECT", "STT_NOTYPE") or re.search(r"(Text|Data)(Start|End)$", s.name)
            if (s.name and data_like and s["st_shndx"] != "SHN_UNDEF"
                    and s["st_info"]["bind"] == "STB_GLOBAL" and not s.name.startswith((".", "$"))):
                out[s.name] = (s["st_value"], s["st_size"])
    return out


def text_range(funcs, labels):
    """The vaddr range of the main segment's code: from its first function to the start of .data."""
    lo = min(a for a in funcs if a >= P.MAIN_VRAM + 0x40)
    hi = min(l.addr for l in labels if l.section == ".data")
    return lo, hi


def rom_symbol_sites(romsyms):
    """For each ROM symbol, the ROM offsets of a lui/addiu pair that loads its address.

    Links the decomp's objects again with --emit-relocs to see where each %hi/%lo relocation landed."""
    out = P.OUT / "NGVE_relocs.elf"
    ld = ["mips-linux-gnu-ld", "-T", "NGVE.ld", "-T", "undefined_syms_auto.txt", "-T", "undefined_funcs_auto.txt",
          "-T", "symbols.ld", "--no-check-sections", "--emit-relocs", "-o", str(out)]
    subprocess.run(ld, check=True, cwd=P.ROOT)
    sites = {}
    with open(out, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        for sec in elf.iter_sections():
            if not sec.name.startswith(".rel"):
                continue
            target = elf.get_section(sec["sh_info"])
            base = target["sh_addr"]
            hi = {}
            for r in sec.iter_relocations():
                sym = symtab.get_symbol(r["r_info_sym"])
                if sym.name not in romsyms:
                    continue
                addr = base + r["r_offset"] if r["r_offset"] < base else r["r_offset"]
                t = r["r_info_type"]
                if t == 5:  # R_MIPS_HI16
                    hi[sym.name] = addr
                elif t == 6 and sym.name in hi:  # R_MIPS_LO16, paired with the last HI16
                    sites.setdefault(sym.name, []).append((hi[sym.name], addr))
    return sites


class ByteMap:
    """For each byte offset in the ROM-backed part of port_ram, the scalar unit that starts there."""

    def __init__(self):
        self.units = {}  # offset -> (size, kind)
        self.starts = []

    def put(self, off, size, kind):
        # Remove units that overlap [off, off + size).
        for o in range(off - 7, off + size):
            u = self.units.get(o)
            if u and o + u[0] > off and o < off + size:
                del self.units[o]
        self.units[off] = (size, kind)


def label_directive_units(lab):
    """Units of a label from its splat directives: [(vaddr, size, kind, operand)]."""
    out = []
    for vaddr, d, operand in lab.items:
        size, n, kind = P.directive_units(d, operand)
        if kind in ("b", "s"):
            continue
        ops = P.split_operands(operand)
        for i in range(n):
            out.append((vaddr + i * size, size, kind, ops[i] if i < len(ops) else ""))
    return out


def apply_type(bm, base, lay, label_size):
    units = lay["units"]
    size = lay["size"]
    if lay["open"] and size:
        reps = max(1, label_size // size)
    else:
        reps = 1
    for r in range(reps):
        for o, s, k in units:
            bm.put(base + r * size + o, s, k)


# The host objects in port_win, after port_hw (see ram.S): name and the bytes reserved for it. These are the sound
# player's objects that its memory in port_ram points to (src/audio/mus/mus_internal.h). port_win.h gives each size
# to the C code, which checks that the object fits.
WINDOW_OBJECTS = [
    ("musGlobals", 0x50),     # ALGlobals: the synthesizer; alGlobals and musReverb.synth point to it
    ("musPlayer", 0x20),      # ALPlayer: the synthesizer's list of players starts here
    ("musHeap", 0x10),        # ALHeap: the synthesizer keeps a pointer to it
    ("musDmaState", 0x10),    # MusDmaState: the load filters keep a pointer to it
    ("musBuffer", 0x2000),    # the output of the audio command lists (A_SAVEBUFF)
    ("musDropBuffer", 0x2000),
    ("musDmaZeros", 0x1000),  # what a request for wave data that the port cannot serve reads (A_LOADBUFF)
]


def main():
    types = json.loads((P.GEN / "types.json").read_text())
    tvars = types["vars"]
    funcs_sig = types["funcs"]
    labels = P.all_data_labels()
    fixed = P.fixed_symbols()
    funcs = elf_functions()
    tlo, thi = text_range(funcs, labels)
    ram_lo, ram_hi = P.RAM_BASE, P.RAM_BASE + P.RAM_SIZE

    # ---- Symbols in port_ram.
    defined_in_c = set(funcs_sig)
    for path in P.game_sources():
        defined_in_c |= set(re.findall(r"^(?:extern )?const \w+ (\w+)\[\]", path.read_text(), re.M))
    syms = {}
    for lab in labels:
        size = lab.size
        if size is None:
            size = sum(P.directive_units(d, o)[0] * P.directive_units(d, o)[1] for _, d, o in lab.items)
        syms[lab.name] = (lab.addr, size)
    for name, addr in fixed.items():
        if name not in syms:
            syms[name] = (addr, 1)
    # Labels whose data is an .incbin (the microcode) have no items; the ELF has their addresses.
    for name, (addr, size) in elf_data_symbols().items():
        if name not in syms:
            syms[name] = (addr, size or 1)
    romsyms = {n: v[0] for n, v in syms.items() if 0xB0000000 <= v[0] < 0xC0000000}
    hw = {n: v for n, v in syms.items() if not (ram_lo <= v[0] < ram_hi) and n not in romsyms}
    ram = {n: v for n, v in syms.items() if ram_lo <= v[0] < ram_hi and n not in defined_in_c and not n.startswith(".")}

    lines = ["/* Generated by tools/gen/gen_ram.py. The game's data at its N64 offsets inside port_ram. */",
             # 32-bit Windows and Apple's Mach-O put an underscore before each C name.
             "#if (defined(_WIN32) && !defined(_WIN64)) || defined(__APPLE__)",
             "#define SYM(x) _##x",
             "#else",
             "#define SYM(x) x",
             "#endif",
             "\t.bss", "\t.globl\tSYM(port_ram)",
             # On Linux port_ram starts at a 32 MB boundary, which puts it at the same address in every build. The
             # memory hash of the tests (platform.c) reads values in port_ram's range as offsets into it, so the
             # hashes of two builds agree only when port_ram has the same address in both. Windows keeps 4 KB:
             # its object format has no alignment that large. Apple's libraries keep 16 KB, the page size of its
             # arm64 systems.
             "#if defined(__ELF__)", "\t.p2align\t25", "#elif defined(__APPLE__)", "\t.p2align\t14", "#else",
             "\t.p2align\t12", "#endif",
             # The displaced test build (docs/port64.md) puts 32 MB of guard before port_ram: the range of the words
             # that the game stores for pointers. Any use of such a word as a pointer faults there.
             "#if defined(PORT_PTR_DISPLACED)", "\t.globl\tSYM(port_ram_guard)", "SYM(port_ram_guard):",
             "\t.skip\t0x2000000", "#endif",
             "SYM(port_ram):"]
    pos = 0
    for name, (addr, size) in sorted(ram.items(), key=lambda kv: (kv[1][0], kv[0])):
        off = addr - ram_lo
        if off > pos:
            lines.append(f"\t.skip\t{off - pos}")
            pos = off
        lines += [f"\t.globl\tSYM({name})", f"SYM({name}):"]
    lines.append(f"\t.skip\t{P.RAM_ALLOC - pos}")
    # Hardware registers and other addresses outside RDRAM: never used by the port, but referenced.
    lines += ["\t.p2align\t4", "\t.globl\tSYM(port_hw)", "SYM(port_hw):"]
    for name in sorted(hw):
        lines += [f"\t.globl\tSYM({name})", f"SYM({name}):", "\t.skip\t16"]
    # The memory hashes (platform.c, the brute forcer's engine) end here. After it come the host objects that the
    # game's memory points to (port_win): a Ptr32 can hold a pointer to them (src/port/ptr32.h).
    lines += ["\t.globl\tSYM(port_hw_end)", "SYM(port_hw_end):", "\t.p2align\t4", "\t.globl\tSYM(port_win)",
              "SYM(port_win):"]
    for name, size in WINDOW_OBJECTS:
        lines += [f"\t.globl\tSYM({name})", f"SYM({name}):", f"\t.skip\t{size}", "\t.p2align\t4"]
    lines += ["\t.globl\tSYM(port_win_end)", "SYM(port_win_end):"]
    lines.append("#if defined(__ELF__)")
    lines.append('\t.section\t.note.GNU-stack,"",@progbits')
    lines.append("#endif")
    (P.GEN / "ram.S").write_text("\n".join(lines) + "\n")
    w = ["/* Generated by tools/gen/gen_ram.py. The host objects in port_win (ram.S) and the bytes reserved for each. */",
         "#ifndef PORT_WIN_H", "#define PORT_WIN_H"]
    w += [f"#define PORT_WIN_SIZE_{name} {size:#x}" for name, size in WINDOW_OBJECTS]
    w.append("#endif")
    (P.GEN / "port_win.h").write_text("\n".join(w) + "\n")

    # ---- Byte map of the ROM-backed labels (data and rodata).
    bm = ByteMap()
    loaded = [l for l in labels if l.section != ".bss" and ram_lo <= l.addr < ram_hi and not (tlo <= l.addr < thi)]
    ptr_hint = {}  # vaddr -> symbol name the directive refers to
    for lab in loaded:
        for vaddr, size, kind, operand in label_directive_units(lab):
            bm.put(vaddr - ram_lo, size, kind)
            if size == 4 and re.match(r"[A-Za-z_]", operand):
                ptr_hint[vaddr] = operand
    # splat splits a table wherever something points inside it. An array declared without a size (extern T x[])
    # therefore runs on over the following labels that have no C type of their own.
    typed = 0
    typed_ranges = []
    ordered = sorted(loaded, key=lambda l: l.addr)
    for i, lab in enumerate(ordered):
        lay = tvars.get(lab.name)
        if not lay:
            continue
        extent = lab.size or lay["size"]
        if lay["open"]:
            end = lab.addr + extent
            for nxt in ordered[i + 1:]:
                if nxt.addr != end or nxt.name in tvars or nxt.section != lab.section:
                    break
                end = nxt.addr + nxt.size
            extent = end - lab.addr
        apply_type(bm, lab.addr - ram_lo, lay, extent)
        typed_ranges.append((lab.addr, lab.addr + (lab.size or lay["size"])))
        typed += 1
    for name, addr in fixed.items():
        lay = tvars.get(name)
        if lay and ram_lo <= addr < ram_hi:
            apply_type(bm, addr - ram_lo, lay, lay["size"])

    # Words that hold pointers.
    with open(P.ROOT / "baserom.us.z64", "rb") as f:
        rom = f.read()

    def word_at(vaddr):
        r = vaddr - P.MAIN_VRAM + P.MAIN_ROM
        return int.from_bytes(rom[r:r + 4], "big")

    data_lo = min(l.addr for l in loaded)
    data_hi = max(l.addr + (l.size or 0) for l in labels if l.section == ".bss")
    known_lo = sorted(a for n, (a, s) in ram.items() if a < P.MAIN_VRAM)
    ptrs = []
    fnptrs = {}
    heuristic = []
    unresolved = []
    # splat writes a word as a symbol reference when its value looks like an address. A C type says better: inside
    # a label that the C declares with a non-pointer type, and inside the known byte streams, the hint is ignored.
    typed_ranges.extend(BYTE_STREAMS)
    typed_ranges.sort()
    starts = [a for a, b in typed_ranges]

    def in_typed(vaddr):
        i = bisect.bisect_right(starts, vaddr) - 1
        return i >= 0 and typed_ranges[i][0] <= vaddr < typed_ranges[i][1]

    for vaddr, op in list(ptr_hint.items()):
        if in_typed(vaddr) and bm.units.get(vaddr - ram_lo, (0, ""))[1] not in ("p", "fp"):
            del ptr_hint[vaddr]
            continue
        bm.put(vaddr - ram_lo, 4, "p")
    for off, (size, kind) in sorted(bm.units.items()):
        if size != 4:
            continue
        vaddr = off + ram_lo
        v = word_at(vaddr)
        if kind == "i" and vaddr not in ptr_hint:
            # An untyped word: a pointer only if it points into the game's code, data or bss, or at a buffer
            # in low memory.
            if not ((P.MAIN_VRAM <= v < data_hi) or v in known_lo):
                continue
            heuristic.append((vaddr, v))
        elif kind not in ("p", "fp", "i"):
            continue
        if v == 0:
            continue
        if not (ram_lo <= v < ram_hi):
            if kind in ("p", "fp"):
                unresolved.append((vaddr, v))
            continue
        if tlo <= v < thi:
            name = funcs.get(v)
            if name and name in funcs_sig:
                fnptrs[off] = name
            else:
                unresolved.append((vaddr, v))
            continue
        ptrs.append(off)
        bm.units[off] = (4, "p")

    # ---- ramtab.c
    runs = {2: [], 4: [], 8: []}
    for off, (size, kind) in sorted(bm.units.items()):
        if size not in runs:
            continue
        r = runs[size]
        if r and r[-1][0] + r[-1][1] * size == off:
            r[-1][1] += 1
        else:
            r.append([off, 1])
    c = ["/* Generated by tools/gen/gen_ram.py: how to convert the game's data after it is copied from the ROM. */",
         '#include "port.h"', ""]
    for size, r in runs.items():
        c.append(f"const u32 port_swap{size}[] = {{")
        c += [f"    {o:#x}, {n}," for o, n in r]
        c.append("    0, 0 };")
    c.append("const u32 port_ptrs[] = {")
    c += [f"    {o:#x}," for o in ptrs]
    c.append("    0 };")
    names = sorted(set(fnptrs.values()))
    for n in names:
        sig = funcs_sig[n]
        params = ", ".join(sig["params"]) or "void"
        if sig["varargs"]:
            params += ", ..."
        c.append(f"{sig['ret']} {n}({params});")
    # The functions in order of first use. A slot holds its function's index; the loader writes the word for it
    # (PORT_FN_WORD in ptr32.h).
    table = []
    for o, n in sorted(fnptrs.items()):
        if n not in table:
            table.append(n)
    c.append("const PortFn port_fn_table[] = {")
    c += [f"    (PortFn){n}," for n in table]
    c.append("};")
    c.append(f"const unsigned int port_fn_count = {len(table)};")
    c.append("const PortFnReloc port_fnptrs[] = {")
    c += [f"    {{ {o:#x}, {table.index(n)} }}, /* {n} */" for o, n in sorted(fnptrs.items())]
    c.append("    { 0, 0 } };")
    c.append(f"const u32 port_main_rom = {P.MAIN_ROM:#x};")
    c.append(f"const u32 port_main_vram = {P.MAIN_VRAM:#x};")
    c.append(f"const u32 port_main_size = {max(l.addr + (l.size or 0) for l in loaded) - P.MAIN_VRAM:#x};")
    (P.GEN / "ramtab.c").write_text("\n".join(c) + "\n")

    # ---- Layouts of the structures the bank loaders convert.
    sc = ["/* Generated by tools/gen/gen_ram.py: the scalar fields of structures read from the ROM. */",
          '#include "port.h"', ""]
    for name, lay in sorted(types["structs"].items()):
        units = [(o, s) for o, s, k in lay["units"] if s > 1]
        sc.append(f"static const u16 units_{name}[] = {{ " + ", ".join(f"{o}, {s}" for o, s in units) + " };")
        sc.append(f"const PortLayout port_layout_{name} = {{ {lay['size']}, {len(units)}, units_{name} }};")
    (P.GEN / "layouts.c").write_text("\n".join(sc) + "\n")

    # ---- ROM symbols.
    sites = rom_symbol_sites(set(romsyms))
    h = ["/* Generated by tools/gen/gen_ram.py. ROM asset addresses, read from the player's ROM at start-up. */",
         "#ifndef PORT_ROMSYMS_H", "#define PORT_ROMSYMS_H"]
    for n in sorted(romsyms):
        h.append(f"#define {n} (*port_rom_{n})")
    h.append("#endif")
    (P.GEN / "port_romsyms.h").write_text("\n".join(h) + "\n")
    rc = ["/* Generated by tools/gen/gen_ram.py. */", '#include "port.h"', ""]
    for n in sorted(romsyms):
        rc.append(f"u8 (*port_rom_{n})[1];")
    rc.append("const PortRomSym port_romsyms[] = {")
    for n in sorted(romsyms):
        s = sites.get(n, [])
        if s:
            hi, lo = s[0]
            rc.append(f"    {{ (void **)&port_rom_{n}, {romsyms[n]:#x}, {hi - P.MAIN_VRAM + P.MAIN_ROM:#x}, "
                      f"{lo - P.MAIN_VRAM + P.MAIN_ROM:#x} }},")
        else:
            rc.append(f"    {{ (void **)&port_rom_{n}, {romsyms[n]:#x}, 0, 0 }},")
    rc.append("    { 0, 0, 0, 0 } };")
    (P.GEN / "romsyms.c").write_text("\n".join(rc) + "\n")

    print(f"ram: {len(ram)} symbols, {len(hw)} outside RDRAM, {len(romsyms)} ROM symbols "
          f"({sum(1 for n in romsyms if n in sites)} with code sites)")
    print(f"data: {len(loaded)} labels, {typed} with C types; swaps: "
          + ", ".join(f"{len(r)} runs of {s}" for s, r in runs.items()))
    print(f"pointers: {len(ptrs)} data, {len(fnptrs)} functions, {len(heuristic)} by value only, "
          f"{len(unresolved)} unresolved")
    log = [f"{a:08X} -> {v:08X} heuristic" for a, v in heuristic] + [f"{a:08X} -> {v:08X} unresolved" for a, v in unresolved]
    (P.GEN / "ram.log").write_text("\n".join(log) + "\n")


if __name__ == "__main__":
    sys.exit(main())
