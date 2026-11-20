"""Compare the machine code of two trees of object files, function by function.

Each object in tree A is compared with the object at the same path in tree B. The disassembly is normalised: addresses
and relocation offsets are dropped, and on ELF a constant that the code reads from .rodata is replaced by its bytes. A
function that differs is then checked again on its floating-point operations and its calls only. A difference there
can change the logic and must be explained; a difference in register choice or block order cannot.

Usage: python3 tools/cmpcode.py A B [--objdump PROG] [--list]      (needs pyelftools for ELF objects)
  --objdump  the objdump to use (i686-w64-mingw32-objdump for MinGW objects; the default reads the first object)
  --list     list every function that differs, not only those whose operations or calls differ
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

FPOPS = re.compile(r"^(add|sub|mul|div|sqrt|min|max)(ss|sd)$|^cvt|^ucomis|^comis")


def rodata(path):
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        return {}
    with open(path, "rb") as f:
        if f.read(4) != b"\x7fELF":
            return {}
        f.seek(0)
        e = ELFFile(f)
        return {s.name: s.data() for s in e.iter_sections()
                if s.name.startswith(".rodata") and s["sh_type"] != "SHT_NOBITS"}


def functions(path, objdump):
    secs = rodata(path)
    out = subprocess.run([objdump, "-d", "-r", "--no-show-raw-insn", "-w", str(path)], capture_output=True,
                         text=True).stdout
    fs = collections.OrderedDict()
    cur = None
    for line in out.split("\n"):
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if m:
            cur = m.group(1)
            fs[cur] = []
            continue
        if cur is None or not line.strip() or "\t" not in line:
            continue
        line = re.sub(r"^\s*[0-9a-f]+:\s*", "", line)
        line = re.sub(r"\b[0-9a-f]+ <", "<", line)
        line = re.sub(r"\s[0-9a-f]+: (\w+)", r" \1", line)
        m = re.search(r"R_386_32\s+(\.rodata\S*)$", line)
        if m:
            sec = m.group(1)
            if ".str" in sec:
                line = re.sub(r"\$0x[0-9a-f]+", "$STR", line).replace(sec, ".rodata.str")
            else:
                d = re.search(r"(?:\$)?0x([0-9a-f]+)", line)
                if d and sec in secs:
                    off = int(d.group(1), 16)
                    size = 16 if sec.endswith("cst16") else 8 if sec.endswith("cst8") else 4
                    val = "JUMPTABLE" if line.startswith("jmp") else secs[sec][off:off + size].hex()
                    line = line.replace(d.group(0), "K[" + val + "]", 1).replace(sec, ".rodata")
        fs[cur].append(re.sub(r"<[^>]*>", "", line))
    return fs


def operations(lines):
    c = collections.Counter()
    for line in lines:
        op = line.split()[0] if line.split() else ""
        rel = re.search(r"(?:R_386_PC32|DISP32)\s+(\S+)", line)
        if rel and op in ("call", "jmp"):
            c["call " + rel.group(1)] += 1
        if FPOPS.match(op):
            k = re.search(r"K\[([0-9a-f]+)\]", line)
            c[op + (" K" + k.group(1) if k else "")] += 1
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    a, b = pathlib.Path(args.a), pathlib.Path(args.b)
    same = differ = opsdiff = 0
    for oa in sorted(a.rglob("*.o")):
        rel = oa.relative_to(a)
        ob = b / rel
        if not ob.exists():
            print(f"{rel}: only in A")
            continue
        fa, fb = functions(oa, args.objdump), functions(ob, args.objdump)
        for n in fa:
            if n not in fb:
                print(f"{rel}: {n} only in A")
                continue
            if fa[n] == fb[n]:
                same += 1
                continue
            differ += 1
            ca, cb = operations(fa[n]), operations(fb[n])
            if ca != cb:
                opsdiff += 1
                print(f"OPS {rel}: {n}  A: {dict(ca - cb)}  B: {dict(cb - ca)}")
            elif args.list:
                print(f"{rel}: {n}")
        for n in fb:
            if n not in fa:
                print(f"{rel}: {n} only in B")
    print(f"{same} functions the same, {differ} differ, {opsdiff} of them in floating-point operations or calls")
    return 1 if opsdiff else 0


if __name__ == "__main__":
    sys.exit(main())
