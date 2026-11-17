# Performance plan: 30–60 fps on 3DS without clock tricks (2026-11-13)

Goal from the user: **30–60 fps at the normal CPU clock, ideally 30 fps on Old 3DS**,
with the same image quality. Azahar's CPU clock override (300 %) is ruled out.
This file is the work order for the next agent. Read it fully before starting.
Step 1 (native `world_cull`) is specified in detail; the later steps reuse its method.

---------------------------------------------------------------------------------------

## 0. Where we stand

Level 1 in Azahar (100 % clock), `tools/azbench.ps1` window scene 1200–1500:

| Build                                         | ms/frame | fps  |
|-----------------------------------------------|----------|------|
| before this session                           | ~101     | ~10  |
| + native iMUSE decoder (`native_imuse.c`)     | ~100     | ~10  |
| + masked guest memory (`TPM_GMEM_MASKED`)     | ~73      | 13.6 |
| + step 0a: no `CPU_FN_TRACE` in 3DS build     | ~71      | 14.1 |
| + step 1 phase A: native `world_cull` (calls callees via guest stack wrappers; `source/core/native_world.c`, shadow-verified on host, 2151 calls 0 diffs) | **~68**  | 14.7 |

Step 1 status (2026-11-13): phase A done and verified. Not done: phase B (direct
C cores for 0x47e34d/0x47dad6/0x47ceb9/0x41af30/0x4075c6/0x4074d4 instead of
guest-stack calls), world_cull timer on 3DS (≤ 4 ms target), and branch coverage
for lighting (`0x89fe34`), depth bias, dark, cache-full, water, and node/texture
range kills (all 0 hits in level 1; need a scene or synthetic test).

Target: 33 ms/frame (30 fps). That needs about **−40 ms**.

Inclusive function timers (build `make TPM_DEFS=-DTPM_FN_TIMERS`, see §1.3), masked build,
300 frames in level 1 (`bench/masked_log.txt` has the full log):

```
frame          wmain_0043e9f2   73.71 ms   (whole guest frame)
render         wmain_0043f57a   30.34
  world_cull   wmain_004199b0   12.49      <- step 1
  world_emit   wmain_00406830   10.81      <- step 2
  skeleton     wmain_00404730    6.37      <- step 4
render_tail    wmain_00484901    2.99
obj_update     wmain_00411028   20.52
  obj_17930    wmain_00417930   12.73  (40 calls)   <- step 3
  actor_xform  wmain_0040f1e0    7.42  (52 calls)
  actor_emit   wmain_00402155    6.21  (6 calls)
  actor_light  wmain_0040fc10    2.42  (51 calls)
  obj_3a5ff    wmain_0043a5ff    2.13
ai_22130       wmain_00422130    1.74
anim_0a7d1     wmain_0040a7d1    2.44
imuse_decode   imuse_00921c70    1.05
```
The indented children under obj_update overlap each other (their sum is more than the
parent), so some of them call each other. Check the call tree before you rely on the
numbers. About **15 ms of the frame is not covered by any timer** (HLE: present/flip,
DrawPrimitive, other game code). Step 0 finds out where it goes.

### Measurement caveat (important, do not re-litigate)
- Azahar does not implement `ConfigureNew3DSCPU` (its log shows "unimplemented function"),
  so at 100 % it runs at 268 MHz with no L2 cache. That is Old-3DS-like CPU speed.
  Azahar is also not cycle-accurate. A real New 3DS in its normal 804 MHz mode
  (`osSetSpeedupEnable`, already called in `main.c`) will very likely be 2–3x faster
  than these numbers. Treat Azahar 100 % as the **Old 3DS proxy**. The user decides
  how to judge it. If real hardware is available, measure there too.
- Always compare like with like: same scenario, same window (scene 1200 and 1500 lines),
  normal build (no timers) for fps, timer build only for breakdowns. The timer build costs
  about 1 %.

---------------------------------------------------------------------------------------

## 1. Tools that exist now

### 1.1 Builds
- 3DS: `MSYSTEM=MSYS /c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd "/g/Media/RecompsDecomps/Star Wars Phantom Menace" && make -j12'`
  (about 2 min full). The Makefile has `-DTPM_GMEM_MASKED` (3DS only) and `$(TPM_DEFS)`
  for diagnostic defines. **After changing TPM_DEFS do `rm -rf build`.**
- Host (PowerShell): `$env:HOSTMAKE_TMP="$env:LOCALAPPDATA\Temp"; & .\tools\hostmake.ps1 build-host/tpm_host.exe`.
  Host tests: `test_override`, `test_imuse` (and others in `Makefile.host` TESTS).
- Regenerate lifted code after any `tools/lifter` change:
  `rm -rf source/gen build-host/source/gen && python -I tools/lifter/lift_all.py game_files/GAMEDATA source/gen`

### 1.2 Benchmark
`powershell -File tools/azbench.ps1 -Frames 2000 -Label <name>` copies `tpm3ds.3dsx` to
the Azahar SD, runs menu → level 1, kills Azahar and prints ms/frame per 300 scenes.
It also restores `scenario.txt` and copies the log to `bench/<name>_log.txt`.
**Never take screenshots of the user's desktop.** To check the screen output, simulate it
offline (see PLAN.md "Graphics fix 1").

