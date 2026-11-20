"""Find the C type of every game variable that the port loads from the ROM, as a list of scalar fields.

The decomp declares each variable with `extern` in the files that use it. For each source file, this tool lists
those declarations with clang, writes a probe file that declares `__typeof__(name) *__ptype_name`, and compiles it
for big-endian MIPS with debug information. The DWARF data then gives the exact layout, which is the same as on
the N64. It also writes the layouts of the structures that the port's bank loaders convert.

Output: gen/types.json
  {"vars": {name: layout}, "structs": {name: layout}}
  layout = {"size": n, "open": bool, "units": [[offset, size, kind], ...]}
  kind: "i" integer, "f" float, "p" data pointer, "fp" function pointer
"""
import json
import pathlib
import re
import sys

from elftools.elf.elffile import ELFFile

import portlib as P

# Structures whose layouts the bank and level loaders need.
STRUCTS = ["Texture", "ObjectBankEntry", "ObjectInstance", "AnimState", "AnimInfo", "Object", "Mesh",
           "ObjectSprite", "PosKey", "RotKey", "Vtx"]

DECL_RE = re.compile(r"VarDecl 0x[0-9a-f]+ <[^>]*> (?:col|line):[\d:]+(?: used)?(?: referenced)? (\w+) '.*' extern")


def extern_names(src):
    out = P.run(["clang", "--target=mips-linux-gnu", *P.CFLAGS, *P.DEFINES, "-DPORT_TYPES_ONLY", "-include", str(P.PORT / "port_pre.h"),
                 *P.INCLUDES, "-fsyntax-only", "-Xclang", "-ast-dump", str(src)]).stdout
    return set(DECL_RE.findall(out))


