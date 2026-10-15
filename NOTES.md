# LOTR: The Return of the King (GameCube, GKLE69): native macOS port recon

Goal: run the game natively on Apple Silicon, without an emulator.
Approach: an *understanding* decompilation, where readable C serves as the spec for a reimplementation. This is not a matching (byte-exact) decomp, so the original CodeWarrior compiler is not needed.

## What the binary is

| | |
|---|---|
| Disc | NKit v01 GameCube image, game ID `GKLE69` (EA, USA) |
| Executable | `extracted/sys/main.dol`, 1.69 MB. Code is 1.45 MB (`.init` 0x80003100, `.text` 0x800055E0–0x8016F578). Entry 0x80003154, `main` 0x800A01DC |
| Compiler | Metrowerks CodeWarrior (MSL runtime, TRK debug kernel). Mostly **C**, little C++ |
| SDK | Nintendo Dolphin SDK, release builds Apr 2003 |
| Game build | `Build Beta 1.64b Sep 25 2003` |
| Small-data bases | r2 = `0x80259B20` (`_SDA2_BASE_`), r13 = `0x80257840` (`_SDA_BASE_`) |

### Engine layers (from source-file strings)
- **Platform layer `LL*`**: the porting seam. `LLDisp_Gc`, `LLObj_Gc`, `LLPict_Gc`, `LLBufferMgr_Gc`, `LLAram_Gc` (audio RAM), `LLMC_Gc` (memory card), `LLFont`, `LLTexGrp`. The `_Gc` suffix implies sibling PS2/Xbox/PC implementations existed.
- **Render**: `RenderCtx_Gc`, `ShaderObject*_Gc`, `WeightedSkinCommonGC`, `FrameBuf`, `ScreenEffects`, `Viewport`, `LightingManager`.
- **Utility `U*`**: `URes`, `UFstPart`, `UStream`, `UHeap`, `UMemPool`, `UFont`, `UMiscNGC`, `UAudMemStack`.
- **Game**: `Character`, `ActorManager`, `EntityManager`, `Camera`, `SplineCam`, `Physics_Interface`, `Collision*`, `TriggerManager`, `RegionManager`, `*DTB` (data tables: Player, Npc, Weapon, Abilities, ...).
- **Video**: `M4Decoder.cpp` (EA's own codec) at 0x80166D28.

## Code inventory

| Region | Functions | Size | Named |
|---|---|---|---|
| Game + engine (< 0x80129220) | 5,320 | ~1,176 KB | ~0 (a few by source-file tag) |
| SDK / MSL / middleware (≥ 0x80129220) | 1,181 | ~280 KB | 837 (~70%, dtk + Dolphin signature DB; includes some false matches on short functions, e.g. `cPlayer::PostPhysicsUpdate`, `AIRegisterDMACallback` inside GX) |

- **Direct hardware calls are few.** About **180 of 5,320** game functions (~3.5%) call SDK APIs or write the GX FIFO directly: GX 60, OS 67, CARD 15, DC 14, DVD 12, inline FIFO writes 11. That doesn't make the rest platform-neutral: the render path is still shaped by GX (vertex descriptors, TEV, display lists).
- **Paired singles (Gekko SIMD) are common**: **880 / 5,301** game functions use `psq_*` / `ps_*` (counted on the asm in `decomp/build/asm`). A recompiler must translate these, and they mostly appear as float math in the decompiled C.
- **Threading and async**: `OSThread.c` is the most-referenced source tag (26). OS threads, alarms, interrupt-driven DVD callbacks and the async GX FIFO make the OS shim harder than the call-site count suggests.
- Ghidra decompiles **6,513 / 6,513** functions with 0 failures and 2 bad-data blocks.
- `UMem_AllocTracked(size, flags, align, "File.c", line)` at 0x80009258 has 130 call sites. Its file/line arguments label which source file each caller belongs to.
- `main` is a state machine over a table at `0x8017AFA0` (16-byte entries: enter/update/next callbacks). State lives in `0x802511DC`; -3 means quit.

## Data formats (all EA 4CC chunk streams: `tag[4]`, `be32 size` incl. header)
- `.scg` (levels, characters): `CTRL` / `SHOC` containers holding `SHDR` / `SDAT` / `RPNS` / `SONO` / `SWVR`, padded with `FILL` / `PADD`. `FILL` always pads to the next 64 KB boundary, and with <8 bytes left it has no size field. Padding holds little-endian x86 stack addresses, so the assets came from Windows PC tools.
  - **Layout A** (`LoadOnce`, `GlobScen`, `Player/*`): one chunk stream. 26 files parse start to end with `tools/chunks.py`.
  - **Layout B** (18 level files, `Game/<lvl>/*.scg`): a `STOC` table at 0 (entries `name_off, offset, size, type`; names at TOC+name_off) lists an `initial_load` segment at 0x10000, then streamed `zone_<lvl>_zoneN` segments, all in the same chunk format. **After the last zone there's a large untabled tail** (min00: 0x1140000–0x4170000, ~48 MB, no 4CC tags). It's probably streamed audio. Unconfirmed.
- `.vid` (FMV): `VID1` / `HEAD` (`VIDH`: frame count, dimensions) then `FRAM` → `VIDD` blocks. Proprietary codec (no MPEG-4 start codes). `M4Decoder.cpp` in the DOL is the spec.

## Toolchain (all local, in `tools/`)
- `gcm_extract.py`: GameCube/NKit disc → `extracted/{sys,files}`.
- `dtk` 1.8.4 (decomp-toolkit, arm64): function boundaries and splitting. Config in `decomp/config.yml`.
- `dsy_match.py`: Dolphin `totaldb.dsy` signature matching. Trust it only in the SDK region; small functions collide.
- Ghidra 12.1.4 + Cuyler36 GameCube loader (Gekko/Broadway language, compiled .sla) + **decompiler built from source for arm64** (the release ships none for mac_arm_64).
- Ghidra scripts: `ApplySymbols.java` (names + source-file tags), `SetSDA.java` (r2/r13/GQR0), `DumpDecomp.java` (whole-program C → `decomp/c/`).
- Project: `decomp/ghidra/rotk.gpr`. Opening it in the Ghidra GUI shows the same names.

Re-run the dump:
```
tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless decomp/ghidra rotk -process main.dol -noanalysis \
  -scriptPath tools/ghidra_scripts -preScript SetSDA.java 80259B20 80257840 \
  -preScript ApplySymbols.java decomp/renames.tsv decomp/srcfile_refs.tsv -postScript DumpDecomp.java decomp/c
```
Add names to `decomp/renames.tsv` (addr, 0, name) as functions get identified.

## GX usage vs Aurora (step 1, done)

Inputs: `decomp/calls.tsv` (21,545 call edges from Ghidra, `ExportCalls.java`) and `decomp/gx_used.txt`. Aurora was checked at commit `5179721` (2026-10-12), shallow-cloned read-only to `third_party/aurora`. Aurora is MIT, runs on macOS over Metal (Dawn/WebGPU), and also provides DVD (nod: ISO/RVZ/…), PAD (SDL3), CARD (Dolphin .gci/.raw), VI, THP, and parts of OS/MTX.

- **The game reaches 118 GX functions** (87 called directly from game code). I identified 25 unnamed GX-library functions from their register writes and recorded them in `decomp/renames.tsv`. Note that some Dolphin-DB names in the SDK region are wrong (`AIRegisterDMACallback` and `IFFifoAlloc` inside the GX library).
- **Aurora defines 97 of the 118.** The 21 missing:
  - **No-op on a host** (FIFO/hardware management): `GXInitFifoLimits`, `GXGetFifoLimits`, `GXEnableBreakPt`, `GXDisableBreakPt`, `GXResetWriteGatherPipe`, `GXAbortFrame`, `GXSetMisc`, `GXSetGPMetric`, `GXClearBoundingBox`, `GXInitTexCacheRegion`, `GXInitTlutRegion`, `GXSetTexRegionCallback`, `GXSetCopyClamp`, `GXSetDispCopyFrame2Field`.
  - **Need real work**: `GXLoadTexObjPreLoaded` (TMEM preload, so map it to a normal load), `GXLoadTexMtxIndx` (a single `LOAD_INDX_C` FIFO command, which Aurora's command processor already decodes), and `GXPokeAlphaMode` / `GXPokeAlphaUpdate` / `GXPokeBlendMode` / `GXPokeDstAlpha` / `GXPokeZMode` (CPU→EFB poke state; find the effect that uses it before deciding).
  - Also check `GXSetZTexture`, `GXCopyTex` (render-to-texture), and `GXSetDispCopy*`, which look partial in Aurora.
- **Aurora's GX is a real big-endian FIFO byte stream plus a command processor** (`lib/gx/fifo.hpp`, `command_processor.cpp`: BP/CP/XF loads, indexed loads, call-DL, draw prims). `GXCallDisplayList` feeds raw bytes into it. So:
  - The game's prebuilt display lists (inside `.scg`) can be fed in unchanged.
  - Recompiled stores to `0xCC008000` (the 11 inline-FIFO game functions plus inlined `GXPosition*` / `GXColor*` vertex writes) can be routed straight to `aurora::gx::fifo::write_u8/u16/u32`.
  - Design point: GX array bases and display-list pointers in recompiled code are GameCube addresses, so they need translating to host pointers into the emulated RAM block before Aurora dereferences them.
- **Direct MMIO from game code is only GX FIFO (11 functions) and VI (6 functions).** Everything else (DI, SI, EXI, AI, DSP/ARAM, PI, MI) is touched only inside the SDK, so replacing SDK functions covers it.
- **Not covered by Aurora: audio.** AX / AI / DSP / ARAM need our own HLE → CoreAudio. `AR.cpp` exists in Aurora and is worth checking for ARAM.

## DolRecomp on arm64 (step 2, done)

DolRecomp `02e27ac` (GPL-3.0) is cloned in `third_party/dolrecomp`. It builds natively on arm64 with CMake, and all 20 of its tests pass. Workflow and commands: `recomp/README.md`.

- **Translation:** `--gamecube --cpu gekko --map decomp/rotk.map` decodes **370,654 / 370,664** instructions (9 embedded data, 1 unknown). It writes 92 C chunks (93 MB) in 0.5 s and names 950 functions from our map.
- **It compiles for arm64 with clang `-O2`: 92/92 chunks, 0 errors, 0 warnings, 17 MB of objects, ~85 s on 8 cores.**
- **Runtime:** the objects need 43 runtime functions (memory access, FP/paired-single helpers, exceptions). `libdr_cpu.a` provides all of them.
- **Linked binary:** `recomp/rotk_boot` (native arm64 Mach-O) is linked from the chunks plus a ~250-line chassis (`recomp/chassis/main.c`).
- **Boot progress, natively:** `__start` → `OSInit` (FPU/cache/exceptions/interrupts, EXI/SRAM, audio DSP boot, memory protection, `Config24MB`) → `DVDInit` → alarms → static ctors → **`main`** → game init → `VMInit` → **`VIWaitForRetrace`** (`0x80127DA4`), called from game code at `0x80007360` (inside the `LLDisp_Gc` range). The main thread sleeps there, and the scheduler idles in `SelectThread`, waiting for a VI interrupt that nothing delivers yet.
- **Chassis fixes needed so far** (each verified by rerunning):
  - Clear BSS *before* loading sections, because the header's BSS range spans `.sdata`.
  - `sc` → resume at SRR0. The OS's 0xC00 handler only syncs caches.
  - EXI CR start bit clears on read.
  - DSP CSR/mailbox handshake bits.
  - Advance the timebase (CPU cycles / 12).
  - Real-mode physical PC → `0x8xxxxxxx`.
  - FP-unavailable → set MSR[FP].
- **Fallback instructions hit:** `dcbf`, `dcbst`, `icbi`, `mfspr`/`mtspr` HID2/HID0. Skipping them is harmless.
- **The C backend is slow by design.** Each chunk is one function with a per-instruction `switch`/`goto`, `ctx->pc` stores, and a cycle counter. The LLVM backend (needs LLVM 19/20) is the performance path.

### Interrupts, threads, devices (step 3, done)

`recomp/chassis/` now has `main.c` (boot, dispatch, exception delivery) and `hw.c` (devices). Exceptions are delivered the way the OS's low-memory stub does it: save state into `*OSCurrentContext` (`0x800000D4`), then enter the handler from `OSExceptionTable` (pointer at `0x80251924`) with r3 = exception number and r4 = context. The OS's own `__OSDispatchInterrupt`, alarm handler, and `OSSwitchFPUContext` (real lazy FPU switching) run unmodified.

Device model:
- **PI:** INTSR/INTMR.
- **VI:** retrace at 60 Hz on enabled DI registers; line counter.
- **Decrementer:** SPR 22.
- **DI:** `0xA8` reads served from the NKit ISO (FST offsets are valid ISO offsets), inquiry, and the rest complete immediately, with TC interrupt.
- **ARAM DMA:** 16 MB, with interrupt.
- **DSP:** CSR with write-1-to-clear bits; mailboxes; boot ROM greeting `0x8071FEED`; the boot-task upload is acknowledged with `0xDCD10000` + interrupt.
- **AI:** sample counter.
- **EXI / SI:** transfers complete immediately.
- **CP:** FIFO breakpoint fires at once.
- **PE:** finish interrupt on the draw-done BP write seen in the FIFO stream.
- **Write-gather pipe:** bytes are counted and scanned. Aurora is not hooked up yet.

**Result (40 s run):** the game reaches its front end and runs its main loop:
- 4,660 frames completed against 4,674 VI retraces, so it paces at 60 fps.
- 78.8 MB of GX commands generated.
- 158 disc reads (9.8 MB): `LoadOnce.scg`, `GlobScen.scg`, `Game/fro/fro03.scg`, `MemCard/banicons.tpo`.
- The DSP task is running.
- **Guest time runs at ~195% of real time on one M1 core with the C backend.** Front-end load only; gameplay and real rendering will cost more.

Env knobs: `TRACE_AT=<pc>` (backtrace), `DSP_LOG=1` (mail log), `DI_LOG=1` (disc read log).

### Graphics through Aurora (step 4, first frames)

`recomp/` is now a CMake project: Aurora as a subdirectory, the 92 chunks as `rotk_chunks`, and the chassis (`main.c`, `hw.c`, `gpu.c`) linked into `build/rotk`. `capture.sh` runs it and screenshots only its window (`tools/winid`).

**FIFO model (chassis):** write-gather-pipe bytes go to guest RAM at the PI write pointer, wrapping base..end. That's correct for display-list recording too, verified by dumping real CP/XF/draw bytes from a recorded list. When the CPU FIFO is linked, the GPU drains read->write pointer at the draw-done BP write and at breakpoint arming. Halves that straddle the ring's wrap point are joined first; feeding them separately caused a reader overrun.

**Aurora patches** (`recomp/patches/aurora-retail-fifo.patch`; Aurora's own API uses custom `GX_AURORA` commands with host pointers, a retail stream carries hardware registers with GameCube addresses):
- Guest-RAM hook (`aurora_gx_set_guest_ram`) and raw feed (`aurora_gx_feed_fifo`); FIFO processing set to `Inline`.
- CP array bases `0xA0`-`0xAF` resolve into guest RAM. Upload size grows per draw from the highest index used; the whole-RAM size aborted in `gfx::push`.
- `CALL_DL` recurses into guest RAM.
- BP texture Image3 sets the data pointer (`texObjId = 0` uses the content-hash cache).
- TLUT loads (`0x64`/`0x65`) and per-map SETTLUT (`0x98`/`0xB8`): TMEM offsets are mapped onto Aurora's 20 palette slots.
- **Retail screen bias:** scissor = register - 2 * box-offset register, and the viewport bias is 342 (Aurora native uses 340). The empty scissor this caused was the black screen.

**Result:** the front end renders natively: backdrop, dialog, fonts, leaf cursor (`docs/screens/frontend_with_text.png`). 78 draws/frame, all sampled textures resolve, still >160% real time.

**Known issues:**
1. Menu text shows string keys (`nomedia ENG`, `playanyway ENG`): the localized string lookup fails. Find the string table load or language source.
2. The game renders 512x448 and the display copy is a stub, so the frame is unscaled in the window. Scale the EFB->XFB copy.
3. No controller input, so the game can't leave this screen.
4. Audio: DSP answers only the boot handshake.

Debug env: `GPU_STATS=1` (draws/textures per 60 frames), `AURORA_DUMP_DRAWS=n` (per-draw state), `AURORA_DEBUG_CLEAR=1` (magenta clear), `AURORA_VERBOSE=1`.

### Input, timers, virtual memory (step 5)

- **Controller (port 0):** the SI model in `hw.c` answers type `0x0900`, origin and polling. `chassis/input.c` maps the keyboard and the first SDL gamepad. Keys: arrows = stick + D-pad, Enter = START, X = A, Z = B, S = X, A = Y, Q/W = L/R, E = Z, IJKL = C-stick. Computed registers answer any access width. `AUTOPRESS="A@6,START@20"` scripts presses for unattended tests.
- **Timer bug, fixed:** DolRecomp emits `mtdec` and other SPR moves as *fallback instructions*, and the chassis skipped them. So the decrementer was never programmed and OS alarms never fired, which stalled the SDK's delayed SI type re-query and with it the PAD reset sequence. The fallback now decodes `mtspr`/`mfspr` into the device model's SPR callbacks; cache ops stay no-ops. Result: about 60 decrementer interrupts per second, and PAD reset completes.
- **Virtual memory:** the game uses the SDK VM library (`VMInit`): an MMU-mapped window at `0x7E000000` paged from ARAM. The runtime had no MMU, so those reads returned 0, and an asset binary search (`fn_8004FB58`) never found its key. Fix: `cpu_set_flat_vm()` (`recomp/patches/dolrecomp-flat-vm.patch`) backs the window with 32 MB of flat host memory, as if every page were resident.
- **Result:** pressing A on "play anyway" starts a new game, which loads `Player/Char_Gan.scg` and `Game/hel/hel01.scg` (Helm's Deep) and starts the intro movie `Movies/helmfmv1.vid`. The movie stalls on a black screen: only 1.9 MB of 51 MB read in 40 s, START/A don't skip it, and it's drawn as a textured quad (2 draws/frame). Most likely cause: playback is paced by the audio clock, and the DSP only answers the boot handshake (no AX microcode). **Next: AX audio HLE** (DSP mailbox command lists, voice mixing, AI DMA), or find what the movie player waits on and unblock it.
- Speed stays at about 155-177% of real time on one M1 core for menus and loading. Gameplay is not measured yet.

### Intro movie investigation (step 6)

- **Hot-function profile** (`PC_HIST=<s>`): during the movie the CPU runs the EA video decoder: macroblock loop `fn_801669A0` (near `M4Decoder`, `0x80166D28`), bit reader `fn_8016D7F4`, skipped-block copy `fn_8016BF08`. So the decoder is running.
- **Rendering:** each frame is one 6-stage TEV draw (Y 576x512 I8 in map0, U/V 288x256 I8 in maps 1/2; konst colors and TEV regs hold the YUV->RGB coefficients), then the display copy, then an opaque clearing quad. The draw covers the frame (magenta-clear test). Luma stays at 16-22 (video black) for the whole run, so the black screen matches the decoded content: the movie is stuck in its opening frames.
- **AI DMA timing** added in `hw.c`: 32 kHz stereo blocks replay continuously and raise AIDINT, about 200/s, which drives AX's frame callback.
- **AX:** the DSP answers each command list (`0xBABE<size>` + address) with `DSP_YIELD` `0xDCD10002` + interrupt, and the CPU answers `0xCDD1....`. That's 200 lists/s, but no voice processing or mixing.
- **Still stuck:** the movie still reads only about 2 MB of `helmfmv1.vid`. Most likely it syncs to its soundtrack voice, whose position only advances when the DSP processes voice parameter blocks (PBs).
- **Next:** AX HLE voice processing. Parse the command list (PB list address, output buffers), advance every active voice's position from its sample-rate ratio with loop/end handling, and write the PBs back. Doing the mixing on top of that (ADPCM/PCM8/PCM16 decode, volume, output into the AI buffer -> SDL audio) also gives sound. The PB layout depends on the AX version; Dolphin's AX HLE (`Source/Core/Core/HW/DSPHLE/UCodes/AX*.cpp`) is the reference.

### Audio HLE and the intro movie (step 7)

- **AX microcode:** HashEctor `0xe2136399`, the standard GameCube AX (same as Billy Hatcher / MP5 / TMNT). `chassis/ax_hle.cpp` adapts Dolphin's AX HLE (GPL-2.0+): mail state machine, all command-list commands, PB read/update/write, accelerator (ADPCM/PCM8/PCM16 + loops), linear resampling (the polyphase coefficients live in DSP ROM, so linear is used, as Dolphin does without them), volume envelope, LPF, 9-buffer mixing, and output to the AI buffer. `chassis/audio.c` streams finished AI DMA blocks to SDL3 (32 kHz stereo); `NO_AUDIO=1` mutes it.
- **The movie was never stalled:** it reads exactly 24 frames per guest second, and its opening chunks are tiny. The black screen came from four Aurora gaps on the retail path:
  1. **Row-60 matrix:** the movie draws with position matrix row 60 (`0xF0` in XF memory). Aurora only had slots for rows 0-59 and treated `0xF0` as the GX_IDENTITY texture matrix, discarding the data. Fix: an 11th matrix slot (`MaxTexMtx = 11`); texgens that use GX_IDENTITY are still synthesized in the shader.
  2. **Manual texcoord scale:** texcoords are in texels (`GXSetTexCoordScaleManually`), so the sampling uv becomes `uv * SU scale / texture size` in retail mode. With automatic scaling this reduces to `uv`.
  3. **Indexed matrix loads** (`LOAD_INDX_*`, e.g. `GXLoadTexMtxIndx`) now grow the retail array size.
  4. **Per-frame storage** raised from 8 to 64 MiB, and **logic-op blending** approximated with blend equations (after Dolphin), instead of a fatal error.
- **Debugging lessons:** my first luma check sampled only the letterbox rows (video black). Dumping the whole frame-buffer region showed real decoded frames. The `WATCH=addr,len [WATCH_PC=lo,hi]` write watchpoint and `DUMP_MEM=addr,len,path` were the decisive tools.
- **Result:** the New Line logo and the Isengard/Orthanc recap play natively (`docs/screens/movie_*.png`). The menu text is now localized ("There is no Memory Card in Slot A..."); the earlier `nomedia ENG` key text was a side effect of the timer/VM bugs.
- **Next:** see what follows the movie (gameplay?), check that movie and in-game audio is audible, scale the 512x448 frame to the window, and memory card support (Aurora's CARD can back it with a file).

### Gameplay and speed (step 8)

- **Gameplay renders:** the movie runs about 3:40 and ends in Helm's Deep: Gandalf, skinned soldiers, fire, fog, HUD (health, 0/1000 score) and tutorial prompts ("A Speed", "B Parry"). See `docs/screens/gameplay_helms_deep.png`. No new Aurora fixes were needed.
- **Real-time pacing:** `main.c` used `clock()` (process CPU time, which also counts Dawn/audio threads) and never throttled, so light scenes ran up to 1.8x too fast. It now uses the monotonic wall clock and sleeps whenever the guest timebase gets ahead. `NO_THROTTLE=1` disables pacing. `PERF_LOG=1` prints, every 10 s, the "capacity": how fast the guest could have run in that window.
- **Gameplay is CPU-bound at ~55-58% of real time** on one M1 core (menus/movie 170-230%). Profile (`sample` + `PC_HIST`, which now ranks dispatch roots by host time and ns per guest cycle):
  - ~75% of main-thread time is recompiled guest code. The top hotspot is a paired-single vertex transform cluster (`0x800A9C74`-`0x800AAC80`, ~40%): psq_l + ps_madds loops at 3.5-5 ns per guest cycle (real Gekko: 2.06).
  - ~11% is Aurora FIFO processing, done synchronously on the CPU thread at `GXSetDrawDone`.
  - ~4% is other MMIO and `hw_tick`.
- **`chassis/fast_cpu.h`:** inline fast paths swapped into the generated code through `DOLRECOMP_CPU_HEADER`:
  - main-RAM loads/stores;
  - single, double and paired FP ops without FPRF/FI/FR bookkeeping;
  - quantized psq_l/psq_st.

  Anything unusual (MMIO, NaN/inf/denormal results, enabled FP exceptions, lwarx reservations, `WATCH` journal) falls back to the exact library helper. **Gotcha:** the game runs with FPSCR.NI=1, so the fast paths must accept NI and send only tiny results to the library. Result: movie 170% → 220%, gameplay 47% → 57%. Forcing `always_inline` made no measurable difference and costs a 10-minute rebuild.
- **What remains for full speed:** about 25-30 host instructions per guest instruction, because the C backend keeps all guest registers in `CPUState` memory. Real gains need:
  - register caching across blocks (DolRecomp LLVM backend; it targets the ModernGekko ABI, so this means porting the chassis);
  - Aurora FIFO processing on its own thread (~10%);
  - cheaper dispatch for indirect calls (tiny functions like `fn_800C4908` are dispatched millions of times).

### Playable: sound, saves, scaling, Mac app (step 9)

- **Sound for movies and music was a disc-timing bug.** DI reads used to finish instantly. With an instant read the game's loader skips `logos.vid` and the front end and starts the level and `helmfmv1` at once. The scene's movie-audio entity is then never created: it is "role 3", registered by `fn_800A863C` → `fn_8004F288`, with its slot read back by `fn_8004F3AC`. So the movie controller's audio commands (`fn_80091608` types 7/9) go out with owner `0xFF` and are dropped, giving silent movies and no music. DI reads now complete after ~0.5 ms + len / 3 MB/s (`DI_INSTANT=1` restores the old path). The audio RMS per 5 s then matches Dolphin's dump almost exactly, and the boot flow matches Dolphin (copyright → THX → EA GAMES → intro).
  - **Not used:** DTK disc streaming. The header flag is set, but `DVDPrepareStream` isn't linked. The level `.scg` tails are DSP-ADPCM streams (16-coefficient header), played through AX from ARAM rings refilled by `fn_800278EC`.
  - **Dolphin as an oracle:** use `/Applications/Dolphin.app` with a throwaway `-u` folder, `-b`, `[DSP] DumpAudio=True` and `[Interface] SkipNKitWarning=True`. It writes `Dump/Audio/*dspdump.wav` and `*dtkdump.wav`. Screenshots work through `tools/winid Dolphin`.
- **Memory card** (`chassis/exi.c`): real EXI channel registers (CSR/MAR/LEN/CR/DATA, TC/EXI/EXT interrupts) and a 16 Mbit Nintendo card using Dolphin's command protocol (ID, status, read array, sector erase, page program, interrupts), backed by `~/Library/Application Support/rotk/MemoryCardA.raw`.
  - **SRAM:** the card reports "unlocked", so the CARD library checks the SRAM flash-ID checksum (SRAM byte 0x3A) instead of unlocking. So the IPL device now serves SRAM: command `0x20000100` reads it, and the flash ID is derived from the card header the way Dolphin does it. Before this the game said "damaged".
  - **DSP task switching** (`hw.c` DSP state machine plus `ax_resume`): the AX → CARD-unlock → ROM → AX-resume sequence, with ucodes identified by Dolphin's HashEctor (AX `e2136399`, CARD `65d6cc6f`).
- **Scaling:** the retail display copy (BP 0x52, display flag) sets Aurora's logical framebuffer to the copy-source size (512×448). The window fit uses a 4:3 display aspect (`vi::display_aspect`), and `AuroraSetViewportPolicy(FIT)` is called once, because Aurora's window fit flag starts off.
- **Green movie frames (two causes):**
  1. Aurora's persistent pipeline cache is keyed by GX state, not by shader source. `gpu.c` now wipes it whenever the executable changes.
  2. The real culprit: `retail_fifo()` was still false while Aurora recompiled cached pipelines during `aurora_initialize`, so those shaders lacked texel→normalized texcoords. Guest RAM is now registered before initialize.
- **GPU thread** (`gpu.c`): the CPU emulation runs on a worker thread; the main thread owns the window, SDL events and Aurora. FIFO bytes go through a double-buffered queue, draw-done (PE finish) fires only when the GPU thread has drawn that frame, and at most one frame is in flight. `SYNC_GPU=1` restores the single-thread path.
- **Speed in heavy gameplay:** 57% → ~88-97% (later scenes 103-115%; menus and movies 150-185%). The gains came from:
  - the GPU thread;
  - `-O3 -mcpu=apple-m1`;
  - PGO (`ROTK_PGO`, profile in `recomp/pgo/`);
  - native no-op stand-ins for DCStoreRange/DCFlushRange/DCInvalidateRange/ICInvalidateRange;
  - guest-memory access through packed one-member structs, so TBAA proves guest memory can't alias `CPUState`.

  The rest is guest code (86% of the CPU thread), dominated by the paired-single skinning cluster at `0x800A9C74`-`0x800AAC80` and the 4×4 matrix-array transform `fn_800C4908`.
- **Mac integration:**
  - **Launch:** `rotk <disc>` boots straight from the ISO (boot.bin, bi2, FST and DOL are read from the disc), and `tools/make_app.py` builds `dist/Return of the King.app` (bundled dylibs, icon from the disc banner, ad-hoc signed). Launched with no arguments, it asks for the disc image once through a macOS file chooser.
  - **Window and display:** proper window title, 1280×960 default size, anisotropic filtering, optional MSAA.
  - **Fullscreen:** Control-Command-F or Option-Return, using SDL's non-Spaces fullscreen. The animated Spaces transition never completed for this window.
- **Verified end to end** (`docs/screens/gameplay_combat.png`): with no input, the game boots through copyright, THX and EA GAMES into the intro, then into Helm's Deep with sound and music. Scripted attack presses score 0 → 90 → 180 and advance the tutorial ("A Speed" → "Y Fierce"). The game formats a blank card itself.

## Routes to native

1. **Static recompilation + runtime** (fastest path to "it boots"). Translate all PowerPC code mechanically to C, then supply a host runtime for the SDK and hardware. Recompiling the original code means the `.scg` / `.vid` / `M4Decoder` handling comes along unchanged, so only the hardware layer is new. Existing pieces (as of Oct 2026, self-reported, unverified here):
   - **DolRecomp**: GameCube/Wii DOL → C lifter, CPU only (no runtime). C output should compile for arm64.
   - **Aurora** (encounter): GX/VI/PAD compatibility layer on WebGPU. Used by the Metroid Prime PC port and runs on macOS via Metal. This is the candidate GX → Metal layer.
   - **GekkoAOT**: complete pipeline (DolRecomp + LLVM, AuroraGX, native OS/threads, DSP/AX HLE, disc VFS). Pre-alpha, **Windows/Linux x86-64 only** (arm64 is on its roadmap), GPL-3.0, no fully playable titles yet. Its runtime design is the best reference, and porting it to macOS arm64 may beat building our own.
   - Proven per-game recomps exist (Wind Waker, Luigi's Mansion, SoulCalibur II "RingOut"), all Windows/x86-64.
   Remaining work: OS/thread/alarm shim, DVD → files, ARAM/AX/DSP → CoreAudio, CARD → save files, plus per-game fixes.
2. **Reimplementation from decomp** (cleanest result, longest). Name and type the game code, then rewrite it into portable C against new `LL*` backends. This route also needs the data formats fully reverse engineered. Twilight Princess, Melee, Pikmin and Metroid Prime have PC ports done this way.

Both are months of work. The `LL*` seam and the small number of direct hardware calls make this game a better candidate than most. Paired singles, threading and GX are the hard parts.

## Next steps
1. ~~**Both routes:** map GX usage~~ **Done**: see "GX usage vs Aurora". Aurora covers it apart from a short list.
2. ~~**Route 1:** try DolRecomp~~ **Done**: compiles and boots to `main` natively (see "DolRecomp on arm64"). **Next: interrupt delivery and threads**, then DVD via Aurora/nod, then GX FIFO → Aurora.
3. **Both:** name the engine core: allocator family, `URes` / `UFstPart` resource loader, `LL*` interfaces, the main state table entries.
4. **Route 2 only:** a `.scg` dumper that follows `SHOC` → `SHDR` / `SDAT`, and identify the untabled tail of the level files.
5. Run `npx rea-agents setup` so the REA MCP tools (Ghidra provider) are available in future sessions.

## Reproducing the toolchain (gotchas)
- **Ghidra decompiler for arm64** (the release ships none for `mac_arm_64`). In `Ghidra/Features/Decompiler/src/decompile/cpp`: edit `Makefile` Darwin block to `-arch arm64` and `-mmacosx-version-min=11.0`, then `mkdir -p ghi_opt && touch ghi_opt/depend` (skips the bfd dependency scan), then `make ghidra_opt` from a clean object dir, then copy `ghidra_opt` to `Ghidra/Features/Decompiler/os/mac_arm_64/decompile`. The target is `ghidra_opt`, not `decomp_opt`.
- **GameCube loader**: unzip into `~/Library/ghidra/ghidra_12.1.4_PUBLIC/Extensions/`, set `version=12.1.4` in `extension.properties`, and compile the spec with `support/sleigh -a <ext>/data/languages`.
- **`extracted/sys/main.map` is intentionally empty.** Without it the loader opens a "Load Symbols?" dialog and headless import fails with `HeadlessException`.
