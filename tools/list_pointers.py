"""List the pointers in the game's data: the structures with pointer members, and the globals in port_ram whose
type holds a pointer. The 64-bit port (docs/port64.md) stores these as Ptr32 words.

The tool reads the debug information of the Linux build's game and runtime objects (build/linux/src/game and
src/port). Pointers that the game keeps in integer fields (display-list words, s32 casts) have no pointer type and
are not listed.

A member or global of type Ptr32<T>, Fn32<F> or PortMesg (src/port/ptr32.h) is a structure of one 32-bit word, so
it does not count as a pointer.

Usage: python3 tools/list_pointers.py [--globals] [--check [--strict]] [--audio]   (after `make`; needs pyelftools)
  --globals  list the globals too
  --check    count what is left to convert: raw pointer members, globals in port_ram that hold raw pointers (in any
             compile unit's declaration of them), functions whose parameter types differ by a Ptr32 between
             compile units, and functions that take or return a Ptr32. --strict exits with 1 while the count is not 0.
  --audio    read the sound player's objects too (build/linux/src/audio)
"""
import collections
import glob
import pathlib
import re
import sys

from elftools.elf.elffile import ELFFile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def strip(die):
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        if "DW_AT_type" not in die.attributes:
            return None
        die = die.get_DIE_from_attribute("DW_AT_type")
    return die


def name_of(die):
    a = die.attributes.get("DW_AT_name")
    return a.value.decode() if a else None


