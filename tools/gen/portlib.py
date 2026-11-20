"""Shared settings for the table generators (gen_types.py, gen_ram.py).

The generators read the decomp's split ROM, ELF and symbol files from a gloveworld checkout that has run `make setup`
and `make` (the GLOVEWORLD environment variable, or ../gloveworld next to this repository), and this repository's
own game sources for the C types. They write gen/, which is committed: it holds offsets, sizes and names only.
"""
import os
import pathlib
import re
import subprocess

WM = pathlib.Path(__file__).resolve().parents[2]
ROOT = pathlib.Path(os.environ.get("GLOVEWORLD", WM.parent / "gloveworld")).resolve()
PORT = WM / "src" / "port"
OUT = WM / "build" / "gen-work"
GEN = WM / "gen"
GAME_SRC = WM / "src" / "game"

# The N64 address range that the port mirrors in one block of WebAssembly memory (all of RDRAM).
RAM_BASE = 0x80000000
RAM_SIZE = 0x800000
# port_ram also holds the heap, which runs 16 MB past the end of RDRAM (port_main.c, PORT_HEAP_END).
RAM_ALLOC = RAM_SIZE + 0x1000000

# The main code segment: ROM offset 0x1000 is loaded at 0x80100000.
MAIN_ROM = 0x1000
MAIN_VRAM = 0x80100000

DEFINES = ["-DM2CTX", "-D_LANGUAGE_C", "-DF3DEX_GBI", "-D_FINALROM", "-DNDEBUG", "-DPORT"]
INCLUDES = ["-I" + str(GEN), "-I" + str(PORT), "-I" + str(WM / "include"),
            "-I" + str(WM / "include" / "PR"), "-I" + str(GAME_SRC)]
# gnu89 accepts the decomp's implicit declarations. Characters are unsigned, as with the original compiler.
CFLAGS = ["-std=gnu89", "-funsigned-char", "-fno-builtin", "-w"]


def game_sources():
    return sorted(GAME_SRC.glob("*.c"))


def fix_strings(text):
    """GCC 2.7.2 accepts a raw newline inside a string literal; clang does not. Write it as \\n."""
    out = []
    i = 0
    n = len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            if c == "\\":
                out.append(text[i:i + 2])
                i += 2
                continue
            if c == '"':
                in_str = False
            out.append("\\n" if c == "\n" else c)
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j])
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i) + 2
            out.append(text[i:j])
            i = j
            continue
        if c == "'":
            m = re.match(r"'(\\.|[^\\'])*'", text[i:])
            out.append(m.group(0))
            i += len(m.group(0))
            continue
        if c == '"':
            in_str = True
        out.append(c)
        i += 1
    return "".join(out)


def write_fixed_sources():
    """The game's sources. They are compiled as they are (raw newlines in strings were written as \\n once)."""
    return game_sources()


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw)


# ---------------------------------------------------------------------------
# The decomp's data assembly (asm/data/*.s and the INCLUDE_RODATA files), written by splat.

ITEM_RE = re.compile(r"/\*\s*([0-9A-F]+)?\s+([0-9A-F]{8})(?:\s+[0-9A-F]+)?\s*\*/\s*\.(\w+)\s*(.*)$")


class Label:
    def __init__(self, name, section, path):
        self.name = name
        self.section = section
        self.path = path
        self.addr = None
        self.size = None
        self.items = []  # (vaddr, directive, operand text)

    def __repr__(self):
        return f"Label({self.name} {self.addr:#x} {self.size:#x} {self.section})"


def parse_data_asm(path):
    """Return the labels of one splat data file, with their items."""
    labels = []
    section = None
    sizes = {}
    cur = None
    for line in path.read_text().splitlines():
        s = line.strip()
        if s.startswith(".section"):
            section = s.split()[1].rstrip(",").strip('"')
        elif s.startswith("nonmatching"):
            parts = [p.strip() for p in s[len("nonmatching"):].split(",")]
            if len(parts) > 1:
                sizes[parts[0]] = int(parts[1], 0)
        elif s.startswith("dlabel ") or s.startswith("glabel "):
            cur = Label(s.split()[1], section, path)
            labels.append(cur)
        elif s.startswith("enddlabel") or s.startswith("endlabel"):
            cur = None
        elif cur is not None:
            m = ITEM_RE.search(s)
            if m:
                vaddr = int(m.group(2), 16)
                if cur.addr is None:
                    cur.addr = vaddr
                cur.items.append((vaddr, m.group(3), m.group(4).strip()))
    for lab in labels:
        if lab.name in sizes:
            lab.size = sizes[lab.name]
    return labels


def directive_units(directive, operand):
    """(element size, count, kind) for one data directive. kind: 'i' integer, 'f' float, 'b' bytes, 's' space."""
    if directive in ("word", "float"):
        n = len(split_operands(operand))
        return 4, n, "f" if directive == "float" else "i"
    if directive in ("short", "half"):
        return 2, len(split_operands(operand)), "i"
    if directive == "byte":
        return 1, len(split_operands(operand)), "b"
    if directive == "double":
        return 8, len(split_operands(operand)), "f"
    if directive in ("asciz", "ascii"):
        return 1, ascii_len(operand) + (1 if directive == "asciz" else 0), "b"
    if directive == "space":
        return 1, int(operand.split(",")[0], 0), "s"
    raise ValueError(f"unknown directive .{directive} {operand}")


def split_operands(operand):
    return [p.strip() for p in operand.split(",") if p.strip()]


def ascii_len(operand):
    s = operand.strip()
    assert s[0] == '"' and s[-1] == '"', s
    body = s[1:-1]
    n = 0
    i = 0
    while i < len(body):
        if body[i] == "\\":
            nxt = body[i + 1]
            if nxt in "01234567":
                j = i + 1
                while j < len(body) and j < i + 4 and body[j] in "01234567":
                    j += 1
                i = j
            elif nxt == "x":
                j = i + 2
                while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                    j += 1
                i = j
            else:
                i += 2
        else:
            i += 1
        n += 1
    return n


def all_data_labels():
    """Every label in the decomp's data, rodata and bss assembly."""
    files = sorted((ROOT / "asm" / "data").glob("*.s"))
    files += sorted(p for p in (ROOT / "asm" / "nonmatchings").glob("*/*.s"))
    labels = []
    for f in files:
        text = f.read_text()
        if "dlabel" not in text:
            continue
        labels.extend(parse_data_asm(f))
    labels = [l for l in labels if l.addr is not None]
    # A label without a size runs to the next label of its file; the last one to the end of its items. Summing
    # the items alone would miss .align padding.
    by_file = {}
    for l in labels:
        by_file.setdefault(l.path, []).append(l)
    for group in by_file.values():
        group.sort(key=lambda l: l.addr)
        for i, l in enumerate(group):
            if l.size is not None:
                continue
            if i + 1 < len(group):
                l.size = group[i + 1].addr - l.addr
            else:
                l.size = sum(directive_units(d, o)[0] * directive_units(d, o)[1] for _, d, o in l.items)
    return labels


def fixed_symbols():
    """Symbols the linker places at fixed addresses (undefined_syms_auto.txt, symbols.ld)."""
    syms = {}
    for name in ("undefined_syms_auto.txt", "symbols.ld"):
        for line in (ROOT / name).read_text().splitlines():
            m = re.match(r'\s*"?([A-Za-z_.][\w.]*)"?\s*=\s*(0x[0-9A-Fa-f]+)', line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    return syms
