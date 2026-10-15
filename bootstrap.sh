#!/bin/sh
# Build the native macOS port from your own disc image.
# usage: ./bootstrap.sh <disc.iso> [--no-pgo] [--app]
#
# The repository ships no game data. This script reads the game's code (main.dol) from the
# disc you give it, recompiles it to C with DolRecomp, and builds recomp/build/rotk.
# Safe to re-run: every step skips work that is already done.
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)

DOL_SHA1=86ff4c72e0f90be7bcd72d5ec9bef2f2fb20c473   # GKLE69 (USA), rev 0
PGO=use
APP=0
ISO=
for a in "$@"; do
  case $a in
    --no-pgo) PGO= ;;
    --app) APP=1 ;;
    -*) echo "unknown option $a" >&2; exit 2 ;;
    *) ISO=$a ;;
  esac
done
[ -n "$ISO" ] || { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ -f "$ISO" ] || { echo "no such file: $ISO" >&2; exit 1; }
ISO=$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 ($2)" >&2; exit 1; }; }
need clang "install Xcode or the Command Line Tools"
need cmake "brew install cmake"
need git "install Xcode or the Command Line Tools"
need python3 "install Xcode or the Command Line Tools"
if command -v brew >/dev/null 2>&1; then
  for p in sdl3 abseil freetype libpng; do
    brew list --versions "$p" >/dev/null 2>&1 || echo "warning: Homebrew package $p is not installed (brew install $p)" >&2
  done
fi
JOBS=$(sysctl -n hw.ncpu)

echo "== third-party sources"
git submodule update --init --recursive
apply_patch() {  # <tree> <patch>: apply once
  if git -C "$1" apply --reverse --check "$ROOT/$2" 2>/dev/null; then
    echo "$2 already applied"
  else
    git -C "$1" apply "$ROOT/$2"
  fi
}
apply_patch third_party/aurora recomp/patches/aurora-retail-fifo.patch
apply_patch third_party/dolrecomp recomp/patches/dolrecomp-flat-vm.patch
apply_patch third_party/dolrecomp recomp/patches/dolrecomp-fast-fp-helpers.patch
apply_patch third_party/dolrecomp recomp/patches/dolrecomp-llvm-inline-single-fp.patch

echo "== game code from your disc"
python3 tools/gcm_extract.py "$ISO" extracted --sys-only
got=$(shasum -a 1 extracted/sys/main.dol | cut -d' ' -f1)
if [ "$got" != "$DOL_SHA1" ]; then
  echo "main.dol sha1 is $got, expected $DOL_SHA1." >&2
  echo "This port supports only The Return of the King, USA (GKLE69). Other regions and revisions" >&2
  echo "have different code addresses, so decomp/rotk.map and the chassis hooks do not match them." >&2
  exit 1
fi

echo "== DolRecomp"
cmake -S third_party/dolrecomp -B third_party/dolrecomp/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build third_party/dolrecomp/build -j"$JOBS" --target dolrecomp dr_cpu

echo "== recompile main.dol to C"
if [ ! -f recomp/out/.dol_sha1 ] || [ "$(cat recomp/out/.dol_sha1)" != "$got" ]; then
  rm -rf recomp/out
  third_party/dolrecomp/build/dolrecomp -j"$JOBS" --gamecube --cpu gekko --map decomp/rotk.map \
    extracted/sys/main.dol recomp/out
  python3 recomp/tools/add_entry_hooks.py decomp/symbols_final.tsv recomp/out/generated/chunks >/dev/null
  echo "$got" > recomp/out/.dol_sha1
else
  echo "recomp/out is up to date"
fi
python3 recomp/tools/add_hle_points.py recomp/chassis/hle_points.txt recomp/out/generated/chunks >/dev/null

echo "== native build (the first build fetches Dawn and takes a while)"
cmake -S recomp -B recomp/build -DCMAKE_BUILD_TYPE=Release -DROTK_PGO="$PGO"
cmake --build recomp/build -j"$JOBS"

mkdir -p "$HOME/Library/Application Support/rotk"
printf '%s' "$ISO" > "$HOME/Library/Application Support/rotk/disc_path"

if [ "$APP" = 1 ]; then
  python3 recomp/tools/make_app.py --iso "$ISO"
  echo "done: open \"recomp/dist/Return of the King.app\""
else
  echo "done: recomp/build/rotk \"$ISO\""
fi
