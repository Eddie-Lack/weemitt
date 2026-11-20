"""Find or replace the definition of a C function in a source file.

Usage: python3 tools/cfunc.py FILE NAME              print the definition
       python3 tools/cfunc.py FILE NAME --replace TEXT  replace the whole definition with TEXT
"""
import re
import sys


def function_span(text, name):
    """The span of the definition of NAME, from the start of its line to its closing brace, or None."""
    m = re.search(r"^[A-Za-z_][\w \t\*]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.M)
    if not m:
        return None
    depth = 0
    i = m.end() - 1
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return m.start(), i + 1
        elif text.startswith("//", i):
            i = text.find("\n", i)
            continue
        elif text.startswith("/*", i):
            i = text.find("*/", i) + 2
            continue
        elif c == '"' or c == "'":
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j
        i += 1
    return None


def main():
    path, name = sys.argv[1], sys.argv[2]
    text = open(path).read()
    span = function_span(text, name)
    if span is None:
        sys.exit(f"{name} not found in {path}")
    if len(sys.argv) > 4 and sys.argv[3] == "--replace":
        text = text[:span[0]] + sys.argv[4] + text[span[1]:]
        open(path, "w").write(text)
    else:
        print(text[span[0]:span[1]])


if __name__ == "__main__":
    main()
