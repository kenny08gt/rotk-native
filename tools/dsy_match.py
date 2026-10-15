"""Name DOL functions using Dolphin's totaldb.dsy (HashSignatureDB checksum)."""
import struct, sys, re

def load_dol(path):
    d = open(path, "rb").read()
    u = lambda o: struct.unpack_from(">I", d, o)[0]
    secs = [(u(0x48+i*4), u(i*4), u(0x90+i*4)) for i in range(18) if u(0x90+i*4)]
    def read(addr):
        for a, off, sz in secs:
            if a <= addr < a + sz: return u(off + addr - a)
        raise KeyError(hex(addr))
    return read

def checksum(read, start, size):
    s = 0
    for a in range(start, start + size, 4):
        opc = read(a); op = opc & 0xFC000000; aux = op >> 26; op2 = op3 = 0
        if aux == 4:
            op2 = opc & 0x3F
            if op2 in (0, 8, 16, 21, 22): op3 = opc & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15): op2 = opc & 0x03FF0000
        elif aux in (19, 31, 63): op2 = opc & 0x7FF
        elif aux == 59:
            op2 = opc & 0x3F
            if op2 < 16: op3 = opc & 0x7C0
        elif 32 <= aux < 56: op2 = opc & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x1FFFF)
        s ^= op | op2 | op3
    return s

def main(dol, dsy, asm, out):
    read = load_dol(dol)
    db = {}
    raw = open(dsy, "rb").read()
    n = struct.unpack_from("<I", raw, 0)[0]
    for i in range(n):
        cs, sz, nm = struct.unpack_from("<II128s", raw, 4 + i * 136)
        db.setdefault((cs, sz), nm.split(b"\0")[0].decode("latin1"))
    addrs = []
    pending = None
    for line in open(asm):
        m = re.match(r"# \.\w+:0x[0-9A-Fa-f]+ \| 0x([0-9A-Fa-f]+) \| size: 0x([0-9A-Fa-f]+)", line)
        if m: pending = (int(m.group(1), 16), int(m.group(2), 16)); continue
        if line.startswith(".fn ") and pending:
            addrs.append((line.split()[1].rstrip(","), *pending)); pending = None
    hits = agree = disagree = 0
    with open(out, "w") as o:
        for name, a, sz in addrs:
            nm = db.get((checksum(read, a, sz), sz))
            if nm:
                hits += 1
                if not name.startswith("fn_"):
                    if name == nm: agree += 1
                    else: disagree += 1
            o.write(f"{a:08X}\t{sz:#x}\t{name}\t{nm or ''}\n")
    print(f"db={n} funcs={len(addrs)} matched={hits} agree_with_dtk={agree} disagree={disagree}")

main(*sys.argv[1:5])
