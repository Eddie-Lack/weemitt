"""Find calls in the game's code whose arguments differ from the called function's definition.

On the N64 such calls often worked by accident: arguments travel in registers, so a call with too few arguments
passes on whatever the caller's own registers held, and an integer passed for a float parameter arrives as its
bits. On 32-bit x86 every argument is on the stack. A missing argument becomes stack garbage, and a float passed
to an unprototyped function becomes an 8-byte double. This tool lists every call whose argument count, or whose
argument sizes, differ from the definition, so that each can be fixed in the C.

It also lists calls through a declaration whose parameter is a pointer where the definition's is an integer, or
the other way, or whose pointer depth differs (s32 * against Platform **). Both are 4 bytes on 32-bit, but not on a
64-bit host (docs/port64.md).

Usage: python3 tools/check_calls.py        (run from the repository root, after `make`)
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FLAGS = ["-m32", "-std=gnu89", "-funsigned-char", "-fsyntax-only", "-w",
         "-DM2CTX", "-D_LANGUAGE_C", "-DF3DEX_GBI", "-D_FINALROM", "-DNDEBUG", "-DPORT",
         "-Igen", "-Isrc/port", "-Iinclude", "-Iinclude/PR", "-Isrc/game", "-include", "src/port/port_pre.h"]


def ast(path):
    out = subprocess.run(["clang", *FLAGS, "-Xclang", "-ast-dump=json", str(path)], cwd=ROOT,
                         capture_output=True, text=True)
    if out.returncode:
        sys.exit(out.stderr)
    return json.loads(out.stdout)


def walk(node, fn, ctx=None):
    if isinstance(node, dict):
        ctx = fn(node, ctx)
        for v in node.get("inner", []):
            walk(v, fn, ctx)


def size_of(qual):
    q = qual.replace("const ", "").replace("volatile ", "").strip()
    if q.endswith("*") or "(*)" in q or q.endswith("]"):
        return 4
    if q in ("double", "f64", "long long", "unsigned long long", "s64", "u64", "OSTime"):
        return 8
    return 4


def call_params(ctype):
    """The parameter types of a function type such as 'f32 (Vec3 *, s32 *)'."""
    depth = 0
    start = None
    for i, c in enumerate(ctype):
        if c == "(":
            if depth == 0:
                start = i + 1
            depth += 1
        elif c == ")":
            depth -= 1
    if start is None:
        return None
    # The outer parameter list is the last parenthesised group at depth 0.
    depth = 0
    end = len(ctype.rstrip()) - 1
    for i in range(end, -1, -1):
        c = ctype[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                start = i + 1
                break
    inner = ctype[start:end]
    params, depth, cur = [], 0, ""
    for c in inner:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            params.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip() and cur.strip() != "void":
        params.append(cur.strip())
    return [p for p in params if p != "..."]


def indirection(qual):
    """How many levels of pointer a parameter type has (an array parameter is a pointer)."""
    if "(*)" in qual:
        return 1
    return qual.count("*") + qual.count("[")


def main():
    defs = {}      # name -> list of parameter types, from definitions
    calls = []     # (file, line, caller, callee, [arg types after default promotion], callee type at the call)
    for src in sorted((ROOT / "src" / "game").glob("*.c")):
        tree = ast(src)

        def visit(n, ctx):
            kind = n.get("kind")
            if kind == "FunctionDecl" and n.get("inner") and any(c.get("kind") == "CompoundStmt" for c in n["inner"]):
                params = [p.get("type", {}).get("qualType", "?") for p in n["inner"] if p.get("kind") == "ParmVarDecl"]
                if n.get("loc", {}).get("file", str(src)).endswith(".c") or True:
                    defs.setdefault(n["name"], params)
                return n["name"]
            if kind == "CallExpr" and n.get("inner"):
                callee = n["inner"][0]
                name = None
                ref = callee
                while ref.get("kind") in ("ImplicitCastExpr", "ParenExpr") and ref.get("inner"):
                    ref = ref["inner"][0]
                if ref.get("kind") == "DeclRefExpr":
                    name = ref.get("referencedDecl", {}).get("name")
                    ctype = ref.get("type", {}).get("qualType", "")
                if name:
                    args = []
                    for a in n["inner"][1:]:
                        args.append(a.get("type", {}).get("qualType", "?"))
                    line = n.get("range", {}).get("begin", {}).get("line") or n.get("range", {}).get("begin", {}).get("expansionLoc", {}).get("line")
                    calls.append((src.name, line, ctx, name, args, ctype))
            return ctx

        walk(tree, visit)
    problems = 0
    for f, line, caller, callee, args, ctype in calls:
        if callee not in defs:
            continue
        params = defs[callee]
        variadic = "..." in ctype
        if len(args) != len(params) and not (variadic and len(args) >= len(params)):
            print(f"{f}:{line}: {caller} calls {callee} with {len(args)} arguments; it takes {len(params)}")
            problems += 1
            continue
        for i, (a, p) in enumerate(zip(args, params)):
            if size_of(a) != size_of(p):
                print(f"{f}:{line}: {caller} calls {callee}: argument {i + 1} is {a} ({size_of(a)} bytes); "
                      f"the parameter is {p} ({size_of(p)} bytes)")
                problems += 1
            elif ("float" in a or "f32" in a) != ("float" in p or "f32" in p) and "*" not in a + p:
                print(f"{f}:{line}: {caller} calls {callee}: argument {i + 1} is {a}; the parameter is {p} "
                      f"(the bits pass unchanged on the stack)")
    print(f"{problems} calls whose argument count or sizes differ from the definition")
    mixed = set()
    for f, line, caller, callee, args, ctype in calls:
        if callee not in defs:
            continue
        declared = call_params(ctype)
        if declared is None or len(declared) != len(defs[callee]):
            continue
        for i, (d, p) in enumerate(zip(declared, defs[callee])):
            if indirection(d) != indirection(p) and (f, callee, i) not in mixed:
                mixed.add((f, callee, i))
                print(f"{f}:{line}: {caller} calls {callee}: parameter {i + 1} is declared {d} here; "
                      f"the definition has {p}")
    print(f"{len(mixed)} parameters declared with another pointer depth than the definition")


if __name__ == "__main__":
    main()
