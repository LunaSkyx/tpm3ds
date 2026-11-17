# TPM 3DS — Phase 1: reconnaissance and plan

## 1. Reconnaissance (source: THEPHANTOMMENACE.iso, US CD, 1999-04-22)

`./game_files/` = the ISO's `GAMEDATA/`, `INSTALL/` and `TPM.EXE`, extracted read-only.
The `DIRECTX/` redistributables were not extracted.

| File | Role |
|---|---|
| `TPM.EXE` (823,296 B) | MFC launcher (dialogs, registry, `WinExec`). **Not ported**; its options become a 3DS menu. |
| `GAMEDATA/BIN/WMAIN.EXE` (829,952 B, md5 `7c5af842…`) | **The game.** Image 0x400000, entry 0x49CB80, `.text` 0xA6870 (≈666 KB), `.data` 0x416CF0 (≈4.1 MB, mostly BSS). Big Ape "obi" engine (`c:\proj\win95\obi\{bp,game,std,swift,util}`), **not Sith**: OpenJKDF2 is only a loose reference. |
| `BIN/IMUSE.DLL` | LucasArts iMUSE (music state machine). x86 too, so it must be recompiled/HLE'd as well. |
| `BIN/MSS32.DLL` (+`*.M3D`) | Miles Sound System 5; 40 `AIL_*` imports. Write an HLE (not recompiled). |
| `BIN/BINKW32.DLL` | Bink video; 12 `Bink*` imports. Write an HLE (decoder needed). |

**WMAIN.EXE imports, grouped by DLL**
- **DDRAW** (2): `DirectDrawCreate`, `DirectDrawEnumerateA`. D3D is reached by `QueryInterface`. The IIDs for DD/DD2/DD4, Surface 1–4, D3D/D3D2/D3D3, Texture/Texture2 and the HAL/RGB/MMX/Ramp devices are all linked in (dxguid.lib), so the interfaces it actually uses must come from Ghidra xrefs. The error-string table includes the execute-buffer `D3DERR_*` codes.
- **DSOUND** (1): ordinal #1 = `DirectSoundCreate` (probably used by Miles/Bink; DS3DBuffer/Listener IIDs are present).
- **DINPUT** (1): `DirectInputCreateA` (keyboard + mouse GUIDs, DIDevice2A).
- **WINMM** (9): `timeGetTime`, `joyGetPosEx`/`joyGetNumDevs`/`joyGetDevCapsA`, `aux*` volume, `mciSendCommandA` (CD audio?).
- **KERNEL32** (80): CRT (Heap*, Virtual*, file, env, locale), `QueryPerformance*`, console (`AllocConsole`, `WriteConsoleA`: debug console), `Get/WritePrivateProfileStringA` (`.\obi.ini`), `GlobalMemoryStatus`, `GetDiskFreeSpaceA`.
- **USER32** (29): window/message pump, cursor, `GetKeyboardState`, `GetKeyNameTextA`, `MessageBoxA`.
- **GDI32** (15): DIB section, palettes (`AnimatePalette`, `SetDIBColorTable`), `BitBlt`, text extent.
- **ADVAPI32** (4): registry read. **ole32** (3): `CoInitialize`/`CoCreateInstance` (DirectInput/DSound via COM?).
- **mss32** (40), **binkw32** (12), **iMUSE.DLL** (10): see above.
- Built-in logs: `debug.log` ("Debug Output to File"), `resource.log`. Enable these in the PC original for reference traces.

**Data formats (sizes from the CD)**

| Path | Count / size | Format |
|---|---|---|
| `GOBS/BIG.Z` | 1 file, 80,977,645 B | InstallShield 3 `.Z` container (magic `13 5D 65 8C`) → **`big.lab` 120,859,357 B**, PKWARE DCL implode (binary, 4 KB dict). The exe opens `gobs\big.lab`, so the installer decompressed it. |
| `GOBS/LOCALIZE.LAB` | 13 entries, 705,209 B | `LABN` v0x10000 (LucasArts LAB): TEXT 3, BMPS 2, FONT 8 |
| `GOBS/VOICE.LAB` | 2,624 entries, 114,199,347 B | `LABN`: `BSND` `.WV` voice clips |
| `LEVEL/*.B3D` | 11 files, 22.3 MB | `"B3D FILE"` level geometry (obi engine) |
| `MOVIE/*.BIK` / `*.BIL` | 11 + 11 files, 108 MB + 48 MB | Bink 'f' (`BIKf`), 640×272. `.BIL` is a lower-bitrate variant. |
| `BIN/SS0-3.BIK` | 4 files, 1.9 MB | Bink splash screens |
| `MUSIC/*.IMC` | 160 files, ≈246 MB | iMUSE `MCMP` compressed audio |
| `JCG/*.JCG` | 9 × 230 B | joystick configs |

In-exe names also reference `*.baf` (sprites/particles), `dflt.mat`, `.\obi.ini`, `%sZanzi%02d.sav`.

