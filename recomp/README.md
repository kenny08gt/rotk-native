# Native macOS (Apple silicon) build of GKLE69

The Return of the King (GameCube, USA) recompiled to arm64 with DolRecomp's C backend, plus a host
"chassis" (CPU dispatch, devices, audio, input) and Aurora (GX on Metal via Dawn). No emulator at run
time: the game's PowerPC code is compiled ahead of time to native code.

## Play

```
./build/rotk "/path/to/Lord of the Rings, The - The Return of the King (USA).nkit.iso"
```

or build the app (`python3 tools/make_app.py --iso <disc>`) and open `dist/Return of the King.app`; it
asks for the disc image once and remembers it (`~/Library/Application Support/rotk/disc_path`).

Controls (keyboard, port 1; the first connected game controller also works):

| GameCube | Key | | GameCube | Key |
|---|---|---|---|---|
| Stick / D-pad | arrow keys | | A | X |
| START | Return | | B | Z |
| X | S | | Y | A |
| L / R | Q / W | | Z | E |
| C-stick | I J K L | | | |

Fullscreen: Control-Command-F or Option-Return (or start with `FULLSCREEN=1`). The picture keeps the
TV's 4:3 shape (`ROTK_STRETCH=1` fills the screen). `ROTK_MSAA=4` adds anti-aliasing, `ROTK_ANISO=<n>`
sets texture filtering (default 16).

Saves go to a 251-block memory card image at `~/Library/Application Support/rotk/MemoryCardA.raw`
(the game formats it the first time; a Dolphin raw card image works too). `MEMCARD=<path>` uses another
file, `MEMCARD=none` leaves the slot empty.

## Build

Needs Xcode (clang), CMake and the Homebrew packages Aurora links (sdl3, abseil, freetype, libpng).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROTK_PGO=use   # pgo/rotk.profdata
cmake --build build -j8
```

Options: `ROTK_FAST_CPU` (inline memory/FP fast paths, `chassis/fast_cpu.h`, default ON),
`ROTK_PGO=gen|use` (profile-guided optimization of the generated code; to refresh the profile build with
`gen`, play a while with `LLVM_PROFILE_FILE=pgo/rotk-%p.profraw`, then
`xcrun llvm-profdata merge -o pgo/rotk.profdata pgo/*.profraw`), `ROTK_TRACE_HOOKS` (function-entry
hooks for `FN_TRACE`, debugging only).

`ROTK_BACKEND=llvm` (experimental) links DolRecomp's LLVM backend instead of the C chunks. That backend
promotes guest registers to host registers within each function and calls guest functions directly.
It needs LLVM 20 (`brew install llvm@20`), a DolRecomp build with `-DDOLRECOMP_ENABLE_LLVM=ON`, and its
objects in `out-llvm`:

```
../third_party/dolrecomp/build-llvm/dolrecomp -j10 --backend llvm --runtime recompcore --targets host \
  --semantics exact --gamecube --cpu gekko --map ../decomp/rotk.map ../extracted/sys/main.dol out-llvm
cmake -S . -B build-llvm -DCMAKE_BUILD_TYPE=Release -DROTK_BACKEND=llvm && cmake --build build-llvm -j8
```

The LLVM functions can only be entered at their region leaders; for any other pc the fallback hook
hands over to the C chunks, which the LLVM build also links (as `cfunc_*`). It runs the game, but
in the first big fight it executes as many host instructions per guest cycle as the C build:
every exact floating-point operation is an out-of-line call (`chassis/fast_helpers.c` supplies the
fast paths), and guest registers are written back around each call.

Third-party trees are patched: `patches/aurora-retail-fifo.patch` (Aurora, MIT) and
`patches/dolrecomp-flat-vm.patch` (DolRecomp, GPL-3.0). `chassis/ax_hle.cpp` is adapted from Dolphin
(GPL-2.0+).

Regenerating the C from the DOL:

```
../third_party/dolrecomp/build/dolrecomp -j8 --gamecube --cpu gekko --map ../decomp/rotk.map ../extracted/sys/main.dol out
python3 tools/add_entry_hooks.py ../decomp/symbols_final.tsv out/generated/chunks   # no-op unless ROTK_TRACE_HOOKS
```

## Test and debug runs

`rotk <sysdir|disc> <disc> <symbols.tsv> <max_dispatches> <seconds>` is the scripted form used by
`capture.sh <outdir> <seconds> <interval>` (runs and screenshots the window every few seconds).

| Variable | Effect |
|---|---|
| `AUTOPRESS="A@6,START@20"` | scripted button presses (seconds since start) |
| `NO_AUDIO`, `AUDIO_DUMP=x.wav` | mute; record the game's audio output |
| `PERF_LOG=1`, `NO_THROTTLE=1` | per-10 s speed capacity and fps; run unpaced |
| `ROTK_CPU_CLOCK=x` | guest CPU overclock (guest code costs 1/x of its usual time) |
| `NO_HLE=1`, `HLE_VERIFY=1` | turn off the native versions of hot routines (`chassis/hle.c`); check each call against the generated code and time both |
| `SYNC_GPU=1`, `NO_GPU=1` | GPU on the CPU thread; no window |
| `PC_HIST=<s>` | hot functions by host time after `s` seconds |
| `TRACE_AT=a,b TRACE_ARGS=1 TRACE_N=n` | dispatcher-level call trace with args |
| `FN_TRACE=a,b FN_TRACE_N=n` | every entry to these functions (`ROTK_TRACE_HOOKS` build) |
| `WATCH=addr,len [WATCH_PC=lo,hi]`, `DMA_TRAP=addr` | who writes guest memory (CPU / device DMA) |
| `DUMP_WORDS=a,b`, `DUMP_MEM=addr,len,path`, `THREADS_AT=<s>` | inspect state |
| `DSP_LOG`, `DI_LOG`, `SI_LOG`, `EXI_LOG`, `AX_LOG`, `DI_INSTANT` | device logs; instant disc reads |
| `GPU_STATS`, `AURORA_VERBOSE`, `AURORA_DUMP_DRAWS=n` | renderer statistics and dumps |

`./bench.sh <label>` plays into the first big Helm's Deep fight with scripted presses and prints the fight's
speed and fps, plus host instructions per guest cycle, which is the comparison to trust when other
programs load the Mac. Logs go to `runs/bench/`.

Native routines hook into the generated code at the addresses in `chassis/hle_points.txt`
(`tools/add_hle_points.py`, run by `bootstrap.sh`): loop heads, and the entries and return sites of
functions called from inside the same chunk.

`tests/fast_cpu_diff.c` checks `fast_cpu.h` against the exact DolRecomp helpers on random inputs:
`clang -O1 -w -I../third_party/dolrecomp/src tests/fast_cpu_diff.c ../third_party/dolrecomp/build/libdr_cpu.a -o /tmp/fcd && /tmp/fcd`.