### 1.3 Function timers
`FN_TIMERS` in `tools/lifter/lift.py` lists (prefix, address, label) entries. With
`-DTPM_FN_TIMERS` each listed lifted function gets a wrapper. Every 300 frames
`core/prof.c` logs ms/frame and calls/frame. Index 0 must stay the frame function
0x43e9f2. The labels file `source/gen/fn_timers_tab.h` is generated. Overridden
(native) functions are skipped by the generator. To time a native function, add
`fn_timer_begin/fn_timer_end` in its C code by hand, or time its caller.

### 1.4 Instruction counter (host)
`tools/icount/` counts executed x86 instructions per lifted function (README in the
Makefile header). Use it for call counts and to see which instructions inside a function
are hot. `python -I tools/icount/report.py 40 -i`.

---------------------------------------------------------------------------------------

## 2. Roadmap (in order) — superseded by §7 Plan v2

| Step | What | Expected win (Azahar ms) | Risk |
|------|------|------|------|
| 0 | Timer coverage for the 15 ms gap, quick global flags | 0–5 | low |
| 1 | Native `world_cull` 0x4199b0 (§3) | −8 … −9 | medium |
| 2 | Native `world_emit` 0x406830 (+ its hot callee 0x41b070) | −6 … −8 | medium |
| 3 | Native `obj_17930` 0x417930 / `actor_xform` 0x40f1e0 / `actor_light` 0x40fc10 | −8 … −12 | medium |
| 4 | Native `skeleton` 0x404730 (+0x405cf0), `actor_emit` 0x402155 | −5 … −7 | medium |
| 5 | Renderer / HLE (r3d fetch + GPU submit) inside the gap from step 0 | −3 … −6 | low |
| 6 | Old 3DS: memory budget (§6) | – | high |

Steps 1–4 together should bring Azahar to about 35–40 ms. Step 5 and the step-0 findings
decide whether 33 ms is reached. Re-measure after every step and update §0 and PLAN.md.

### Step 0 details (do first, it is small)
1. Add timers for the remaining frame children. Find them in the lifted body of 0x43e9f2
   (`grep "call 0x" `) and in 0x43f57a. Add HLE-side timing for
   `DrawPrimitive`/`Flip`/`Present` using `pal_time_us` in `hle_d3d.c`/`hle_ddraw.c`.
   Log it together with the fn timers.
2. Try cheap global flags, one at a time, and keep what wins:
   - Remove `-DCPU_FN_TRACE` from the 3DS CFLAGS. It writes a ring entry on every
     lifted function entry. Keep it in the host build.
   - `-O3` vs `-O2` for `source/gen` only (watch code size and I-cache: ARM11 has 16 KB
     I-cache, so bigger code can be slower).
   - Thumb for gen code is probably not worth it (ARM11 Thumb-1 has no VFP access).

---------------------------------------------------------------------------------------

## 3. Step 1 — native `world_cull` (0x004199b0)

### 3.1 What it does
It is called once per frame from `render` 0x43f57a (call site 0x43f5ac, the only caller):
`world_cull(world = [0x8a0060], [0x6f83e4]+8, 0)`. Only arg 1 is used; it re-reads the
view matrix from `[0x6f83e4]+8` itself. Right before it, 0x419800 resets the vertex
cache counter `[0x5bb5c0] = 0`. Right after it come 0x4848f2 and then
`world_emit` 0x406830, which reads what `world_cull` wrote.

For every polygon in the visible bucket lists it:
1. kills the polygon (sets `node->poly = 0`) if it faces away, is hidden, or has an
   invalid texture;
2. picks the front or back texture;
3. transforms, projects and lights each vertex **once per frame** into the vertex cache
   at 0x5bf9f0 (64-byte slots, max 0x4000);
4. stores the clip-code OR in the list node.

Full listing: `docs/asm/004199b0_world_cull.txt` (674 instructions, 0x4199b0–0x41a34a).
Lifted reference: `source/gen/wmain_gen_004.c`, `void wmain_004199b0(cpu_t *cg)`.

### 3.2 Data (names are ours; verify offsets against the listing)
**World `w`** (arg 1, kept in `esi` / `[esp+0xfc]`):
`+0x50` u32 time (ms), `+0x54` float time, `+0x60` (passed to 0x4075c6),
`+0x9c` vertex array (32-byte `Vtx`), `+0xa0` float per-vertex phase array,
`+0xb0` int texture count, `+0xb8` texture table (40-byte entries, `+0x24` flags bit 8 = animated),
`+0xbc` int last bucket index, `+0x624 + 4*i` object pointers.

**Bucket lists**: `[0x8bc880 + 4*b]` for `b = 0..w+0xbc` (inclusive, signed compare;
skipped if `w+0xbc < 0`). Node: `+0` poly ptr (0 = killed), `+4` next, `+8` clip-OR (u32).

**Poly `p`**: `+0,+2,+4,+6` u16 vertex indices, `+8` i16 current texture,
`+0xa` u8 darkening amount, `+0xf` flags (bit 0x40 = texture animated),
`+0x10/14/18` float plane normal, `+0x20` i16 front texture, `+0x22` u16 back texture,
`+0x25` u8 vertex count (3/4; <3 → kill), `+0x26` u8 (used by 0x41a350),
`+0x27` bit 0x80 (single-sided test only if render flag 2), `+0x28` flags
(4 = belongs to a moving object, 0x10 = hidden → kill), `+0x29` u8 sub-object,
`+0x2a` u8 object index, `+0x2b` u8 (0 → clear light-mode bit 4), `+0x2c` u16 render
flags, `+0x2e` u8 light mode, `+0x36` i8 vertical offset (×1/64).

