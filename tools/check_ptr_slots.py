"""Check the type of every pointer slot that the loader relocates in the game's data.

port_load_ram (src/port/loaders.c) fills port_ram from the ROM and rewrites the words that gen/ramtab.c lists: the
N64 pointers (port_ptrs) and the pointers to functions (port_fnptrs). Each such word must have a type that stores
the loader's word: Ptr32<T>, Fn32<F> or PortMesg (src/port/ptr32.h). This tool maps each slot to the global in
gen/ram.S that holds it, and then to the member at that offset in the global's type, from the debug information of
the Linux build.

- A raw pointer is an error: on a 64-bit host it is 8 bytes.
- An integer or another scalar is listed: the game reads the word as a number and converts it itself.
- gen/ram.S splits the game's data at the N64's symbols, and an array that a file declares without a size (as
  screenList[]) often goes on past the next symbol, a D_xxxxxxxx label. A slot in such a label is taken as an
  element of the unsized array before it, when no declared global lies between them, and counted as an array tail.
- A slot in a global that no compile unit declares, or in the gap after a declared global, is counted as untyped.

Usage: python3 tools/check_ptr_slots.py [--list]        (after `make`; needs pyelftools)
  --list   list every slot that is not a Ptr32, Fn32 or PortMesg
It exits with 1 when a raw pointer is left.
"""
import bisect
import collections
import glob
import pathlib
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORD_TYPES = ("Ptr32<", "Fn32<", "PortMesg")


def strip(die):
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    return die


def name_of(die):
    a = die.attributes.get("DW_AT_name")
    return a.value.decode() if a else None


def size_of(die):
    die = strip(die)
    if die is None:
        return None
    if die.tag == "DW_TAG_array_type":
        n = 1
        for sub in die.iter_children():
            if sub.tag == "DW_TAG_subrange_type":
                if "DW_AT_count" in sub.attributes:
                    n *= sub.attributes["DW_AT_count"].value
                elif "DW_AT_upper_bound" in sub.attributes:
                    n *= sub.attributes["DW_AT_upper_bound"].value + 1
                else:
                    return None
        e = size_of(die.get_DIE_from_attribute("DW_AT_type"))
        return None if e is None else n * e
    a = die.attributes.get("DW_AT_byte_size")
    return a.value if a else None


def classify(die, off):
    """What the word at byte offset off of a value of type die is: 'word', 'pointer', 'scalar' or 'none'."""
    die = strip(die)
    if die is None:
        return "none"
    if die.tag == "DW_TAG_pointer_type":
        return "pointer"
    if die.tag == "DW_TAG_array_type":
        elem = die.get_DIE_from_attribute("DW_AT_type")
        es = size_of(elem)
        if not es:
            return "none"
        return classify(elem, off % es)
    if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        n = name_of(die) or ""
        if n.startswith(WORD_TYPES):
            return "word"
        best = "none"
        for m in die.iter_children():
            if m.tag != "DW_TAG_member" or "DW_AT_type" not in m.attributes:
                continue
            mo = m.attributes["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in \
                m.attributes else 0
            t = m.get_DIE_from_attribute("DW_AT_type")
            ms = size_of(t)
            if ms is not None and mo <= off < mo + ms:
                k = classify(t, off - mo)
                # In a union, a word view wins over a pointer view, which wins over a number.
                if k == "word" or (k == "pointer" and best != "word") or best == "none":
                    best = k
        return best
    return "scalar"


def main():
    nm = subprocess.run(["nm", str(ROOT / "build" / "linux" / "gen" / "ram.o")], capture_output=True,
                        text=True).stdout
    syms = {}
    for line in nm.split("\n"):
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    base = syms.get("port_ram")
    if base is None:
        sys.exit("check_ptr_slots: build the Linux build first (make)")
    names = sorted((v - base, k) for k, v in syms.items() if k != "port_ram")
    offsets = [o for o, _ in names]

    types = {}
    for obj in sorted(glob.glob(str(ROOT / "build" / "linux" / "src" / "*" / "*.o")) +
                      glob.glob(str(ROOT / "build" / "linux" / "src" / "audio" / "*" / "*.o"))):
        with open(obj, "rb") as f:
            for cu in ELFFile(f).get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag == "DW_TAG_variable" and "DW_AT_type" in die.attributes and name_of(die) in syms:
                        t = die.get_DIE_from_attribute("DW_AT_type")
                        # The declaration in a game file wins over one in the runtime's C files.
                        if name_of(die) not in types or "/src/game/" in obj:
                            types[name_of(die)] = (t, size_of(t))

    text = (ROOT / "gen" / "ramtab.c").read_text()
    ptrs = re.search(r"port_ptrs\[\] = \{(.*?)\};", text, re.S).group(1)
    slots = [("data", int(x, 16)) for x in re.findall(r"0x[0-9a-fA-F]+", ptrs) if int(x, 16)]
    fns = re.search(r"port_fnptrs\[\] = \{(.*?)\};", text, re.S).group(1)
    slots += [("function", int(x, 16)) for x in re.findall(r"\{\s*(0x[0-9a-fA-F]+),", fns)]

    def unsized_array(t):
        return t is not None and t[1] is None and strip(t[0]) is not None and strip(t[0]).tag == "DW_TAG_array_type"

    count = collections.Counter()
    tails = collections.Counter()
    listed = []
    for what, off in slots:
        i = bisect.bisect_right(offsets, off) - 1
        sym = names[i][1]
        inner = off - names[i][0]
        t = types.get(sym)
        where = f"{sym}+{inner:#x}"
        if t is None and sym.startswith("D_"):
            # The tail of an unsized array declared before this label.
            j = i - 1
            while j >= 0 and names[j][1] not in types and names[j][1].startswith("D_"):
                j -= 1
            if j >= 0 and unsized_array(types.get(names[j][1])):
                t = types[names[j][1]]
                inner = off - names[j][0]
                tails[names[j][1]] += 1
                where = f"{sym}, {names[j][1]}+{inner:#x}"
        if t is None or (not unsized_array(t) and (t[1] is None or inner >= t[1])):
            k = "untyped"
        else:
            k = classify(t[0], inner)
            k = {"none": "untyped"}.get(k, k)
        count[(what, k)] += 1
        if k != "word":
            listed.append(f"  {what} slot {off:#x}: {where}: {k}")
    if "--list" in sys.argv:
        print("\n".join(listed))
        for name, n in sorted(tails.items()):
            print(f"  {n} slots typed as the tail of {name}[]")
    print(f"{sum(tails.values())} slots lie in the tails of {len(tails)} unsized arrays and are typed by them")
    for what in ("data", "function"):
        c = {k: count[(what, k)] for k in ("word", "pointer", "scalar", "untyped")}
        print(f"{what} slots: {sum(c.values())}: {c['word']} Ptr32/Fn32/PortMesg, {c['pointer']} raw pointers, "
              f"{c['scalar']} integers or other scalars, {c['untyped']} untyped")
    return 1 if count[("data", "pointer")] + count[("function", "pointer")] else 0


if __name__ == "__main__":
    sys.exit(main())
