"""Extract a GameCube disc image (GCM/ISO, incl. NKit) into a directory.

usage: gcm_extract.py <disc> <outdir> [--sys-only]   (--sys-only: boot.bin, apploader, main.dol, FST)
"""
import struct, sys, os

def u32(b, o): return struct.unpack_from(">I", b, o)[0]

def main(iso_path, out_dir, sys_only=False):
    with open(iso_path, "rb") as f:
        hdr = f.read(0x440)
        assert u32(hdr, 0x1C) == 0xC2339F3D, "not a GameCube disc"
        dol_off, fst_off, fst_size = u32(hdr, 0x420), u32(hdr, 0x424), u32(hdr, 0x428)
        sys_dir = os.path.join(out_dir, "sys")
        os.makedirs(sys_dir, exist_ok=True)
        f.seek(0); open(os.path.join(sys_dir, "boot.bin"), "wb").write(f.read(0x440))
        f.seek(0x440); open(os.path.join(sys_dir, "bi2.bin"), "wb").write(f.read(0x2000))
        f.seek(0x2440); apl_hdr = f.read(0x20)
        apl_size = 0x20 + u32(apl_hdr, 0x14) + u32(apl_hdr, 0x18)
        f.seek(0x2440); open(os.path.join(sys_dir, "apploader.img"), "wb").write(f.read(apl_size))
        # DOL size = max(section offset + size)
        f.seek(dol_off); dh = f.read(0x100)
        dol_size = max(u32(dh, i*4) + u32(dh, 0x90 + i*4) for i in range(18))
        f.seek(dol_off); open(os.path.join(sys_dir, "main.dol"), "wb").write(f.read(dol_size))
        f.seek(fst_off); fst = f.read(fst_size)
        open(os.path.join(sys_dir, "fst.bin"), "wb").write(fst)
        if sys_only:
            print(f"DOL {dol_size} bytes at {dol_off:#x}")
            return

        n = u32(fst, 8)
        strtab = n * 12
        def name(i):
            o = strtab + (u32(fst, i*12) & 0xFFFFFF)
            return fst[o:fst.index(b"\0", o)].decode("shift_jis", "replace")
        files_dir = os.path.join(out_dir, "files")
        listing = []
        def walk(start, end, path):
            i = start
            while i < end:
                is_dir = fst[i*12] == 1
                nm = name(i)
                p = os.path.join(path, nm)
                if is_dir:
                    os.makedirs(p, exist_ok=True)
                    nxt = u32(fst, i*12 + 8)
                    walk(i + 1, nxt, p)
                    i = nxt
                else:
                    off, ln = u32(fst, i*12 + 4), u32(fst, i*12 + 8)
                    f.seek(off)
                    with open(p, "wb") as o:
                        remaining = ln
                        while remaining:
                            chunk = f.read(min(remaining, 1 << 20)); o.write(chunk); remaining -= len(chunk)
                    listing.append((os.path.relpath(p, files_dir), ln, off))
                    i += 1
        os.makedirs(files_dir, exist_ok=True)
        walk(1, n, files_dir)
        with open(os.path.join(out_dir, "filelist.tsv"), "w") as o:
            for p, ln, off in listing: o.write(f"{p}\t{ln}\t{off:#x}\n")
        print(f"DOL {dol_size} bytes at {dol_off:#x}; {len(listing)} files")

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--sys-only"]
    main(args[0], args[1], sys_only="--sys-only" in sys.argv)