**Vtx** (32 bytes): `+0,+4,+8` float xyz, `+0xd` u8 "in cache this frame", `+0xe` u16 cache index.

**Cache slot** (`0x5bf9f0 + 64*i`): `+0` u8 clip code, `+1` u8 "colour done",
`+2` u16 world vertex index, `+4` u32 ARGB, `+7` u8 fog/darkening alpha (overlaps the A byte of +4),
`+0x18/+0x1c` floats (written by 0x41af30, uv/screen), `+0x20` float |N·L|.
Everything else in the slot is written by the native 0x41af30.

**Globals touched** (all must end up identical):
| addr | meaning |
|------|---------|
| 0x5bb610 | current object index (init 0xff) |
| 0x5bb57c | current object active (0/1) |
| 0x5bb5a8 | current object pointer |
| 0x6bf9f0 | current sub-object index (0xff = none) |
| 0x5bf9e4 | sub-object record ptr = obj + sub*0x9c + 0x84 (pivot at +8..+0x10, matrix at +0x14) |
| 0x5bb5c8 | object→camera matrix (0x47dad6 output) |
| 0x5bb548 | inverse sub-object matrix (0x47ceb9) |
| 0x6bf9f8 | camera position copy from 0x868704..0c |
| 0x5bb588 | camera position in object space (0x47e34d) |
| 0x5bb598/59c/5a0 | scratch vector (written as floats, also by callees) |
| 0x6bfa08/0c/10 | transformed vertex (camera space) |
| 0x5bb5c0 | cache count (≥0x4000 → **return immediately**, mid-polygon) |
| 0x5bf9e8 | depth bias, temporarily changed and restored when render flag 4 is set |
| 0x6bfa14 | set to 1 at entry |
| 0x8a0064 | counter of polygons culled by clip codes |
| read only | 0x89fe30/44/48 (render toggles → mask), 0x5bb594 (OR'd into flags), 0x89fe34 (lighting on), 0x8a011c (camera object, pos at +0x24), 0x868704..0c, 0x868710, 0x868718, 0x6f83e4 |

Constants in .rdata: 0x4a81a8 = 0.0, 0x4a81b4 = 0.5, 0x4a81bc = 255.0, 0x4a81c0 = −1.0,
0x4a81c4 = 1/64, 0x4a81c8 = 240.0, 0x4a81cc = −0.25, 0x4a81d0 = 180.0. Read them
from guest memory (`LFRD32`) instead of hard-coding, like the lifted code does.

### 3.3 Pseudocode (derived from the listing; the listing is the authority)
```
mask = ~( ([0x89fe30]==0 ? 4 : 0) | ([0x89fe44]==0 ? 1 : 0) | ([0x89fe48]==0 ? 0x800 : 0) )
viewm[12] = copy of 12 dwords at [0x6f83e4]+8          (local, esp+0xc8)
[0x5bb610]=0xff; [0x5bb57c]=0; [0x6bfa14]=1
for b in 0..w.lastBucket:
  for node in list[b]:
    p = node.poly; n = p.nverts
    if n < 3: kill
    pf = (p.flags2c | [0x5bb594]) & mask
    lm = p.lightmode; if p.b2b == 0: lm &= ~4
    if p.f28 & 4:  (object polygon)
       if p.obj != [0x5bb610]: select object (globals above); [0x6bf9f0]=0xff; active = obj && *obj
       if active && p.sub != [0x6bf9f0]:
          call 0x409170(obj, w.ftime)                    (lifted, animation)
          [0x6bf9f0]=p.sub; sub = obj + p.sub*0x9c + 0x84; [0x5bf9e4]=sub
          0x47dad6(0x5bb5c8, view, sub+0x14); 0x47ceb9(0x5bb548, sub+0x14)
          [0x6bf9f8..] = [0x868704..]; 0x47e34d(0x5bb588, 0x6bf9f8, 0x5bb548)
    if p.f28 & 0x10: kill
    vp[k] = w.vtx + 32*p.idx[k]
    bb = pf & 8                                           (billboard / sprite poly)
    side = -1.0                                           (x87 value; "front")
    if !bb && (!(p.f27 & 0x80) || (pf & 2)):
       d = vp[0].xyz - camera   (object: f32(f32(v - [0x5bb588]) - sub.pivot); else v - [0x8a011c]+0x24)
       stored to 0x5bb598.. as floats, each component rounded on store
       side = (nx*dx + ny*dy) + nz*dz                     (double chain, see 3.4)
       if !(side < 0) && !(pf & 2): kill
    tex = (side <= 0 or unordered) ? (int16)p.texFront : (uint16)p.texBack
    if tex < 0 || tex >= w.texCount: kill
    animTex = 0
    if p.curTex != tex: animTex = w.tex[tex].flags & 8; p.f0f = animTex ? p.f0f|0x40 : p.f0f&~0x40
    p.curTex = tex
    if pf & 0x30: call 0x41ae50(w, p)                     (lifted, texture frame)
    if bb: call 0x41a350(w, p, &vp, &quad[0])             (lifted, 4 corners x 3 floats)
    yoff = p.yoff ? f32((double)(int8)p.yoff * (1/64)) : 0
    andc = 0x3f; orc = 0
    for k in 0..n-1:
      v = vp[k]
      if !v.cached:
        i = [0x5bb5c0]; s = slot(i)
        if bb: [0x6bfa08..] = quad[k]
        elif active && p.f28&4: [0x5bb598..] = f32(v - pivot); 0x47e34d(0x6bfa08, 0x5bb598, 0x5bb5c8)
        else: 0x47e34d(0x6bfa08, &v.xyz(copy), viewm)
        if lm & 3: call 0x428615(v, 0x6bfa08, p)          (lifted, water wave, uses sin)
        if pf & 4:  (depth-biased projection, keep uv of the unbiased one)
           save = [0x5bf9e8]; 0x41af30(0x6bfa08, s); u = s.18; t = s.1c
           [0x5bf9e8] = f32(call 0x427c9a(v, cam+0x24) + [0x5bf9e8])   (returns st0)
           0x41af30(0x6bfa08, s); [0x5bf9e8] = save; s.18 = u; s.1c = t
        else: [0x6bfa0c] = f32(yoff + [0x6bfa0c]); 0x41af30(0x6bfa08, s)
        s.done = 0; s.argb = 0xff000000
        if [0x89fe34] && (lm & 4):
           d = f32(v - camera)  → 0x5bb598..
           dot = (nx*dx+ny*dy)+nz*dz; len = sqrt((dx*dx+dy*dy)+dz*dz); r = dot/len   (double)
           s.f20 = f32(r < 0 or NaN ? -r : r)
           if p.dark:
              a = sin( f32((phase[idx_k] + [0x868718]) * 240.0) )   (call 0x494272, CRT sin)
              x = ((a - (-1.0)) * 0.5) * (double)p.dark; clamp to [0,255] (0 if <0; 255 if >255)
              s.b7 = (u8) __ftol(x)                       (0x49a44c, truncation)
        v.cached = 1; v.cacheIdx = i; s.vidx = p.idx[k]
        if ++[0x5bb5c0] >= 0x4000: RETURN (whole function)
      else: i = v.cacheIdx; s = slot(i)
      andc &= s.clip; orc |= s.clip; ci[k] = i
    if andc: [0x8a0064]++; kill
    if bb && (orc & 0x80): kill
    node.clipOr = orc
    if animTex: continue (keep, no colour pass)
    for k: if !slot(ci[k]).done: 0x4075c6(vp[k], tmp4, w+0x60); slot.argb |= 0x4074d4(tmp4) & 0xffffff
    if pf & 0x800:
      for k: if !slot.done:
        dk = __ftol( ((sin(f32((phase[idx]+[0x868718]-(-0.25))*180.0)) - (-1.0)) * 0.5) * (double)p.dark )
        r,g,b of slot.argb each = max(0, c - dk) (signed 32-bit test); A unchanged
    for k: slot(ci[k]).done = 1
  kill = node.poly = 0, then next node
```
Watch for these, all visible in the listing:
- With `bb`, or with the single-sided skip path, the x87 value is the −1.0 loaded at
  0x419c07, so the **front** texture is used. When `side == 0` it is also the front
  texture (the test is `ah & 0x41`).
- `node.clipOr` is written as `orc & 0xff` (u32 store).
- The colour pass reads `slot.done` **before** the final loop sets it, so two polygons that
  share a vertex colour it only once.
- The cache-full return leaves the current polygon half done. Reproduce it exactly.

### 3.4 Floating-point rules (needed for bit-exact results)
The lifter models x87 as C `double`. Loads go float→double, every `fstp dword` rounds to
float (`LFWR32`). Values that stay on the x87 stack across instructions stay `double`.
So:
- A single `+ − × ÷ sqrt` of two floats whose result is stored straight to a float gives
  the same bits whether done in `float` or in `double`-then-round. **Use `float` there; it is
  faster on the ARM11 VFP.**
- Chains that stay on the stack (the dot products, `dot/len`, the sin argument, the
  darkening expression) must be computed in `double` in the same order as the listing.
  The order of `faddp` operands matters for NaN only; keep the listing's order anyway.
- `sin`: the lifter lowers `fsin` to C `sin()` (`lift.py:782`). The CRT path
  0x494272 → 0x49c7f0 → 0x49d5e8 / 0x49c80d also checks for NaN/inf and may call the
  CRT error handler (writes CRT globals). Call native `sin()` only for finite arguments
  and fall back to the lifted 0x494272 otherwise. Or always call the lifted one first,
  then optimise once the shadow check (§3.6) passes.
- `__ftol` (0x49a44c): use the same helper or expression the lifter uses for `fistp`
  with truncation. Out of range or NaN gives 0x80000000; the later `sub/jns` relies on that.
- Copies done with `fld dword`/`fstp dword` (the uv save/restore) are bit copies except for
  signalling NaNs. Do them as `double` round trips like the lifted code, not as `memcpy`.

### 3.5 Implementation
1. New file `source/core/native_world.c`. Register 0x004199b0 in `OVERRIDES`
   (`tools/lifter/lift.py`) and regenerate. The lifted body then becomes
   `wmain_004199b0_ref` (host only, `TPM_OVERRIDE_REF`). Remove 0x004199b0 from
   `FN_TIMERS` (overrides are skipped) and time it inside the native code with
   `fn_timer_begin/end`, using the same label index if you keep the table order.
2. Calling convention at the boundary: on return, ESP = entry ESP + 4. EBX/ESI/EDI/EBP
   are restored and the x87 stack is balanced (empty on entry and exit). At 0x43f5b1 the
   caller only does `add esp, 0xc`, then calls 0x4848f2 and 0x406830 with no arguments,
   so EAX/ECX/EDX/eflags are **dead**. Set them to something defined anyway
   (e.g. the values the original leaves on the common exit path, if cheap) and document
   the choice.
3. Native callee cores. Split the bodies of the already-native 0x47e34d, 0x47dad6,
   0x47ceb9, 0x41af30, 0x4075c6 and 0x4074d4 in `native_overrides.c` into
   `static inline` (or header `native_math.h`) functions that take guest addresses and
   do only the memory effects. The `cpu_t` wrappers keep their exact register and flag
   behaviour for `test_override`. Call the cores directly from `world_cull`: no guest
   stack, no `cpu_t` sync. **This is where most of the speed comes from.**
   The cores must not change behaviour, so `test_override` must still pass.
4. Rare callees (0x409170, 0x41ae50, 0x41a350, 0x428615, 0x427c9a, and 0x494272 for
   non-finite input) stay lifted. Write a small helper that sets ESP, writes the args and
   a return address (use the original return address after the call instruction,
   e.g. 0x419b31), calls `wmain_xxx(c)` and pops. For 0x427c9a and 0x494272, take the
   result from `st[ftop]` and pop the x87 stack, exactly like the lifted call sequence in
   `wmain_gen_004.c` around 0x41a05d. Before calling, reserve the original frame
   (body ESP = entry ESP − 0xf8: `sub esp,0xe8` plus 4 pushes). Locals then sit at the
   listing's offsets, e.g. vp array at body ESP+0x68 and quad at +0x98 (both passed to
   0x41a350), so the pointers passed to lifted callees are real guest memory.
