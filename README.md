# The Return of the King, native on Apple silicon

A native macOS (arm64) port of *The Lord of the Rings: The Return of the King* for the GameCube
(USA release, game ID `GKLE69`).

The game's PowerPC code is recompiled ahead of time to C with
[DolRecomp](https://github.com/ExpansionPak/DolRecomp) and compiled with clang for the M-series CPU.
Graphics go through [Aurora](https://github.com/encounter/aurora), which draws the GameCube's GX
command stream with Metal. A small host layer (the "chassis") stands in for the console's hardware:
interrupts and timers, the disc drive, controllers, memory card and audio DSP. No emulator runs while
you play. The game's own code runs natively.

> **No game data is included.** This repository contains no disc image, game code, assets or
> recompiled output. You need your own copy of the game. The build reads the code from your disc and
> recompiles it on your Mac.

## Status

The game plays from boot to gameplay, with sound:

- The boot sequence, logos, menus, the intro movie and Helm's Deep gameplay all render correctly.
- Music, sound effects and movie audio work through the GameCube's audio DSP, which is
  reimplemented on the host.
- Memory card saves go to a file on your Mac. The game formats it the first time.
- The picture keeps the 4:3 shape at any window size, with fullscreen, anisotropic filtering and
  optional MSAA.
- Speed is 100% or more in menus, movies and most levels. The heaviest combat scenes run at about
  88-97%, so they can slow down a little. That work continues.

Only the first levels are tested so far. Expect bugs further into the game. Please open an issue if
you find one.

## Requirements

- A Mac with Apple silicon (M1 or later).
- Xcode or the Command Line Tools (`xcode-select --install`).
- [Homebrew](https://brew.sh) packages:

  ```
  brew install cmake sdl3 abseil freetype libpng
  ```

- An image of your own disc of *The Return of the King* (USA). You can dump it with a Wii or a
  Wii U and [CleanRip](https://wiibrew.org/wiki/CleanRip). Plain `.iso` and `.gcm` images work, and
  so do NKit `.iso` images. Other regions (PAL, Japan) are not supported yet, because their code is
  at different addresses.

## Build and play

```
git clone --recursive https://github.com/kenny08gt/rotk-native.git
cd rotk-native
./bootstrap.sh "/path/to/your/Return of the King (USA).iso" --app
open "recomp/dist/Return of the King.app"
```

`bootstrap.sh` does the following:

1. It downloads the pinned Aurora and DolRecomp sources (git submodules) and applies the patches in
   `recomp/patches/`.
2. It reads `main.dol` (the game's executable) from your disc and checks its SHA-1, so you know that
   you have the supported release.
3. It recompiles `main.dol` to C (about 93 MB of generated code) and builds `recomp/build/rotk`.
4. With `--app`, it makes `recomp/dist/Return of the King.app`, with the icon taken from your disc's
   banner.

The first build downloads Dawn (the WebGPU library that Aurora uses) and can take a while.
Later runs reuse what is already built. The app remembers where your disc image is
(`~/Library/Application Support/rotk/disc_path`). If you move the image, it asks for it again.

You can also start the game from the terminal:

```
recomp/build/rotk "/path/to/your/disc.iso"
```

## Controls

Keyboard (player 1). The first game controller you connect (Xbox, PlayStation, Switch Pro and so on)
also works.

| GameCube | Key | | GameCube | Key |
|---|---|---|---|---|
| Control stick, D-pad | Arrow keys | | A | X |
| START | Return | | B | Z |
| X | S | | Y | A |
| L / R | Q / W | | Z | E |
| C-stick | I J K L | | | |

| Action | How |
|---|---|
| Fullscreen | Control-Command-F or Option-Return, or start with `FULLSCREEN=1` |
| Stretch to fill the screen | `ROTK_STRETCH=1` |
| Anti-aliasing | `ROTK_MSAA=4` |
| Texture filtering | `ROTK_ANISO=<1-16>` (default 16) |
| Mute | `NO_AUDIO=1` |

To use the settings with the app, start it from the terminal, for example
`ROTK_MSAA=4 "recomp/dist/Return of the King.app/Contents/MacOS/rotk"`.

## Saves

The memory card in slot A is a 251-block card image at
`~/Library/Application Support/rotk/MemoryCardA.raw`. A raw card image from Dolphin also works. Use
`MEMCARD=<path>` to use another file, or `MEMCARD=none` to start with an empty slot.

## Troubleshooting

| Problem | What to do |
|---|---|
| `main.dol sha1 is ..., expected ...` | Your disc is a different region or revision. Only the USA release (`GKLE69`) is supported. |
| The build fails with a profile or `profdata` error | Your clang can't read the bundled optimization profile. Run `./bootstrap.sh <disc> --no-pgo --app`. The game runs a little slower. |
| CMake can't find SDL3, abseil or freetype | Install the Homebrew packages from [Requirements](#requirements). |
| Picture or speed problems | Run from the terminal with `PERF_LOG=1` and include the output in an issue. |

## How it works

| Path | Contents |
|---|---|
| `bootstrap.sh` | Builds everything from your disc |
| `recomp/chassis/` | The host layer: exception and interrupt delivery, the DI/SI/EXI/AI/DSP devices, AX audio, GX FIFO to Aurora, input |
| `recomp/patches/` | Changes to Aurora (retail GX FIFO support) and to DolRecomp (flat guest memory) |
| `recomp/pgo/rotk.profdata` | Profile for profile-guided optimization of the generated code (counters only) |
| `decomp/rotk.map`, `decomp/*.tsv` | Function addresses and names found during reverse engineering |
| `tools/` | Disc extractor, asset chunk parser, Ghidra scripts |
| [`NOTES.md`](NOTES.md) | How the port was made, step by step, with what went wrong and why |
| [`recomp/README.md`](recomp/README.md) | Build options and every debugging setting |

## Credits

- [DolRecomp](https://github.com/ExpansionPak/DolRecomp): the GameCube/Wii static recompiler.
- [Aurora](https://github.com/encounter/aurora): the GX implementation on WebGPU/Metal.
- [Dolphin](https://github.com/dolphin-emu/dolphin): the AX audio HLE is adapted from its code, and
  its documentation of the hardware made this possible.
- [decomp-toolkit](https://github.com/encounter/decomp-toolkit),
  [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and the
  [GameCube loader](https://github.com/Cuyler36/Ghidra-GameCube-Loader) were used for the analysis.

## License

GPL-3.0 (see [`LICENSE`](LICENSE)). The build links DolRecomp's runtime (GPL-3.0), and
`recomp/chassis/ax_hle.cpp` is adapted from Dolphin (GPL-2.0-or-later). Aurora is MIT.

*The Lord of the Rings: The Return of the King* is a trademark of its owners. This is an unofficial
fan project. It is not affiliated with or endorsed by Electronic Arts, New Line Cinema, Nintendo or
the Tolkien Estate. It does not include or distribute any of their copyrighted material.