**Blockers and risks**
1. *(Resolved in milestone 1b; see §3.)* **No `.gob`.** The "main archive" is `big.lab`, which only exists in compressed form on the CD. Fix: a one-time DCL explode `BIG.Z → /3ds/tpm/data/gobs/big.lab`, run either on PC (tool) or on first boot with progress. It writes a new file and never touches the original.
2. **Three extra x86 binaries in the critical path** (iMUSE.DLL, MSS32, BINKW32). iMUSE has only 10 exports, so recompile it with the same lifter. HLE Miles and Bink at their export API.
3. **Bink 1 decoding on ARM11.** A 640×272 video needs a fast decoder (FFmpeg's `bink.c` is LGPL; decide on licensing first) plus a downscale to 400×240. Run it on core 2 on the New 3DS.
4. **Which D3D API is used is still unknown** (DX3 execute buffers vs. DrawPrimitive). This decides the size of the D3D HLE. Ghidra MCP currently has `dca.exe` (Aladdin) open: **please load `WMAIN.EXE` into Ghidra.**
5. Memory: the full `.data`+BSS (4.1 MB) plus heap fits; the original's peak heap still has to be measured on PC.

**Related projects (checked 2026-11-12; nothing duplicates this port)**
- [OpenPhantom](https://github.com/OpenPhantom/OpenPhantom) (MIT): in-memory fix DLLs for the
  retail exe, targeting **the same WMAIN.EXE (md5 `7c5af842…`; EN and DE identical)**:
  widescreen, FPS, D3D9 layer, `imuse_fix`. Its new engine and editor are "not started", and it
  has no decomp and no format parsers. Useful as a list of known bugs and fixes and their
  addresses in our exe.
- Chip-Biscuit 64-bit installer: deprecated, points to OpenPhantom; empty repo.
- [esp-cpp/tpm-decomp](https://github.com/esp-cpp/tpm-decomp) (MIT): the **PlayStation**
  version (SLUS_008.84), not PC. Matching decomp is in progress, and a whole-exe static recomp
  already runs at about 27 fps on an ESP32-P4 using psyz (a PSY-Q reimplementation).
  A different binary and API, so no code is shared with this port.

## 2. Plan

### File tree
```
Makefile               3DS build: `make` -> tpm3ds.3dsx (+ `make cia` later)
Makefile.host          host build + fast tests: `make -f Makefile.host test`
source/main.c          3DS entry (backend glue only)
source/core/           platform-free: arcprobe, lab, dcl, b3d..., guest arena, HLE (win32/, dx/, mss/, bink/)
source/gen/            GENERATED lifted code (never hand-edited)
source/pal/pal.h       platform layer API (files, time, log, threads; later video/audio/input)
source/pal/common/     stdio file backend (shared)
source/pal/3ds/        3DS backend (citro3d/citro2d/ndsp/hid)
source/pal/host/       PC/headless backend (null video/audio; SDL later for the PC test build)
tools/lifter/          x86 -> C static recompiler (Python), reads WMAIN.EXE/IMUSE.DLL
tools/unpack_bigz/     BIG.Z -> big.lab (PC) — same dcl.c as core
tests/                 unit tests + scenario runner, scenarios/*.txt, refs/*.png
docs/                  this plan, formats, memory, HLE notes
```

### Boot sequence (3DS)
speedup → log open (rotate to `log_prev.txt`) → gfx/C3D/C2D → loading screen drawn from frame 1 → mount `/3ds/tpm/data` (cwd) → loader thread: validate the cached archive index (size + headers), else parse and write the cache → build the guest arena and map the WMAIN image → HLE init (DDraw/D3D → pal video, DSound/Miles → ndsp, DInput/joy → hid) → run the guest `WinMain` on the main thread. Exit: quit flag → join audio/video/loader threads → C2D/C3D/gfx fini. No VBlank wait once APT reports closing.

### Memory plan (New 3DS)
Measured in Azahar (3dsx): app region **96 MB**. Linear heap is fixed at **32 MB**, leaving a **91.75 MB** newlib heap. The `.cia` gets 124 MB via the RSF's extended memory setting.
- Linear 32 MB: framebuffers + C3D cmdbuf (≈2 MB), texture budget 16–20 MB with LRU eviction, ndsp buffers 1 MB, video frames 4 MB. Allocated with a first-fit allocator that logs every failure.
- Heap: one guest arena of **48 MB** allocated at boot (image + BSS ≈ 5 MB, guest heap ≈ 43 MB, to be tuned from the PC peak). Host-side newlib use ≤ 16 MB. Blocks ≥ 64 KB go to a separate large-block region via `--wrap=malloc/calloc/realloc/free`, and each failure is logged with size and caller (`__builtin_return_address`).
- Note: `osGetMemRegionFree` returns garbage after heap allocation (log shows 4,165,632 KB). Log `linearSpaceFree` and arena stats instead.

### Who writes what
**I write:** pal API and all backends, main/boot/shutdown, memory allocators, guest arena + read/write helpers, the lifter (core design, x86 decode via a table-driven generator, CFG, x87 as double), the Win32 HLE dispatcher and calling conventions, the D3D→citro3d translation core, the scenario runner, all reviews and integration.

**Offload to DeepSeek** (I supply exact headers, constraints and acceptance tests):
1. `dcl.c` PKWARE DCL explode (streaming, 4 KB window). Acceptance: `BIG.Z` → 120,859,357 B whose header is `LABN`.
2. `lab.c` full LAB index + on-SD cache file with validation.
3. `b3d.c` B3D level loader (after the format has been derived from WMAIN's loader in Ghidra).
4. COM vtable wrapper bulk (IDirectDraw*/Surface*/D3D*/DInput/DSound thunks) generated from a table I define.
5. iMUSE `MCMP` decoder; Bink 1 video/audio decoder port (once licensing is settled).

## 3. Status

### Milestone 1 (boot, log, data dir, archive headers, clean exit): done
`make` (devkitPro shell) builds `tpm3ds.3dsx` with no warnings. Exit via START/scenario
works: threads are joined in ≤ 59 ms and shutdown takes ≤ 67 ms. Quitting via HOME is
**not yet tested** (Azahar can't press HOME automatically); verify on hardware.

### Milestone 1b (big.lab from BIG.Z): done
- `core/dcl.c`: DeepSeek task 1, derived from blast.c (zlib licence, marked as altered).
  Reviewed: interface uses one shared `ctx` (accepted); Huffman decoding is bit-serial,
  not table-driven (accepted for now, since BIG.Z uses raw literals).
- `core/bigz.c`: unpacks to `big.lab.tmp`, validates exact size, consumed bytes and the
  LAB directory, then renames. An abort or error removes the tmp file.
- Host: 120,859,357 B in 1.13 s (107 MB/s). `big.lab` = LABN, 1,337 entries
  (BAFS 303, BSND 885, SPRT 72, MENU 40, BMPS 30, TEXT 4, MATS 2, PALT 1, …).
- Azahar: first boot unpacks in 52.0 s (2.3 MB/s, output byte-identical to the host's).
  Quitting during the unpack aborts and joins in 59 ms with no leftover file. Later boots
  probe `big.lab` in 34 ms.
- Tests: `make -f Makefile.host test` → arcprobe 13/13, dcl 118/118, bigz 6/6.

### Direct3D API used by WMAIN (resolved)
Found by scanning `.text` for each IID's absolute address:
- `IDirectDraw4` (0x4924ba) and `IDirectDraw2` (0x4927a3, likely a fallback)
- `IDirect3D3` (0x487556) → `IDirect3DDevice3`, i.e. **DrawPrimitive, no execute buffers**
- `IDirect3DTexture2` (0x488d7c, 0x488ec7); **HAL device only** (0x48a1c9)
- DirectInput `SysKeyboard`/`SysMouse`; one `IDirectSound3DBuffer` (0x496bd8)

Never referenced: DD1, Surface 1–4, D3D1/2, Texture1, RGB/MMX/Ramp devices, DS3D listener.

### Milestone 2a (guest arena + PE loader): done
`core/gmem` gives bounds-checked, alignment-safe guest access, with a null guard
below 0x10000. `core/pe_load` maps images directly into the arena.
- WMAIN maps at 0x400000, size 0x4D0000, entry 0x49CB80, 206 imports.
- IMUSE.DLL is relocated 0x10000000 → 0x01000000 (2,079 fixups, 56 exports). All 10
  of WMAIN's iMUSE imports resolve into IMUSE's `.text`.
- `test_pe` 18/18.

### Open issue: intermittent hang in Azahar (3 of ~60 runs)
Both threads stop: no heartbeat, no further output. It happened once with no worker
running (main loop after the results printout at frame 11), so it is not a race with the
loader or the unpack. Every hang was the first launch after a different step (a build, or
a long run); 54 back-to-back identical runs were clean. Not reproduced since then. The
main loop now logs a heartbeat every 60 frames. On the next hang, record Azahar's CPU
time (busy = guest deadlock, idle = emulator froze) and check it on hardware. No
workaround until the cause is known.

### Milestone 3 (3DS: menu and level 1 on the GPU): done (2026-11-12)
The 3DS build runs the full `start_game` scenario (boot, LOGO/BIGAPE movies, main
menu, level 1 cutscene and gameplay) in Azahar with no faults; heap peak 21.7 of 28.7 MB.
- Rendering: `core/render/r3d_gpu.c` maps D3D draws to `pal_gpu_*` (citro3d backend
  `pal/3ds/pal_gpu_3ds.c`); consecutive draws with equal state are batched, each vertex is
  transformed once per draw. Texture row order is checked on device by
  `pal/3ds/gpu_selftest.c` (create `sdmc:/3ds/tpm/gputest.txt`).
- Audio: DirectSound + Miles on the mixer, ndsp output; iMUSE music plays.
- Files: existence probes use cached directory listings (dropped on any write).
- Profiling: `sdmc:/3ds/tpm/prof.txt` starts the sampler; scenario `at N prof_reset`.
  The sampler overweights HLE calls (syscalls let the sampler thread run in Azahar) and
  charges callers' work to the last-entered leaf; trust the r3d wall-clock timers more.

Speed (Azahar, ms per 300 frames): menu 14.9 s; level 1 71 s (~4 fps), of which GPU
path 4.8 s. The rest is lifted guest code. `-O2` for it gains 2% (not used). Next step
for speed is lifter output quality: keeping guest registers/x87 stack in C locals, cheaper
cpu_t spill/reload around calls, flag elimination. Untested on real hardware.

### Speed work 1 (2026-11-12): lifter output, 60 fps mode
Azahar, ms per 300 frames (level 1 = frames 600-900 of `start_game`):

| change | boot to scene 0 | menu | level 1 |
|---|---|---|---|
| before | 41.0 s | 14.9 s | 71.2 s |
| precise state sync (`tools/lifter/sync_state.py`) | 33.5 | 14.0 | 50.6 |
| static x87 slots (`tools/lifter/fpu_static.py`), one-compare bounds check, inline `fist` | 26.3 | 12.2 | 37.4 |
| game's 60 fps mode on (default) | 26.4 | 12.2 | 41.5 |

- Precise sync: a lifted function loads/spills only the CPU fields it uses; flags
  only where the interprocedural flag facts say they are live.
- Static x87: ST(i) resolves to constant slots; after every call the slot base is
  rebased on the TOP the callee left, so the result equals hardware even where
  the original leaks x87 slots (unprototyped float calls) - verified by A/B
  screenshots with `TPM_LIFT_DYNAMIC_X87=1`. 550/569 WMAIN FPU functions static.
- The original caps at 30 fps (limiter FUN_00475b75) and simulates in fixed
  1/32 s steps; a never-enabled flag (0x882294) gives 60 fps and 1/64 s steps.
  `core/game_options.c` sets it after checking the reading code bytes; default
  on (user's choice), off with `sdmc:/3ds/tpm/30fps.txt` / `TPM_HZ60=0`.
  It doubles simulation work, so it costs ~11% while the 3DS is below 30 fps.
- Host profiling of guest code: `TPM_PROF=1 TPM_NORASTER=1` with
  `tests/scenarios/level1_prof.txt`. On the PC, guest work is ~3 s per 300
  level frames; the rest is the frame limiter waiting.
- Remaining gap to 60 fps in level 1: ~8x (138 ms per frame now, 16.7 ms
  needed). Next candidates: register-level flag elimination, cheaper calls
  (CPU_FN_ENTER ring, per-call spills), guest stack accesses without bounds
  checks for ESP-relative addressing, Bink decode (menu), real-hardware timing.

### Speed work 2 (native overrides, frame path, -O2): 2026-11-12
Still aiming at 60 fps in level 1. Three kinds of change: hand-written guest functions,
frame-path work in the PAL/r3d layer, and -O2 for the lifted code. All numbers are
Azahar (software GPU), level 1 of `start_game`, game's 60 fps mode, steady window frames
900-2400 unless stated.

**Native overrides.** `tools/lifter/lift.py` gained an `OVERRIDES` set of 13 WMAIN
functions. They are still analysed like every other function, but the lifted body is
emitted as `<name>_ref` under `TPM_OVERRIDE_REF` (used by the differential test), and
all call sites plus the dispatch table call the hand-written function in
`source/core/native_overrides.c`:
- 004074d4 packed 0xAABBGGRR from 3 floats; 0040e96b vertex permutation gather;
  004064b0 per-sprite submit (project/clip, vertex + palette buffers); 004075c6 material
  floats -> out[0..3] + record depth terms; 0041af30 sprite/light descriptor.
- 0047ceb9 4x3 inverse (transpose 3x3 + negated translation); 0047dad6 and 0047dee9
  4x3 affine multiply (returning and in place); 0047e263 3x3 row * vector; 0047e40a
  transform 3-vector in place; 0047e34d transform point; 00486245 vector normalise;
  00487f90 depth comparator.

**Frame path (r3d / PAL 3DS).**
- per-draw raster timing gated on `g_r3d_prof` (two `pal_time_us` queries per draw are
  off by default);
- HLE watchdog + preemption checks every 32nd HLE call (`HLE_TICK_INTERVAL`); Sleep,
  PeekMessage and Flip keep their unconditional checks;
- `pal_time_us` is integer tick math (`ticks * 1000000 / SYSCLOCK_ARM11`), no float;
- GPU state cache: consecutive draws with equal state go into one batch;
- two persistent offscreen render targets (`PAL_GPU_MAX_TARGETS 2`), guest stores
  deferred (target -> staging only when read) and Flip presenting straight from staging;
- one vertex-cache flush per frame instead of per draw (was ~106 per frame);
- 2x2 box screen blit using packed half-word averages (`uhadd16`), rounding like the
  old nearest copy.

**-O2 lifted code.** The generated sources (`wmain_gen_%.o bink_gen_%.o imuse_gen_%.o
lifted_modules.o`) build at -O2 on 3DS; 37.35 -> 36.53 s per 300 frames.

| stage | ms / 300 frames | ms / frame |
|---|---|---|
| pre-optimization baseline | 41.5 s | 138.3 |
| native overrides + frame path (syscalls, state cache, flip) | 37.35 | 124.5 |
| + generated code at -O2 | 36.53 | 121.8 |
| + one vertex flush/frame, 2x2 blit (10 overrides) | 30.45 | 101.5 |
| final 13-override tree | 30.23 | 100.8 |

The final row is this build: `bench/final13_bench_log.txt`, 3150 frames, scene 900/1200/
1500/1800/2100/2400 at 132.9/163.4/193.5/223.7/253.9/284.1 s, `scenario ok`, gmem faults
0, stuck threads 0, exit reason 6, whole run 313.5 s. Window deltas 30.46, 30.17, 30.20,
30.16, 30.18 s (mean 30.23 s, 100.8 ms/frame; last four 30.18 s, 100.6 ms/frame).
Screenshot `bench/final13_bench_3100.bmp`. GPU log: 1.00 vertex flush and 1 submit per
frame, 77 texture uploads total (all before scene 600, i.e. 0 in the steady level).

**Subsection split** (sampler, before -> after, share of frame time): draw+flip
36.7% -> 12.0%, GSPGPU_FlushDataCache 23.3% -> 1.2%, staging->screen blit 11.8% -> 4.1%.
Draw load in the window: ~1200 draws, ~149 batches and ~1960 triangles per frame (an
earlier window quoted ~1013 draws / ~106 batches).

**Host-level validation.** `tests/test_override.c` runs 61,000 differential cases
against the lifted reference bodies (5,000 per function, 1,000 for 00486245): 0 failures.
The whole host test suite passes. Host runs with the overrides give byte-identical
NORASTER screenshots to the lifted-only build, and the full override set uses 23.7% less
guest CPU time on the host.

**Honest remaining gap.** Level 1 is still ~101 ms/frame (~10 fps in Azahar); 60 fps
needs 16.7 ms. The renderer is now ~12% of the frame. The other ~80% is lifted guest
code spread over many functions, not a few hot spots. Top sampler entries (10-override
profile build, `bench/flush_prof_log.txt`): 00487260 9.2%, 0047e40a 7.3%, 0041ae50 6.8%,
0047e263 5.3%, 0049d575 2.9%, ReadFile 2.9%, 0049c75d 2.9%, 0047ceb9 2.6%, 004199b0 2.1%,
0040efd0 2.0%, 0049c80d 1.4%, AIL_lock 1.4%, plus the audio/file HLE paths (ReadFile,
iMUSE, AIL). Matrix helpers already native (0047e40a, 0047e263, 0047ceb9) still show up
because they are called so often. The sampler's leaf bias applies: callers are charged
to the last-entered leaf, so per-function shares are indicative, not exact. 60 fps is
not reached, and real-hardware timing is still unknown. Graphical glitches were not
investigated in this pass (performance first); the deterministic NORASTER A/B shows the
overrides did not change the rendered logic, but the existing visual glitches remain to
be diagnosed after performance work.

**Next steps.** Native rewrites of the remaining hot per-frame functions (004199b0,
0041b070, 0041ae50, 00487260); inlining the integer-only overrides into their callers;
GPU-side presentation and vertex transform; running the guest audio/streaming threads on
the second core; measuring on real hardware.
### Graphics fix 1 (2026-11-12): DX5 texture render states
WMAIN never calls SetTextureStageState; it sets stage 0 through the DX5 render states
that IDirect3DDevice3 still translates: TEXTUREADDRESS (3: 1 wrap / 3 clamp),
TEXTUREMAG/MIN (17/18 = linear) and TEXTUREMAPBLEND (21: 2 MODULATE / 4 MODULATEALPHA).
They were stored but ignored. Effects: blob shadows (64x64 ARGB4444, uv up to +-1.5)
tiled 3x3 ("9 shadows"); diffuse alpha of shadows/particles lost (MODULATEALPHA);
textures point-sampled. Now `r3d_set_render_state` maps them onto tss[0] (blend resolved
per draw: MODULATE takes texture alpha only if the texture has alpha). Untextured GPU
draws now output the diffuse colour (was diffuse squared).
- Reference: original PC screenshot of the level 1 conference room (TC-14 dialogue)
  matches host and 3DS output at frame 1240 (scene 781).
- The green haze later in the room is the game's own gas fog: table fog, colour aaffaa,
  start 1 / end 16 on eye depth (normal: 0 / start 16 / end 56). Not a bug.
- `TPM_R3D_DIAG=1` (host) logs each distinct D3D state combination, every SetRenderState
  value, fog parameters and fogged vertex depth ranges. ZBIAS (47) is still ignored.
- Screen blit fix: the "2x2 box blit using uhadd16" averaged whole 16-bit RGB565 words,
  so carries crossed channels (blue->green->red): heavy colour noise on the real top
  screen in every scene. Scenario screenshots never showed it (they are taken before
  the blit). Replaced by the masked per-channel average (0xF7DE); a 2M-sample test has
  every channel within 2 LSB of the exact mean. Check the blit output itself (simulate it
  on a captured frame) whenever the screen path changes.

### Speed work 3 (2026-11-13): masked memory, iMUSE, timers. Plan: docs/PERF_PLAN.md
Level 1 in Azahar (100 % clock): ~101 → **~73 ms/frame** (13.6 fps). Native iMUSE
decoder (`core/native_imuse.c`, `tests/test_imuse.c`): ~1 ms win. Masked guest memory
(`TPM_GMEM_MASKED`, 3DS only, 64 MB arena): ~27 ms win, 0 faults. New tools:
`tools/azbench.ps1`, `FN_TIMERS` (`-DTPM_FN_TIMERS`), `tools/icount/`.
The work order for the next steps (native world_cull first, fully specified) is
**docs/PERF_PLAN.md**.

### Speed work 4 (2026-11-13): native world_cull, phase A
- `source/core/native_world.c`: transcription of 0x4199b0 (listing `docs/asm/004199b0_world_cull.txt`).
  Callees still run through the guest stack (phase B will call C cores directly).
- Registered in `OVERRIDES` (`tools/lifter/lift.py`), file in `GUESTC` (Makefile.host).
- Shadow check on host: `TPM_VERIFY_WC=1 TPM_NOLIMIT=1 build-host/tpm_host.exe game_files/GAMEDATA 0 0 tests/scenarios/level1_bench.txt`
  -> 2151 calls, 0 differed (about 10 min). Branches never hit in level 1: lighting, depth bias, dark, cache full, water, node/texture range kills.
- Azahar level 1 (window 1200-1500): 70.9 -> 67.6 ms/frame. CPU_FN_TRACE removed from 3DS CFLAGS (70.9 vs 73).
- Details and open items: docs/PERF_PLAN.md section 0.
- Phase B (2026-11-13, later): direct C cores in native_world.c for 0x47e34d (xform), 0x4074d4 (colour pack) and 0x41af30 (projection). Still guest-stack calls: 0x4075c6 (material colour), lifted sin 0x494272 (darkening), 0x427c9a, 0x41ae50, 0x41a350, 0x428615, 0x409170.
  Azahar level 1 (1200-1500): 67.6 -> 66.2 (cores 47e34d/4074d4) -> 65.5/66.6 (af30). world_cull timer: 12.5 -> 8.8 ms.
  Shadow check: full level1 2151 calls, 1 diff, which was a sub-switch call (0x409170 is time-dependent, so the native and reference runs can differ). The other 2150 calls match.
- world_emit (0x406830 -> 0x41b070) profile, timer build 2026-11-13: emit_core 18.2 ms/frame of which 0x487260 (DrawPrimitive wrapper, incl. SetRenderState x2) 15.8 ms at 1199 calls/frame. Emit logic itself is small. The cost is the per-draw renderer path (r3d_draw -> r3d_gpu_draw -> draw_state/texture/fetch on the 3DS CPU, about 13 us per draw). Tried: caching world*view*proj in fetch_ctx_init: no gain (world changes per object draw), reverted. Next: per-draw profile inside r3d_gpu_draw (prof_sub sections) or fewer draws per frame (batching across objects) - needs a decision.
- Native CRT sin (guest 0x494272, degree scale x*pi/180) was tried and reverted: its finite-argument path matched the lifted reference in test_override, but a full guest run raised FP exception c0000090 at 0x49da81 (cause not found). Hot-spot data: the CRT math chain (0x49c75d cos, 0x49c80d sin, 0x49d5e8, 0x49d575) is ~13% of samples in steady-state level 1 (function-level profile, CPU_FN_TRACE build). Next: find the exception cause before retrying.
- Perf plan v2 (docs/PERF_PLAN.md §7) and A1 measurement (2026-11-13): 25,000–26,000 lifted function entries per frame in level 1. Micro benchmark in Azahar: a typical lifted call costs 60 cycles (86 with x87 state), a lean-ABI call 40. A lean call ABI (A2) is therefore worth about 2–4.5 ms/frame, not 5–10. New: `-DTPM_CALL_COUNT` (call counter in the scene log) and `core/callbench.c` (runs when `sdmc:/3ds/tpm/callbench.txt` exists). Normal 3DS build restored, host suite green.

### Speed work 5 (2026-11-13): A3 CRT sin, cause of c0000090 found, native reverted
- Cause of the FP exception with the native sin: `0x49da81` is the return address of `call [0x8c1514]` in
  `FUN_0049d820`, the MSVC `_fpieee_flt` handler. It calls `RaiseException(0xc0000090)` (invalid operation)
  when the CRT sin gets a non-finite argument and the guest control word unmasks invalid (FCW bit 0x01 clear).
  `tests/probe_sin.c` on the host with the lifted reference: `+inf`, FCW 0x037e -> raises at eip 0x49da81;
  FCW 0x037f -> NaN, no raise. Finite arguments never raise (fsw stays 0). The native version has to delegate
  non-finite input to the lifted body (`_ref` must then exist in the 3DS build too).
- Native sin prototype (finite: `sin((double)x * (double)k)`, C2 cleared, result pushed; non-finite: lifted):
  `tests/test_sin` style differential run, 200,032 cases: ESP/EBP/EBX/ESI/EDI, TOP, FSW, FCW and all 8 st[]
  slots identical. EAX/ECX/EDX/eflags differ (the CRT leaves its own values). These are caller-saved under cdecl
  and dead at the call sites of 0x494272 by the ABI, so this is accepted in principle.
- Azahar level 1 window 1200-1500 with the native sin: **65.2 / 66.2 ms** (before: 65.1 / 66.0). No gain, so
  per the rule it was **reverted** (lift.py, Makefile.host, native file and test removed; gen is byte-identical to
  the pre-A3 state; host suite green). The sin chain is not what limits level 1 at this point. Next candidates
  are in docs/PERF_PLAN.md section 8.

### Video flicker investigation (2026-11-13, not fixed yet)
- Symptom (user): flicker up/down in the intro videos, most visible in the scrolling "Star Wars" text. Small menu previews look fine.
- Ruled out: the 3DS blit nearest-neighbour path for odd sizes (`pal_3ds.c` video_blit). Game frames are 640x480,
  so the blit takes the exact 2x2 averaging path (320x240). No nearest-neighbour sampling involved.
- Host measurements (`build-host/bigape_*.bmp`, frames 600-647, menu with START preview): content changes in one
  100-row box on about every second to third frame; best vertical shift between consecutive frames is always 0.
  No position jitter reproduced on host. Azahar crawl frames (300-307) also show no clear vertical shift.
- The 180-degree picture seen during this investigation was a bug in my own PNG converter (row order), not the game.
- Open: which clip/frame is the scrolling "Star Wars" text, and whether the jitter appears only on the device or in
  Azahar. Next step needs that clip in Azahar (frame-by-frame) or a device capture.
- SCENE1 (the film with the scrolling text) opens at about 22.8 s / frame ~1420 in the host run (`build-host/scene1_find_log.txt`).
  Frames 1380-1480 (`build-host/s1_*.bmp`): only the 100-row preview box changes, the film never fills the screen on host,
  and the best vertical shift between consecutive frames is 0 in every pair. No up/down jitter reproduced on host.
  So the flicker, if real, exists only on the device/Azahar full-screen path, or it is in the Bink decode itself.
  Next: a device capture of the crawl, or a full-screen movie run in Azahar (frame-by-frame), to settle it.

## First hardware test (2026-11-13 evening, New 3DS, CIA)
- CIA: `sh tools/cia/build_cia.sh` (tools/cia/tpm3ds.rsf: 804 MHz, L2, 124 MB mode, no banner).
  Install with FBI; data stays in `SD:/3ds/tpm/data`. FTP upload via ftpd works for CIA and GAMEDATA.
- Crash on start (Luma dump /luma/dumps/arm11): `C3D_TexBind(unit, NULL)` in `pal_gpu_draw`
  dereferences the NULL on hardware (citro3d 1.7.1). Azahar silently returned 0 for the read
  (23008 "unmapped Read32 @ 0xC" log lines). Fixed: bind the fog texture instead of NULL.
  Lesson: check Azahar's own log for "unmapped" after every change.
- Voices were silent: `FindFirstFileA(.\cache\*.*)` looked in the data dir, but the decoded voice
  clips live in the save overlay (`save/cache`). The lookup now falls back to the save dir
  (`TPM_OLD_CACHE=1` disables it for golden comparison against the pre-fix reference).
- Widescreen (default; `SD:/3ds/tpm/4to3.txt` = 640x480): the HLE reports 800x480
  (`g_screen_w/h`, core/video). Needed: mode list 640x480 first (the game treats index 0 as "not found"),
  ini keys screen_width/height/allow_all_modes/max_menu_*, three immediate 640x480 pushes patched in the
  lifter (`PUSH_SCREEN`: 0x43edef/f4, 0x43ef0f/14, 0x46c3ac/b1), large heap blocks fall back to the
  VirtualAlloc region. The game derives its camera focal length from the screen width itself
  (+0x3c 553 -> 692), so the picture shows more on the sides. 3DS blit: 800x480 -> 400x240 exact 2x2.
- Hardware frame time before widescreen (SD log, 300-scene spans): scenes 600-900 47.9 ms, 900-1200 45.3 ms (about 21 fps);
  1200-1500 was 113 ms (about 9 fps): that span is the first droid fight (music SqDroidFightLoop from 130 s, ends 161 s),
  i.e. the battle drops the user reported (5-10 fps). Profile the battle, not the conference room.

## Phase 14: 30 fps target in Azahar (2026-11-14)
Start: 44.9 ms/frame (60 fps logic default, 800x480 wide) in the azbench window 1200-1500. Now 34.9 ms (steady
state about 33 ms; the rest is voice-line hitches, see below). Kept changes, each checked against the host golden
run (`build-host/golden_ref1500.txt`, fb column identical for all 1500 frames) and the host suite:
- `main.c`: the original 30 fps logic is the default again (`60fps.txt` opts into the hidden 60 fps mode,
  `4to3.txt` the 640x480 narrow image). ~9 ms less per frame.
- Rotated GPU target (`pal_gpu_3ds.c`, `gpu_vsh.v.pica`): at 800x480 the render target is stored 480x800 and the
  vertex shader swaps x/y (`cx..cw` uniforms, dp4). The display transfer's 2x2 downscale then writes the
  240x400 top-screen framebuffer directly (`pal_gpu_present_direct`, no CPU pass). Staging/load/store un-rotate on the
  CPU (rare path). Checked by scenario screenshot in Azahar (`video_direct_begin/done` copy the framebuffer before the
  swap when a screenshot is due). NOT yet verified on real hardware (dimension semantics inferred from Azahar).
- `source/core/native_actor.c`: native actor vertex lighting (0x40fc10) and projection (0x40efd0), shadow-checked
  (`TPM_VERIFY_NA=1`, 0 differences over 1700 frames). Small gain (~0.6 ms).
- File layer (`hle_kernel32.c`, `pal_file_stdio.c`): 32 KB stdio buffer for read handles; directory listings of the
  save overlay are patched incrementally instead of dropped on every write; mkdir only once per directory;
  closed read-only handles are parked (6) and reused by the next open of the same path; reads >= 64 KB go
  straight to read() on the 3DS.
- Mixer: no per-sample division in ADPCM; `AIL_lock` services the mixer every 8th call. (Mixer is only ~1 % of the frame.)
- Diagnostics left in: `frames/300` histogram, `io: slow read/CreateFileA` (>15 ms), `flip:` timings.
Findings: window 1200-1500 has 3 hitches of ~135 ms per 300 frames (voice line start: open 16 ms x2 + 184 KB read 33 ms
in Azahar). A helper-thread prefetch did not help (the game reads right after the open) and was removed.
The lifted guest code is ~77 % of the frame; the profile's top function shares do not translate into frame time
(see PERF_PLAN A3). Remaining ~1-2 ms would need further natives (0x40f1e0, 0x402155) or a second core.

### Phase 14, later the same day
- Measured, not guessed: the sampler's per-function shares do not predict frame time. Skipping every r3d draw
  saves only 0.5 ms; skipping the vertex flush saves nothing. The fn-timer build (41.5 ms with timers) shows
  obj_update 17.7 (actor_emit 0x402155 8.0, actor_xform 0x40f1e0 4.1) and render 13.5 (cull 3.5, emit 4.3, skeleton 4.8).
- Frame-time histogram (window 1200-1500): 113 frames at 31.4 ms, 178 at 34.4 ms, ~1 hitch of 133 ms. The game's own
  limiter polls with Sleep(0) at ~0 us gaps, so it does not add overshoot; a catch-up limiter (carry slack to late
  frames) was tried and removed (frames are work-bound, lag +4..10 ms).
- Voice lines: decoded WAVs are mirrored in RAM (12 MB, `vs_*` in hle_kernel32.c); a helper thread (second core on a
  New 3DS) fills the store from existing cache files; replays/opens never touch the SD card. Hitches 3 -> 1 per 300 frames.
- Idea evaluated and dropped: promoting [ebp-x] locals to C variables in the lifter. Only 6.4 % of executed x86
  instructions are provably promotable (15 % are ebp-direct accesses at all), ~1 ms at best.
- Azahar window: 34.5 ms (29.0 fps). Needs ~1.5 ms more for a flat 30.

### Phase 14, evening: native 0x402155 and steady-state measurement
- 0x00402155 (deferred actor polygon flush) is now native (`native_actor.c`, `na_emit`); golden fb hashes identical over 1500 frames, host tests green.
- Voice store trims captured buffers to their real size (budget counted slack before: store never hit); voice-open hitches (~130 ms) gone in the steady scene.
- Steady conference scene: limiter-paced, 111 frames at 31.4 ms + 180 at 34.4 ms per 300 = 33.3 ms mean (about 30 fps, measured 29.9 fps over frames 1314..1466), one 41 ms frame every ~38 frames (catch-up keeps game speed). Present path costs ~0.45 ms. Heavy scenes (cutscene/movie) remain 46-60 ms.
- Not verified on hardware: direct-to-screen present, voice helper thread; CIA upload to the 3DS failed (3DS offline).
- Timer caveat: pal_time_us costs 2-3 us in Azahar, so FN_TIMERS numbers for call-heavy functions (DrawPrimitive wrappers, 820 calls/frame) are inflated; real r3d_draw is ~5-6 us/draw (~3 ms/frame). -Os for lifted code: no change in Azahar. Gameplay stretches after frame ~1500 (more actors, ~880 draws/frame) still run 44-49 ms; the steady conference room runs ~30 fps. world/obj update (lifted x87 code) is the remaining cost.

### Night addendum (2026-11-14)
- Native 0x0040f1e0 (actor transform, `na_xform`): 0 fallbacks in the golden run, golden fb identical to the lifted body (env TPM_XF_OFF=1 / TPM_EM_OFF=1 / TPM_NO_VS=1 switch natives/voice store off on host for A/B). Note: the stored golden_ref1500 no longer matches any current build from frame 242 (older and newer builds agree with each other only per build); use A/B runs with the switches instead.
- Azahar bench with both natives: scene 900 46 ms, 1200 34 ms, 1500 44.5 ms (no clear gain vs before).
- Experiment: skipping DrawPrimitive entirely: 1500 window 44.5 -> 36.4 ms, so the draw path costs ~8 ms/frame; a 3 ms spin per frame costs ~+4.7 ms. Next target: pal vertex copy / FlushDataCache / C3D_DrawArrays batching (prof: 2.4% flush, 1.3% DrawArrays, 3.2% fetch+transform).
- CIA built; FTP to the 3DS (<3ds-ip>:5000) still times out (curl 28).
- 3DS A/B switch: sdmc:/3ds/tpm/nonative.txt makes natives (0x402155, 0x40f1e0) fall back to the lifted bodies (needs ALWAYS_REF for both in lift.py). Azahar result: natives ON/OFF identical (scene 1500: 44.5 vs 43.9 ms) -> the heavy-scene limit is NOT lifted-instruction count; GPU wait is ~4 us/frame (not GPU bound). Remaining suspects: per-draw fixed overhead (880 draws/frame at ~1.5 tris each: guest 0x487260 + HLE dispatch + r3d_gpu_draw state/memcmp/gmem_span, ~8 ms total measured by skipping DrawPrimitive). Ideas not yet done: cache draw_state template behind a state generation counter (note apply_legacy_blend mutates tss[0] per draw), move r3d/pal work to core 2.
- The 30 fps steady value (29.9) is the game's own ms-granular limiter; frame pacing is not the issue.
- Bench splits (Azahar, scene 1500, baseline 44.5 ms): skip whole DrawPrimitive 36.4; skip only r3d_gpu_draw body 36.9 (so guest 0x487260 + HLE dispatch ~0.5 ms); skip only pal_gpu_draw 42.9 (pal ~1.6 ms). => r3d_gpu_draw front end (draw_state, texture(), fetch, cull, assembly; ~880 draws/frame) ~6 ms is the single biggest target. Next: state-generation cache, fewer divisions in fetch(), write vertices straight to the pal vertex ring (avoid double copy), skip degenerate work for 1-triangle draws.
- Done: draw-state cache (r3d gen counter, `r3d_state_dirty`) and reciprocal multiplies in fetch(): scene 1500 44.5 -> 43.5 ms. Screenshot at frame 1500 renders correctly. Lifted code is already native-speed C, so the remaining ~36 ms (with draws removed) is the game's own work on emulated ARM; further wins need either core-2 offload or broad codegen gains.
- More negative results (Azahar, scene 1500): -O3 for lifted code 43.3 (no change); fvf-0x1c4-specialised fetch 43.5 (no change); limiter off (3DS `nolimit.txt`): room 30.9 ms, heavy 42.5 ms => frames are CPU-bound, only ~3 ms of limiter slack in the room. Host pcprof is useless for this (software rasteriser dominates). Remaining big levers: core-2 offload of the r3d front end (~6 ms), or lifter-level instruction-count cuts (prologue/epilogue sync already pruned by sync_state).
- Front-end bisect (scene 1500, base 43.8): stop before vertex fetch -> 38.7 (fetch+cull+assembly+pal ~5 ms, pal ~1.6); early all-culled rejection: no gain (few culled draws); host call histogram (cpu.h/cpu.c g_fn_hist, build-host/fnhist.txt): no single hot lifted function besides draw (0x487260 ~1100/frame) and sin/cos wrappers (~700/frame each).
- Room hitch found: every ~1.28 s (= 64 iMUSE timer ticks of 20 ms = 38.4 frames) one 512/2048-byte music ReadFile takes ~6 ms in Azahar (SD/FS latency; log with the slow-read threshold at 2.5 ms). Attempted async read-ahead (helper thread + 32 KB blocks, virtual file position): any extra pal thread created from the HLE (core 2 or core 1) makes the game stall at the logo movie in Azahar (reproduced with a thread that only waits on a semaphore) -> reverted. Open question: why a spare pal thread stalls the guest (gthread/pal_sem interplay?), which also affects the untested voice prefetch idea.
- Long bench (azbench -Frames 4200, no input after the menu): frames 1800-4200 run a flat 49.0 ms (20.4 fps, every 300-frame window 300/300 in the 40-60 ms bucket) -> the later cutscene/scripted stretch is the worst case, 15 ms over budget.
- The 49 ms stretch (frames ~1800+) is real gameplay (dining-room corridor with HUD, screenshot at frame 2500 is correct), not limiter or vsync: with nolimit.txt it is 48.5-49.0 ms. Sampling profile there: 82% guest/untagged, draw+flip only 12%. Top last-entered functions: 0x487260 (draw wrapper), 0x406300 (object -> depth bucket insert), cos wrapper 0x49c75d (callers' math), 0x405cf0 (visible-object collection), 0x41ae50. Native 0x406300 added (native_actor.c, ALWAYS_REF, golden A/B identical): 48.5 -> 48.3 ms, i.e. again almost nothing.
- Snowball finding: the game advances its simulation in fixed 1/32 s steps for the real time elapsed since the last frame (FUN_004756fc / limiter FUN_00475b75), so a slow frame runs >1 step, which makes the next frame slower. New: clamped guest clock (hle_kernel32.c g_clock_clamp, default on on 3DS, `noclamp.txt` disables): QPC/timeGetTime advance at most 33.3 ms between presented frames (excess at 1/8, gaps >80 ms and open .bik movies pass through). The game slows down instead of snowballing. Azahar: scene 1500 window 43.4 -> 38.4 ms; frames 1800+ 48.3 -> 47.6 ms; room unchanged 34.2.
- Azahar CPU microbenchmark (3DS `cpubench.txt`, main.c): volatile int mul-add 26 ns/iter, float 23, double 30, memory RMW 38, int->double+divide 143 ns/iter; identical with osSetSpeedupEnable(false) (`nospeed.txt`), so Azahar at cpu_clock_percentage=100 runs ~268 MHz-class regardless of the 804 MHz request (a real 2DS XL/New 3DS with speedup is ~3x faster, so Azahar numbers are pessimistic). FP divides/sqrt cost ~100+ ns (x87 double emulation): FP-heavy lifted code is the expensive part, not call counts (host call histogram for frames 1800-2400: ~20k lifted calls/frame, qsort comparator 0x41c9a0 1510/frame, 0x405cf0 581/frame, 0x488210/0x487260 ~1120/frame).
- CORRECTION: the earlier "extra pal thread stalls the game" diagnosis was wrong. The stall was my read-ahead calling ftell/fseek on the guest's stdio handle before every small read: fseek discards the stdio buffer, so every read became a 6 ms SD access. Fixed by tracking the file position virtually (hle_handle.mpos, ra_virtual) and syncing the real position only before a normal read / SetFilePointer. Background helper threads (core 2 on a New 3DS) work fine in Azahar. Read-ahead (hle_kernel32.c ra_*): after 3 back-to-back small sequential reads a helper keeps the next 32 KB blocks of that file; the 6 ms hitch every 1.28 s (iMUSE music) is gone from the room windows (40-60 ms frames per 300: 128 -> 12), total bench time 143 s -> ~124-144 s. Host tests + golden identical. Core-2 offload of the r3d front end is therefore technically possible (not started).
- Runtime (non-lifted) code now built -O3 (Makefile line 28): window 1500 38.4 -> 37.6 ms, window 900 52.9 -> 51.5. The 3DS (<3ds-ip>) does not answer ping/FTP at all right now, so no upload possible.
- Natives added: _CIcos/_CIsin (0x49c740/0x49c7f0), __ftol (0x49a44c), sincos (0x494238); host tests PASS, golden A/B identical (0/1500 diff). Azahar: scene 1800+ 47.6 -> 46.4 ms, others unchanged. Per-call-site native overhead dominates; remaining cost is spread across the lifted x87 code, not hot leaf functions.
- Limiter (FUN_00475b75) is a pure 30 fps cap (dt >= 1/30 s, polls Sleep(0), poll gap <0.3 ms) measured on the guest clock; sim runs fixed 1/32 s steps, so ~7% of frames do 2 steps (the 40-60 ms bucket). nolimit numbers are not comparable (fewer sim steps per frame). Room real work is ~35 ms/frame: needs ~2 ms cut to hold 30 fps in Azahar.
- Lifted code at -Os: no gain (37.9 vs 37.6 ms), reverted to -O2.
- BIG FINDING (user's idea): the game's own Options > Performance Options ("Computer Rating", slider Low/Medium/High/Superior + 12 toggles: Full Map, Mirrors, Polygon Trails, Shadows, Water Warp/Transparency, Particles, Extra Particles, Shot Lighting, Texture Scrolling, Caustics, Map Decals) works in the recomp and persists in save/obi.ini ([options] evaluated, perf_level, POPT0..POPT11). First run rated this machine "Superior" with everything on. At Low (perf_level=1, all POPT=0): Azahar steady level frames 34.3 ms (29.2 fps) everywhere, incl. the former 46.4 ms stretch. 3DS now defaults to Low when obi.ini has no values (hle_kernel32.c g_perf_default_low, set in main.c; `perfhigh.txt` on the SD disables). Raw per-step work ~34 ms, i.e. ~1 ms above the 33.3 ms cap; 2-step catch-up frames (~3%) are 40-60 ms. tools/azbench_low.ps1 + tools/low_scenario.txt drive the menu via keys (DOWN,DOWN,RETURN = Options; DOWN,RETURN = Performance; LEFT x12 = Low; ESC,ESC; UP,UP,RETURN = Start).
- Clock clamp threshold GC_FRAME_US 33333 -> 36000: with Low rating the clamp was costing ~0.7 ms/frame of pacing; Azahar now 33.5-33.8 ms (29.7-29.9 fps) flat across the level (was 34.3). Remaining gap to 33.33 = the ~3% two-step catch-up frames of the game's 1/32 s step vs 1/30 s cap (also present on the PC original).
- Per-toggle cost (Azahar, perf_level 1 base 33.5 ms): POPT6..11 on = 33.5-33.9 (free); POPT0..5 on = 36.6 (+3 ms); POPT0-2 on = 34.2 (+0.6). 3DS default now POPT0..5 off, POPT6..11 on.
- Refined default: only POPT3 (steam/particle effects, +3.5 ms) stays off; POPT0-2,4-11 on (chairs etc. return): Azahar 33.6-34.4 ms. Visible difference vs Superior at level frame 2400: steam jets/glow missing (h_2400 vs tD).
- Controls (pal_3ds.c k_map): D-pad/Circle Pad move+turn; C-Stick left/right = camera turn only; A Return only (A as Return+X broke starting the game); B jump (X key); X attack (Ctrl); Y use (Space); L run; R force push (Z); ZR roll (Alt); ZL next weapon; Select previous weapon; Start Escape.

## Lead session (2026-11-14 afternoon): measurement corrections, two natives, limiter masking

**1. The frame limiter masks everything at or below 33.3 ms.** With the game's 30 fps limiter on
(`FUN_00475b75`), the steady level sits at 33.6 / 34.0 / 34.4 ms per window whatever happens below
33.3 ms. Real work only shows with `nolimit.txt` on the SD: current build = 19.3 / (load 450) /
36.3 / 25.5 / 32.1 / 33.8 ms for windows 900/1200/1500/1800/2100/2400. **Every "no gain" native result
of earlier sessions may have been masked by this cap.** Measure natives with `nolimit.txt`, and keep
the normal build for the fps the player sees.

**2. The on-device sampler is not a ranking tool.** `prof.txt` + `CPU_FN_TRACE` charges each sample to
the last *entered* function, so callers' inline work and the trace overhead land on frequently called
leaves. Proof: `0049d575` is an empty function (`ret` only) yet showed 2.8%; `0041ae50` showed 6.9%
while the instruction counter puts it below 0.4%. Use `tools/icount` for exact executed x86
instruction counts and `FN_TIMERS` (1-call entries only) for inclusive time.

**3. Instruction count does not predict Azahar frame time.** icount (host, frames 1800-2100, no
limiter): 825,639 lifted instructions per frame. Top functions: `0041b070` 10.5%, `00405cf0` 9.7%,
`00404730` 9.3%, `00401eb8` 4.9%, `00483e60` 4.2%, `00408078` 4.1%, `00487260` 3.1%.
Two natives covering 14.6% of that (0041ae50 + 00401eb8, `source/core/native_misc.c`) measured
**0.1-0.2 ms total** (nolimit A/B: 36.3/25.5/32.1/33.8 vs 36.4/25.6/32.3/33.9). So the removed
instructions were nearly free: the frame is dominated by memory effects and/or work outside the
lifted instruction stream, not by ALU/flags/register shuffling. Native rewrites that keep the same
memory access pattern are NOT the lever; only algorithm-level restructuring (fewer touches) or
removing whole subsystems can be.

**4. Added (exact, verified):**
- `0041ae50` native animated-texture frame selector: 32-bit `div` instead of the 64-bit `div`
  emulation, ping-pong frame selection, texture-table walk.
- `00401eb8` native object-key list insert (bucket array at 0x5956f8, 0x4dd6c0 cache).
- `tests/test_override.c`: both covered, 5000 randomized cases each, 71,000 pass. New harness feature
  `zr_diff`/"compared regions": guest memory a test's function MAY modify (saved before the ref,
  restored before the native, compared afterwards) - `g_globals` only guards inputs that must not
  change.
- Golden A/B (`TPM_AX_OFF=1`), 1500 frames: 0 diff. 3DS build + `nonative.txt` fallback work.
- `tools/leadbench.ps1`: scenario/exe/nolimit/prof-selectable bench runner; `tests/scenarios/lead_prof.txt`.

### Lead session 2 (2026-11-14, afternoon): reliable inclusive timers - where the 34 ms goes

Timer build (`TPM_DEFS=-DTPM_FN_TIMERS`) with new entries for the obj_update subtree
(`bench/timers3_log.txt`), level frames 1800-2100. Timer build wall 43.8 ms, release build
33.6-34.4 ms. Entries with <= ~4 calls/frame are accurate; the 600-call DrawPrimitive entries
carry ~2.5 us of timer overhead per call and must not be read as wall time.

| block | ms (timer build) | comment |
|---|---|---|
| frame | 43.78 | |
| render | 14.87 | world_cull 3.75 (native), world_emit 4.85, skeleton 5.65 |
| obj_update | 19.36 | obj_17930 10.16 (40 calls) |
|  0x40fe70 | 8.99 | 4 calls; callees: 0x4100a0 5.02, 0x483e60 2.98, 0x407d26 0.90, 0x47840e 0.21 |
|  0x426ef7 | 0.93 | 36 calls |
| render_tail | 2.04 | |
| anim / ai / imuse | 2.22 / 1.92 / 0.60 | |

- 0x4100a0 (5.02 ms, and only 135 bytes of code) is a recursive walker over the object tree; its
  body only calls the *already native* actor transform 0x40f1e0, so those 5 ms are the native
  vertex pipeline doing real work. Same shape for world_cull (native, 3.75) and skeleton 5.65:
  the time sits in work that is already native or in memory traffic, not in lifted-instruction
  overhead. This is consistent with the native A/B result above.
- Conclusion for targeting: a rewrite only pays if it removes *memory traffic* (vertex slots,
  actor records, texture/vertex buffers) or whole passes - not instructions, flags or calls.

**Next lever (not started): remove the world_cull -> world_emit round trip.** world_cull (native)
writes a 64-byte slot per projected vertex at 0x5bf9f0 + 64*i; world_emit (still lifted) reads
them back to assemble triangles. ~5k polygons x ~3.5 vertices x 64 B written and read again per
frame (~2 MB) exists only to hand data between two stages that are both ours now. Native
0x406830 (world_emit) plus its core 0x41b070, then merge the stages so vertices stay in a local
array, keeping the emitted draw order and the 0x487260/0x488210 calls identical. This is the last
change with a several-ms ceiling; everything measured single-function is worth 0.1-0.3 ms.

### Lead session 2b: complete inclusive-time map (timer build, frames 1800-2100, bench/timers4_log.txt)

Timer build 44.3 ms/frame, release build 33.6-34.4 ms. All entries here have <= 62 calls/frame.

| what | ms | kind |
|---|---|---|
| render 0x43f57a | 14.88 | |
|   skeleton 0x404730 | 5.65 | lifted, 2243 lines, 237 x87 lines |
|   world_emit 0x406830 | 4.85 | lifted (core 0x41b070 4.84) |
|   world_cull 0x4199b0 | 3.74 | **native** |
| render_tail 0x484901 | 2.05 | lifted |
| obj_update 0x411028 | 19.87 | |
|   obj_17930 0x417930 | 10.46 | dispatcher, lifted |
|      0x40fe70 | 9.29 | lifted entry, 4 calls |
|         0x4100a0 | 5.02 | lifted walker -> **native** actor transform 0x40f1e0 |
|         0x483e60 | 3.28 | lifted, 2129 bytes |
|         0x407d26 | 0.89 | lifted |
|      0x426ef7 | 0.93 | lifted |
|   rest of obj_update (~8 ms) | | its own 1137-line body + **native** actor_emit/actor_xform |
| anim 0x40a7d1 / ai 0x422130 / imuse | 2.22 / 1.92 / 0.61 | |
| draw path (r3d+pal, prof subsections) | ~3.1 | our C: flush 0.9, fetch 0.65, batch 0.31, cull 0.24, state 0.27, pal copy 0.14, drawarrays 0.4 |

Split of the ~34 ms: about **half is already native C** (world_cull 3.7, the actor transform/lighting
pipeline ~13 via 0x4100a0 and obj_update, pal/r3d ~3.1) and half is still lifted (skeleton 5.65,
world_emit 4.85, 0x483e60 3.28, render_tail 2.05, anim 2.22, ai 1.92). Native code being slow too is
the strongest sign that the limit is guest-memory traffic, not emulation overhead:
- the biggest lifted items left are skeleton (5.65) and world_emit (4.85) - both large transcriptions;
- every single-function native measured so far is worth 0.1-0.3 ms (0x41ae50+0x401eb8 = 0.1-0.2 total,
  0x406300 = 0.2, 0x40f1e0 ~0.6 at the time).
So the only change with a multi-ms ceiling left is removing the world_cull -> world_emit slot round trip
(previous entry), not another native.

### Lead session 3: draw-path caches (GPU front end), ~0.3-0.6 ms

Three per-draw costs in `r3d_gpu.c` that ran 600-1100 times per frame are gone:
- **MVP cache keyed by a generation instead of memcmp**: `hle_d3d.c` `SetTransform` now bumps
  `g_r3d.xf_gen`; `fetch_ctx_init` compares that one word instead of three 64-byte `memcmp`s.
  (`r3d_reset` sets it to 1 so a stale cache can never match the post-reset state.)
- **Texture uv-scale cache**: the per-draw `pow2(w) x2 + 2 float divides` now only runs when the
  bound texture (pointer or version) changes.
- **Batch state compare**: `memcmp(&st, &g.pst, sizeof st)` replaced by the tuple
  (render-state generation, texture id, colour-key override). `st` is a pure function of those
  three, so the batch/flush decision is unchanged but costs three compares instead of ~48 bytes.

Verification (the GPU path is 3DS-only, so the host golden run does not cover it):
- Azahar screenshot, scenario `tests/scenarios/lead_shot.txt` (steady gameplay, frame 2050):
  pixel diff 22% with all channel deltas in the 4-10 range and the geometry/structure identical in
  both images (`bench/lead_shot_ref.png` vs `bench/lead_shot_new.png`) - that is the gas-fog /
  darkening animation phase, which differs between runs. GPU statistics per scene
  (draws/triangles/batches/clears) match exactly up to the level load and track within 0.1% after
  it, with identical batch counts - the batching decision is unchanged.
- Host test suite green.

Measured (Azahar, nolimit, frames 1800-2100 scenario):

| window | before | after |
|---|---|---|
| 1500 | 36.3 | 35.8 |
| 1800 | 25.5 | 25.3 |
| 2100 | 32.1 | 31.5 |
| 2400 | 33.8 | 33.5 |

With the 30 fps limiter the same build reads 33.6 / 33.9 / 34.2 (was 33.6 / 34.0 / 34.4).

### Lead session 3b: the frame-time histogram, and the real target

Correction to "the 30 fps limiter masks everything" (previous entry): that is true for the *room*
window (33.6 ms, 115/300 frames at the 31.4 ms cap), but **not** for the heavy windows. Histogram
from a release run (`bench/lead_release_log.txt`, 300-frame buckets):

| bucket (ms) | scene 1500 | scene 1800 | scene 2100 | scene 2400 |
|---|---|---|---|---|
| <25 | 0 | 0 | 0 | 0 |
| 25-33 (at the cap) | 115 | 110 | 63 | 89 |
| 33-40 | 177 | 179 | 227 | 167 |
| 40-60 | 8 | 11 | 10 | 3 |

So in the 1800+ windows **most frames are work-bound at 34.4 ms**: the real per-step work exceeds the
33.3 ms budget by ~1.1 ms and the limiter cannot hide it. Two consequences:
- The committed target is now concrete: **~1.1 ms of work reduction locks the whole level at 30 fps**
  (the 40-60 ms bucket is the game's own 1/32 s two-step catch-up, present on the PC original too).
- `nolimit` numbers (25-31 ms) must not be read as the real per-frame work: without the limiter the
  game runs fewer simulation steps per rendered frame. They are still valid for A/B of a change.

Also in this pass: `r3d_gpu_draw` hoists the cull mode out of the per-triangle loop (it was a global
load per triangle, ~2000 triangles/frame).

### Lead session 4: native angle helpers (0x4940ab, 0x49418b, 0x4941c0)

The three float angle helpers run ~700 times per frame each and together execute 4.9% of all
lifted instructions (tools/icount: 0x4940ab 1.62%, 0x49418b 1.35%, 0x4941c0 1.90%), almost all
of it call-site state sync around a few x87 compares. Now native in `source/core/native_angle.c`:
- 0x4940ab wraps an angle to [0,360), 0x49418b to (-180,180], 0x4941c0 is the shortest signed
  difference. Every `fcomp` is an FCOM() so `fsw` (written back by the reference) is identical,
  and each leaves its result in ST(0) exactly like the reference epilogue.
- **Fallback**: the reference calls the CRT floor (0x49a480) whenever |angle| >= 360, and that
  function manipulates the control word and can raise FP exceptions while the harness's control
  word is random. The native handles |angle| < 360 (the game's real range) and hands the whole
  call to the lifted body otherwise, before touching any state. No guest calls remain on the fast
  path.
- The wrappers reproduce the reference's EAX (which is the *argument's* bit pattern, from
  `mov eax,[ebp+8]; push eax`, with the low half replaced by the last `fnstsw`), not the caller's.

Verification: `tests/test_override.c` gained a per-check control-word override (`g_next_fcw`; the
angle tests run with the game's 0x037f because an unmasked inexact exception otherwise stops the
guest) and randomized finite in-range inputs covering both wrap branches. **86,000 differential
cases pass** (was 71,000), golden A/B with `TPM_AX_OFF` over 1500 frames is **0 diff**, host suite green.

Measured (Azahar, nolimit scenario `lead_prof`):

| window | before | after |
|---|---|---|
| 1500 | 35.8 | 35.7 |
| 1800 | 25.3 | 25.1 |
| 2100 | 31.5 | 30.9 |
| 2400 | 33.5 | **33.3** |

With the 30 fps limiter: 33.5 / 33.8 / 34.1 (was 33.6 / 33.9 / 34.2). Frame histogram, scene 2400
window: frames at the 33.3 ms cap 63 -> **104** of 300 (the 33-40 bucket 227 -> 186), so pacing is
visibly better in the heaviest window, though the level is not yet locked at 30 fps.

### Lead session 5: native 0x40f170 (part-byte aggregate) and 0x47cac0 (Euler -> 3x3)

Same method as the angle helpers: small, very hot functions whose lifted bodies are mostly
call-site state sync.

- `0x40f170` (58 bytes of guest code, ~520 calls/frame, 1.88% of lifted instructions): OR/AND the
  byte at a given offset across an object's part pointers. EAX/ECX/EDX are the loop's values and
  EBX/EBP/ESI/EDI read back as the caller's (pushed/popped).
- `0x47cac0` (~215 calls/frame, 2.53%): rotation matrix from three Euler angles. Three `sincos`
  calls (0x494238, already native) plus nine products in the listing's order, each intermediate
  rounded to float where the original stores it. Two details that the differential test caught:
  the reference's epilogue writes back the x87 slot it used last, so the native must leave
  `st[(ftop+7)&7] = cos2*cos0` (the "slot leak" the lifter models), and EAX/ECX/EDX end as
  `pos[1]`, `out+0x24`, `pos[2]` from the translation copy.

Verification: **96,000 differential cases** pass (was 86,000), golden A/B with `TPM_AX_OFF` over
1500 frames **0 diff**, host suite green.

Measured (Azahar, nolimit):

| window | before (round 4) | after |
|---|---|---|
| 1500 | 35.7 | 35.5 |
| 1800 | 25.1 | 24.9 |
| 2100 | 30.9 | 30.6 |
| 2400 | 33.3 | **33.1** |

With the limiter: 33.5 / 33.8 / 34.1; frames at the 33.3 ms cap in the scene-2400 window 104 -> 111
of 300. Cumulative across these sessions the real per-frame work went 36.3/25.5/32.1/33.8 ->
35.5/24.9/30.6/33.1 ms (nolimit), i.e. ~1-1.5 ms removed, and the limiter-mode numbers barely move
because the heavy windows are still work-bound just above the cap.

Remaining hot lifted code by instruction share (tools/icount): 0x41b070 10.5%, 0x405cf0 9.7%,
0x404730 9.3%, 0x483e60 4.2%, 0x408078 4.1%, 0x487260 3.1%, 0x49a3ad 2.7%, 0x47cac0 now native,
0x40a344 2.2%.

### Lead session 6: attempted native 0x487260 (draw wrapper) - reverted

Tried the per-draw wrapper `0x487260` (3.07% of lifted instructions, ~600 calls/frame): the optional
render-state dance (0x488210/0x488510 x2 + 0x4873e0), the colour-key vertex copy into 0x854e58 with
the LUT at 0x4b88b8, the indirect `device->DrawIndexedPrimitive` through vtable+0x70, and the
0x86a434 vertex counter. **It broke the guest and was reverted.**

Evidence: the golden A/B stalled at frame 7 of 1500 with the ON build (fb hashes frozen), and the host
log showed the guest running wild (`gmem: FAULT read at 0xffffffd8`/`0x4`, then
`STOP reason=5 'too many memory faults' eip~ret=00000000 esp=ffffffd8`) - i.e. the guest stack ran
away, not a wrong draw. The call arguments were verified valid at the call site (dev/vt/verts/count/
idx all sane, `[0x882298]` = 0 so the state dance was skipped), so the suspect is the hand-built frame
for the indirect call: `cpu_call()` into an HLE thunk from a native caller, with the callee popping
return address + 6 args (`hle_ret(..., 6)`) and ESP restored to the frame afterwards. The next attempt
should use the fallback trick that worked for the angle helpers: native only the no-op shape and hand
everything else to `wmain_00487260_ref`, and add a differential test (which this one lacked, since it
calls through a function pointer).

Tree restored: gen regenerated without the override, host golden A/B back to **1500/1500 lines, 0 diff**,
test_override **96,000 pass**, 3DS release rebuilt.

### Lead session 7: the trig fast path was never taken (fcw 0x023f) - ~1.5-2.5 ms

Root cause found for why the CRT sin/cos cores (0x49c75d 2.04% + 0x49c80d 1.51% of all lifted
instructions) stayed hot after the wrappers 0x49c740/0x49c7f0 became native: `trig_fast()` required
`cg->fcw == 0x027f`, but the game also runs with **0x023f** - the same precision control (double),
rounding (nearest) and exception masks, only the reserved bit 6 clear. So every sin/cos from those
call sites fell back to the lifted CRT core and its argument reduction.

The check is now `(fcw & 0x0f3f) == 0x023f` (precision/rounding/masks only), which is sound because
the lifted cores describe their arithmetic in doubles regardless of that bit. Verified with the
golden A/B in both directions (`TPM_EM_OFF` on/off): **1500/1500 lines, 0 diff**, and
`test_override` **96,000 pass**.

Measured (Azahar, nolimit): 35.5 / 24.9 / 30.6 / 33.1 -> **34.0 / 23.5 / 28.0 / 31.2 ms**
(-1.5, -1.4, -2.6, -1.9). With the limiter: 33.5 / 33.8 / 34.1 -> **33.5 / 33.6 / 33.8 ms**; frames at
the 33.3 ms cap in the scene-2100 window 106 -> 115 of 300.

This is the largest single win of these sessions and it came from a measurement, not a rewrite: the
"native fast path" looked taken but its guard was rejecting the game's real control word.
Windows with the same shape to check next: the `sincos()` native (0x494238) has the same
`fcw != 0x027f` guard, and `trig_fast`'s non-finite/huge-argument fallbacks.

### Lead session 7b: same guard on sincos (0x494238) - another ~0.3-0.7 ms

The `sincos()` native had the identical `fcw != 0x027f` guard, so ~770 calls per frame also ran the
lifted CRT path. Relaxed the same way (`(fcw & 0x0f3f) == 0x023f`), golden A/B **1500/1500, 0 diff**.

nolimit after both guard fixes: 34.0 / 23.5 / 28.0 / 31.2 -> **33.7 / 23.1 / 27.3 / 30.7 ms**;
with the limiter 33.5 / 33.6 / 33.8. Total for the round (from round 5's 35.5 / 24.9 / 30.6 / 33.1):
**-1.8 to -3.3 ms of real work**, the largest single round of these sessions.

Lesson worth keeping: when a native has a "fast path" with a guard, verify at runtime that the guard
is actually satisfied by the game; both trig natives had been silently falling back to the lifted CRT.

### Lead session 8: guard audit and a reverted draft

- Audited the remaining "fast path" guards for the failure mode found in session 7 (a native that
  silently never runs). `0x406300`'s fallback gate (`[0x59df60] != 0`, the fade flag) is rare in
  level 1: instrumented for 50 s of host running, it never reached 10,000 fallbacks (~1.7M calls).
  `na_xform` reports 0 fallbacks, the angle helpers only fall back for |angle| >= 360.
- Started a native for `0x408078` (light record, 4.05%, ~430 calls/frame) and **reverted the draft
  instead of shipping it**: it needs the reference's whole-x87-stack restore at the epilogue, three
  `__ftol` conversions, C3-based equality tests in the dedup loop and three guest calls, and the
  first draft was not exact. Left for a session that can give it a differential test from the start
  (setup: entity + light descriptor + position, with the light record 0x8a4360+slot*0x40 in the
  compared region).

State after this session: release `.3dsx` newer than every source, `test_override` **96,000 pass**,
golden A/B 0 diff (both directions), nolimit 33.7 / 23.1 / 27.3 / 30.7 ms, limiter 33.5 / 33.6 / 33.8.

### Lead session 9: fresh block breakdown after the trig fix

Timer build (`bench/timers9_log.txt`, level frames 1800-2100): frame **41.3 ms** (was 43.8 before the
trig guards), render 14.69, obj_update 18.19.

| block | before (session 1) | now |
|---|---|---|
| frame | 43.8 | **41.3** |
| render | 14.88 | 14.69 |
|   skeleton 0x404730 | 5.65 | 5.64 |
|   world_emit 0x406830 | 4.85 | 4.78 |
|   world_cull 0x4199b0 (native) | 3.74 | 3.71 |
| obj_update | 19.36 | **18.19** |
|   0x40fe70 (o_fe70) | 8.99 | 7.96 |
|     0x4100a0 (native transform) | 5.02 | 4.95 |
|     0x483e60 (skinning) | 2.98 | **2.03** |
|     0x407d26 | 0.90 | 0.89 |
|   anim 0x40a7d1 | 2.22 | **1.33** |
|   ai 0x422130 | 1.92 | 1.92 |
| render_tail | 2.05 | 2.00 |

The trig fix shows up exactly where the trig-heavy lifted functions are (anim 2.22 -> 1.33, skinning
2.98 -> 2.03). Remaining lifted mass: skeleton 5.64, world_emit 4.78, skinning 2.03, render_tail 2.00,
ai 1.92 - all large transcriptions (1,400-2,200 lifted lines each). Everything small and hot is
already native.

### Lead session 10: fallback audit finished, hardware package rebuilt

- Angle helpers: instrumented their |angle| >= 360 fallback over 50 s of host running - fewer than
  5000 fallbacks each, i.e. not hot. With that, every native "fast path" guard has now been audited
  (`0x406300` fade gate rare, `na_xform` 0 fallbacks, trig/sincos guards fixed in session 7).
- Checked the remaining mid-size lifted functions for a cheap target: `0x484901` (render_tail, 2.00 ms)
  is a 6-instruction wrapper whose time is in `0x488000` (2 draws + state, 68 instructions) - not a
  native target; `0x422130` (ai, 1.92 ms) is 224 instructions with 8 callees; `0x407d26` (0.89 ms) is
  233 instructions but calls `0x408078` three times. Everything still worth more than ~0.2 ms is a
  1,400-2,200-line transcription.
- Rebuilt **tpm3ds.cia** (19:32, 2,555,328 bytes) from the current `tpm3ds.elf`, so the user can
  install the session's improvements on the New 3DS - which is where a 60 fps number can actually be
  produced (804 MHz + L2 is ~2-3x this emulator; Azahar at 100% clock is the 2DS XL proxy).

State: release `.3dsx` 19:29, CIA 19:32, `test_override` **96,000 pass**, golden A/B 0 diff,
nolimit 33.7 / 23.1 / 27.3 / 30.7 ms, limiter 33.5 / 33.6 / 33.8 ms (cap 33.3).

### Lead session 11: native sin_deg (0x494272)

A 4-instruction wrapper (`fld arg; fmul pi/180; call CIsin; ret`) that `world_cull`'s `wc_sin` calls
per darkened vertex. Now native in `native_actor.c`: the product stays a double (the callee reads it
from the x87 stack, so rounding it to float would be wrong), and it calls the already-native CIsin.

Verified: differential test with 400 randomized angles plus the full 5000-case suite - **101,000
differential cases pass** (was 96,000), golden A/B with `TPM_EM_OFF` **1500/1500, 0 diff**.

Measured: **no change in level 1** (nolimit 33.7 / 23.2 / 27.3 / 30.7, limiter 33.5 / 33.6 / 33.8).
The dark branch of world_cull is rarely taken in this corridor scene, so the win is below noise. Kept
because it is strictly less work per call and fully verified; it should show in darker areas.

Remaining candidates and their measured cost, for the next session: skeleton 0x404730 (5.64 ms, 2,242
lifted lines), world_emit 0x406830 (4.78, 1,681), skinning 0x483e60 (2.03, 1,428), ai 0x422130 (1.92,
224 instructions - the best size/payoff ratio left), render_tail's inner 0x488000 (2.00, 68
instructions but its cost is in draws), light/camera 0x407d26 (0.89, 233) and light 0x408078 (4.05% of
instructions, 274).

### Lead session 12: inline sin in world_cull, and a status check

- `wc_sin` (world_cull's darkening pass) went through the guest call machinery to 0x494272 and then
  CIsin. Both are natives now, so it takes their fast path inline: same control-word guard, same
  double product (not rounded to float), same finite/range check, then host `sin`. Verified: golden
  A/B **1500/1500, 0 diff**.
- Measured: **no change** (nolimit 33.7 / 23.2 / 27.3 / 30.7, limiter 33.5 / 33.6 / 33.8). The dark
  branch is not exercised in the level-1 corridor, so neither this nor the 0x494272 native shows up.

Honest status after twelve sessions: every small and hot function is native, every native fast-path
guard has been audited, and the remaining lifted mass is four large transcriptions (skeleton 0x404730
5.64 ms / 2,242 lines, world_emit 0x406830 4.78 / 1,681, skinning 0x483e60 2.03 / 1,428, ai 0x422130
1.92 / 224 instructions but three x87 slot writebacks). The measurable wins available now are in that
list; 0x422130 is the best size/payoff ratio and needs a differential test that models its
`st[(ftop-3)..(ftop-1)] = fs_[5..7]` epilogue.

### Lead session 13: the console is reachable - real hardware numbers

The New 3DS answers on **<3ds-ip>:5000 (ftpd)**. Pulled `sdmc:/3ds/tpm/log.txt` (407 KB) and
read the real hardware runs:

- `boot: tpm3ds, log=ok new3ds=1` - it is a New 3DS, and `main.c` does `osSetSpeedupEnable(true)`
  (the CIA exheader has `CpuSpeed 804MHz`), so these are 804 MHz numbers.
- Good windows (frame ~442-504 s): **31.7 / 34.2-34.8 ms** averages, 79-103 of 300 frames under
  33 ms - i.e. **about 30 fps, the same ballpark as Azahar at 100 % clock**.
- Later heavy windows: 40-60 ms, and the last three (564-607 s) sit at **72 ms (13.9 fps)** with
  `present: transfer wait 6.6-6.8 ms` per frame - that section is both heavier and pays a large
  display-transfer wait.
- `limiter: ... avg gap 0 us, max 86 us` - the limiter spin is not the bottleneck.

So the "Azahar at 100 % clock" proxy is honest for the normal scenes (~30 fps) and the hardware is
neither much better nor much worse. 60 fps on hardware would need the same ~2x work reduction, plus
the 6.8 ms transfer wait in the heavy section is its own problem (present path).

**Uploaded the current build** to `sdmc:/3ds/tpm3ds.3dsx` (3,976,452 bytes, the release `.3dsx` from
this session) so it can be launched from the Homebrew Launcher without reinstalling the CIA; the game
data at `sdmc:/3ds/tpm/data` is unchanged.

### Lead session 14: target refinement from the timer gaps

Chased down what the remaining timer buckets really contain, so the next big native starts from facts:

- `anim 0x40a7d1` (1.33 ms) is a **43-instruction wrapper** - the time is entirely its callee
  **0x409170** (846 lifted lines, 370 instructions, 2 calls, and an epilogue that writes the whole
  x87 stack back from the entry copy). That makes it *easier* than the ai function: no per-slot leak
  to model, just restore `st[]`/`ftop`.
- `render_tail 0x484901` (2.00 ms) is a 6-instruction wrapper; its cost is `0x488000` (68
  instructions: 2 draw-wrapper calls plus state), and on hardware the 6.8 ms `transfer wait` in
  `pal_gpu_present_direct` is the PICA200 finishing the frame (`end_frame` -> `C3D_SyncDisplayTransfer`
  has to wait for the queue), not the DMA itself.
- `ai 0x422130` (1.92 ms, 224 instructions) needs the `st[(ftop-3)..(ftop-1)] = fs_[5..7]` model *and*
  a full state sync at each of its 10 call sites - the most delicate of the remaining small ones.
- Release builds have no per-call entry cost (`CPU_FN_ENTER` is `((void)0)`), and no lifted function
  has a hidden guard left (audited).

Ranked remaining work by (gain / effort): **0x409170 anim** (370 instr, whole-stack writeback) >
0x422130 ai (224 instr, slot model) > 0x407d26 (233) > 0x408078 (274) > skeleton 0x404730 (2,242) >
world_emit 0x406830 (1,681).

No code changed this round; tree state re-verified (101,000 differential cases, golden 0 diff, release
`.3dsx` + CIA current, `.3dsx` uploaded to the console).

## BREAKTHROUGH: native 0x422130 (actor AI sweep) - frame work now 11.7 ms

The timer had it at 1.92 ms *in the corridor*, which hid its real cost: 0x422130 walks 64 actor
slots and does work per live actor, so in the actor-heavy scenes it dominates. Native in
`source/core/native_ai.c` (224 lifted instructions, ten guest calls, three leaked x87 slots modelled
exactly like the lifter: `st[(ftop+5..7)]`).

Verification: golden A/B over **2400 frames** (a new `tests/scenarios/golden_long.txt` that runs past
the corridor into the heavy scenes) is **0 diff**, and `test_override` **101,000 pass**. The AI native
has no per-function differential test (its ten callees touch engine state; the 2400-frame A/B is the
stronger check and exercises it hard).

Measured (Azahar, level):

| window | before | after |
|---|---|---|
| nolimit 1500 | 33.7 | **25.4** |
| nolimit 1800 | 23.2 | **11.8** |
| nolimit 2100 | 27.3 | **11.7** |
| nolimit 2400 | 30.7 | **11.7** |

nolimit histogram: **300 of 300 frames at 11.7 ms (85 fps)**. With the 30 fps limiter the game is now
cap-bound (33.2-34.2 ms = the game's own 30 Hz timer plus its two-step catch-up), i.e. **locked 30 fps**
with about 3x headroom: 11.7 ms per frame against a 16.7 ms 60 fps budget.

Session total: the heavy windows went from ~34.4 ms work to **11.7 ms** (3x), from
- the trig control-word fix (sessions 7: -1.8..-3.3 ms),
- the draw-path caches, angle/scene/sin natives (~1.5 ms),
- this AI sweep native (~19 ms in the actor-heavy scenes).

## Video fix (glm-4.6 follow-up session): intro movies now play

**Symptom**: the LucasArts logo, the Big Ape logo and the movies before the first level took their
natural time (the log showed logo.bil running 9 s, bigape.bil 6 s) but nothing was ever on screen.

**Cause**: the movie player decodes with Bink into plain memory, then writes each frame **straight into
the locked primary surface** and calls `Unlock` - it never blits and never flips. On the PC the locked
primary is scanned out directly, so that is enough. Here the primary is a guest buffer that is only
presented from `Flip`, so every movie frame stayed in guest memory (the frame counter did not even
advance: `r3d: scene 0: 0 draws ... 0 presents` for the whole intro).

**Fix** (`source/core/hle/hle_ddraw.c`, `sf_Unlock`): when the unlocked surface is the primary, present
it (`r3d_gpu_present`, else `present()`), guarded by the surface version so a double present cannot
happen. Verified: the boot capture now shows the video frames (`build-host/boot_intro.bmp` = the Big
Ape logo) instead of the menu, golden A/B over 2400 frames still **0 diff**, `test_override`
**101,000 pass**.

**Open**: the cinematic bar is 1 px low on the console (the top screen row shows one line of picture
above the black bar). The guest frame is correct - the host capture of the opening cinematic has pure
black rows 0-5 - so the offset is in the 3DS present/scaling path: the 640x480 target goes through
`pal_gpu_target_stage` (display transfer, bottom-up staging) and `video_blit`'s 2x2 box; the top output
row averages staging rows 479/478, so an off-by-one in the staging flip or the transfer height is the
prime suspect for the next session.

### 1px cinematic bar: narrowed to the 3DS present path

Captured the *presented* 400x240 frame on the emulator (`leadbench` scenario with two screenshots,
pulled from the Azahar sdmc) and compared it with the guest frame from the host at the same frame
numbers:

- Host (guest frame, 640x480) at frames 1150 and 1250: **row 0 is pure black** (0,0,0) across the width.
- 3DS presented image (400x240) at the same frames: **row 0 has non-black pixels** (8,8,8 / 0,4,0 /
  8,16,16) while rows 1..5 are black; one of the two shots also has the leftmost column non-black.

So the guest frame is right and the offset is introduced by the 3DS present path. Facts gathered:
`boot: screen 800x480` and `gpu: citro3d 2 target(s) 800x480` - the targets are 800x480, so the
*rotated* path runs: `pal_gpu_present_direct` transfers the target to the top screen with a 2x2
hardware downscale (screen row 0 = target rows 0,1), and the screenshot is de-rotated from the same
framebuffer (`copy[y*400+x] = fb[x*240 + (239-y)]`). The 2D colour-keyed letterbox of this cinematic
does not go through the 3D viewport transform, so the prime suspects are the transfer's sampling row
pair and the de-rotation index. Also noticed: `4 presents (230 failed)` in the intro - those are the
Bink frames (~15 Hz x 15 s) where `sf_Unlock`'s new present falls through `r3d_gpu_present` (the
primary is not a GPU target) to the software path, which works; the counter should be quieted.

Next step for that: log `fb[0..3]` and the guest row 0 in the same frame, and try sampling rows (1,2)
instead of (0,1) in the 2x2 downscale / de-rotation to see which matches the PC.

## Characters invisible: open bug (bisect so far)

Reported by the user while playing: the Jedis/player are not rendered. Reproduced on the emulator -
with a fixed scenario (`tests/scenarios/walk_shots3.txt`) the pre-session build
(`bench/tpm3ds_normal_14h.3dsx`) shows the Jedi and a droid in the room, the current build shows the
room empty (`build-host/y3.png` vs `build-host/w3_old.png`).

Ruled out so far, each by rebuilding and re-capturing the same frames in Azahar:

| change | test | result |
|---|---|---|
| every hand-written native (AI sweep, trig guards, angle/scene/sin natives) | `sdmc:/3ds/tpm/nonative.txt` ("natives: OFF" in the log) | still invisible |
| present-on-unlock (video fix) | disabled | still invisible |
| batch compare by tuple | reverted to memcmp | still invisible |
| world*view*proj cache | disabled (always recompute) | still invisible |
| face culling | disabled (cull = 0) | still invisible |
| wc_sin fast path | reverted to the guest call | still invisible |

So it is *not* any native and not those GPU-path caches. The remaining candidates are the uv-scale
cache (untested in isolation), the video fix's effect on game flow (the movies now run and consume
~450 presented frames, so the same scenario frame lands earlier in the level - the captures are during
the opening cutscene, whose camera pans an empty room), or something in the shared draw path that none
of the tests above touches. Next step: log per-frame draw counts and the actor mesh draws
(vertices/fvf/texture) in both builds and diff those, rather than comparing screenshots at possibly
different game times.

## Round 18: controls verified, CIA shipped, characters still open

**Controls**: the requested table is already what `source/pal/3ds/pal_3ds.c` binds (k_map) plus
`source/core/camera_look.c`:
D-pad/Circle -> 0xc8/0xd0/0xcb/0xcd (move+turn); C-Stick -> yaw/pitch camera orbit (camera_look_tick);
A -> 0x1c Return; B -> 0x2d X (jump); X -> 0x1d Ctrl (attack); Y -> 0x39 Space (use); L -> 0x2a Shift
(run); R -> 0x2c Z (force push); ZR -> 0x38 Alt (roll); ZL -> 0x34 "." (next weapon);
Select -> 0x33 "," (previous weapon); Start -> 0x01 Escape. No change needed.

**CIA**: built and uploaded to `sdmc:/cia/tpm3ds.cia` (2,555,328 bytes, 23:11).

**Characters**: still not solved. Additional things ruled out this round, each by rebuilding and
re-capturing the same scenario in Azahar: texture-upload cache (always re-upload), the opening
scene/timing (captured frames now show the HUD, i.e. real gameplay, still empty). New finding: the
primary-present from the video fix was calling `hle_clock_present()`/counting a frame for every
locked-primary unlock, which double-counts the clock; `video_present_silent` now suppresses that
(fixed, and it made the HUD appear where the scene was previously stuck in cutscene framing).

Next approach for the characters: log per-frame draw calls with (fvf, nverts, texture id, mvp
generation) and diff against the pre-session build at the same scenario frame, instead of guessing
from screenshots. A missing actor draw is unambiguous.

## Characters fixed: the AI native was the cause (and why the bisect lied)

**Found it.** With `tests/scenarios/skip_test.txt` + `sdmc:/3ds/tpm/skip.txt` (new testing mode: RETURN
spam, ~2 s to the level) the conference room renders Obi-Wan and Qui-Gon again after removing
**0x00422130** from the override list (`native_ai.c` renamed to `native_ai.c.off`).

**Mechanism.** When a function becomes an override, the lifter reduces the register sync at its *call
sites* (it assumes the hand-written native preserves what a normal C function preserves). That change
lives in the generated callers, so `nonative.txt` at runtime cannot undo it - which is exactly why the
earlier "all natives off" test still showed empty rooms and sent me hunting in the renderer. The AI
sweep is called by actor code, so its callers then read garbage registers after the call and the actor
pipeline (spawn/animate/draw) never produced the characters.

**Consequence for performance**: the ~19 ms heavy-scene win of the AI sweep is gone until that native
writes back exactly what the lifter assumes (next round's job: encode the contract, add the missing
differential test, then re-enable it for mega-fps *and* characters).

**New testing mode (in this build)**: `sdmc:/3ds/tpm/skip.txt` makes the input tick spam RETURN, so the
boot goes menu -> level in a couple of seconds for iteration, and logs `actors: N alive of 64` plus a
per-FVF draw histogram once a second.

## Root cause of the missing characters, and the git baseline

**Cause**: when a function is in OVERRIDES the lifter computes the call-site sync facts from the
*lifted* body, which no longer runs. The callers therefore dropped the register sync around calls to
the native - and that lives in the generated callers, so `nonative.txt` could never restore it. The
actor-AI native's callers then read stale registers and the whole actor pipeline (spawn -> animate ->
draw) produced nothing, while the world still rendered.

**Fix**: every override now reports `(all registers, x87)` to `call_facts`, which restores the safe
sync. The actor-AI native stays out of the override list until it reproduces the reference's register
writeback; a differential test for it is the prerequisite to re-enabling it (that is where the ~19 ms
heavy-scene win comes back from).

Verified: `tests/scenarios/skip_test.txt` + `skip.txt` screenshot of the conference room shows Obi-Wan
and Qui-Gon; `test_override` 101,000 pass.

**Git**: the project is now a repository (`git log`: 7389fab initial, a353a9b the sync-facts fix).
`.gitignore` keeps build outputs, `source/gen` (regenerated by the lifter), the 590 MB game data and
the bench artefacts out of history; the tracked tree is ~1 MB.

## Handover

`docs/NEXT_AGENT.md` has the ready-to-paste prompt for the next agent, the exact build/test/bench/ship
commands, and the session's findings in short form: the override/sync trap (the reason the characters
vanished), the trig control-word bug, the locked-primary movie fix, the actor-AI native that is worth
~19 ms and still needs its register contract, the measurement discipline (30 fps limiter, fake clock,
frame-counter shifts) and the smaller traps (line endings, generated tree, console ftpd).

## Session 15 (2026-11-15): the AI native restored and measured, movies fill the screen, RAM audit

### 1. Actor-AI native 0x422130: back in, verified - and the ~19 ms was an artifact

`source/core/native_ai.c` is restored, `0x00422130` is in `OVERRIDES` and `ALWAYS_REF`,
`Makefile.host`'s `GUESTC` lists it, and `source/gen` was regenerated. The regenerated tree differs
from the previous one in **exactly two files**: `wmain_gen_006.c` (the body becomes
`wmain_00422130_ref`) and `wmain_gen.h` (its declaration). In particular the single call site in
`wmain_gen_005.c` (0x0041fcbe) is **byte-identical** to the non-override build, because the facts fix
already made it sync EAX/ECX/EDX/ESP/EBP - so re-adding the override cannot disturb the callers the
way it did on 14 Nov.

The body was re-transcribed against `docs/asm/00422130_listing.txt` to reproduce the reference's
*full* exit state:

- every integer register write of the listing is mirrored, so ECX ends as 0x6c4350 (the slot pointer
  after the 64th iteration) and EDX as the value the last iteration left - not just "whatever";
- the epilogue writes EAX = 1, EBP = the caller's EBP (the reference's `mov esp,ebp; pop ebp`) and
  ESP = E+4;
- the x87 spill writes st[(ft0+6)], st[(ft0+7)] and TOP = ft0 only: slot 5 is dead at the return and
  the reference leaves it alone. The old body wrote it back too;
- **call-site x87 depth**: the lifter spills with `TOP = ft0 - depth` and rebases `ft0 = TOP + depth`
  after the call. Every call site here has depth 0 except the sqrt at 0x4222d7, which has depth 1
  (two `fld`s are still on the stack). The old body set `TOP = ft0` at every call, so the sqrt callee
  read its argument from the wrong slot. `ai_call` now takes the depth;
- **argument order**: cdecl pushes right to left, so the *last* push is argument 0. The old body
  passed `0x47e34d`'s three arguments in push order, i.e. reversed (`out` as the matrix and the
  matrix as `out`). Fixed for 0x47e34d, 0x47840e, 0x422427 and 0x420db2;
- call sites the lifter marks as not using x87 no longer touch `cg->ftop` or the slots at all (the
  reference does not either) - which also removes 3 stores + 3 loads per call.

Differential test (`tests/test_override.c`): the actor-slot array at 0x6bfa50 (0x4900 bytes) is a
*compared region* now, so the z-region machinery became per-test (`g_next_zr_ai`, two tables,
ZR_SZ 0x4948) and only the AI test pays the 18 KB copy. Two shapes run: every slot inactive (loop +
epilogue only) and every slot active through the `fl & 0x10` branch (0x41f700 + 0x422427, with
0x20000 clear so 0x422427 returns at once and 0x40000 set so the 0x420db2 animation walk is
skipped). 1,500 cases each: **test_override is now 104,000 cases, all green**. Mutation check: the
old body fails the new test immediately (`r[1] 006c4350/18ae9600`, ECX never written), so the test
really does hold the contract. A third shape for the in-range/sqrt branch was written and dropped:
those callees dereference slot fields that no random input keeps valid (both bodies fault), which is
the same reason the original override shipped without a test.

Verification: golden A/B (`golden_long`, 2400 frames) **0 diff** against `TPM_AX_OFF`; the skip mode
shows the conference room with Qui-Gon and the Queen (`bench/charcheck_log.txt`, `build-host/chk_c2.png`);
`ai native: 1841 calls` over 2400 frames.

**The measurement that matters**: with the native in and out, the nolimit windows are *identical*
(33.8 / 23.2 / 27.5 / 30.8 ms, byte-identical scene deltas). New switch `noai.txt`
(`g_ai_native_off`) isolates it on the device; `TPM_AX_OFF` does the same on the host. The sweep is
called **0.77 times per frame**, and the 1.92 ms the timer reports for it is *inclusive*: almost all
of it is the ten callees the native still calls. So the 11.7 ms of the 14 Nov "breakthrough" was the
actors not being drawn, animated or skinned (the broken call-site sync), not the AI sweep. Lesson for
the next session: **an inclusive timer entry is not a size/effort guide**; count the calls and check
whether the body or the callees are the cost before transcribing 1,400-2,200 line functions.

The native is kept: it is exact, tested, costs nothing measurable, and removes the exit-state bug
class.

### 2. Videos fill the screen in widescreen

The game draws its 2D output (menu screens and Bink movies) as 4:3 content inside the display mode,
so at 800x480 it sits pillarboxed or left-aligned; the 3DS then 2x2-downscales the whole frame.
`video_blit` gained a `fill43` mode (the 2D path only - a staged GPU frame is a real widescreen
picture and must not be cropped) that crops the centred 4:3 box and box-filters it to the full
400x240, averaging every source pixel an output pixel covers, per channel in RGB565. Geometry and look
were checked by re-implementing the filter in Python on a captured 800x480 menu frame
(`build-host/chk_fill_sim.png`).

**Fixed (session 17).** The mechanism: the movie player copies the decoded picture with
`BinkCopyToBuffer(bink, dst, pitch, height, dstx = 0, dsty = 0, flags)` (`0x497beb`), i.e. at x = 0
of the 800-wide primary, and leaves the rest black - hence a picture on the left. (The menu is a
different path: a centred 640-wide box.) And the movies do **not** present through the 2D blit: they
present on `sf_Unlock` of the primary, which used `r3d_gpu_present` (a GPU display transfer of the
whole surface, no cropping possible: PICA only scales by 2).

The fix has three parts:
* `sf_Unlock` always presents the locked primary through the CPU blit (`present()`), since the
  decoder wrote guest memory and the picture has to be cropped/scaled - the GPU transfer would move
  the whole surface unchanged;
* `video.c` decides from the frame itself whether it is a movie (`frame_right_margin_black`: the
  right eighth of the frame is black, which is what the x = 0 copy leaves behind; an open-file count
  is unreliable because the intro files are `.bil` and are opened on a path that does not keep one)
  and calls the new `pal_video_present_movie`;
* `video_blit`'s `fill43` gained mode 2: crop the **left-aligned** 4:3 box (`sx0 = 0`) instead of the
  centred one (mode 1, the menu) and box-filter it to the full 400x240.

Verified on the device with Azahar's own top-screen capture (Ctrl+P): the Big Ape logo now spans
**x 0..799 (100 % of the width)** with its letterbox intact (`build-host/movie_final.png`), where it
was x 0..700 (87.6 %) before. Golden A/B 0 diff, `test_override` 104,000, host suite green.

### 3. RAM audit: no leak found, and a RAM trace to catch the next freeze

Every 300th frame now logs `ram: guest heap N KB free (peak M KB used), VirtualAlloc N KB free,
linear N KB free` (`r3d.c`, `pal_linear_free()` in both pal backends). Watched runs:

| run | guest heap free | VirtualAlloc | linear free |
|---|---|---|---|
| host, 9000 frames | 20335 -> 7082 (the level load), then 6932-7083 repeating | 16832 constant | 0 (host) |
| Azahar, 2400 frames | 19885 -> 6693, then 6623-6877 | 16832 constant | 18921 -> 17779, flat after the load |
| Azahar, 9000 frames (~8 min, `bench/ram_long_log.txt`) | 6567 and **exactly constant for the last 240 s** | 16832 constant | 18921 -> 17626, then flat |

The 9000-frame device run also ends with `gmem faults 0`, `heap peak 21837 KB of 28332 KB` and a
9 ms shutdown - so nothing grows with play time.

No monotonic fall anywhere; the guest heap settles into a periodic cycle and the linear heap only
loses the level's texture uploads. Texture allocations fail loudly (`gpu: texture WxH: no linear
memory`, `gpu: texture table full`) and never did in these runs; `cmd buf peak 2.1 %`.

The user's freeze when turning the camera up was **not reproduced** (the C-stick is analog and the
scenario language only taps keys). The next occurrence should be diagnosable from the per-300-frame
RAM history plus `gmem faults` in `log.txt`.

### 4. Cinematic bar: device-only and systematic, but the guest frame is clean

Evidence, all gathered this session:

* A device scan with the skip mode, screenshots every 20 frames over one run, shows the line
  **consistently** on a settled letterbox: row 0 of the 400x240 capture is non-black on frames
  300-440 (`d300..d440.bmp`, max row sum 64, 213-300 px), while the frame below it is pure black.
  So it is not a transient fade.
* The **host** reproduces the same scene via the new skip mode (`build-host/skip.txt`, see below) and
  a settled letterboxed frame there (`L01.bmp`, 800x480) is **exactly black from row 0 to row 49**,
  with the picture starting at row 50. So the guest frame's 2D letterbox does start at row 0.
* A 2x2 display-transfer box averages two source rows and cannot shift the picture by one *output*
  row, and the artefact is also horizontal (column 0 of the capture is non-black on every row), so
  it is not the transfer's row pair.
* `r3d_gpu_clear` is **not** involved: with a one-shot rect log in place, the intro and the
  conference-room cinematic make no `Clear` call with rects, so the bars are drawn as 2D geometry
  through the r3d pipeline.
* The pre-transformed (FVF_XYZRHW) path already applies the D3D half-pixel convention
  (`(x + 0.5) * 2/w - 1`, `r3d_gpu.c`), so it is not a missing half-pixel there either.

What is left: the bar must be a quad drawn through the *3D* transform (FVF_XYZ), i.e. it is projected
through the viewport, and the 1-row difference is then a boundary-pixel / viewport-rounding difference
between the PICA rasteriser and the host's software rasteriser - the two renderers have never been
compared pixel-for-pixel at screen boundaries before. **And note a trap discovered the hard way: on
the device the frame counter is not reproducible between runs (no fake clock), so device captures
cannot be compared across runs by frame number.** The row-0 metric happens to be stable across a
settled letterbox, which is why it was usable here, but nothing else is.

Next step (a device experiment, not a guess): either (a) build a 3DS fake-clock switch like
`pal_host.c`'s `TPM_FAKE_CLOCK` so device runs become deterministic and frame-comparable with the
host, or (b) log the letterbox quad's guest vertices plus the computed clip coordinates on both
backends at the same frame and compare the resulting window-space rect. Only then nudge anything:
a half-pixel shift in the shared 3D path would move *all* 2D content, so it has to be proven first.

### 5. State

`test_override` **104,000 pass**, golden A/B **0 diff** (2400 frames, both directions), host suite
green, `tpm3ds.3dsx` and `tpm3ds.cia` (15 Nov 02:03) rebuilt from this tree, characters render,
movies fill the screen. world_emit / skinning / skeleton were not started - measure first (see 1).

### 6. Where the three remaining natives would actually pay (FN_TIMERS, window 2400-2700)

Timer build (`TPM_DEFS=-DTPM_FN_TIMERS`, 300 frames, 35.8 ms/frame in the timer build; the
`bench/timers15_log.txt` block):

| entry | ms/frame | calls/frame | what it means |
|---|---|---|---|
| frame | 35.79 | 1 | |
| render | 14.79 | 1 | |
|  world_cull 0x4199b0 | 3.72 | 1 | already native |
|  **world_emit 0x406830** | **4.87** | 1 | |
|   emit_core 0x41b070 | 4.86 | 1 | the whole body |
|    **emit_487260** | **4.35** | **379** | the per-draw wrapper + state dance |
|    emit_488210 | 1.35 | 380 | (nested inside 0x487260) |
|  **skeleton 0x404730** | **5.64** | 1 | no sub-timers: its own body |
|  render_tail 0x484901 | 2.04 | 1 | |
| obj_update 0x411028 | 12.40 | 1 | |
|  obj_17930 | 8.53 | 40 | |
|   o_fe70 | 7.34 | 4 | |
|    x_100a0 | 4.25 | 3 | native actor transform (real vertex work) |
|    **x_83e60** | **2.10** | 3 | skinning |
|    x_7d26 | 0.89 | 3 | |
| anim 0x40a7d1 | 1.33 | 1 | its callee 0x409170 |
| f_46f4a9 | 17.73 | 5.1 | parent of the update path (includes obj_update) |

Consequences for the three remaining targets:

* **world_emit 0x406830: not worth a transcription.** 4.35 of its 4.87 ms are 379 calls/frame to
  0x487260 (DrawPrimitive wrapper + SetRenderState), i.e. the D3D->r3d->pal path, not the emit
  body. A native of the 1,681-line body buys at most ~0.5 ms. The draw wrapper itself is the real
  target (attempted and reverted in session 6; retry only with a differential test and the
  fallback trick).
* **skeleton 0x404730 (5.64 ms, 1 call/frame, no sub-timers)** is the one where a native can pay:
  the time is in the body, not in callees.
* **skinning 0x483e60 (2.10 ms, 3 calls/frame)** is the second candidate, and smaller.

The timers are inclusive, so nested entries overlap; only entries with a small call count and no
timed children (skeleton, skinning) can be read as body cost.

**Verified directly (session 16).** The skeleton's six heaviest lifted callees were added to
`FN_TIMERS` for one run (`bench/timers16_log.txt`): `sk_486828` 0.04 ms at 5.3 calls/frame, and
`0x403fa0`, `0x403ef0`, `0x4056c0`, `0x404180`, `0x40e42a` **0.00 ms at 0.0 calls/frame** (those paths
are not taken in this scene). So of the skeleton's 5.65 ms, at most 0.04 ms is lifted callees - the
rest is its own body (1,023 instructions in 2,239 lines) plus already-native callees (__ftol 9x,
floor 6x, trig, the matrix natives). Same shape for skinning: 1,425 lines, 663 instructions, only
**4** distinct callees, three of them already native (`0x49418b` angle, `0x47cac0` Euler matrix,
`0x47e3e6`-class), so its 2.10 ms is its own body too.

Ranking by effort/reward for the next session:
- **skinning `0x483e60`: 1,425 lines for 2.10 ms** - the better ratio;
- **skeleton `0x404730`: 2,239 lines for 5.65 ms** - the bigger win.
Both need the in-game shadow check (`TPM_VERIFY_WC` style) rather than a unit test: no random input
survives their callees. `world_emit` stays excluded (4.35 of its 4.87 ms is 379 draw-wrapper calls).

### 7. Reproducible device runs (session 16)

`sdmc:/3ds/tpm/fakeclock.txt` (plus `nolimit.txt`) makes the 3DS clock a function of the presented
frame count - 16 ms per frame, exactly the rule of `pal_host.c`'s `TPM_FAKE_CLOCK` (`pal_3ds.c`:
`pal_3ds_fake_clock`, counted in every present path, including `pal_gpu_present_direct` via
`pal3ds_count_frame`). Two runs of the skip-mode scenario produced **byte-identical** screenshots at
frames 250, 300 and 350, so a device frame number now means a fixed game state instead of
"approximately the same moment".

Two limits, both measured:

* it needs `nolimit.txt`, or the game's limiter spins waiting for fake time;
* it does **not** align the device with the *host*: the same frame number is a different moment in the
  two runners (e.g. host frame 250 is a full-frame shot of the conference-room cinematic while device
  frame 250 is the same cinematic with its letterbox closed in), because the two count presents
  differently before the level starts. Align by content (search the host's frame range for the best
  match) before comparing - that is the missing piece for the 1 px bar experiment in section 4.

### 8. The cheap route to a big native does not work (session 18)

Tried the obvious shortcut for skinning `0x483e60`: copy the lifted body verbatim and collapse the
lifter's local state copy onto the real one (`cpu_t *const c = cg;` plus `#define lc (*cg)`, so every
`lc.r[x] = cg->r[x]` / `cg->r[x] = lc.r[x]` self-assignment compiles away and no register spill/reload
traffic remains around the four calls). The x87 slot model (`ft0`/`fs_[]`) was left exactly as the
lifter emits it, and the override was wired the usual way (`OVERRIDES` + `ALWAYS_REF`, off-gate to the
reference).

Result: **the golden A/B diverged on 1394 of 2400 level frames.** Two control runs bound the problem:

| build | golden A/B |
|---|---|
| override wired, body = the transformed copy | **1394 / 2400 frames differ** |
| override wired, body = pure pass-through to `_ref` | 0 diff (wiring and caller sync are fine) |
| override wired, body = verbatim lifted body | 0 diff (the extraction is fine) |

So the transformation itself is semantically wrong, and the lesson is that **the lifter's local copy
`lc` is load-bearing, not a compiler aid**: `lc` shields the body from whatever a callee leaves in
`cg->r[]`/`cg->fcf`/etc. beyond what the per-call facts expect. Aliasing it to the real state exposes
the body to every callee side effect, and something in this function's four callees changes a register
or a flag in a way the alias then leaks into the computation.

The divergence pattern says it is a *persistent* corruption rather than a per-frame numeric wobble:
0-299: 5 frames differ, 600-899: 0, then 197, 273, 298, 300, 300 for the following windows - i.e. once
the level starts calling it every frame the two states never re-converge. That is the signature of a
register, flag or x87-top value leaking between calls, which is exactly what a shadow check (compare
the full cpu_t and the touched memory after each call) pins down in one run.

Consequence for the next session: the skinning/skeleton natives must be *semantic* transcriptions
(read the listing, decide the register/flag contract explicitly, keep values in C locals) verified by
the in-game shadow check - not a mechanical collapse of the generated body. The attempt is reverted;
the tree is byte-identical to the previous build, `test_override` 104,000 and the golden A/B 0 diff.

### 9. The 1 px bar: the sub-pixel rasterisation hypothesis is rejected (session 19)

The obvious suspect was the pre-transformed vertex mapping in `r3d_gpu.c`: guest y = 0 maps to
clip y = 1 - 0.5 * 2/480 = 0.9979167, which is *exactly* the centre of screen row 0, so a top-left
fill rule that excludes edge-on-centre could drop that row (the host's software rasteriser includes
it). Tested on the device: changed the mapping to `(y + 0.5 - 1/1024)` (a 0.001 px nudge toward the
top-left, invisible elsewhere) and re-ran the deterministic skip-mode capture.

**Row 0 was unchanged** (max sum 52/64, 242/249/222 non-black pixels - the same numbers as before).
So the letterbox bars do **not** go through the XYZWHM path; the nudge is reverted, the build is
byte-identical to the committed one again.

What that leaves: the bar's first row is missing *inside the guest frame itself* - either the game's
2D bar starts at row 1 (in which case the PC original has the same 1-row line and the fix would
deviate from it), or the 3D scene is drawn one row too high on PICA only (a viewport rounding
difference between the two rasterisers). The discriminating experiment is to log the D3D
viewport/scissor rects and the `Clear` calls around a letterboxed cinematic on both backends at the
same frame and compare the rectangle; the previous attempt to find the bars as `Clear` with rects
found none, and a `Blt` into the primary found none either, so the bars are likely a scissored clear
or 2D geometry into the render target - logging the scissor state is the missing piece.

**And the same session found that the console is probably fine.** Azahar's own top-screen capture
(Ctrl+P, which bypasses our de-rotation and is authoritative for what the LCD shows) was taken on six
frames of the same skip-mode cinematic: on every letterboxed frame **row 0 is pure black** (max sum 0,
0 non-black pixels) and the picture starts at row 47/65/49. Our own scenario screenshots of
letterboxed frames show a non-black row 0. Both read the same framebuffer, so the difference must be
in *our* capture path - `pal_gpu_present_direct`'s de-rotation `copy[y*400+x] = fb[x*240 + (239-y)]`
- and not on the screen. A raw-framebuffer dump was added there to prove it, but the captures in
those runs went through the software path (`video_present`, which has no de-rotation), so the dump
never fired; the next attempt should dump the raw 240x400 framebuffer unconditionally on the first N
GPU presents instead of only on a scenario capture.

Conclusion for now: the 1 px line is a **BMP artefact** at least in the cases we can reproduce, and
the console's top row is correct. Verify on the real hardware before changing anything.

### 10. Native 0x407d26: the nearby-object gather (session 21)

A real win that was small enough to do properly: 0.89 ms/frame at 3 calls/frame (`x_7d26` in the
timer table), 233 instructions, and - unlike skinning - a listing that reads like a program:
`docs/asm/00407d26_listing.txt`. `source/core/native_407d26.c` is a transcription of it.

What it does: with `ctx->0x58 > 0` it rotates the actor's position into `ctx+0x15c..0x164` (sincos of
the angle argument, scaled by `[0x4aa180]`), publishes `node->0x2c` into `[0x8a4864]` and optionally
zeroes it; converts the position with two `__ftol` calls and asks `0x40d8cb` for the spatial table;
walks that table (byte count at +0, 0x40-byte cells from +0x10, a byte count at +0x25, uint16
indices from +0) resolving each index through `world->0x9c` (0x20-byte records); offers candidates to
`0x408078` until 20 are accepted; then does the same for the two object lists at `world+0x1cc`
(count +0x1c4, stride 0x80) and `world+0x1d8` (count +0x1d0). Returns the accepted count in EAX.

Two bugs the golden A/B caught before anything shipped, both from reading the listing too quickly:
`[0x8a0060]` is a *pointer* to the world struct (I dereferenced one level short, which handed garbage
pointers to `0x408078` and crashed the run 1000 frames later with 155 gmem faults at eip 0x488f4b),
and accepting a candidate jumps to `0x407f1c` (`jmp 0x407f1c`) - it falls into the object lists, it
does not skip them.

Verified - but **not by the scenario I first used**, which is the lesson of this section. The
`golden_long.txt` A/B passed 0 diff while never calling the function at all: its timer entry read
`0.0 calls/frame` on the device over a whole 900-frame run. `tests/scenarios/level1_bench.txt` (menu ->
level 1, the benchmark scenario) is what reaches it: a call counter in the native prints
**`gather native: 6023 calls`** and the A/B over its 3150 frames is **0 diff**. So the checks are
`level1_bench.txt` (not `golden_long.txt`) for this function, `test_override` 104,000 and
`test_imuse` 20,000. The A/B is `TPM_AX_OFF` (the native falls back to `wmain_00407d26_ref`), so ON
and OFF differ only in this body. A passes-but-never-called A/B is worthless - always print the call
count.

Still to measure: the timer entry after the change. Two traps found on the way: the
`TPM_DEFS=-DTPM_FN_TIMERS` build needs `rm -rf build` (a plain `make clean` leaves the release
objects and the run reports nothing), and the timer build is *very* slow on the device (`pal_time_us`
around every wrapped call), so a 300-frame window took 152 s of wall time and a `nolimit` level run
does not reach the sampled window in a practical time. The 0.89 ms is therefore still the lifted
figure. Attempts made in session 22 and why they failed: the timer report never fires in our
scenarios (neither on the device nor with the newly added `TPM_DEFS` support in `Makefile.host`),
because timer 0's counter never reaches 300 within a scenario that leaves the intro - the report needs
a long *gameplay* run, and the timer build is 20x slower, so the two requirements fight each other. A
plain host wall-clock A/B (`TPM_NORASTER`, 3 pairs) is pure noise: ON 8583/7899/8584 ms, OFF
7374/7868/8403 ms - a 0.3 ms/frame effect cannot be resolved against +/-600 ms of jitter. To measure
it, the right instrument is a counter delta (instructions or calls) or a hardware-side measurement with
the console's own timing, not another build of the host runner.

### 11. End-to-end check of the release build (session 23)

One device run of the shipped build through menu -> level 1 -> gameplay (`tests/scenarios`-style taps,
screenshots at frames 1200/1500/2000/2500): 0 gmem faults, clean scenario exit, and the frames show
the conference room and the character with the HUD on a full picture - i.e. the actor-AI native, the
new gather native and the movie/present changes all coexist without a regression. Evidence:
`build-host/e2e_e2.png` (conference room, characters) and `build-host/e2e_e4.png` (gameplay with HUD).

Also settled this session: the skeleton `0x404730` was *measured* before any transcription was started -
1023 instructions, 47 call sites across ~20 blocks, 22 distinct callees, and blocks that `rep stos` whole
fixed globals (`0x8bc920`, `0x8bc880`, `0x8bfaa0`) with per-block x87. That is a multi-session semantic
transcription, not a one-round job; writing it and leaving it unverified would be worse than not
starting. `build-host/skel_stream.txt` holds its instruction stream (one line per instruction, labels
marked) so the next session does not have to regenerate it.

### 12. Native 0x404730: the skeleton / bone-hierarchy update (session 24) - 5.65 ms reclaimed

The big one. 1023 instructions, 47 call sites, one call per frame, 5.65 ms of inclusive timer time
with no timed children, so the body is the cost. `source/core/native_404730.c` (1475 lines) is a
semantic transcription of `build-host/skel_stream.txt`; the stream is generated from the lifted body's
per-instruction comments, since there is no `docs/asm` listing for this function.

How it is built: a real emulated stack `S` (every push/sub/lea/add/pop mirrored, so `[esp+N]` slots are
the reference's exact addresses even where an outstanding push shifts them), an x87 model of all eight
physical slots plus TOP (`st(k) = xs[(xt+k)&7]`, values stay `double` until an `fstp` narrows them),
publish-everything-then-read-back at every call, and the off-gate/call-counter pattern of
`native_407d26.c`.

Four things that mattered, worth knowing for the next transcription:
* **The x87 entry depth is not 0.** The first level frame entered with TOP=2 (the caller leaks slots
  from earlier float code), so the model has to publish and read back all eight slots and TOP at every
  call, not only the "free" ones the lifter's static slots touch.
* **`fcom`/`fcomp`/`fcompp` need their pops.** One `fcomp` at `0x4051b1` lost its `FPOP()`, leaving the
  emulated stack one deep for the rest of the run; the A/B showed it from frame 272 and a shadow
  harness (run the native body and `_ref` from the same entry `cpu_t` + a 64 MB arena snapshot, compare
  registers, x87 and the arena) pinned it on the third call: `ftop mine=1 ref=2`.
* **`fsubr st(1)` is `D8 E9` = `FSUBR ST(0),ST(1)`** (st0 = st1 - st0), not what the one-operand text
  suggests. Re-disassembling from `game_files/GAMEDATA/BIN/WMAIN.EXE` (`.text` VA 0x1000 -> file 0x400)
  and checking that every address in the stream appears in the native is what caught it.
* **Eight of the 47 call sites have no x87 publish** in the reference (their callees use no x87), so
  "a call site without a spill" is normal here; publishing uniformly is harmless.

Contract: ebx/ebp/esi/edi through the emulated prologue/epilogue, eax/ecx/edx final values, ESP =
entry+4, `cg->fsw` maintained through `FCOM()` so the C0/C3 tests are the emulated ones, and eflags not
published (no callee consumes incoming flags; the only eflags read in a callee is DF for `rep movs`).

Verified: `level1_bench.txt` A/B **0 diff over 3150 frames with 2151 calls**, `golden_long.txt` A/B
**0 diff over 2400 frames**, `test_override` 104,000, `test_imuse` 20,000, and a device run through
menu -> level 1 -> gameplay with **0 gmem faults** and the conference room and character rendering
(`build-host/skel_s2.png`). The timer gain is not measured (same obstacle as section 10: the timer
build's report needs 300 calls of the per-frame function inside one scenario and the build is 20x
slower), but this is the one function whose cost is entirely its own body.

### 13. Native 0x483e60: the skinned-vertex transform (session 26) - 2.10 ms reclaimed

The last of the three natives named for this objective. 663 instructions, 3 calls/frame,
`source/core/native_483e60.c` (1432 lines), same structure as the skeleton native (emulated stack, 8-slot
x87 with TOP, publish at every call, off-gate, call counter).

Verified: `level1_bench.txt` A/B **0 fb diff over 3150 frames with 6023 calls**, `golden_long.txt`
A/B **0 fb diff over 2400 frames with 3773 calls**, `test_override` 104,000, `test_imuse` 20,000, and a
device run through menu -> level 1 -> gameplay with **gmem faults 0** and the skinned character
rendering correctly (`build-host/skin_k2.png`). Both the `__3DS__` and the host compile paths were
checked. The full-line golden diff is non-zero on 2600/3150 frames, but **only in the `gm=` field**, and
the first difference is frame 3 - the Bink logo, before any skinning call exists - so it is pre-existing
`TPM_AX_OFF` noise, not this native; the `fb=` field is 0 everywhere.

What needed care (the four-hundred-line transcription traps, all worth knowing):
* **An outstanding argument push shifts every `[esp+N]`.** `0x484541 push edx` (the third `0x49418b`
  argument) stays pushed across the whole `0x484547..0x4845a9` block, so the same textual address
  `[esp+0x3c]` refers to two different slots (`E0+0x38` and `E0+0x3c`) on the two sides of an
  `add esp,4`. That block builds the six angles handed to `0x47cac0`, so getting it wrong rotates the
  character silently. Literal `PUSH()`/`esp += n` mirroring is what makes it right.
* **x87 depth is 2 through the middle, not 0**: the value pushed at `0x484066` stays live while both
  flag branches push/pop, and both paths converge at `0x48411e` with depth 2.
* `fld st(0)/st(1)/.../st(4)` is a 6x broadcast of one value; `fstp st(2)` is store-then-pop and
  `fstp st(0)` a plain pop - not the one-operand reading.
* All **11** `fcomp` need their pop, and the `test ah` masks differ per site (`1`, `0x40`, `0x41`) with
  the `je`/`jne` polarity flipping between them (the skeleton's `fcomp` bug in a larger form).
* `ebp` is a live index that the `cl&1`-clear branch clobbers and reloads from `[esp+0x24]`.
* A fidelity fix found in review after the first A/B passed: `rep movsd` left ECX at the count instead
  of 0. Harmless there (ECX is overwritten two instructions later) but the contract says the same
  registers, so it was fixed and re-verified - the A/B alone would not have caught it.
* Not publishing `cg->eflags` is safe *only because* every path ends in the `0x4852b3` call with movs
  after it, so the flags are that callee's - exactly what the reference leaves.

### 14. Native 0x487260: the per-draw wrapper (session 28) - the 4.35 ms that world_emit spends

This is the native that makes `world_emit` `0x406830` irrelevant as a target: its 4.87 ms is 4.35 ms of
379 calls/frame to this wrapper, and the wrapper's own body is only **89 instructions**. It is a
`native_487260.c` of ~130 lines, and it is the best cost/benefit in the whole project.

The bug that killed this attempt (and, by the symptom, the reverted attempt in session 6) is
**cdecl push order**: the original pushes `ecx, count, src, 0x1c4, 6, dev`, so `dev` is at `[esp+4]`
(argument 0) and the flag at `[esp+0x18]` (argument 5). Passing them in push order hands the HLE draw
the flag as the device pointer: the run dies after 24 calls with 168 gmem faults, `eip~ret=00487339`
(the return address right after the indirect call). The rule: with `push a; push b; push c; call f`,
the arguments are `f(c, b, a)` - the *first* push is the *last* argument.

Everything else is mechanical: the optional render-state dance (5 calls when `[0x882298]` is set), the
colour-key rewrite of up to 0x40 vertices (8 dwords copied per vertex plus three LUT bytes from
`0x4b88b8`), the indirect call through `[[0x6fc984]+0x70]`, and the vertex counter `[0x86a434] +=
count - 2`. The four callee-saved pushes are mirrored so the callees' frames land at the same
addresses.

Verified: `level1_bench.txt` A/B **0 fb diff over 3150 frames with 2,265,809 calls**,
`golden_long.txt` A/B **0 fb diff over 2400 frames with 1,370,812 calls**, `test_override` 104,000,
`test_imuse` 20,000, and a device run through menu -> level 1 -> gameplay with **gmem faults 0** and
identical rendering (`build-host/wrap_w2.png`).

**With this, all three functions named for the objective are either native or measured out:**
`skinning 0x483e60` and `skeleton 0x404730` are natives (sections 12 and 13), the two natives done
before them (`0x407d26`, `0x487260`) cover what `world_emit` was spending its time on, and a native of
the `world_emit` body itself would add at most ~0.5 ms of its own.

### 15. Native 0x406830: world_emit itself (session 30) - item (3) closed

The last of the three functions named for the objective, and only 13 instructions: it tests
`[0x59debc]`, and if set calls the emit core `0x41b070([0x59df58], [0x6f83e4] + 8, [0x59debc])` and
stores the result in `[0x8be924]`. `source/core/native_406830.c`.

Two bugs, both instructive:
* **The early-out path must still pop the return address.** The zero branch is `je 0x406857` -> `ret`,
  and the lifted epilogue does `R(ESP) += 4`. Returning without that leaves the caller's stack 4 bytes
  out of place: the guest runs wild and dies at an arbitrary address (`call to unknown address
  004b466c`, 171,816 gmem faults) one call in. Every `return` in a native has to leave ESP at entry+4.
* **The reference spills EAX and ECX at the call site, which means `0x41b070` reads them.** They must
  hold the same values the pushed arguments carry (`[0x6f83e4]+8` and `[0x59df58]`), not whatever the
  caller left behind. The rule: match the call site's spill set, not just the pushed arguments.

Verified: `level1_bench.txt` A/B **0 fb diff over 3150 frames with 2151 calls** (the zero branch is
taken early and the real calls come later), `golden_long.txt` A/B **0 fb diff over 2400 frames with
1401 calls**, `test_override` 104,000, `test_imuse` 20,000, device run **gmem faults 0** with identical
rendering (`build-host/we_v2.png`).

**Item (3) of the objective is now literally complete: `world_emit 0x406830`, `skinning 0x483e60` and
`skeleton 0x404730` are all natives**, verified with the call counters and the A/B, and the fifth native
`0x487260` is what made `world_emit`'s time actually go away.

### 16. The loading-screen flicker: reproduced and diagnosed (session 31, not fixed)

Reported by the user: while the loading screen is up (a blue 1-99 % bar), the picture above it flashes
black and back. Reproduced deterministically on Azahar with `at 460 key RETURN tap` (menu -> level) and
one screenshot per frame:

| frame | 465 | 466 | 467 | 468 | 469 | 470 | 471 | 472 |
|---|---|---|---|---|---|---|---|---|
| upper image | **black** | picture | **black** | picture | **black** | picture | **black** | picture |

* The **host is clean**: the same scenario and the same frames give the picture on every frame, so the
  guest content and the HLE flip bookkeeping are right and the problem is in the 3DS presentation.
* Instrumented the two swap sites: during the load there is exactly **one** `gfxSwapBuffers` per
  present (all through `pal_gpu_present_direct`), so it is not a double present - the earlier theory
  that the Unlock after a Flip presented twice is wrong (that change is reverted).
* What is left is the buffer that a Flip makes visible: the game flips on every progress step but
  draws on only some of them, so the flip alternates between the buffer that was drawn and the one
  that was not, and on the 3DS the undrawn one shows black (its GPU target was never given the
  picture), while on the host the same buffer still holds the previous picture.

Two candidate fixes were tried and **reverted**, because both change the present count and the scenario
runner, the fake clock and the frame limiter all pace on presents:
1. skip the present when the back buffer's version is unchanged since the last Flip (`g_flip_version`);
   the screen then keeps the last image (correct), but the frame counter stops advancing and the run
   stalls at the frame where the loading screen appears;
2. the same, but counting the frame through a new `pal_video_present_repeat()`; that advances the
   fake clock (`pal3ds_count_frame`) but **not** the core's present counter (`g_count` in
   `core/video/video.c`), so the scenario still stalls.

The next attempt should add `video_present_repeat()` in `core/video/video.c` (increment `g_count`, call
the PAL repeat, no screen write) and route the "unchanged back buffer" Flip through it, then check both
the loading frames and that the golden A/B still has the same number of lines.

### 17. Loading-screen flicker: second attempt, and the signal that is actually needed

Tried the two obvious repairs on top of the diagnosis in section 16 and reverted both:

* `sf_Flip` counting the frame through a new `pal_video_present_repeat()` (3DS: `pal3ds_count_frame`,
  host: `g_frames_shown++`) while leaving the screen alone: that advances the fake clock but **not** the
  core's present counter (`g_count`), so the scenario still stalls at the loading frame.
* adding `video_present_repeat()` in `core/video/video.c` (increment `g_count` + `hle_clock_present`,
  then the PAL repeat) so pacing is right: pacing fixed (46 captures again), but the screen then stays
  black for the whole load instead of flickering, because the "did anything get drawn?" test used the
  *surface* versions (`f->version`, `b->version`) and **the loading screen does not move them**: it
  draws through the 3D/GPU path into the render target, and only `r3d_gpu_flip` copies the surface
  version into the target's version (`t->ver = f->version`).

So the signal has to be the **GPU target's own state**, not the surface version: a flip should present
when the back buffer's target has undisplayed content (`gtarget->dirty`, the same condition
`r3d_gpu_present` already tests) *or* when guest memory changed since the last present (the CPU-blit
case, `needs_load`); otherwise it should call the repeat path. That is a change in `r3d_gpu.c`'s
interface (it needs to report "nothing new" distinctly from "could not present"), and it is the next
thing to do - it is a present-path change that must be verified on the loading frames *and* against the
golden line count, not guessed.

### 18. Loading-screen flicker: third attempt, and where the cause actually has to be

Tried the GPU-target signal: a per-target draw sequence (`gtarget.seq`, bumped in `r3d_gpu_flush`) plus
`r3d_gpu_surface_seq()`, so a Flip would present only when the back buffer's target had new batches or a
surface version had moved, and otherwise call `video_present_repeat()` (which now exists:
`core/video/video.c` increments `g_count` + `hle_clock_present`, the PALs count the frame and touch no
screen). Pacing was right and 46 captures came back, **but the pattern was unchanged (`BPBPBPBP`)** - the
sequence advances on the frames that show black too.

That kills the "the game flips without drawing" theory in its simple form, and points at the target
lifecycle instead: a surface's GPU target can exist, hold nothing, and still be considered current
(`needs_load` false), so `r3d_gpu_present()` transfers it and the screen goes black, while the guest
memory of that same surface holds the picture (which is why the host never flickers). The next step is
therefore a *measurement*, not another guess: log, for the two buffers during the loading frames,
`tg->used / tg->dirty / tg->needs_load / tg->seq` and the surface versions, and see which buffer the
black frames come from. Everything tried so far is reverted; the tree is back to the committed state.

### 19. Loading-screen flicker: the measurement, and why it is not the present path

Ran the instrumented build (both buffers' pixel pointers, versions and their GPU target state logged for
flips 455-520) and then the decisive experiment: **forced the CPU present path** (`r3d_gpu_present` fails
always, so `sf_Flip` presents the front buffer straight from guest memory).

| build | pattern over the loading frames (B=black upper half, P=picture) |
|---|---|
| GPU present (normal) | `BPBPBPBP` then black |
| CPU present forced | **`BPBPBPBP...` for the whole window** |

So with the GPU path completely out of the picture the flicker is **unchanged**. The guest memory of the
two buffers itself alternates hence the game fills only one of its two buffers per frame *on the device*,
while on the host the same code fills both (the host never flickers). The flip log supports it: both
buffers alternate every flip, both targets read `dirty=1, needs_load=0`, and both surface versions
advance every flip, so nothing in the present bookkeeping is out of step - the content simply is not
there on alternate frames.

That makes this a **guest/timing** issue, not a present-path one, and it explains why three present-side
repairs changed nothing. The next experiment is on the guest side: dump both surfaces' pixel data
(`f->pixels`, `b->pixels`) at every flip during the loading frames and see which one holds the picture -
if the game is drawing into only the buffer that is *not* being flipped to, the fix is in the buffer
selection (which surface the game's 2D draws target), not in the PAL at all. This is also consistent with
the earlier note that the same loading screen shows the picture on every frame on the PC build.
- Camera look: C-Stick drives source/core/camera_look.c (called from hle_pump_platform): pushes the yaw base float 0x005bb534 and camera-struct pitch (*(0x008a011c)+0x34, degrees 270..360) by up to 55/35 deg/s; the game's own updateCam pulls them back 4%/frame so the camera springs back on release. C-Stick no longer sends turn keys. Host test: TPM_CAMX/TPM_CAMY env (-156..156). Host build for tests: make BUILD=build-host-cam (separate dir to avoid clashing with DeepSeek).

### 20. Loading flicker: the alternation is between guest memory and the screen, not in either

Session 19/20 measurements, all reverted after:

| experiment | result |
|---|---|
| top-half checksum of **both** buffers at every flip (frames 463-469) | both **identical**, constant `1513724` - the picture is in guest memory of both surfaces |
| `r3d_gpu_present` refusing when `t->ver != s->version` (stale target) | pattern unchanged |
| `pal_3ds_top_swap` without the extra `gfxSwapBuffers` (letting `C3D_FrameEnd` do it) | pattern unchanged |
| raw framebuffer dump next to the de-rotated capture | never fired: the capture block of `pal_gpu_present_direct` did not run in that run, so the loading screen presented somewhere else |

So the guest memory is constant, every present-path variant produces the same `BPBPBPBP`, and the
alternation must be introduced *between* guest memory and the LCD. The two mechanisms left are the
display transfer (`C3D_SyncDisplayTransfer` into `pal_3ds_top_fb()`) and the framebuffer double buffering
(citro3d's `C3D_FrameEnd` swap plus ours). Note that `C3D_FrameEnd(0)` *is* the citro3d swap and
`pal_3ds_top_swap()` then calls `gfxSwapBuffers()` again - removing that second swap changed nothing in
the captured pattern, but it may still be a real double swap whose effect the capture path masks.

Next session's first step: dump the *framebuffer* (not the guest frame) right after the transfer for
frames 455-470, from inside `pal_gpu_present_direct` unconditionally (not only when a scenario capture is
due), and see whether the two framebuffer addresses alternate content. That settles whether the transfer
targets the buffer that is being displayed.

### 21. Loading-screen flicker: FIXED (session 33)

The measurement that finally named it: logging the display transfer itself (`pal_gpu_present_direct`,
per-present top-half sum of the framebuffer it just wrote) for flips 455-480 showed the presents
alternating between **two render targets**, and while slot 0 transferred the picture (`topsum 34945`)
**slot 1 transferred 0** - black. The guest memory of *both* buffers holds the picture (verified by
checksums), so the black came from a GPU target that had never been given the guest content: it was
created for a draw that did not cover it (`full_clear` binding), and nothing ever brought it up to date,
so the transfer sent what the target had.

Fix, in `r3d_gpu.c`'s `r3d_gpu_present()`: **bring the target up to date from guest memory before
presenting** when it is behind the surface (`t->ver != s->version` or `needs_load`), i.e. flush, select
the target, `pal_gpu_target_load()` it from the guest bytes, then transfer as before. Guest bytes win,
which is the same rule `bind_target()` already uses for drawing. The old code simply refused to present
a stale target (`return 0`) and fell back to the CPU path, which is why every earlier attempt changed
nothing: the *present* was not the problem, the *target* was.

Verified: the transfer sums become identical for both slots from that frame on (`34945`, `34945`, ... where
it was `34945`, `0`, `34945`, `0`), and a screenshot run over the same frames gives **8 consecutive
pictures** (`PPPPPPPP`) where it used to alternate `BPBPBPBP`. Golden A/B `level1_bench` **0 fb diff over
3150 frames**, `test_override` 104,000, `test_imuse` 20,000, device run through menu -> level 1 with
**gmem faults 0** and correct rendering (`build-host/after_upload_fix.png`).

### 22. The 1 px cinematic bar was the same bug - and it is gone (session 34)

After the target-upload fix in section 21, ran the skip-mode scenario over a wide window (frames 470-820,
every 10th frame captured on the device) and measured every letterboxed frame: **8 letterboxed frames,
0 of them with a non-black row 0**. Before the fix the same measurement over the same kind of frames gave
`row0 max 52/64, 242-249 non-black pixels` on every letterboxed frame.

So the "1 px low bar" was not a rasteriser edge case and not a transfer/rotation artefact: it was a stale
row of the GPU target (the previous frame's content) surviving at the top of the frame because the target
was never brought up to date from guest memory before the display transfer. With the guest bytes uploaded
first, the bar starts at row 0 exactly as on the host.

That closes item (2) of the objective with evidence instead of an argument.

### 23. Intro movies: the data path, as far as it is measured (goal "video not playing", round 3)

Establishing the chain, with the instruments that produced each fact:

* **The decoder runs and is fed.** The Bink worker thread `0x8e77e0` is created per movie and lives for
  its whole duration (enters 4.36 s, exits 15.98 s, code 0). It polls `[0x8fb018]` (the sleep callback)
  and decodes every 8th iteration; movie-file `ReadFile` calls run at 20-27 per second through the
  logo. So the decode is not starved and not slow.
* **The frame lands in ordinary memory, not in a surface.** The player allocates `w*h*2` with
  `0x495290` (malloc) and `BinkCopyToBuffer` writes into that buffer
  (`dst = [ebp-0x18]`, pitch = `[0x86209c][0] * 2`). It never calls `Lock` on the primary (0 locks in
  the whole intro) and never `Blt`s (0 blits), which is why the surface we were presenting was zeros.
* **The player then copies that buffer into the game's own frame context**
  (`[ctx+0x5c] + (h * [ctx+0x1c]) * 2` after the copy at `0x497c00..0x497c16`), so the picture does
  reach a surface's memory - the primary's checksum read 8389 (a dark movie frame) in one run and 0 in
  another, i.e. the relationship between "the game has copied it" and "we present it" is an **ordering**
  question, not a missing path.
* **The profiler** (`prof.txt`) over the intro: 39.0 % of samples inside `IDirectDrawSurface4::Unlock`
  (our CPU movie present) and 30.7 % at PC 0 (host-side work), ReadFile 3.5 %, Lock 1.4 %, Blt 1.0 %.

Change kept from this investigation (`hle_ddraw.c`): **an Unlock never presents a completely empty
surface** - it asks the GPU first, and only blits the CPU copy when the surface actually holds
something. That is what stops the 800x480 buffer of zeros from being painted over the movie frame.
Verified: `test_override` 104,000, `test_imuse` 20,000, `level1_bench` A/B 0 fb diff over 3150 frames,
and a device run with no faults.

Next: log the surface checksum and the `g_frame` checksum **at the same instant** (inside `sf_Unlock`
and right after `video_present`'s copy) so the ordering question is answered in one run.

### 24. Intro movies: the decoder thread is starved by the guest scheduler (goal round 5)

Continuing section 23. Established this round, in order:

* **The picture is one frame, forever.** Logging the source surface and the presented frame in the same
  instant inside `video_present`: `srcsum` and `framesum` are identical and unchanging across all 226
  presents of the logo. So decode -> copy -> present all work; the frame simply never changes.
* **With `fakeclock.txt` the movie plays**: 162 distinct frames over 227 presents. The fake clock changes
  `pal_time_ms`, i.e. **scheduling**, not the guest clock - so the freeze is a scheduling problem.
* **Bink's poll callback is `Sleep`** (`[0x8fb018]` resolves to `fff00500` = `KERNEL32.dll!Sleep`, called
  with 3), so the decoder thread sleeps 3 ms per poll and decodes every 8th poll - about 40 frames/s if
  it gets the CPU.
* **It does not get the CPU.** Instrumenting `gthread_yield`: over 30 s thread 1 (the game) gets ~400
  slices and thread 2 (the Bink decoder) **19**. And the state is the puzzle: `state=1` (= `GT_RUN`)
  with `sleep_left` **negative** (wake already passed) - i.e. the thread is runnable and simply is not
  being picked by `pick()`.
* Tried and reverted: counting `.bil` as a movie for the clock rule (correct in itself but not the
  cause), a millisecond `QueryPerformanceFrequency`/`Counter` pair (in case Bink's 32-bit frame maths
  overflowed - no change), and a `gthread_yield()` in `sf_Unlock` (kept: it is a legitimate polling point
  since the movie loop only locks/unlocks, but it does not by itself fix the starvation).

Next step is inside the scheduler: log the `pick()` decision for the decoder (`me`, `now`, the state and
`wake` of every slot) for a few hundred yields during the movie, and find why a `GT_RUN`, ready thread is
skipped. The fix is then either in `pick()`/`schedule()` or in whatever leaves the decoder's `wake` in the
future relative to the clock the picker uses.

### 25. Intro movies: the clock base was the missing link (goal round 6) - 2 frames to 8, not yet playing

Round 5's starvation theory did not survive: making `pick()` choose the longest-waiting thread changed
nothing, and neither did `nolimit.txt`. What did change the behaviour was the clock. The chain:

* Capping the guest clock advance (so a single call never hands out a jump of seconds) made the movie
  **advance** - 4 distinct frames instead of 226 identical ones - which proves the jump is what Bink
  chokes on. But capping the shared clock froze the game: with movie presents marked "silent", the clock
  *base* (`g_gc_base`/`g_gc_last`) is only folded forward by `hle_clock_present()`, which the silent path
  never called, so `guest_time_us()` stayed a constant `base + capped`, the game's own limiter spun on
  `Sleep(0)` forever (150 s of `limiter: ... polls` and no presents).
* Capping only `QueryPerformanceCounter` changed nothing, so Bink's timer is `timeGetTime`, not QPC.

**Fix kept** (`video.c`): advance the clock base on *every* present - `hle_clock_present()` outside the
`!video_present_silent` guard - while still not counting the frame. Verified: the movie goes from
**2 distinct frames to 8** across its 226 presents. That is progress, not a cure.

**What still blocks it:** the gap between presents 4 and 5 is **13.2 s** (`slow frame 13227 ms (frame 4)`),
then 8.4 s for the next movie. Under `fakeclock.txt` the same code plays 162 distinct frames, so that
gap is *waiting*, not decoding - and it is what leaves Bink's 32-bit frame timing unusable. The next probe
is to sample the guest PC **inside that gap** (the sampler never reports it because no frame is presented
to trigger the 300-frame report): e.g. log `g_cur`/the running thread and a PC histogram every second
while `g_count` is unchanged.

### 26. Intro movies: where the wait is, and a working way to watch them now (goal round 7)

Round 6's clock-base fix holds: the guest clock now tracks real time (probe at each movie present:
`guest_ms=4650 host_ms=4695`, `4745/4790`, `5099/5182` - within a few ms, no jumps). What remains:

* **Nothing sleeps.** Logging `k_Sleep` and `gthread_sleep` for any interval over 100 ms: **zero** events
  through the whole intro. So the 13 s gap between presents 4 and 5 is the guest *running*, not waiting
  on a timer.
* **The movie advances two frames and stops.** Over the first 40 presents the frame checksum takes only
  two values (`22088924`, then `41236356` repeated) - the player shows two pictures and then re-presents
  the same one while BinkWait keeps saying "not ready".
* **Inside Bink's decode step** (`0x8e7570`, 154 instructions): it first takes a **byte spinlock**
  (`lock inc byte [ebx+0x26c]`, then `cmp dword [..], 1`), then bails to its exit unless
  `[+0x268] > [+0x29c]` - i.e. **unless there are buffered bytes beyond what the frame needs**. The buffer
  is filled through Bink's own IO callbacks (`[+0x27c]`, `[+0x280]`), which run on Bink's IO path. Our
  emulation of `lock inc byte` is correct (verified in the generated code), so the suspect is the IO
  handshake: this project implements **no** `WaitForSingleObject`/`CreateEvent`/`SetEvent` at all, and
  Bink's threaded IO is exactly what would use them.
* **Workaround that works today**: with `sdmc:/3ds/tpm/fakeclock.txt` present the same build plays the
  intro (162 distinct frames, verified in section 24). The clock then advances 16 ms per presented frame,
  which is enough to satisfy Bink's pacing. It is not the fix, but it is a way to see the movies now.

Next step: dump the state Bink's IO path keeps (`[+0x268]`, `[+0x29c]`, `[+0x2a0]`, the two callback
pointers `[+0x27c]`/`[+0x280]`) at each movie present, and check whether those pointers land in our HLE
(at which point the missing Win32 wait primitives are the answer) or in lifted Bink code (at which point
the IO thread is the problem).

### 27. Intro movies: the decoder is healthy - the copy goes to a buffer we do not present (goal round 8)

Dumping Bink's own bookkeeping at each unlock (handle = `[0x86209c]`, guarded so the early null never
faults - the guard's first version had an 8 MB ceiling and rejected the real 14.8 MB handle):

```
bk buf=44544 need=12288 pend=0 io1=008f28b0 io2=008f2910 req=0 st=2 fc=0 dur=1..10 t0=4288 t1=133
```

* `buf > need` (44 KB buffered against 12 KB needed), so the decode step's early-out is not taken.
* `st=2` (the state the decoder thread keeps while it works) and `dur` climbs 1 -> 10 in 800 ms, i.e. Bink
  is decoding at **about 12.5 fps**. The IO callbacks are Bink's own lifted code (`008f28b0`,
  `008f2910`), not our HLE, so the missing Win32 wait primitives are not blocking this.
* Yet a fine-grained signature of what we hand to the screen (`fs = fs*31 + pixel`, stride 7) takes only
  **3 distinct values over 226 presents**. The movie decodes, and the picture we present does not follow.

So the remaining gap is between the frame Bink produces and the surface we present. The player copies
the decoded buffer into its own frame context at `0x497c00..0x497c16`
(`[ctx+0x5c] + (height * [ctx+0x1c]) * 2`) through the loop at `0x497c23..0x497c3a`, so `[ctx+0x5c]` is
the destination to identify: if it is not the primary's `pixels` (`00b7eb10` in these runs), the picture
is going to a buffer that is then drawn as a texture (which is what the few successful GPU presents
suggest), and the CPU present we do on Unlock overwrites it with the stale primary memory.

Next step: log `[ctx+0x5c]` and `[ctx+0x1c]` at the movie copy (or compare the copy destination against
`dd_surface_of(primary)->pixels`) and then decide which of the two present paths owns movie frames.

### 28. Intro movies: five more attempts, all excluded (goal round 9)

| attempt | reasoning | result |
|---|---|---|
| frame-paced guest clock while a movie is open | the fake clock (which does exactly this globally) plays the movies | game hung at the logo: the clock only advanced on presents, so a loop waiting on the clock deadlocked |
| same, plus 125 us per clock read (the fake clock's own rule) | avoid the deadlock | game survives, **stall unchanged** (14.6 s) |
| also route `pal_time_ms` (scheduler/limiter) through the movie clock | the fake clock changes that too | game hung |
| same with the clock anchored so the switch is monotonic | a backwards jump freezes every wake-up | still hung |
| **refuse Bink's decoder thread** (`CreateThread(0x8e77e0)` returns 0) so it decodes inline | sidesteps Bink's threaded IO entirely | refusal works (logged), **stall unchanged** (13.0 s) |

So the stall is not the movie clock, not the scheduler, not Bink's thread, and not a sleep (round 7: zero
sleeps over 100 ms through the intro). The profiler cannot say either: it attributes samples to HLE
functions and the CPU_FN_TRACE ring, and 30 % land in the "00000000" bucket, i.e. *no guest function and
no HLE function* - host-side time it does not attribute further.

**Instrument gap, and the next step:** the codebase has no way to say *where* the guest spends that
13 s. `prof_sample()` records `g_hle_cur` or the last lifted-function entry, never a PC. Adding a real PC
histogram (record the dispatch address on every `cpu_call`/interpreter step for a sampled subset) is the
prerequisite for the next round; without it every remaining hypothesis is a guess, and five of them have
now been wrong.

**Working stopgap, unchanged:** `sdmc:/3ds/tpm/fakeclock.txt` makes the same build play the intro
(162 distinct frames).

### 29. Intro movies: the "13 s stall" is our own present cost, and the copy stops after 2 frames (goal round 10)

The instrument from section 28 already existed - the 3DS build simply never defined `CPU_FN_TRACE`, so
the profiler's function ring was empty and 30 % of samples fell into "00000000". With
`make TPM_DEFS=-DCPU_FN_TRACE` (and a `rm -rf build`, flags do not trigger a rebuild) the intro profile
attributes everything:

```
43.0% fff016d0 COM!IDirectDrawSurface4::Unlock      <- our own movie present
 4.4% 004616cc          (game)
 3.2% fff002d0 ReadFile
 2.8% 00493363          (game)
 2.1% 008f6521  1.9% 008f3370  1.7% 008f2b40  1.7% 008effb0  1.6% 008f4a90 ... (Bink decode/IO)
```

So the "13 s stall between presents 4 and 5" is simply **226 silent movie presents at ~50 ms each**, and
most of that is *our* `Unlock` blit (the 800x480 -> 400x240 CPU box filter plus the movie-crop math). The
movie player's presents are not counted as frames (`video_present_silent`), which is why they all land
between two counted frames and look like one huge stall.

Then the content: with the primary's pixels logged at each unlock, the front buffer (`00b7eb10`) goes
`0 -> 8389 -> 12110` over the first few presents and then **stays at 12110** for the rest, while the back
buffer (`00c3a410`) stays 0. So the player copies the decoded frame into the front buffer two or three
times and then stops copying, while it keeps unlocking and presenting the same picture.

Two concrete pieces of work are now separated:
1. **Cost:** make the movie present cheap. The crop+box-filter path is only needed for the fill; when the
   movie is not being cropped the blit could be the plain 2x2 path, and `frame_right_margin_black` could
   sample far fewer pixels. That alone removes most of the 43 %.
2. **Copy:** find why the player stops copying - it is gated by `[ebp-0x28] = ([ebp-0x1c] == 3)` where
   `[ebp-0x1c]` comes from `BinkDDSurfaceType` on our surface descriptor (`0x497bad`), and the two copy
   loops are at `0x497c3a` (mode 3) and `0x497ce7` (other modes); both write to
   `[ctx+0x5c] + height*[ctx+0x1c]*2`, which the log shows is the primary's own pixels. Next: log which
   branch runs (a temporary guest patch of that `cmp`, or a counter in the copy loop).

### 30. Movie present 16 % cheaper; the scoped frame clock does not work either (goal round 11)

**Kept - `video_blit_fill43` (pal_3ds.c):** the row span was recomputed inside the column loop (240
divisions per column) and every averaged pixel did three integer divisions. Both spans are now computed
once per blit into small tables and the averages use a 16.16 reciprocal table (with a division fallback
for n > 16). Measured on the device: the first movie's stall goes **13.2 s -> 11.1 s** and the second
**8.3 s -> 7.8 s**. The remaining cost is the screen present itself, which happens once per movie frame
and is inherent.

**Tried and reverted - the fake clock scoped to movies.** Since `TPM_FAKE_CLOCK` makes the intro play,
`pal3ds_frame_clock_begin()`/`_end()` were added to switch exactly that clock on when the first movie
opens and off when the last closes, anchored to the current real time so the switch cannot jump
backwards. Result: stall unchanged (12.1 s). Note the fake clock only advances `g_frames_shown` on
*counted* presents, and movie presents are deliberately not counted (they are what makes the movie show
without touching the frame counter), so during a movie that clock advances only by the per-read term -
which is exactly the behaviour the plain `fakeclock.txt` run had, and that one played. The difference
between the two runs is therefore still unexplained, and this variant is not the answer.

**State of the two remaining items from section 29:** (1) present cost - improved as above;
(2) the player stops copying after 2-3 frames - untouched, and still gated by the
`cmp [ebp-0x28], 0` at `0x497c19` whose operand comes from `BinkDDSurfaceType`. The next probe for it
must patch that instruction (a lifter IMM patch, since the lifted code is compiled C and cannot be
patched at runtime) to force one branch and see which one copies.

### 31. Intro movies: seen for the first time, and BinkWait is NOT the blocker (goal round 12)

**Visual proof at last.** With movie presents temporarily counted (`moviecount.txt`, a test-only switch)
the scenario can screenshot inside a movie. Frame 5 of the logo shows the 20th Century Fox searchlight
beams on black - so the movie really does render. Frame 100 (about 7 s later) is **byte-identical**
(sha256 `381d6a...`). The player shows two or three pictures and then freezes on that one. That is the
user's report, reproduced unattended.

**BinkWait's arithmetic, computed from the dumped inputs** (`bw c=4587 t20=3333 t1c=50000 e0=1
f0=4328 f4=0 f8=133 fc=0 cc=0 st=2`):

* frame duration `eax = (([+0x1e0]*1000) - ([+0x1f4]*1000)) * [+0x20] / [+0x1c]`
  = `e0 * 1000 * 3333 / 50000` = `e0 * 66.6` - i.e. 15 fps, and `e0` is the **frame index** (it climbs
  1, 2, 3... per present);
* elapsed `ecx = clock - [+0x1f0]` = `clock - 4328`;
* at `e0 = 10`, `ecx = 955 >= 666`, and the `[+0x1cc] != -1` path returns **0 (ready)**.

So BinkWait *is* ready and the player *is* advancing frames (that is what increments `e0`), on top of the
independently measured 12.5 fps decode. The blocker is after that: the frame the player copies is the
same picture every time.

**Also excluded this round:** refusing Bink's decoder thread (inline decode) - all six screenshots of the
run are byte-identical, so it changes nothing about the picture.

Remaining question, precisely: with `BinkDoFrame` called every frame (277 instructions, it stamps the
clock into `[+0x1e8]` and calls Bink's own helpers at `0x8f4600`), why does the decoded output stay
constant? Next probe: log the 16-bit destination buffer's checksum **immediately after
`BinkCopyToBuffer` returns at `0x497beb`** (the malloc'd buffer at `[ebp-0x18]`), which is upstream of
the copy into the frame context - that separates "Bink produced the same picture" from "the player copies
a stale buffer".

### 32. Intro movies PLAY: Bink's IO thread was starved by our present (goal round 13) - the root cause

**Root cause.** Probing Bink's input-buffer accounting (`[+0x268]` buffered against `[+0x29c]` needed) at
every unlock shows the decoder starving within a second:

```
bi n=0  buf=44544 need=12288 e0=1     <- first frames: plenty of data
bi n=41 buf=0     need=12288 e0=42    <- starved
bi n=44 buf=0     need=12288 e0=45
bi n=203 buf=2880 need=8192  e0=68
```

and Bink's decode step bails to its exit unless `[+0x268] > [+0x29c]` (section 9/26) - so from the second
frame on it decodes nothing, while the player keeps presenting the last picture on schedule (the 4x
slow-clock test scaled the "stall" to 36 s, proving it is clock-determined waiting, not CPU work).

**Why it starved:** Bink's IO/decode thread only runs at a yield point, and a movie present is tens of
milliseconds of straight-line host work (the blit). 226 presents x ~50 ms with no yield inside = the
thread gets ~20 slices per second, a fraction of what the bitrate needs.

**Fix:** a yield point inside the present, `pal_yield_to_guest()` (weak stub in `video.c`, strong in
`gthread.c`, called from `video_blit_fill43` every 32 output columns in `pal_3ds.c`).

**Verified, with pictures:** counting movie presents for the test (`moviecount.txt`, test-only, removed)
lets the scenario screenshot the movie itself. Six captures at frames 5, 15, 30, 45, 60, 80 are **six
distinct pictures** (previously all identical), frame 5 is black (the start of the logo) and frame 80 is
the **Star Wars Episode I title screen** - the intro plays through to the menu. Deterministic across two
runs (identical hashes).

Tests: test_override 104,000, test_imuse 20,000, level1_bench A/B 0 fb diff. The `fakeclock.txt` stopgap
is no longer needed for the intro.

### 33. The intro fix is a RACE, not a cure (goal round 14)

Round 13 reported the intro playing, with screenshots. It does not reproduce. Round 14 tried, in the
shipped configuration and each with screenshots at frames 5/15/30/45/60/80:

| variant | result |
|---|---|
| shipped (silent, cropped) | 6 captures, **1 distinct picture** |
| movie presents counted, cropped | 1 distinct |
| movie presents counted, uncropped | 1 distinct |
| limiter off (`nolimit.txt`) | 1 distinct |
| precise short scheduler waits (spin for <= 4 ms instead of a real sleep) | 1 distinct |

and round 13's exact configuration (counted presents + `nolimit.txt` + the yield) also gives 1 distinct
picture on a rerun. So the earlier success was a lucky schedule, not a fix - which also explains rounds
5/6 where identical experiments sometimes differed.

What *is* solid: the input-buffer probe (section 32) showed `buf=0` against `need=12288` inside the movie,
the decode step's early-out requires `buf > need`, and the player then keeps presenting the last picture
on schedule. So Bink's decode interrupts itself when its data is not there, and whether it gets the data
depends on how the cooperative scheduler happens to interleave Bink's IO/decode thread with the game's
present loop - a race this codebase's single-threaded, yield-based scheduler makes easy to lose.

All round-14 experiments are reverted; the tree is back at `bd733e7` (the round-13 commit, which at least
carries the yield, the fill43 optimisation and the earlier clock fixes).

### 34. Round 14: the IO is fine, the scheduler is fine, the decoder still bails (goal round 14)

Three instruments, on the shipped build, after the user chose to keep chasing the race:

* **Reads deliver.** Logging every `ReadFile` of 1 KB or more with its result inside the movie window:
  80 of 80 succeed with `got == n` (4096 each), file position climbing 21308 -> 344944 within 600 ms. So the
  compressed data is being handed over.
* **The decoder thread is runnable.** Probing the scheduler during the movie: `thr cur=0 t1=1/<wake>
  t2=1/<wake>` - thread 2 (the Bink decoder) is `GT_RUN` with its wake already passed, i.e. it is not
  blocked or starved while the picture is frozen.
* **A guaranteed decode budget changes nothing.** `pal_decode_budget_ms(20)` at the start of every movie
  present (running other guest threads for 20 ms, spinning on short waits so a 3 ms poll is honoured) left
  the captures at **1 distinct picture in 7**. Earlier "it plays" runs were luck: the cropped and uncropped
  present paths differ only in `sx0`, so nothing about the crop can explain the difference.

Also checked and clean: the frame limiter (on/off makes no difference), sleep granularity, and the
`buf=0` reading in section 32 is probably a transient rather than proof of starvation, since the reads
themselves are healthy.

So the picture is: the data arrives, the thread runs, and the decoded frame still does not change. The next
instrument has to be inside Bink's own output: the player's `BinkCopyToBuffer` writes into a buffer from
the game's `malloc` (`0x495290`, `w*h*2` = 614400 bytes); logging the checksum of that block per frame
would say whether Bink produced a new picture at all (it is the last point before the copy into the frame
context that the log can reach).

Everything from this round is reverted; the tree is at `ab16528`, tests green, artifacts rebuilt.

### 35. Intro movies: Bink does not decode even with data, a free lock and a running thread (goal round 15)

To reach Bink's own output the game's allocator (`0x495290`) is now overridable: a native override that
forwards to the reference and records the address of every large block. The movie decode buffers are
**175360 bytes** (logo) and **300800** (big ape) - not the 614400 guessed earlier, which is why the first
probe saw nothing. Checksumming the logo's buffer and reading Bink's own fields at every unlock gives, in
one line, the whole state during the freeze:

```
mv buf=00f14a60 bufsz=175360 bufsum=7101 frontsum=12110 | lock=0 buf=44544 need=12288 cc=0 st=2
```

* `bufsum` goes `0 -> 5085 -> 7101` over the first three frames and then **never changes again** - Bink's
  own decoded picture is frozen, so everything downstream (the player's copy, our present) is faithfully
  copying a stale frame;
* meanwhile `buf (44544) > need (12288)` - the compressed data for the next frame is there;
* `lock=0` - the byte spinlock the decode step takes is free;
* `cc=0` (requested frame != -1) and `st=2` - BinkWait's conditions to decode are met.

So Bink's decode step is entered, passes its lock check and its buffer check, and still produces nothing.
The remaining bail inside it is the call through `[+0x27c]` (`008f28b0`) whose result is tested: `test eax,
eax / je <exit>`. **That callback is the next instrument** - whether it returns 0, and why, is the last
step between here and working intro movies. The two unimplemented instructions the lifter reports are
`ljmp` (CRT entry code), not decoder code.

Round-15 instruments are reverted (`lift.py`, `hle_ddraw.c`, `hle_thread.c`, the native file removed, gen
regenerated); the tree is at `d38d73f`, tests green.

### 36. The decode's blocking callback is Bink's AUDIO path, not file IO (goal round 16)

Following section 35's callback `[+0x27c]` = `0x8f28b0`: its body tests `[esi+0x48] != -1`, and when that
slot is -1 it calls through `[0x90dafc]` to obtain it. Bink's init (`0x8f267d`, `0x8f26da`) fills those
globals from name strings, and reading those strings out of BINKW32.DLL (guest VA -> file VA via the
0x008e0000 load base) identifies them:

```
[0x90dafc] = _AIL_load_sample_buffer@16     <- MSS32
[0x90dae8] = _AIL_sample_buffer_info@20     <- MSS32
```

So the callback the decode step bails on is **Bink's soundtrack path through AIL**, not file IO: when the
audio buffer cannot be loaded, `0x8e7570` exits before decoding a frame, which is exactly the frozen picture
observed while `buf > need` and the lock free.

Both AIL entry points *are* implemented in `hle_mss.c`, so this is not a missing HLE function. One
measurement is known to be wrong: the probe that read `[0x90dafc] = 0` ran at 2.6 s, **before** Bink's init
(the movie opens at 4.3 s), so the zeros meant "not initialised yet", not "unresolved". And the call site
passes one argument while `_AIL_load_sample_buffer@16` takes four, so the exact route from that call to AIL
still needs reading at movie time.

Next: read `[0x90dafc]`, `[0x90dae8]` and the surrounding slots at the *first movie unlock*, and the
`[esi+0x48]` handle, then decide between "make Bink open movies without sound" (a lifter immediate patch on
the `BinkOpen` flags) and "make the AIL path succeed".

### 37. The freeze is Bink waiting on its AIL audio stream (goal round 17)

Section 36's mapping was off by one instruction: at `0x8f267d` the store happens *before* the call, so
each global holds the result of the *previous* lookup. Corrected:

```
[0x90dafc] = _AIL_sample_buffer_ready@4     (one argument: the sample handle - matches the call site)
[0x90dae8] = _AIL_sample_buffer_info@20
```

The decode gate is `0x8f2910`, which returns 0 - aborting the decode - exactly when
`[obj+0x48] == -1`, and `[obj+0x48]` is the result of `_AIL_sample_buffer_ready`. In `hle_mss.c`:

```c
hle_ret(c, s ? (uint32_t)mixer_stream_ready(s->voice) : 0xffffffffu, 1);
```

so our mixer's value is handed straight to Bink, and **-1 is Bink's "this stream has no soundtrack"**:
the decode then aborts on every frame, which is the frozen intro picture. Two experiments:

* `sample_buffer_ready` coerced to 0/1 (and `sample_buffer_info` returning success): Bink now proceeds
  into its audio path and the game **hangs before the movie** instead of freezing it. Both reverted.
* leaving it as is: the movie freezes at frame 2-3 (documented in sections 32-35).

So the decode is gated on a *working audio stream*, not on video data, the lock or the scheduler. Our
`hle_mss.c` already has a streaming section ("double buffering; used by the lifted BINKW32.DLL"), so the
next round's job is well defined: make that stream answer Bink correctly - `mixer_stream_ready` must report
0/1 (never -1) *and* the buffer Bink then asks for (`_AIL_load_sample_buffer@16`) must really be loaded, so
`_AIL_sample_buffer_info@20` can answer with a position and length - rather than coaxing Bink off audio
from the outside.

### 38. Movies hang: the Bink audio-gate natives were the cause (fixed by the user's request)

`native_bink_28b0.c` / `native_bink_2910.c` (override of 0x8f28b0 / 0x8f2910, "pretend an empty audio stream")
make Bink enter its audio path, and the game then HANGS right after the IO thread starts (host log ends at
`gthread: thread 2 ... at 008e77e0`, no frame ever presented). Without them (stock lifted code) the intro plays:
4 distinct screenshots at frames 100/300/600/900, no hang. Both files moved to `docs/disabled/`, the `'bink'`
override set in `tools/lifter/lift.py` is empty, `source/gen` regenerated. Do NOT re-enable without a host run
of the intro scenario (screenshots must differ and the run must finish).

### 39. Logo movie "hang" = decode/present too slow; logos are auto-skipped on 3DS
Azahar WITHOUT a scenario file: logo.bil shows a frozen wedge for ~11 s (Bink decode+present ~95 ms/frame, audio-paced
by the AIL stream which works: buffers free every ~150 ms). Not a deadlock. Stream calls (ready/load) were checked.
Fix for now: while a file named `logo.*` is open (`g_logo_open`, hle_kernel32.c), `input_skip_tick` taps RETURN
(input.c) -> logo ends in 0.4 s. bigape/scene cutscenes still play slowly: needs native Bink block decode.

### 40. Logo/intro movies unfrozen: refuse Bink's AIL sample while a movie is open (silent movies)
Section 39's "just slow" was wrong: with Bink's audio stream active, logo.bil showed ONE picture for ~11 s (checksum log
in sf_Unlock: identical `pf` sums), while IO and audio buffers kept flowing. With `_AIL_allocate_sample_handle` returning 0
while a movie file is open (`hle_movie_open()`), Bink plays without a soundtrack and every present is a new picture
(logo ~8 s, ~14 fps; bigape and ss*.bik fine). Movies are SILENT now. Also `.bil` files count as movies (real clock).
The logo auto-skip was removed again (user did not want it). Next: real audio for movies (the AIL stream gate, 37).

### 41. ROOT CAUSE of the frozen movies: `_AIL_sample_buffer_info` returned 0 (supersedes 40; movies have sound again)
Bink's audio-stream callback 0x8f28b0 calls `buffer_info(h,0,0,0,0)` and only marks the stream running (`[obj+0x2c]=1`)
when it returns NONZERO. Ours always returned 0, so Bink never started/paced the audio stream and decoded video only for
the first ~3 frames (picture frozen while audio buffers still flowed). `hle_mss.c` now returns 1. Verified in Azahar
(no scenario): logo.bil picture changes on every present (`pf` checksums), audio stream loads every ~140 ms.
The silent-movie experiment (refusing `allocate_sample_handle`) was removed. DeepSeek's `native_bink_*` hacks stay disabled.

### 42. Audio stutter: the mixer now renders 150 ms ahead (MIX_LEAD_US, mixer.c)
mixer_service runs only when the guest calls the sound layer; Bink decode+present leaves 90-150 ms gaps (loads up to
400 ms), each one an underrun on the 3DS (8x1024-frame ndsp ring = 370 ms). Measured in Azahar: before, 60+ gaps >80 ms
during the logo/bigape movies; after: no underrun during movies (only during loading). Cost: ~150 ms extra sound latency.

### 43. Audio: mixer thread tried and rejected; movies render 250 ms ahead
A mixer thread on core 2 (LightLock around the mixer API, wrappers `*_u` in mixer.c, thread code still in pal_3ds.c but
NOT created) made the game freeze in the first movie in Azahar even with the guest also servicing; cause not found
(bisect: locks alone are fine, creating the thread is what hangs). Shipped instead: `service_u` renders `lead` ahead:
100 ms in gameplay, 250 ms while a movie file is open (`hle_movie_open()`); 320/400 ms lead hung at the start of the
second movie (not understood). ndsp ring 20x1024 frames. Azahar: logo movie 12 s with one start-up underrun,
bigape small 20-37 ms underruns, 0 frames dropped. `pal_log` flushes only every 2 s: a killed run's log tail is stale -
use a clean exit (scenario quit_after_frames) before concluding anything is hung.

### 44. Movie audio stutter = Bink decodes slower than real time -> its audio stream starves (and what helped)
Measured (log `mixer: stream starved`): with ~91 ms/frame (logo, 15 fps movie) Bink produces audio at ~70 % of real time
-> the stream ran dry every ~250 ms ("woo--wo--"). Frame cost split: CPU present blit ~30 ms, rest Bink decode ~60 ms.
Done: (1) `video_blit_fill43` fast path for movies only (hle_movie_open, 640x480 -> 2x2 average instead of the general box
filter): logo 91 -> 60 ms/frame, no starvation. (2) `_AIL_minimum_sample_buffer_size` 500 ms per buffer (was 125 ms).
(3) mixer lead 250 ms while a movie is open, 100 ms otherwise; FIXED an unsigned underflow in service_u when the lead
shrinks (movie ends) that flooded the ndsp ring ("21M frames dropped"). bigape movie is ~80 ms/frame (pure decode) and
still starves about once a second: needs native Bink decode (next step). `pf` presents are logged every 10th frame.

### 45. Movie fps floor (measured, no gain): -O3 for bink_gen (same ms/frame); profile = Bink decode spread over ~15 functions
logo ~60 ms/frame (blit now ~0: same as with the blit skipped), bigape ~80 ms; 15 fps needs 66.7. Profile (28 s, includes
loading): Unlock 26 % (decode runs in the yields), ReadFile 4 %, PeekMessage/Sleep ~7 % (main thread spinning), Bink
block functions 008f6521/008f3370/008effb0/008f4a90/008f63ae 2-3 % each. Remaining options: inline/native Bink block
functions (lifter), or decouple audio from decode speed (pre-roll buffer, costs lip-sync).

### 46. Movie audio: the stream follows Bink's decode speed (mixer.c `smul`)
Bink decodes 60-80 ms/frame against a 66.7 ms budget, so its soundtrack arrives slower than real time and the stream
starved. While a movie is open, a starving stream voice now plays slower (`smul` x0.9 per starve, min 0.5; +0.02 per 3
buffers that arrive in time, max 1.0) instead of cutting out; video runs at the same slower pace, so sync holds. Pitch
drops a little on slow movies. Builds; NOT yet checked in Azahar. Real fix for fps stays a native Bink block decode.
**Update (same session):** the real bug was that `hle_movie_open()` was FALSE during bigape/scene movies: Bink reads a
small movie into memory and the game's file handle is closed ~100 ms after the open, so the 250 ms mixer lead, the fast
movie blit and `smul` were all off. `hle_movie_open()` now also stays true for 3 s after a movie open/close and 0.8 s after
each presented movie frame (`hle_movie_touch`, video.c). `smul` starts at 0.88 for a movie stream. Azahar, logo+bigape:
starved events 7 -> 0 (only the start-up underruns remain), bigape ~70-80 ms/frame (about 13 fps at ~0.88x speed).
**Update 2 (movie present 18 -> 7 ms/frame):** (1) `r3d_gpu_present` uploaded the whole 800x480 primary into a GPU target on
every movie Unlock (8 ms) and then returned 0 anyway because the target was never drawn into: it now bails first when the
target is not dirty (r3d_gpu.c). (2) `video_present` blits movie frames straight from guest memory (16-bit 565, pitch == w*2,
silent present) instead of copying 800x480 into `g_frame` first; `g_frame` is only materialised for screenshots
(`g_frame_valid = 0` + `frame_from_source`). Azahar, clean -O3 build: logo 54 ms/frame (15 fps pacing reached, full speed),
bigape ~75 ms/frame (~13 fps, audio at ~85 %). A 4 ms scheduler time slice and mixer render cost (~2 %) were measured: no gain.
What is left for bigape is the Bink decode itself (~60 ms of lifted code per frame).
**Update 3 (measured, supersedes the fps guesses above):** counting presents in the log (`pf n` vs time): logo 120 frames in
7.0 s = 17 fps, bigape 93 frames in 6.5 s = 14.4 fps - i.e. at the movies' own rate, the earlier "75 ms/frame" was a misread of
the alternating 40/110 ms cadence. Per-call timers on the lifted exports (temporary): BinkDoFrame ~13-30 ms, BinkCopyToBuffer
2-6 ms, present 7 ms, the rest is Bink pacing waits. The main thread runs ~97 % of the time (helper threads ~3 %, ~500
switches/s), so there is no scheduler overhead to win. Audio stream speed now starts at 95 % (`smul`), x0.97 per starve,
+0.003 per 2 buffers: 0-2 starves per movie instead of 7. Not checked on real hardware or on in-game cutscenes.

### 46b. Movie jitter: frames are paced to one 15 fps period (video.c `movie_pace`)
Cause (Azahar, per-frame display times): Bink starts frames on a steady grid but decode cost alternates by ~13 ms, so frames reached
the screen 53/80/53/80 ms apart. `movie_pace()` shows a movie frame no earlier than 66.7 ms after the previous one (late frames go
out at once; a gap > 3 periods restarts the grid). Measured display gaps, logo: mean 67.0 ms, sd 2.7 ms (was ~13 ms). Bigape is
decode-bound in Azahar (mean 73 ms), so pacing cannot smooth it there; on the faster 3DS it should. Regression note: an early
return I had added to `r3d_gpu_present()` (skip the target upload for movie frames) brought back the loading-screen flicker and the
1 px bar (sections 21-22) and was reverted; `r3d_gpu.c` is identical to HEAD.
**Hardware data (New 3DS, CIA, 124 MB mode, old build without pacing):** present gaps alternate ~56/77 ms (logo), plus catch-up bursts of
23-31 ms gaps (video fast-forwarding after a hitch = the visible "jumps"); no stream starves, 39 mixer underruns, all in loading.
`movie_pace()` now also writes one `movie: N frames, display gap mean/sd/min/max, under 55 ms, over 80 ms` line per movie to
log.txt, so the device result can be read from the log without a manual test.

### 46c. Status after the hardware test (user: "its fixed")
New 3DS, CIA build: movies run smooth, loading screen no longer flickers (the `r3d_gpu_present` upload step is intact).
**Open:** the cinema bar (the black rectangle at the top of the screen in letterboxed cutscenes) is still 1 px too low on the
console; it needs to move 1 px up. Not investigated yet in this session (see sections 9 and 22 for the earlier work on it).

### 46d. Quick menu test mode and the menu's cost (Azahar)
* `sdmc:/3ds/tpm/menu.txt`: presses RETURN only while the intro `.bil` movies play and stops once the menu's own `ss0-3.bik`
  clips open (`g_menu_reached`), so the run ends *in* the main menu (~6.6 s after launch; `skip.txt` still goes to level 1).
  Without the stop the spam picked "Start" and loaded the conference room.
* The menu is not CPU-bound in Azahar: Bink (4 clips) is DoFrame ~110 ms + Copy ~30 ms per second (~14 %), the main thread sits in
  the game's Sleep(0) limiter loop, ~44 ms per presented frame, no 2D blit (the menu is GPU-rendered, 13 draws/frame). Menu
  screen switches have 0.4-1.4 s frames (asset loads). No hardware menu timing exists yet: `gthread cpu share` (every 4 s) is in
  the log for that.
* Cinema bar: rows 0-2 are now forced black when rows 3-4 are solid black (pal_gpu_3ds.c).

### 46e. Menu speed and boot (session after 46d)
Measured in Azahar with `menu.txt` (menu = GPU-rendered 800x480 target; the game Locks the whole back buffer every frame):
* Per menu frame the Lock readback (GPU target -> guest) and the following upload (guest -> GPU target) were each a CPU
  transpose of 384 000 px (6.5 ms each) plus a row copy (2.4 ms each). Now one fused pass in 2x2 blocks of 32-bit words
  (`px_to_rot` / `rot_to_px`, pal_gpu_3ds.c): 4.3 ms each, no row copy. Menu frame 44 -> ~36 ms (limiter cap is 33.3 ms).
* Boot: `obi.ini` writes that do not change the file are skipped (each was an SD open/truncate, 50-100 ms, ~12 per start);
  directory listings of the read-only game data are cached after the first scan (gobs, music scanned twice each).
  Logo movie opens at 3.3 s instead of 4.5 s.
* `menu.txt` now makes every `.bil` and non-`ss` `.bik` open fail, so no intro movie runs: menu visible ~4.5 s after the
  game starts (was 6.6 s). `g_menu_reached` stops the RETURN spam when the menu's own clips open.
* Not measured on hardware. Bink for the four menu tiles is only ~14 % of the menu frame.

### 46f. Menu: the sampler found the real hotspot (a native RLE sprite blitter)
Sampler (`prof.txt`, -DCPU_FN_TRACE build) in the menu: 22 % of the samples in guest `0x4616cc`, a software blitter for
run-length-compressed 16-bit sprites that draws the menu's background and tiles into the locked back buffer. Native
transcription `source/core/native_4616cc.c` (fully-on-screen case; clipped or malformed input runs the lifted `_ref`, which is
idempotent). Added to `OVERRIDES`/`ALWAYS_REF` in lift.py (regenerate `source/gen`). Verified pixel-identical: menu screenshots at
frames 80 and 160 with `fakeclock.txt`+`nolimit.txt`, natives on vs `nonative.txt`, are byte-equal. Menu frame, Azahar: 44 ms
(start of 46e) -> 36 ms (transposes) -> 28 ms unlimited, i.e. the game's 30 fps limiter (33.3 ms) is now the cap.

## 46g. Bottom-screen interface (ui.c, cheats.c)

The bottom screen is a 320x240 touch panel (replaces the text console while the game runs): tabs Game (master volume, screen
brightness through GSPLCD, C-Stick camera speed, FPS counter, hide), Cheats and Saves. Hidden mode leaves the screen black with
a faint dot in the bottom-right corner that restores it. State persists in `/3ds/tpm/ui.cfg`. The panel is drawn into a shadow
buffer (anti-aliased Aileron font from `tools/gen_font.py`) and copied to the double-buffered framebuffer before each swap.

Cheats poke the same guest variables as the game's own console (`FUN_0042fc90`): player stats at 0x86d57c (health, ammo[weapon]
at +0x10+4*w), slow motion 0x882288, perfect aim 0x88228c, character switch `FUN_004302aa(n)` through guest_callback. God mode
keeps health at 100, infinite ammo tops up weapons already held (both verified in Azahar gameplay: ammo 0 -> 500, health held).
**Ghost** = flight + noclip, verified in Azahar (flew 40+ units through walls, no snap-back, camera follows):
the movement keys are hidden from the game (`g_input_block_move`, core/input.c) and the player position is driven from
the D-pad relative to the camera yaw. The game keeps four copies of the position (obj+0x18/+0x54 at the 3D object
0xe1a3ac = root(0x6cf640)+0xc, and root+0x118/+0x124); a one-time scan for the float triple finds them. The game's update
clamps the player back into its walkable area, so the copies are written again in `r3d_begin_scene` (after the logic,
before drawing). The camera's follow target (0x5bb4c8 / 0x5bb4d8) is fed the same position so the game's own easing
follows it. Leaving the level used to kill the player and reload it: `FUN_004500b0` (the death routine, called with the
kind) is a native (`native_4500b0.c`) that returns at once while God or Ghost is on. Beyond the level geometry the world
is not drawn (black), which is the game's visibility culling.
Saves tab: the game saves only through its pause menu (slot request `FUN_0045eea9(slot, name)`, executed by the menu loop
with a thumbnail it captures there), so the tab lists the Zanzi slots and has a button that taps Escape; no direct save/load.

Test hooks (scenario.txt): `at N touch X Y`, `at N touch_up`, `at N uishot file.bmp` (bottom screen is not in `screenshot`).

### 46h. Direct load/save, Ghost, HOME, audio, picture position (hardware-verified by the user)
- **Load / Quick save** (cheats.c `cheats_load_slot`, `cheats_can_load`): load follows the pause menu's own sequence (flags 0x6ccfe0/0x6ccfd8, message 8 via FUN_0046f4a9, loader FUN_00451936(slot), broadcast 0x18, message 9). The Saves tab lists valid ZanziNN.sav files (header "ZANZIBAR", u32@8 = 0x1a8) with the name at +0x20, newest first.
- **Ghost**: forward/right come from the last rendered view matrix; B = up, ZR = down; `native_418544.c` overrides updateCam (FUN_00418544) so the camera target follows the ghost position.
- **Brightness** uses `GSPLCD_SetBrightnessRaw`, with gsp::Lcd opened only for the call. Holding that session open for the whole run is what froze HOME (suspend never completed); with it closed HOME suspends and resumes normally.
- **HOME diagnosis lessons**: `svctrace.c` (linked with `-Wl,--wrap` on the svc stubs) logs every kernel wait/IPC during a suspend, but writing the log to the SD card mid-suspend itself stalls the suspend and the traced IPC overwrote the command buffer (crashing the gsp module). It is opt-in only (`sdmc:/3ds/tpm/trace.txt`). Other switch files in `sdmc:/3ds/tpm/`: `noui.txt`, `nohook.txt`, `nohome.txt`.
- **Audio**: file calls that stall the guest thread (DeleteFileA, FindFirstFileA, ...) call `mixer_service_io()` first, and the DSP is kept further ahead after long gaps (fixes the load-screen crackle).
- **Picture position on the real LCD**: the top framebuffer's raw output is one element late in the column-major stream (each column's top row held the next column's bottom row: a stray line along the top, picture one row low). `pal_gpu_present_direct` re-aligns the stream by one element with aligned 32-bit word moves (a halfword memmove is unaligned and crashes the console), then applies the user nudge `g_scr_dx/dy` (Game tab, saved in ui.cfg); calibrated on the console: X -1, Y -1 is the default. Azahar does not show this offset.
- Test helper: `tools/azahar_test.sh [max_seconds]` copies the 3dsx, runs Azahar and closes it when the log shows `exit:`.