5. Keep all guest-visible stores in the original order wherever a callee could read them
   (e.g. the 0x5bb598 scratch vector before calling 0x47e34d). Elsewhere, values can live
   in C locals and be stored once.
6. Do not cache guest pointers across lifted calls. Lifted callees can write anywhere.

### 3.6 Verification (mandatory before benchmarking)
**A. Shadow check during a real run (host).** This is the main test because it uses real
data and covers every path the level takes. In `native_world.c` under `#ifdef TPM_OVERRIDE_REF`,
when env `TPM_VERIFY_WC=1`:
1. at entry, copy the whole arena (`g_gmem`, `g_gmem_size`) and the `cpu_t` aside;
2. run native, save the result arena and `cpu_t`;
3. restore and run `wmain_004199b0_ref`;
4. compare the arenas. Ignore bytes below the entry ESP (dead stack), compare
   EBX/ESI/EDI/EBP/ESP, `ftop`, `fsw` and `fcw`;
5. log the first 20 differing addresses with old/new bytes (map them to §3.2 names),
   and count frames checked and frames that differed;
6. continue from the **reference** result so the run stays on the reference path.

Run `TPM_VERIFY_WC=1 TPM_NOLIMIT=1 build-host\tpm_host.exe game_files/GAMEDATA 0 0 tests/scenarios/level1_bench.txt`
and other scenarios that reach object polygons (`p.f28 & 4`), billboards (`pf & 8`),
lighting (`[0x89fe34]`), darkening (`p.dark`, `pf & 0x800`) and depth bias (`pf & 4`).
Add counters per branch and print them at exit, so you can **prove every branch ran at
least once**. If a branch never runs in level 1, find a scene that has it (list the
scenarios in `tests/scenarios/`) or write a synthetic case for that branch.
Acceptance: 0 differing frames over all checked frames, every branch counter > 0.

