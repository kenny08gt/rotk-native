#!/usr/bin/env python3
"""Package build/rotk as a double-clickable macOS app.

usage: tools/make_app.py [--iso PATH] [--out DIR]

Creates "<out>/Return of the King.app" with the binary, every non-system dylib it links
(copied into Contents/Frameworks with rewritten install names), an icon made from the
banner on your disc (if --iso is given), and an ad-hoc signature. The game data is not
bundled: on first launch the app asks for the disc image and remembers it.
"""
import argparse
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP_NAME = "Return of the King"
BUNDLE_ID = "local.rotk.native"


def run(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw).stdout


def linked_dylibs(path):
    out = []
    for line in run("otool", "-L", path).splitlines()[1:]:
        lib = line.strip().split(" (")[0]
        if lib.startswith("/usr/lib/") or lib.startswith("/System/") or lib.startswith("@"):
            continue
        out.append(lib)
    return out


def bundle_dylibs(binary, frameworks):
    """Copy the dylib closure next to the binary and point every reference at the copies."""
    os.makedirs(frameworks, exist_ok=True)
    todo, seen = [binary], set()
    while todo:
        obj = todo.pop()
        for lib in linked_dylibs(obj):
            real = os.path.realpath(lib)
            name = os.path.basename(lib)
            dest = os.path.join(frameworks, name)
            if real not in seen:
                seen.add(real)
                shutil.copy2(real, dest)
                os.chmod(dest, 0o755)
                run("install_name_tool", "-id", f"@rpath/{name}", dest)
                todo.append(dest)
            run("install_name_tool", "-change", lib, f"@rpath/{name}", obj)
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", binary)
    return len(seen)


def banner_rgba(iso):
    """The disc's opening.bnr: a 96x32 RGB5A3 image in 4x4 tiles."""
    with open(iso, "rb") as f:
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
        count = struct.unpack(">I", fst[8:12])[0]
        names = fst[count * 12:]
        for i in range(1, count):
            entry = fst[i * 12:i * 12 + 12]
            name_off = struct.unpack(">I", b"\0" + entry[1:4])[0]
            name = names[name_off:names.index(b"\0", name_off)]
            if entry[0] == 0 and name == b"opening.bnr":
                off, _ = struct.unpack(">II", entry[4:12])
                f.seek(off + 0x20)
                pixels = f.read(96 * 32 * 2)
                break
        else:
            return None
    rgba = bytearray(96 * 32 * 4)
    i = 0
    for ty in range(0, 32, 4):
        for tx in range(0, 96, 4):
            for y in range(4):
                for x in range(4):
                    v = struct.unpack(">H", pixels[i:i + 2])[0]
                    i += 2
                    if v & 0x8000:
                        r, g, b, a = (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255
                    else:
                        r, g, b = (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17
                        a = (v >> 12 & 7) * 255 // 7
                    p = ((ty + y) * 96 + tx + x) * 4
                    rgba[p:p + 4] = bytes((r, g, b, a))
    return bytes(rgba)


def make_icon(iso, resources):
    try:
        from PIL import Image
    except ImportError:
        print("Pillow not installed: skipping the icon")
        return False
    rgba = banner_rgba(iso)
    if not rgba:
        return False
    banner = Image.frombytes("RGBA", (96, 32), rgba).resize((960, 320), Image.LANCZOS)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.makedirs(iconset)
        base = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
        plate = Image.new("RGBA", (920, 920), (18, 16, 14, 255))
        base.paste(plate, (52, 52))
        base.paste(banner, (32, 352), banner)
        for size in (16, 32, 64, 128, 256, 512):
            base.resize((size, size), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}.png"))
            base.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}@2x.png"))
        run("iconutil", "-c", "icns", iconset, "-o", os.path.join(resources, "AppIcon.icns"))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", help="disc image, used only to make the icon from its banner")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = ap.parse_args()

    binary = os.path.join(ROOT, "build", "rotk")
    if not os.path.exists(binary):
        sys.exit("build/rotk not found: build first")
    app = os.path.join(args.out, f"{APP_NAME}.app")
    shutil.rmtree(app, ignore_errors=True)
    macos = os.path.join(app, "Contents", "MacOS")
    resources = os.path.join(app, "Contents", "Resources")
    os.makedirs(macos)
    os.makedirs(resources)
    exe = os.path.join(macos, "rotk")
    shutil.copy2(binary, exe)
    n = bundle_dylibs(exe, os.path.join(app, "Contents", "Frameworks"))
    has_icon = bool(args.iso) and make_icon(args.iso, resources)

    info = {
        "CFBundleName": APP_NAME,
        "CFBundleDisplayName": "The Lord of the Rings: The Return of the King",
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleExecutable": "rotk",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "0.1",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "12.0",
        "LSApplicationCategoryType": "public.app-category.action-games",
        "NSHighResolutionCapable": True,
        "GCSupportsControllerUserInteraction": True,
    }
    if has_icon:
        info["CFBundleIconFile"] = "AppIcon"
    with open(os.path.join(app, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    run("codesign", "--force", "--deep", "--sign", "-", app)
    print(f"{app}: {n} bundled libraries, icon: {'yes' if has_icon else 'no'}")


if __name__ == "__main__":
    main()
