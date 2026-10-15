"""Walk EA-style 4CC chunk streams (tag, be32 size incl. 8-byte header) and histogram tags."""
import struct, sys, collections
def walk(path, limit=None):
    d = open(path, "rb").read()
    o, seq, hist = 0, [], collections.Counter()
    while o + 8 <= len(d):
        tag = d[o:o+4]; size = struct.unpack_from(">I", d, o + 4)[0]
        if not all(32 <= c < 127 for c in tag): seq.append(f"?? junk at {o:#x}"); break
        hist[tag.decode()] += 1
        if limit is None or len(seq) < limit: seq.append(f"{o:#010x} {tag.decode()} {size:#x}")
        # FILL pads to the next 64 KB boundary; with <8 bytes left it has no size field.
        o = (o | 0xFFFF) + 1 if tag == b"FILL" else o + (size if size >= 8 else 8)
    return seq, hist, o, len(d)
for p in sys.argv[1:]:
    seq, hist, end, total = walk(p, 25)
    print("==", p, f"parsed {end:#x}/{total:#x}"); print("  " + " | ".join(seq[:25])); print("  ", dict(hist.most_common(25)))
