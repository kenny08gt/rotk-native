# The Return of the King, native on Apple silicon

A native macOS (arm64) port of *The Lord of the Rings: The Return of the King* for GameCube (USA,
`GKLE69`). The game's PowerPC code is recompiled ahead of time to C with
[DolRecomp](https://github.com/ExpansionPak/DolRecomp). It is built with clang and runs with a small
host "chassis" (devices, audio, input, memory card) and [Aurora](https://github.com/encounter/aurora)
(GX on Metal). Nothing is emulated at run time.

**This repository contains no game data.** It has no disc image, no game code, no assets and no
recompiled output. You need your own copy of the game. The build reads the code from your disc and
recompiles it on your Mac.

## Getting started

Requirements: an Apple silicon Mac, Xcode or the Command Line Tools, and Homebrew packages:

```
brew install cmake sdl3 abseil freetype libpng
```

Make an image of your own disc (for example with a Wii and CleanRip). Plain `.iso`/`.gcm` and NKit
`.iso` images work. Then:

```
git clone --recursive <this repo> rotk && cd rotk
./bootstrap.sh "/path/to/your/Return of the King (USA).iso" --app
open "recomp/dist/Return of the King.app"
```

`bootstrap.sh` checks that the disc is the supported release by the SHA-1 of its `main.dol`. It then
patches the third-party trees, recompiles the game to C, and builds `recomp/build/rotk`. The first
build downloads Dawn and takes a while. If your clang rejects the bundled optimization profile, add
`--no-pgo`.

Controls, display options and debugging knobs are in [recomp/README.md](recomp/README.md). How the
port was made, step by step, is in [NOTES.md](NOTES.md).

## What is in here

| Path | Contents |
|---|---|
| `recomp/chassis/` | Host side: exception and interrupt delivery, DI/SI/EXI/AI/DSP devices, AX audio HLE, GX FIFO to Aurora, input |
| `recomp/patches/` | Changes to Aurora (retail GX FIFO support) and DolRecomp (flat guest memory) |
| `recomp/pgo/rotk.profdata` | Profile for profile-guided optimization of the recompiled code (counters only) |
| `decomp/rotk.map`, `decomp/*.tsv` | Function addresses and names found during reverse engineering |
| `tools/` | Disc extractor, chunk-format parser, Ghidra scripts |

## License

GPL-3.0 (see `LICENSE`). The build links DolRecomp's runtime (GPL-3.0), and `recomp/chassis/ax_hle.cpp`
is adapted from Dolphin (GPL-2.0-or-later). Aurora is MIT. The game itself belongs to its rights
holders and is not covered by this license.