def kind(die):
    """'pointer', 'function pointer', the structure's DIE, or None, for a type. Arrays count as their elements."""
    die = strip(die)
    while die is not None and die.tag == "DW_TAG_array_type":
        die = strip(die.get_DIE_from_attribute("DW_AT_type"))
    if die is None:
        return None
    if die.tag == "DW_TAG_pointer_type":
        target = strip(die.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in die.attributes else None
        return "function pointer" if target is not None and target.tag == "DW_TAG_subroutine_type" else "pointer"
    if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        return die
    return None


# A Ptr32, Fn32 or PortMesg passed or returned by value (a pointer to one is fine).
BY_VALUE = re.compile(r"^(const )?(Ptr32<.*>|Fn32<.*>|PortMesg)$")


def from_system(die):
    """Whether a structure is not the game's: the system's headers or the port's own files declare it. The game's
    are in include/, src/game/ and src/audio/."""
    a = die.attributes.get("DW_AT_decl_file")
    if a is None:
        return True
    lp = die.cu.dwarfinfo.line_program_for_CU(die.cu)
    entries = lp.header["file_entry"]
    # DWARF 5 numbers the files from 0, earlier versions from 1.
    idx = a.value if lp.header["version"] >= 5 else a.value - 1
    if not 0 <= idx < len(entries):
        return True
    e = entries[idx]
    d = lp.header["include_directory"][e.dir_index if lp.header["version"] >= 5 else e.dir_index - 1] \
        if (e.dir_index or lp.header["version"] >= 5) else b""
    path = d.decode() + "/" + e.name.decode()
    return path.startswith("/usr") or not any(x in path for x in ("include/", "src/game/", "src/audio/"))


def type_name(die):
    """A short C-like name of a type, for comparing declarations between compile units."""
    if die is None:
        return "void"
    tag = die.tag
    if tag in ("DW_TAG_pointer_type", "DW_TAG_reference_type"):
        t = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
        return type_name(t) + "*"
    if tag in ("DW_TAG_const_type", "DW_TAG_volatile_type"):
        t = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
        return ("const " if tag == "DW_TAG_const_type" else "volatile ") + type_name(t)
    if tag == "DW_TAG_typedef":
        return type_name(strip(die))
    if tag == "DW_TAG_array_type":
        return type_name(die.get_DIE_from_attribute("DW_AT_type")) + "[]"
    if tag == "DW_TAG_subroutine_type":
        return "fn"
    return name_of(die) or "<anon>"


def main():
    check = "--check" in sys.argv
    raw_globals = set()  # globals in port_ram that some compile unit declares with a raw pointer
    params = collections.defaultdict(set)  # function -> the set of its parameter type lists
    ptr32_params = set()  # functions that take or return a Ptr32, Fn32 or PortMesg
    ram = set(re.findall(r"^SYM\((\w+)\):", (ROOT / "gen" / "ram.S").read_text(), re.M)) - {"port_ram"}
    members = {}  # structure name -> [(member, kind or structure name)]
    variables = {}  # global in port_ram -> its kind

    system = set()  # structures that the system's headers declare

    def scan(die, name):
        if name in members:
            return
        members[name] = []
        if from_system(die):
            system.add(name)
        for m in die.iter_children():
            if m.tag != "DW_TAG_member" or "DW_AT_type" not in m.attributes:
                continue
            k = kind(m.get_DIE_from_attribute("DW_AT_type"))
            if k is None:
                continue
            if not isinstance(k, str):
                sub = name_of(k) or "<anonymous at 0x%x>" % k.offset
                scan(k, sub)
                k = sub
            members[name].append((name_of(m), k))

    def holds_pointer(name, seen=()):
        for _, k in members.get(name, []):
            if k in ("pointer", "function pointer") or (k not in seen and holds_pointer(k, seen + (name,))):
                return True
        return False

    objs = sorted(glob.glob(str(ROOT / "build" / "linux" / "src" / "game" / "*.o")))
    objs += sorted(glob.glob(str(ROOT / "build" / "linux" / "src" / "port" / "*.o")))
    if "--audio" in sys.argv:
        objs += sorted(glob.glob(str(ROOT / "build" / "linux" / "src" / "audio" / "*" / "*.o")))
    if not objs:
        sys.exit("list_pointers: build the Linux build first (make)")
    for obj in objs:
        with open(obj, "rb") as f:
            for cu in ELFFile(f).get_dwarf_info().iter_CUs():
                # Only the files built with the game's flags (-fwrapv): not the platform layer or the menu.
                producer = cu.get_top_DIE().attributes.get("DW_AT_producer")
                if producer is not None and b"-fwrapv" not in producer.value:
                    continue
                for die in cu.iter_DIEs():
                    if die.tag == "DW_TAG_typedef" and "DW_AT_type" in die.attributes:
                        t = strip(die)
                        if t is not None and t.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                            scan(t, name_of(t) or name_of(die))
                    elif die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and name_of(die) \
                            and "DW_AT_declaration" not in die.attributes:
                        scan(die, name_of(die))
                    elif die.tag == "DW_TAG_variable" and name_of(die) in ram and "DW_AT_type" in die.attributes \
                            and die.get_parent().tag == "DW_TAG_compile_unit":
                        k = kind(die.get_DIE_from_attribute("DW_AT_type"))
                        if k is None:
                            continue
                        if not isinstance(k, str):
                            sub = name_of(k) or "<anonymous at 0x%x>" % k.offset
                            scan(k, sub)
                            if not holds_pointer(sub):
                                continue
                            k = "structure"
                        variables[name_of(die)] = k
                        raw_globals.add(name_of(die))
                    elif check and die.tag == "DW_TAG_subprogram" and name_of(die) \
                            and "DW_AT_specification" not in die.attributes \
                            and die.get_parent().tag == "DW_TAG_compile_unit":
                        types = []
                        for c in die.iter_children():
                            if c.tag == "DW_TAG_formal_parameter" and "DW_AT_type" in c.attributes:
                                types.append(type_name(c.get_DIE_from_attribute("DW_AT_type")))
                        ret = type_name(die.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in die.attributes \
                            else "void"
                        params[name_of(die)].add(tuple([ret] + types))
                        if any(BY_VALUE.match(t) for t in [ret] + types):
                            ptr32_params.add(name_of(die))

    direct = {n: [(m, k) for m, k in v if k in ("pointer", "function pointer")] for n, v in members.items()}
    direct = {n: v for n, v in direct.items() if v and n not in system and (check or not n.startswith("<"))}
    count = collections.Counter(k for v in direct.values() for _, k in v)
    print("%d structures have %d pointer members (%d of them function pointers):" % (
        len(direct), sum(count.values()), count["function pointer"]))
    for n in sorted(direct, key=str.lower):
        print("  %s: %s" % (n, ", ".join(m + ("()" if k == "function pointer" else "") for m, k in direct[n])))
    count = collections.Counter(variables.values())
    print("%d globals in port_ram hold pointers: %d pointers or arrays of them, %d function pointers, %d structures" % (
        len(variables), count["pointer"], count["function pointer"], count["structure"]))
    if "--globals" in sys.argv:
        for n in sorted(variables, key=str.lower):
            print("  %s: %s" % (n, variables[n]))
    if check:
        # The game's own mismatches (check_calls.py lists them) do not count; a mismatch with a Ptr32 does.
        differ = sorted(n for n, v in params.items()
                        if len(v) > 1 and any(BY_VALUE.match(t) for x in v for t in x))
        for n in differ:
            print("  parameter types differ between compile units: %s %s" % (n, sorted(params[n])))
        for n in sorted(ptr32_params):
            print("  takes or returns a Ptr32: %s" % n)
        members = sum(len(v) for v in direct.values())
        total = members + len(raw_globals) + len(differ) + len(ptr32_params)
        print("check: %d left (%d pointer members, %d globals, %d functions with different parameters, "
              "%d with Ptr32 parameters)" % (total, members, len(raw_globals), len(differ), len(ptr32_params)))
        if "--strict" in sys.argv and total:
            sys.exit(1)


if __name__ == "__main__":
    main()