class Flattener:
    def __init__(self, dwarf):
        self.dwarf = dwarf
        self.cache = {}

    def strip(self, die):
        while die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
            if "DW_AT_type" not in die.attributes:
                return None
            die = die.get_DIE_from_attribute("DW_AT_type")
        return die

    def size(self, die):
        die = self.strip(die)
        if die is None:
            return 0
        if "DW_AT_byte_size" in die.attributes:
            return die.attributes["DW_AT_byte_size"].value
        if die.tag == "DW_TAG_array_type":
            n, open_ = self.count(die)
            return n * self.size(die.get_DIE_from_attribute("DW_AT_type"))
        if die.tag == "DW_TAG_pointer_type":
            return 4
        raise ValueError(f"no size for {die.tag}")

    def count(self, arr):
        n = 1
        open_ = False
        for sub in arr.iter_children():
            if sub.tag != "DW_TAG_subrange_type":
                continue
            if "DW_AT_count" in sub.attributes:
                n *= sub.attributes["DW_AT_count"].value
            elif "DW_AT_upper_bound" in sub.attributes:
                n *= sub.attributes["DW_AT_upper_bound"].value + 1
            else:
                open_ = True
        return n, open_

    def units(self, die):
        """Scalar fields of a type: [(offset, size, kind)]. Open arrays count as one element."""
        die = self.strip(die)
        if die is None:
            return [], 0, False
        key = die.offset
        if key in self.cache:
            return self.cache[key]
        res = self._units(die)
        self.cache[key] = res
        return res

    def _units(self, die):
        tag = die.tag
        if tag == "DW_TAG_base_type":
            size = die.attributes["DW_AT_byte_size"].value
            enc = die.attributes["DW_AT_encoding"].value
            return [(0, size, "f" if enc == 4 else "i")], size, False
        if tag == "DW_TAG_enumeration_type":
            size = die.attributes["DW_AT_byte_size"].value
            return [(0, size, "i")], size, False
        if tag == "DW_TAG_pointer_type":
            kind = "p"
            if "DW_AT_type" in die.attributes:
                tgt = self.strip(die.get_DIE_from_attribute("DW_AT_type"))
                if tgt is not None and tgt.tag == "DW_TAG_subroutine_type":
                    kind = "fp"
            return [(0, 4, kind)], 4, False
        if tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            size = die.attributes.get("DW_AT_byte_size")
            size = size.value if size else 0
            out = []
            members = [m for m in die.iter_children() if m.tag == "DW_TAG_member"]
            if tag == "DW_TAG_union_type":
                members = members[:1]  # the first member describes how the data is stored
            for m in members:
                off = m.attributes["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in m.attributes else 0
                if "DW_AT_bit_size" in m.attributes:
                    # A bit field: its storage unit has the size of its declared type.
                    sub, ssize, _ = self.units(m.get_DIE_from_attribute("DW_AT_type"))
                    out.append((off, ssize, "i"))
                    continue
                sub, ssize, _ = self.units(m.get_DIE_from_attribute("DW_AT_type"))
                out.extend((off + o, s, k) for (o, s, k) in sub)
            return dedupe(out), size, False
        if tag == "DW_TAG_array_type":
            elem = die.get_DIE_from_attribute("DW_AT_type")
            sub, esize, _ = self.units(elem)
            n, open_ = self.count(die)
            if open_:
                n = 1
            out = []
            for i in range(n):
                out.extend((i * esize + o, s, k) for (o, s, k) in sub)
            return out, n * esize, open_
        if tag == "DW_TAG_subroutine_type":
            return [], 0, False
        raise ValueError(f"unhandled {tag}")


def dedupe(units):
    seen = {}
    for o, s, k in units:
        if o not in seen or s > seen[o][0]:
            seen[o] = (s, k)
    return sorted((o, s, k) for o, (s, k) in seen.items())


def layout(fl, die):
    units, size, open_ = fl.units(die)
    return {"size": size, "open": open_, "units": [list(u) for u in units]}


def ctype(fl, die):
    """The C spelling of a parameter or return type that gives the same WebAssembly type."""
    die = fl.strip(die)
    if die is None:
        return "void"
    if die.tag == "DW_TAG_base_type":
        size = die.attributes["DW_AT_byte_size"].value
        if die.attributes["DW_AT_encoding"].value == 4:
            return "float" if size == 4 else "double"
        return "long long" if size == 8 else "int"
    if die.tag == "DW_TAG_enumeration_type":
        return "int"
    if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        return "struct"
    return "void *"


def signature(fl, die):
    ret = ctype(fl, die.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in die.attributes else "void"
    params = []
    varargs = False
    for c in die.iter_children():
        if c.tag == "DW_TAG_formal_parameter":
            params.append(ctype(fl, c.get_DIE_from_attribute("DW_AT_type")))
        elif c.tag == "DW_TAG_unspecified_parameters":
            varargs = True
    return {"ret": ret, "params": params, "varargs": varargs}


def richness(lay):
    return (sum(1 for u in lay["units"] if u[1] > 1), len(lay["units"]))


def main():
    P.GEN.mkdir(parents=True, exist_ok=True)
    srcs = P.write_fixed_sources() + [pathlib.Path(__file__).resolve().parent / "extra_types.c"]
    labels = P.all_data_labels()
    wanted = {l.name for l in labels if l.section != ".bss"}
    wanted |= {n for n, a in P.fixed_symbols().items() if 0x80100000 <= a < 0x801F5680}
    probe_dir = P.OUT / "probe"
    probe_dir.mkdir(parents=True, exist_ok=True)
    results = {}
    conflicts = {}
    funcs = {}
    for src in srcs:
        names = sorted(extern_names(src) & wanted)
        probe = probe_dir / src.name
        text = f'#include "{src}"\n' + "".join(f"__typeof__({n}) *__ptype_{n};\n" for n in names)
        if src.name == "main.c":
            text += '#include "banks.h"\n#include "texture.h"\n'
            text += "".join(f"{s} *__stype_{s};\n" for s in STRUCTS)
        probe.write_text(text)
        obj = probe.with_suffix(".o")
        P.run(["clang", "--target=mips-linux-gnu", *P.CFLAGS, *P.DEFINES, "-DPORT_TYPES_ONLY", "-include",
               str(P.PORT / "port_pre.h"), *P.INCLUDES, "-g", "-O0", "-c", str(probe), "-o", str(obj)])
        with open(obj, "rb") as f:
            elf = ELFFile(f)
            dwarf = elf.get_dwarf_info()
            fl = Flattener(dwarf)
            for cu in dwarf.iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag == "DW_TAG_subprogram" and "DW_AT_low_pc" in die.attributes and "DW_AT_name" in die.attributes:
                        funcs[die.attributes["DW_AT_name"].value.decode()] = signature(fl, die)
                        continue
                    if die.tag != "DW_TAG_variable" or "DW_AT_name" not in die.attributes:
                        continue
                    name = die.attributes["DW_AT_name"].value.decode()
                    if not name.startswith("__ptype_") and not name.startswith("__stype_"):
                        continue
                    ptr = fl.strip(die.get_DIE_from_attribute("DW_AT_type"))
                    target = ptr.get_DIE_from_attribute("DW_AT_type")
                    lay = layout(fl, target)
                    if name.startswith("__stype_"):
                        results.setdefault("__struct__", {})[name[8:]] = lay
                        continue
                    var = name[8:]
                    prev = results.get(var)
                    if prev is not None and prev["units"] != lay["units"]:
                        conflicts.setdefault(var, []).append(src.name)
                        if richness(lay) <= richness(prev):
                            continue
                    results[var] = lay
    structs = results.pop("__struct__")
    (P.GEN / "types.json").write_text(json.dumps({"vars": results, "structs": structs, "funcs": funcs}, indent=0))
    print(f"types: {len(results)} variables, {len(structs)} structs, {len(conflicts)} with differing declarations")
    for k, v in sorted(conflicts.items()):
        print(f"  differing declarations of {k}: {', '.join(v)}")


if __name__ == "__main__":
    sys.exit(main())
