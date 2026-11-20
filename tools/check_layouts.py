"""Compare every structure and union layout between the N64 and the PC build.

The port keeps the game's data at its N64 addresses and loads structures from the ROM, so each structure must have
the same size and field offsets on the PC as on the N64. This tool compiles each game source for big-endian MIPS
with debug information (as gen_types.py does), reads the PC build's debug information from build/linux, and lists
every structure whose layout differs.

Usage: python3 tools/check_layouts.py [--m64] [--audio]       (after `make`; needs clang and pyelftools)
  --m64    compare with a 64-bit compile of each file instead (g++ -m64, into build/layout64), and compare the
           size of every global in port_ram with its size in the 32-bit build. The 64-bit build (docs/port64.md) needs
           both counts at 0.
  --audio  compare the sound player's files (src/audio) too
"""
import pathlib
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MIPS_FLAGS = ["--target=mips-linux-gnu", "-std=gnu89", "-funsigned-char", "-w", "-g", "-O0", "-c",
              "-DM2CTX", "-D_LANGUAGE_C", "-DF3DEX_GBI", "-D_FINALROM", "-DNDEBUG", "-DPORT", "-DPORT_TYPES_ONLY",
              "-Igen", "-Isrc/port", "-Iinclude", "-Iinclude/PR", "-Isrc/game", "-include", "src/port/port_pre.h"]


def strip(die):
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        if "DW_AT_type" not in die.attributes:
            return None
        die = die.get_DIE_from_attribute("DW_AT_type")
    return die


# The 64-bit compile: the game's files are C++, as in the build; the sound player's are C until they become C++.
M64_FLAGS = ["-m64", "-funsigned-char", "-w", "-g", "-O0", "-c", "-DM2CTX", "-D_LANGUAGE_C", "-DF3DEX_GBI",
             "-D_FINALROM", "-DNDEBUG", "-DPORT", "-DPORT_AUDIO_MUS", "-Igen", "-Isrc/port", "-Iinclude",
             "-Iinclude/PR", "-Isrc/game", "-include", "src/port/port_pre.h"]
M64_CXX = ["g++", "-x", "c++", "-std=gnu++14", "-fpermissive", "-fno-exceptions", "-fno-rtti"]
M64_C = ["gcc", "-std=gnu89"]


def audio_is_cxx():
    """Whether the Makefile compiles the sound player as C++."""
    m = re.search(r"^\$\(BUILD\)/src/audio/%\.o:.*\n(?:\t.*\n)*?\t\$\((\w+)\) \$\((\w+)\)",
                  (ROOT / "Makefile").read_text(), re.M)
    return bool(m) and m.group(2) == "GAME_CXXFLAGS"


def type_size(die):
    """The size in bytes of a type, or None when the debug information does not give it (an array of unknown
    length)."""
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    if die is None:
        return None
    if die.tag == "DW_TAG_array_type":
        n = 1
        for sub in die.iter_children():
            if sub.tag != "DW_TAG_subrange_type":
                continue
            if "DW_AT_count" in sub.attributes:
                n *= sub.attributes["DW_AT_count"].value
            elif "DW_AT_upper_bound" in sub.attributes:
                n *= sub.attributes["DW_AT_upper_bound"].value + 1
            else:
                return None
        e = type_size(die.get_DIE_from_attribute("DW_AT_type"))
        return None if e is None else n * e
    a = die.attributes.get("DW_AT_byte_size")
    return a.value if a else None


def variable_sizes(obj, names):
    """name -> size for every global in names that the object declares."""
    out = {}
    with open(obj, "rb") as f:
        for cu in ELFFile(f).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag != "DW_TAG_variable" or "DW_AT_name" not in die.attributes \
                        or "DW_AT_type" not in die.attributes or die.get_parent().tag != "DW_TAG_compile_unit":
                    continue
                name = die.attributes["DW_AT_name"].value.decode()
                if name in names:
                    size = type_size(die.get_DIE_from_attribute("DW_AT_type"))
                    if size is not None:
                        out[name] = max(size, out.get(name, 0))
    return out


def ram_space():
    """name -> the bytes gen/ram.S gives each global in port_ram: from its label to the next label that has space.
    A label right before another one (an alias) gets the space of the next."""
    space = {}
    pending = []
    for line in (ROOT / "gen" / "ram.S").read_text().split("\n"):
        m = re.match(r"^SYM\((\w+)\):", line)
        if m:
            pending.append(m.group(1))
            continue
        m = re.match(r"^\s*\.skip\s+(\w+)", line)
        if m and pending:
            for name in pending:
                space[name] = int(m.group(1), 0)
            pending = []
    space.pop("port_ram", None)
    return space