**B. End-to-end.** Host scenario screenshots (BMP) must be byte-identical between the
build without the override (comment it out of `OVERRIDES`) and the build with it.
`test_override` and `test_imuse` must still pass.

**C. 3DS.** `azbench` (normal build) for fps, and a timer build for the new
`world_cull` time. Target: ≤ 4 ms in Azahar (was 12.5).

### 3.7 After it works (optional, second pass)
- With the shadow check green, try `float` instead of `double` in the remaining chains.
  This is **no longer bit-exact**. Only do it if the gain is measurable, compare
  screenshots visually, and keep a `#define` to switch back. The results only feed
  rendering (vertex cache), not the simulation, so small differences cannot change
  gameplay. The texture choice and the cull decision do depend on the sign of `side`, so
  keep that dot product in `double`.

---------------------------------------------------------------------------------------

## 4. Steps 2–4 (same method)

For each function: listing (`grep -o "/\* [0-9a-f]\{8\}: .* \*/"` over its lifted body,
save it in `docs/asm/`), Ghidra decompile as a reading aid (the listing is the authority),
data/global table, pseudocode, native rewrite calling native cores directly, then a shadow
check (generalise the §3.6 harness into `core/shadow.c` with an address → (native, ref)
table so each new override only adds an entry), then screenshots, then azbench.

- **Step 2 `world_emit` 0x406830** reads the cache slots and node lists written by
  world_cull and emits D3D triangles. Its callee 0x41b070 was 8.7 % of all instructions
  in the icount profile, so inspect it first. With world_cull and world_emit both native,
  consider merging them later (no cache slots in guest memory). That is a larger change;
  only do it after both are verified.
- **Step 3 objects**: 0x417930 (40 calls/frame), 0x40f1e0 (52), 0x40fc10 (51). Small
  per-object functions; called often, so the guest call overhead matters. Confirm the call
  tree first (timers overlap).
- **Step 4 `skeleton` 0x404730** (+0x405cf0) and `actor_emit` 0x402155: bone matrices,
  mostly the already-native matrix helpers.

