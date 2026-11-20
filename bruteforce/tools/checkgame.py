#!/usr/bin/env python3
"""Checks the game's relocatable object (the Makefile's game.o): every writable section must be gstate_data or
gstate_bss, which a savestate copies. Another one would hold game variables that a state misses.

    python3 tools/checkgame.py build/linux/game.o linux|windows
"""
import subprocess
import sys

path, target = sys.argv[1], sys.argv[2]
objdump = 'i686-w64-mingw32-objdump' if target == 'windows' else 'objdump'
out = subprocess.run([objdump, '-h', path], capture_output=True, text=True, check=True).stdout
lines = out.splitlines()
bad = []
for i, line in enumerate(lines):
    parts = line.split()
    if len(parts) < 7 or not parts[0].isdigit():
        continue
    name, size = parts[1], int(parts[2], 16)
    flags = lines[i + 1] if i + 1 < len(lines) else ''
    writable = 'ALLOC' in flags and 'READONLY' not in flags and 'CODE' not in flags
    if writable and size and name not in ('gstate_data', 'gstate_bss'):
        bad.append(f'{name} ({size} bytes): {flags.strip()}')
if bad:
    sys.exit('checkgame: writable sections that a savestate would miss:\n  ' + '\n  '.join(bad))