def layouts(obj):
    """name -> (size, [(member, offset)]) for every named structure, union or typedef of one."""
    out = {}
    with open(obj, "rb") as f:
        elf = ELFFile(f)
        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                name = None
                target = die
                if die.tag == "DW_TAG_typedef" and "DW_AT_name" in die.attributes:
                    name = die.attributes["DW_AT_name"].value.decode()
                    target = strip(die)
                elif die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and "DW_AT_name" in die.attributes:
                    name = die.attributes["DW_AT_name"].value.decode()
                if name is None or target is None or target.tag not in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                    continue
                if "DW_AT_byte_size" not in target.attributes:
                    continue
                members = []
                for m in target.iter_children():
                    if m.tag != "DW_TAG_member":
                        continue
                    mname = m.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in m.attributes else "?"
                    off = m.attributes["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in m.attributes else 0
                    members.append((mname, off))
                out[name] = (target.attributes["DW_AT_byte_size"].value, members)
    return out


def main():
    m64 = "--m64" in sys.argv
    work = ROOT / "build" / "layout-check"
    work.mkdir(parents=True, exist_ok=True)
    work64 = ROOT / "build" / "layout64"
    differ = {}
    seen = 0
    failed = 0
    space = ram_space()
    sizes = {}
    sizes32 = {}
    sources = sorted((ROOT / "src" / "game").glob("*.c"))
    if "--audio" in sys.argv:
        sources += sorted((ROOT / "src" / "audio").glob("*/*.c"))
    for src in sources:
        rel = src.relative_to(ROOT)
        mobj = work / (str(rel.with_suffix("")).replace("/", "_") + ".mips.o")
        flags = MIPS_FLAGS + (["-DPORT_AUDIO_MUS"] if rel.parts[1] == "audio" else [])
        r = subprocess.run(["clang", *flags, str(src), "-o", str(mobj)], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            print(f"{rel}: MIPS compile failed:\n{r.stderr[:2000]}")
            failed += 1
            continue
        if m64:
            nobj = work64 / rel.with_suffix(".o")
            nobj.parent.mkdir(parents=True, exist_ok=True)
            cc = M64_CXX if rel.parts[1] == "game" or audio_is_cxx() else M64_C
            r = subprocess.run([*cc, *M64_FLAGS, str(src), "-o", str(nobj)], cwd=ROOT, capture_output=True, text=True)
            if r.returncode:
                print(f"{rel}: 64-bit compile failed:\n{r.stderr[:1000]}")
                failed += 1
                continue
            for name, size in variable_sizes(nobj, space).items():
                sizes[name] = max(size, sizes.get(name, 0))
            obj32 = ROOT / "build" / "linux" / rel.with_suffix(".o")
            if obj32.exists():
                for name, size in variable_sizes(obj32, space).items():
                    sizes32[name] = max(size, sizes32.get(name, 0))
        else:
            nobj = ROOT / "build" / "linux" / rel.with_suffix(".o")
        if not nobj.exists():
            sys.exit(f"{nobj} is missing: run make first")
        m = layouts(mobj)
        n = layouts(nobj)
        for name, (size, members) in m.items():
            if name not in n:
                continue
            seen += 1
            nsize, nmembers = n[name]
            if size != nsize or members != nmembers:
                bad = [f"{a[0]} {a[1]:#x}/{b[1]:#x}" for a, b in zip(members, nmembers) if a != b]
                differ[name] = f"size {size:#x} on the N64, {nsize:#x} on the PC; " + (
                    "first differing members (N64/PC): " + ", ".join(bad[:4]) if bad else "same member offsets")
    for name, msg in sorted(differ.items()):
        print(f"{name}: {msg}")
    print(f"{len(differ)} of the structures compared differ ({seen} comparisons)")
    if failed:
        print(f"{failed} files did not compile")
    if m64:
        # A few of the game's declarations are larger than their space in gen/ram.S on the N64 too (zero is a
        # Vec3 in a 4-byte slot). So each global's size is compared with its size in the 32-bit build.
        big = sorted(n for n, size in sizes.items() if n in sizes32 and size != sizes32[n])
        for n in big:
            print(f"global {n}: {sizes[n]:#x} bytes on 64-bit, {sizes32[n]:#x} on 32-bit "
                  f"(gen/ram.S gives it {space[n]:#x})")
        print(f"{len(big)} of {len(sizes)} globals in port_ram have another size than on 32-bit")


if __name__ == "__main__":
    main()