---------------------------------------------------------------------------------------

## 5. Rules for whoever does this
- Exactness first: every override is checked against the lifted reference. No
  "looks right" rewrites. Quality must not drop. Compare screenshots.
- Do not take screenshots of the user's desktop (privacy). Simulate screen output offline.
- Another agent (DeepSeek) may work in the same tree. Check file mtimes before editing
  shared files (`native_overrides.c`, `lift.py`, `PLAN.md`).
- Reports to the user: short. Commit only when asked (the tree is not a git repo anyway).
- After each step: update §0 here and add an entry in `docs/PLAN.md` §3.

---------------------------------------------------------------------------------------

## 6. Old 3DS notes (step 6, not started)
- CPU: 268 MHz, 2 cores usable (core 1 is limited for apps), no L2. Azahar 100 % is
  roughly this speed, so the table in §0 is roughly the Old 3DS number.
- Memory is the blocker: Old 3DS apps get 64 MB (80 MB with the extended memory mode in
  a `.cia`). The current layout uses a 64 MB guest arena plus a 32 MB linear heap.
  `TPM_GMEM_MASKED` needs a power-of-two arena. Level 1 needs about 21 MB guest heap
  + ~10 MB VirtualAlloc + ~5 MB image, so 32 MB is too tight unless VirtualAlloc
  reservations shrink. Options: measure the real peak per level, shrink the VirtualAlloc
  reservation, use an 80 MB `.cia` with a smaller linear heap. Decide after steps 1–5.
