#!/usr/bin/env python3
"""Insert ROTK_HLE_POINT(ctx, addr) after the given loop-head labels in DolRecomp's generated chunks.

See chassis/hooks.h. Idempotent; re-run after regenerating out/generated. With a macro name, places
that macro instead (ROTK_CHEAT_POINT with chassis/cheat_points.inc).
usage: add_hle_points.py <points file> <chunks dir> [macro]
"""
import pathlib
import re
import sys

macro = sys.argv[3] if len(sys.argv) > 3 else "ROTK_HLE_POINT"
points = set()
for line in open(sys.argv[1]):
    word = line.split("#")[0].split("//")[0].replace(",", " ").split()
    if word:
        points.add(word[0].upper().removeprefix("0X"))

label = re.compile(r"^label_([0-9A-F]{8}):\n")
found = set()
for path in sorted(pathlib.Path(sys.argv[2]).glob("*.c")):
    lines = path.read_text().splitlines(keepends=True)
    out, n = [], 0
    for i, line in enumerate(lines):
        out.append(line)
        m = label.match(line)
        if m and m.group(1) in points:
            found.add(m.group(1))
            if not any(macro in following for following in lines[i + 1:i + 3]):
                out.append(f"    {macro}(ctx, 0x{m.group(1)}u);\n")
                n += 1
    if n:
        path.write_text("".join(out))
        print(f"{path.name}: {n} points")
missing = points - found
if missing:
    sys.exit("labels not found: " + " ".join(sorted(missing)))
