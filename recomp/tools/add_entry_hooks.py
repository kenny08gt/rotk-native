#!/usr/bin/env python3
"""Insert ROTK_FN_ENTRY(ctx, addr) at every function start in DolRecomp's generated chunks.

The macro is empty unless the chunks are built with ROTK_TRACE_HOOKS (CMake -DROTK_TRACE_HOOKS=ON),
so the default build is unchanged. Re-run after regenerating out/generated.
usage: add_entry_hooks.py <symbols.tsv> <chunks dir>
"""
import pathlib
import re
import sys

starts = set()
for line in open(sys.argv[1]):
    parts = line.split("\t")
    if parts and re.fullmatch(r"[0-9A-Fa-f]{8}", parts[0]):
        starts.add(parts[0].upper())

label = re.compile(r"^label_([0-9A-F]{8}):\n")
for path in sorted(pathlib.Path(sys.argv[2]).glob("*.c")):
    out, n = [], 0
    lines = path.read_text().splitlines(keepends=True)
    for i, line in enumerate(lines):
        out.append(line)
        m = label.match(line)
        if m and m.group(1) in starts and not (i + 1 < len(lines) and "ROTK_FN_ENTRY" in lines[i + 1]):
            out.append(f"    ROTK_FN_ENTRY(ctx, 0x{m.group(1)}u);\n")
            n += 1
    path.write_text("".join(out))
    print(f"{path.name}: {n} hooks")