- The second core: guest audio and streaming threads could run there (PLAN.md "Speed
  work 2" next steps). iMUSE is now only ~1 ms, so this is a smaller win than it used to be.

### Status after phase B (2026-11-13)
- Azahar level 1 (window 1200-1500): 65.5 / 66.6 ms/frame (was 70.9 / 71.1 before step 0+1).
- world_cull timer: 8.8 ms (target <= 4). Remaining guest-stack calls in it: 0x4075c6, lifted sin for darkening, 0x427c9a, 0x41ae50, 0x41a350, 0x428615, 0x409170. Next: 0x4075c6 core, then find the rest of the per-vertex cost (the copy/guard code, gmem accessors).
- Shadow verify: not fully deterministic when the sub-switch runs (0x409170 reads host time). One diff in 2151 calls, sub-switch class; all other calls identical.
- 0x4075c6 core added (wc_core_mat): Azahar level 1 1200-1500 = 65.1 / 66.0 ms. Shadow check: 2151 calls, 1 diff (sub-switch class, time-dependent callee). Remaining guest-stack calls in world_cull: lifted sin (darkening only), 0x427c9a, 0x41ae50, 0x41a350, 0x428615, 0x409170.

---------------------------------------------------------------------------------------

## 7. Plan v2 (2026-11-13, replaces the order in §2)

Lesson from steps 0–1: one hand-written native function took a long time and gave
~3.7 ms (world_cull 12.5 → 8.8 ms timer, frame 68 → 65 ms). At that rate 33 ms takes
10+ such functions. So the order changes: **global wins first** (they apply to all ~70 %
guest time), hand-written natives last and only for what is still hot.

Observed in the lifted code: every guest call (8770 call sites) copies 8 GPRs, eflags,
fsw, fcw, ftop and 2 x87 slots into `cg` and back, and every function copies them in on
entry, even though 1520 of 2084 functions never touch x87 and most only use a few registers.

| Phase | What | Expected (Azahar ms) | Risk |
|-------|------|------|------|
| V | Golden-run verifier: host, fixed scenario, fake clock (fixes 0x409170 nondeterminism), hash of guest memory + draw stream per frame. Every global change must reproduce the hashes for N frames. | 0 (enabler) | low |
| A1 | Measure call overhead: guest calls/frame (icount), cost of the marshalling. | 0 | low |
| A2 | Lean call ABI in `lift.py`: per-function read/write sets (transitive, fixpoint over the call graph), x87 use flag; marshal only live state. Indirect calls keep the full copy. | −5 … −10 | medium |
| A3 | CRT math natives (sin/cos chain 0x494272/0x49c75d/0x49c80d, fldcw helper 0x49d575, ftol): first find the `c0000090` at 0x49da81 (likely fsw exception bits or fcw state the native leaves different), then re-apply. ~13 % of samples. | −5 … −8 | medium |
| A4 | Compiler side: `-O3`/LTO for gen code, hot-function ordering (I-cache 16 KB), try gen-time inlining of small hot leaves (0x41ae50 etc.). Keep only measured wins. | −2 … −5 | low |
| B | Renderer: merge consecutive DrawPrimitive calls with identical state/texture into one GPU draw, skip redundant render-state/texture binds, cheaper per-draw setup (1199 draws × ~13 µs ≈ 15.6 ms now). | −5 … −8 | low/medium |
| C | Second core: GPU submit / r3d fetch (or audio mss32+iMUSE) on core 1 with a command queue; raise core-1 limit with `APT_SetAppCpuTimeLimit`. Output must stay identical (only the thread changes). | −4 … −10 | medium/high |
| D | Remaining hot functions as natives with the §3 method (0x406300/0x405cf0 skeleton, 0x417930, 0x40f1e0), re-profile first. | −5 … −10 | medium |
| E | Old 3DS memory (§6), and a measurement on real New 3DS hardware if available. | – | high |

Order: V → A1 → A2 → A3 → B → A4 → C → D → E. Re-measure (azbench, window 1200–1500)
after each phase, update §0 and PLAN.md. Rough path: 65 → A2 ~57 → A3 ~51 → B ~45 → A4 ~42
→ C ~36 → D ≤ 33. These are estimates, the A1 measurement decides how much A2 is worth.
Note §0 caveat: a real New 3DS (804 MHz + L2) should be well ahead of these Azahar numbers.

### A1 result (2026-11-13, measured)
- Tools: `make TPM_DEFS=-DTPM_CALL_COUNT` counts lifted function entries (`CPU_FN_ENTER`) and logs
  `calls: N lifted function entries/frame` with every r3d scene line. `sdmc:/3ds/tpm/callbench.txt`
  present: `core/callbench.c` runs a call-site micro benchmark at boot (private 64 KB buffer,
  game memory untouched) and logs `callbench` lines. Log: `bench/a1_callcount_log.txt`.
- Calls: level 1 window 1200–1500 = **25,075 / 25,865 lifted entries per frame** (scene 900: 21,482).
  Lower bound for call sites: calls into native overrides do not pass `CPU_FN_ENTER`.
  The counter itself cost ~0.5 ms (65.5/66.6 vs 65.1/66.0).
- Cost per call in Azahar (268 MHz cycles, caller work included, identical in both runs):

  | variant | cycles/call |
  |---|---|
  | empty C call, no marshalling | 24.3 |
  | today, typical site (7 regs + eflags each way) | 59.7 |
  | today, site with x87 state (fsw/fcw/2 slots/ftop) | 86.1 |
  | lean ABI (read set in, write set out) | 39.5 |

  Saving with a lean ABI: 20 cycles (plain site) to 47 cycles (x87 site). Statically 4453 of 8770
  sites carry x87 state, so ~33 cycles on average.
- **A2 estimate: 25,500 calls × 20–47 cycles = 0.5–1.2 M cycles = 1.9–4.5 ms/frame, most likely ~3 ms**
  (≈ 5 % of 65 ms). That is less than the −5…−10 assumed above. The total call overhead, the part a
  lean ABI cannot remove, is ~25,500 × ~40 cycles ≈ 3.8 ms; only inlining or native code removes that.
- Consequence for the order: A2 is still worth doing (global, mechanical, verifiable), but it is no
  longer the biggest item. Do A3 (CRT math, ~13 % of samples) and B (renderer, ~15.6 ms in the
  DrawPrimitive path) with at least equal priority. Cheapest A2 first cut: drop the x87 state
  (fsw/fcw/st slots/ftop) at sites whose callee (transitively) never touches x87; that is the
  47-cycle case.

---------------------------------------------------------------------------------------

## 8. A3 result (2026-11-13)
- Cause of the c0000090 found (details in docs/PLAN.md "Speed work 5"): the CRT sin of a non-finite argument
  goes to `_fpieee_flt` (0x49d820) and raises an SEH exception when the guest FCW unmasks invalid. Finite input
  is safe. Reproducer: `tests/probe_sin.c` (host, lifted reference only).
- Native sin (0x494272) built and checked: finite path bit-exact in the state the game uses (TOP, FSW, FCW,
  st[], ESP/EBP/EBX/ESI/EDI). EAX/ECX/EDX/eflags are not reproduced (caller-saved under cdecl, see PLAN.md).
- Azahar window 1200-1500: 65.2 / 66.2 ms, against 65.1 / 66.0 without it. **No gain, reverted.**
  Lesson: the 13 % CRT sample share of the earlier profile did not turn into frame time. Before any further
  math work, measure the real caller cost first (a timer around 0x494272 in the timer build).
- Status of the work order: A3 done as analysis, not kept. B, A2, A4, C, D not started in this session.

### B measurement (2026-11-13, from bench/prof_draw_log.txt, no code change)
- Batching already exists (`render/r3d_gpu.c`): window 1200-1500 has ~1198 DrawPrimitive calls and only
  ~149 GPU batches per frame. Draws carry ~1.6 triangles each, so the cost is per-draw overhead, not vertices.
- Share of samples: draw+flip together **11.3 %**. Largest items: flip staging->screen blit 4.4 %, vertex
  fetch+transform 1.8 %, target bind 0.6 %, state 0.7 %, texture 0.4 %, cull 0.5 %, batch 0.4 %, GPU flush
  and DrawArrays 1.7 %.
- Consequence: even removing all renderer cost would leave ~58 ms. The guest CPU (lifted code, ~70 % of the
  frame) is what has to shrink. 16.7 ms (60 fps) is not reachable with B, and A3 showed that a single math
  function is not enough either. Decide the target before the next big investment (A2 call ABI with a golden
  verifier, or a different approach to the per-frame guest work).

---------------------------------------------------------------------------------------

## 9. Phase V: golden-run verifier (2026-11-13)
- Host only, `source/pal/host/pal_host.c`:
  - `TPM_FAKE_CLOCK=1`: `pal_time_ms`/`pal_time_us` = 16 ms per presented frame plus a small step per clock
    read (so the clock still moves before the first present; a frame-only clock hung the boot at 0 ms).
  - `TPM_GOLDEN=<file>`: one line per presented frame, `frame N fb=<fnv64 of screen> gm=<fnv64 of all of g_gmem>`.
- Scenario: `tests/scenarios/golden_level1.txt` (menu -> level 1, 1500 presented frames).
- Run: `TPM_FAKE_CLOCK=1 TPM_NOLIMIT=1 TPM_GOLDEN=<file> build-host/tpm_host.exe game_files/GAMEDATA 0 0 tests/scenarios/golden_level1.txt`
- Determinism: two runs of 300 frames gave identical files (600 lines, `cmp` clean).
- Reference for later phases: `build-host/golden_ref1500.txt`, made from the build at the start of phase V
  (= pre-A3 state). Every global change (A2, A4) must reproduce it exactly.

## 10. Phase A2: lean call sites (2026-11-13)
- `tools/lifter/sync_state.py`: `call_facts()` computes, per lifted function, the transitive set of
  registers it mentions (R) and writes (W) and whether x87 is used (fpu), over direct calls (fixpoint).
  Indirect calls, traps, unimplemented instructions and calls to unknown targets are conservative (all).
  Direct call sites now spill only R ∩ caller-written, reload only W ∩ caller-used, and skip x87 sync when
  the callee does not use x87. Tail calls and returns keep the full sync (the caller has no code after them).
  Flags logic unchanged. `lift.py --no-lean` keeps the old full sync.
- Check: host suite green (test_override 61000, test_imuse 20000 ...). Golden run 1500 frames:
  `build-host/golden_lean1500.txt` identical to `build-host/golden_ref1500.txt` (screen + whole arena, every frame).
- Gen diff: only call-site sync lines changed (2445 lines, all `LPUSH` sites).
- Azahar level 1 window 1200-1500 (azbench, normal build): **64.1 / 65.0 ms** (before: 65.1 / 66.0). Gain ~1 ms,
  exact (golden run identical). Side effect: tpm3ds.3dsx 4005260 -> 3896052 bytes (less code).
- Kept. Status: 65 ms -> 64 ms against the 16.7 ms goal of the user, so roughly 4x is still missing.

## 11. Phase A4: compiler flags for the lifted code (2026-11-13)
- `-O3` instead of `-O2` for `source/gen` (3DS Makefile, `wmain_gen_%.o ...`): Azahar window 1200-1500
  **64.0 / 65.1 ms** against 64.1 / 65.0 with `-O2`. No measurable gain, so **reverted** to `-O2`.
  (Earlier the `-O2` choice was never measured against `-O3`; now it is.)
- Not tried: LTO (the gen code is one big unit per file already; expected gain small), hot-function
  ordering (needs a profile per function to be meaningful), inlining small leaves at lifter time
  (would change the lifted output and needs a new golden run for each change).

## 12. Phase D: re-profile and candidate check (2026-11-13)
Timer build (`make TPM_DEFS=-DTPM_FN_TIMERS`), Azahar, log `bench/D_timers_log.txt`, 300 frames, last window:
frame 83.8 ms (timer build, ~20 ms above the normal build because the timers themselves cost time),
render 32.7 (world_cull 7.7 native, world_emit 18.1 with emit_487260 15.8 at 1198 calls/frame, skeleton 6.3),
render_tail 5.3, obj_update 25.3 (actor_emit 12.0 at 6 calls, actor_xform 6.7 at 52, actor_light 2.4 at 51,
obj_17930 12.0 at 40, obj_3a5ff 2.1), ai 1.9, anim 2.3, imuse 1.2.
- Caveat: functions with more than ~100 calls/frame are inflated by the timer reads (emit_487260, emit_488210,
  actor_xform, obj_17930). Only the 1-call functions (skeleton, world_cull, world_emit) are reliable.
- skeleton 0x404730: 2242 lines of lifted C, about 30 distinct callees. An exact native rewrite is a large job
  for about 5 ms. Not started.
- Remaining per-function candidates each give 1-5 ms. Reaching 30 fps (about 30 ms less) is not possible with
  single-function natives in this form. Decision needed: target, or a different structural change (for example
  phase C, the second core).

## 13. Strategy to 30 fps (2026-11-13, current)
Start point: 64 ms (Azahar, 1200-1500). Target: <= 33 ms.
Measured levers and status:
- Done and kept: world_cull native (phase A/B), lean call ABI (A2, golden-exact).
- Tried and reverted: native CRT sin (A3, no gain), -O3 (A4, no gain).
- Renderer per-draw path: ~1200 draws/frame, GPU batching already in place. Remaining CPU cost is per-draw
  setup (draw_state, fetch_ctx_init matrices, bind checks). Small per-draw savings, each needs a golden run.
- Guest code (about 70 % of the frame): obj_update subtree (about 25 ms inflated), world_emit (18 ms).
Plan, in order, each step golden-verified (`build-host/golden_ref1500.txt`) and measured with azbench:
1. Exact renderer per-draw savings in r3d_gpu.c (no change to the emitted triangles).
2. Lean ABI second pass: drop x87 slot copies where transitive fpu is false (already partly done).
3. Native versions of the 1-to-few-call hot functions only where the call count is small and the body is clear.
4. Re-profile after each step; stop a step if it gives under 1 ms.
Decision rule unchanged: keep what measures, revert the rest.

### 13a. Native __ftol (0x49a44c): tried, reverted (2026-11-13)
- Exact native version (stack bytes incl. caller EBP, fcw words, 64-bit result, lazy flags, fcf=0), differential
  test 20000 cases PASS, 1500-frame golden run identical (after fixing the caller-EBP write).
- Azahar 1200-1500: 63.8 / 65.0 ms against 64.1 / 65.0. Within noise, so reverted per rule. Code and test removed.
