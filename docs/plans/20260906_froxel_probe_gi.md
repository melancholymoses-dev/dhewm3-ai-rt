# Froxel Volumetrics + Probe GI — world-space caching arc

**Date:** 2026-08-23 · **Detailed for implementation:** 2026-09-12
**Status:** Arc #2 in ROADMAP.md. The owed profiler checkpoint is taken
(Mars City 2026-09-11: GI 4.41 / Refl 3.24 / Vol 1.53 / AO 1.17 / denoise ~0.65 ms).
**Part A: F0-F2 landed and validated, F3 and F4 dropped, F5 open.
Part B: G0-G4 landed and validated. G6 flipped `r_rtGIProbes` to 1 on 2026-09-19 —
probe GI is the shipped default, decided on appearance, with G5b's flicker veto
consciously waived. The per-pixel path is kept, not retired. G5b written 2026-09-19,
awaiting its in-game checks. Open: G5 fix 2, G5b's validation, and G6's retune.**

---

## Thesis

Volumetrics and GI recompute their sampling **per screen pixel, per frame**, then
fight the resulting noise with screen-space machinery (checkerboard, temporal EMA,
à-trous, bilateral upsample). None of those passes reproject history with motion
vectors, so any screen-space EMA ghosts under motion *by design* — that is
structural, not a lingering bug (the camera-cut and per-slot-history bugs were
fixed 2026-08-31 / 2026-09-06).

Cache the expensive result in **world space** instead, where temporal reuse is
trivially valid, and reduce per-pixel work to an interpolated lookup:

| Feature | Today (screen space) | Proposed (world space) |
|---|---|---|
| Volumetrics | per-pixel ray march, ray query per step per light (`vol_march.comp`) | froxel grid: fill once per cell, integrate per column, trilinear resolve |
| GI | per-pixel hemisphere rays + denoise chain (`gi_ray.rgen` → temporal → à-trous → albedo mod) | irradiance probes: fixed ray budget updates a rotating probe subset, per-pixel = 8 probe fetches |

Standard industry structure (Frostbite/id Tech froxel fog ~2015; DDGI ~2019). The
novelty is only that visibility stays **RT ray queries against the TLAS** instead of
shadow maps — same "no map editing, real occlusion" premise, just not once per pixel.

**Work-unit sketch (1080p, current defaults):**

| | Today | Proposed |
|---|---|---|
| Vol | half-res march: 960×540 px × 8 steps = **4.1 M** step evals | 160×90×64 grid = **0.92 M** cell fills (4.5× fewer, before any rotation) |
| GI | 1 M px ÷ 2 (checker) × 4 samples = **2.0 M** rays + 3 denoise passes | 1024 probes × 128 rays = **0.13 M** rays, denoise chain deleted |

---

## Pillar check

- **Pillar 1 (shadows are the feature):** untouched — no change to the direct shadow path.
- **Pillar 2 (darkness stays black):** the *main risk* of probe GI. Sparse probes
  interpolate, and interpolation leaks light through thin walls into dark rooms.
  Stage G3 (visibility-weighted probes) is mandatory, not polish.
- **Pillar 3 (light the air sparingly):** froxels make each admitted vol light cheaper;
  admission policy (AR0 classifier + dedicated vol selection) is unchanged.
  **Flashlight volumetrics stay deprioritized** (user direction 2026-08-23): the beam
  axis is nearly parallel to every view ray, so there is no parallax and no visible
  shaft structure. The hero case for beam *shape* is side-on fixture/panel shafts.
- **Pillar 6 (overlays before tuning):** every chunk below ships its overlay before
  anything it enables is tuned.

---

# Part A — Froxel volumetrics

## A.1 Locked design decisions

These were open in the original write-up; they are decided here so F0 can start.

| Decision | Choice | Why |
|---|---|---|
| Grid space | Camera frustum, **planar** Z slices (view-space linear depth), exponentially distributed | Planar slices make depth→slice a scalar function in the resolve, no per-pixel ray length |
| Slice distribution | `dist(z) = exp((z+0.5)/Nz · log(maxDist+1)) − 1`, inverse `z(d) = log(d+1)/log(maxDist+1) · Nz − 0.5` | Identical to `vol_march.comp`'s `exp(alpha*logFac)` spacing — same density profile, so an A/B compares like with like |
| Resolve target | A **compute pass writes `vkRT.volBuffer[slot]`**; `vol_composite.frag` is untouched | Zero risk to the composite/HDR path; instant A/B; composite has no depth binding today and does not need one |
| March resolution in froxel mode | Forced to **full res** (`marchScale = 1`) | The resolve is one trilinear fetch — half-res buys nothing and the bilateral upsample is then pure loss |
| Temporal / bilateral in froxel mode | Both **skipped**; froxel does its own EMA in froxel space (F4) | Screen-space EMA on top of a froxel resolve double-smooths and re-imports the ghosting this arc exists to remove |
| Grid images | Per-frame-in-flight slot for scatter/integrated (written and consumed in the same frame); **single shared** history image (F4) | Matches the existing `volBuffer` (per slot) and `volHistory` (shared) split. Per-slot history halves the real update rate — the 2026-09-06 bug |
| Per-cell light culling | GPU compute pass building a 128-bit cluster mask, **F3** — not F0 | Do not build it speculatively; F2's capture decides whether the fill's light loop is the cost |
| Default | `r_rtVolFroxel 0` (march) until F5 | Old path stays compiled and selectable until in-game validated |

**Precision note:** the fill pass builds cell world positions analytically from
`invViewProj`, so it is *not* exposed to the `d²·ulp/znear` depth-reconstruction
error. The **resolve** is — it linearizes the pixel's depth to pick a slice. At
`r_rtVolMaxDist 512` the error is far below one slice, but if `maxDist` is ever
raised past ~2000, re-check against the rule in ROADMAP.md.

**NDC convention:** Doom 3's `projectionMatrix` is GL-convention, so clip-space Z is
`2·depth − 1` and Y is flipped (`1 − 2·uv.y`) — see `rt_ReconstructWorldPos` in
`rt_indirect.glsl`. Mirror that exactly; do not assume Vulkan `[0,1]` Z.

## A.2 Files

| File | Status | Contents |
|---|---|---|
| `neo/renderer/Vulkan/vk_vol_froxel.cpp` | new | CVars, 3D image lifecycle, 3-4 compute pipelines, dispatch entry points |
| `neo/renderer/glsl/vol_froxel_common.glsl` | new → **`GLSL_INCLUDES`** | slice↔distance, cell→world-pos, atlas-free indexing, jitter |
| `neo/renderer/glsl/vol_froxel_fill.comp` | new → `GLSL_SHADER_SOURCES` | per-cell in-scattering (light loop + ray queries + cookies) |
| `neo/renderer/glsl/vol_froxel_integrate.comp` | new → `GLSL_SHADER_SOURCES` | per-column front-to-back Beer-Lambert accumulation |
| `neo/renderer/glsl/vol_froxel_resolve.comp` | new → `GLSL_SHADER_SOURCES` | trilinear fetch → `volBuffer`; also hosts every debug overlay mode |
| `neo/renderer/glsl/vol_froxel_cluster.comp` | new (F3) → `GLSL_SHADER_SOURCES` | per-cluster light bitmask |
| `neo/renderer/Vulkan/vk_raytracing.h` | edit | `vkRT` froxel fields + `VK_RT_*VolFroxel*` prototypes |
| `neo/renderer/Vulkan/vk_backend.cpp` | edit | dispatch order, `VK_SetRenderStage`, 3 new profiler phases |
| `neo/renderer/Vulkan/vk_vol.cpp` | edit | early-out of march/temporal/bilateral when `r_rtVolFroxel 1` |
| `neo/CMakeLists.txt` | edit | shader lists (see the `GLSL_INCLUDES` vs `GLSL_SHADER_SOURCES` split above) |

New files carry the dhewm3-rt GenAI copyright block (`.claude/CLAUDE.md`).

## A.3 Resources

```
froxelScatter[slot]     VK_IMAGE_TYPE_3D  R16G16B16A16_SFLOAT  Nx×Ny×Nz
                        usage STORAGE | SAMPLED, layout GENERAL
                        rgb = in-scattered radiance at cell centre, a = extinction
froxelIntegrated[slot]  same format/extent
                        rgb = accumulated in-scattering camera→cell, a = transmittance
froxelHistory           same format/extent, SINGLE (not per slot)   [F4 only]
froxelSampler           linear/linear, CLAMP_TO_EDGE on U/V/W
froxelClusters[slot]    SSBO, Cx·Cy·Cz × uvec4 (128-light bitmask)  [F3 only]
```

Memory at the 160×90×64 default: 7.0 MiB per image → **~35 MiB** total with history.
`r_rtVolFroxelResX/Y/Z` changes force a device-idle realloc, same pattern as
`r_rtVolHalfRes` in `VK_RT_ResizeVolumetrics`. The grid is **not** tied to swapchain
size — a window resize does not need to reallocate it.

## A.4 Params UBO

Deliberately **vec4-packed** rather than the scalar-offset layout `VolParamsUBO`
uses — that struct has already cost two offset-mismatch debugging sessions.

```c
struct VolFroxelParamsUBO {          // std140, 288 bytes, static_assert
    float invViewProj[16];  //   0  inverse view-projection (GL-convention clip Z)
    float cameraPosW[4];    //  64  xyz = camera world pos
    float camForwardW[4];   //  80  xyz = viewDef->renderView.viewaxis[0]
    int32_t gridDim[4];     //  96  xyz = Nx,Ny,Nz    w = cluster shift (F3)
    float depthParams[4];   // 112  x=maxDist  y=log(maxDist+1)  z=linNum  w=linAdd
    float densities[4];     // 128  x=point y=directed z=flashlight w=whiteNoiseMix
    float strengths[4];     // 144  x=point y=directed z=flashlight w=temporalAlpha
    float anisos[4];        // 160  x=point y=directed z=flashlight w=unused
    int32_t misc[4];        // 176  x=frameIndex y=maxLights z=debugMode w=debugSlice
    int32_t screen[4];      // 192  x=screenW y=screenH z=marchW w=marchH   (resolve)
    int32_t rect[4];        // 208  resolve dispatch rect, march space
    float prevViewProj[16]; // 224  F4 reprojection; identity until then
};
```

`linNum = -proj[14]`, `linAdd = proj[10]`; view distance from depth is
`|linNum / (2·d − 1 + linAdd)|` — the same idiom `BilateralPC` already uses in
`vk_vol.cpp`, copy it rather than re-deriving.

## A.5 Descriptor layouts

**fill** (`vol_froxel_fill.comp`) — set 0, mirrors `volMarchDescLayout` with the
output swapped to a 3D image and depth dropped:

```
0  ACCELERATION_STRUCTURE_KHR   TLAS
1  STORAGE_IMAGE (3D)           froxelScatter        (write)
2  UNIFORM_BUFFER_DYNAMIC       VolFroxelParams
3  STORAGE_BUFFER               vkRT.volLightSsbo    (unchanged vol selection)
4  STORAGE_BUFFER               froxelClusters       (F3; bind a 1-entry dummy before then)
set 1 = vkRT.matDescLayout / matDescSet — light cookies, exactly as vol_march.comp
        binds it. Declare only `sampler2D matTextures[4096]` at set=1 binding=3;
        do NOT #include rt_material.glsl (it uses gl_WorldToObjectEXT, which does
        not exist in a compute shader — this bit us in cookie Stage 3).
```

**integrate** — 0: `froxelScatter` (read, storage image), 1: `froxelIntegrated`
(write), 2: params UBO. One invocation per (x,y) column, `local_size = 8×8×1`.

**resolve** — 0: `froxelIntegrated` as `sampler3D` (COMBINED_IMAGE_SAMPLER,
`froxelSampler`), 1: `volBuffer[slot]` storage image (write), 2: depth sampler,
3: params UBO.

## A.6 Frame graph

Inside the existing `hasRealCamera` block in `vk_backend.cpp` (~line 4763), replacing
the Vol/VolTemporal/VolBilateral trio when `r_rtVolFroxel 1`:

```
  RT_VolFroxelFill      → VK_RTPROF_PHASE_VOL_FROXEL_FILL
      barrier COMPUTE(write) → COMPUTE(read)
  RT_VolFroxelIntegrate → VK_RTPROF_PHASE_VOL_FROXEL_INTEGRATE
      barrier COMPUTE(write) → COMPUTE(read)
      depth barrier ATTACHMENT_OPTIMAL → DEPTH_STENCIL_READ_ONLY_OPTIMAL
  RT_VolFroxelResolve   → VK_RTPROF_PHASE_VOL_FROXEL_RESOLVE
      barrier COMPUTE(write) → FRAGMENT(read)
      depth barrier restored to ATTACHMENT_OPTIMAL
  (render pass resumes) VK_RT_CompositeVolumetrics — unchanged
```

Only the **resolve** needs the depth round-trip; fill and integrate never read depth.
`VK_RT_DispatchVolumetrics` / `...TemporalResolveVol` / `...VolBilateral` early-out on
`r_rtVolFroxel 1`, and the vol path forces `volReadView[slot] = volBuffer[slot].view`
so the composite reads what the resolve wrote.

Add to `vkRTProfilePhase_t` + `VK_RTProfilePhaseName` (`FroxelFill`,
`FroxelIntegrate`, `FroxelResolve`). The 64-event budget has room.

Gating is inherited: the whole block already sits behind `!isSubview && !isMirror &&
hasRealCamera`, which is the correct guard for the GUI overlay's degenerate second
`RC_DRAW_VIEW` — do not add a `viewEntitys` test.

## A.7 CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_rtVolFroxel` | `0` | 0 = march (current), 1 = froxel grid |
| `r_rtVolFroxelResX/Y/Z` | `160 / 90 / 64` | grid dimensions; change forces a realloc |
| `r_rtVolFroxelDebug` | `0` | 1 = slice view, 2 = per-cell light count, 3 = grid-mapping error, 4 = cell age (F4) |
| `r_rtVolFroxelDebugSlice` | `32` | which Z slice mode 1 shows |
| `r_rtVolFroxelDebugGain` | `20.0` | mode-1-only pre-tonemap gain, mirrors `r_rtVolDebugGain`; modes 2/3 emit normalised ramps and ignore it |
| `r_rtVolFroxelTemporal` | `1` | froxel-space EMA (F4) |
| `r_rtVolFroxelAlpha` | `0.1` | EMA blend, current-frame weight (F4) |
| `r_rtVolFroxelRotate` | `1` | update 1/N of Z slices per frame (F4); 1 = full rate |
| `r_rtVolFroxelDump` | `0` | one-shot: grid dims, memory, slice→distance table, cluster occupancy; self-clearing like `r_rtVolDump` |

Reused unchanged: `r_rtVolMaxDist`, `r_rtVolMaxLights`, `r_rtVolDensity`,
`r_rtVolStrength`, `r_rtVolAnisotropy` and all six directed/flashlight knobs. **The
tuning constants keep their meaning** — same phase function, same Cauchy attenuation,
same containment tests, so an A/B at identical settings is meaningful.

## A.8 Chunks

Each chunk is one working session, ends compiling, and leaves the default path
unchanged until F5.

### F0 — grid + fill, nothing reads it ✅ **written 2026-09-12, not yet run**
Allocate the 3D images; write `vol_froxel_common.glsl` (slice↔distance, cell→world);
port `vol_march.comp`'s light loop verbatim into `vol_froxel_fill.comp` (containment,
Cauchy, HG phase, `rayQueryEXT` occlusion, cookies) evaluated at the jittered cell
centre; wire the dispatch + profiler phase; implement `r_rtVolFroxelDump`.
- **Exit:** validation-clean, `FroxelFill` reports a nonzero time, the dump's
  slice→distance table matches the march's step distribution at the same `maxDist`,
  and the default path renders identically (nothing reads the grid yet).
- **Log breadcrumbs from the start** (grid dims, cells dispatched, lights in the
  selection) — not after it breaks.
- **Deviations as built:** (a) the F3 cluster SSBO is not bound as a dummy — F3 adds
  binding 4 when it needs it; (b) XY jitter is fixed at the cell centre and only Z is
  jittered, because XY jitter without F4's EMA to average it is pure added noise;
  (c) the fill runs *alongside* the march rather than replacing it, so F1 can compare
  them in the same frame. Ten vol tuning CVars lost their `static` so both paths read
  one set of constants.

### F1 — overlays ✅ **written 2026-09-12, not yet run**
`vol_froxel_resolve.comp` with the debug modes only: when
`r_rtVolFroxelDebug != 0` it overwrites `volBuffer` and the composite's debug
(non-blending) pipeline displays it. The resolve must run *after* the temporal EMA
has consumed `volBuffer`, or the overlay lands in the march's history.
- **Mode 1** — one Z slice of the scatter grid, masked to where that slice is in
  front of the surface. Unmasked, a slice at 200 units seen through a wall at 50
  shows the room beyond and reads as a mismatch against the march, which stops at
  the surface.
- **Mode 2** — per-cell light count at the pixel's own depth slice (the fill writes
  the count into alpha in place of extinction under this mode). This is the baseline
  F3's cluster cull must reproduce exactly.
- **Mode 3** — grid-mapping error: evaluate the cell world position at exactly this
  pixel's ray and the surface's depth, and compare against `rt_ReconstructWorldPos`.
  Same ray, same planar depth, so the only residual is float precision.
  *(Supersedes the original "cell world-pos hash, stable under rotation" idea — that
  was wrong: the grid is frustum-anchored, so cell indices necessarily change as the
  camera turns and a hash colour is expected to change with them. A direct comparison
  against the known-good depth reconstruction tests the same thing decisively.)*
- **Exit:** mode 3 is green (sub-unit error) everywhere in range — a hue that tracks
  distance, or that flips with camera yaw, means the clip-space convention or the
  planar-depth division is wrong; mode 1 on slice N matches the march's structure at
  that depth (stand still, compare against `r_rtVolDebugMode 2`); mode 2 shows
  plausible counts — high near fixtures, black in sealed corridors.
- This is the chunk that proves the world-position mapping. **Mode 3 is the gate.**
  Do not proceed to F2 while it is anything but green.
- ✅ **Mode 3 passes as of 2026-09-13** (Command Access Junction): green screen-wide
  in range, blue beyond `r_rtVolMaxDist`, black sky. The F0 mapping is proven.
- **Mode 2 first run (Mars City Reception) showed a screen-wide 0-vs-N checkerboard**
  — surface-straddling cells, see F2 below. Overlay changed to take the column max,
  which is immune to the straddle and is the better statistic for catching an F3
  cluster-cull regression anyway. The underlying effect is a real F2 work item.
- **Reading overlays: set `r_rtVolHalfRes 0` first.** At half res the composite
  bilinearly upscales the resolve's output, so colours between blocks are
  interpolation rather than measurements — a heatmap read at half res shows counts
  that were never computed.
- **First run came back saturated red screen-wide, and the cause was real:** Doom 3's projection has an infinite far plane (`proj[10] = -0.999`), so NDC
  z asymptotes to +0.999 and never reaches +1. The ray was built as
  `normalize(pFar - pNear)` with `pFar` unprojected at ndcZ = +1 — which resolves to
  6000 units *behind* the eye, so every ray pointed backwards and every cell position
  flew off to infinity. Fixed by building the ray as `normalize(pNear - eye)`. The
  dump now prints the centre/corner ray's `dot(dir, forward)` and the near-plane
  distance so the same class of fault is visible in the log alone. No other shader
  was affected — they all unproject at the pixel's actual depth.

### F2 — integrate + real resolve, A/B
`vol_froxel_integrate.comp`; resolve's non-debug path (linearize depth → slice
coordinate → one trilinear `texture()`, clamped to the pixel's own depth slice so
cells behind geometry never contribute); `r_rtVolFroxel 1` routes the frame graph.
- **Retune the slice curve's RANGE, not its shape.** Keep the exponential; its
  justification is cell **isotropy** — XY cell size grows linearly with distance, so
  Z spacing ∝ d is what keeps cells cubical, and anisotropic cells are what alias.
  (The current `exp(zc/Nz·log(maxDist+1)) − 1` already gives spacing ∝ `(d+1)`, i.e.
  the textbook `n·(D/n)^(zc/Nz)` with the origin regularized. Shape is fine.)
  **Do not hump the distribution**: mid-range-dense would make mid cells thin-in-Z
  but wide-in-XY while leaving far cells at ~150 units deep, and the straddle
  artifact is worst at the far end. Note also that inverse-square falloff is measured
  from the *light*, not the eye, so it argues for nothing here.

  Both ends of the range are misallocated:
  - **Far — the big one.** At `r_rtVolDensity 0.015`, `T(d) = exp(-0.015d)`, so
    `maxDist 512` is **7.7 optical depths**: T = 0.050 at 200, 0.023 at 250,
    0.00046 at 512. The outer half of the grid is in near-total extinction and
    contributes nothing. Derive the far anchor from the medium instead —
    `d_far = min(maxDist, -ln(T_min)/density)`, T_min ≈ 0.02. Must be derived, not
    a constant: raising density shortens the useful range and lowering it extends it.
  - **Near.** Slices 0–14 cover the first 3 units (znear) — ~22 % of the slices and
    of the fill's ray queries on ~0.6 % of the path. Not *dead* (the integrate pass
    accumulates from slice 0, so they feed the integral the visible slices carry, and
    the march samples that air too — its `t` starts at 0, not znear), just badly
    allocated. Anchor at znear.

  Combined, 64 slices over [3, 261] rather than [0, 512] takes the far cell from
  ~50 units deep to ~18 — ~3× finer, aimed straight at the straddle below, and
  nothing visible is lost because what is cut sits below T = 0.02. Re-measure after;
  this moves every slice, so it belongs with the integrate pass rather than between
  two overlay checks. Nothing consumes the grid until F2, so it is free to change.
- **The final partial cell must be weighted by the fraction of it in front of the
  surface — this is now confirmed, not speculative.** F1's mode 2 measured it: the
  cell containing the surface straddles it, the per-cell jitter lands the sample
  behind the wall about half the time, every light is then occluded, and the result
  was a screen-wide 0-vs-N checkerboard. It scales with distance, because the
  exponential slice curve makes far cells ~150 units deep at the default
  160×90×**64**. Raising `ResZ` shrinks the error but does not remove it.
- **Exit:** side-by-side screenshots march vs froxel on the fan-blade cookie shot and
  a Mars City corridor; profiler capture with `FroxelFill/Integrate/Resolve` vs
  `Vol/VolTemporal/VolBilateral` (compare the **sums** — comparing `Vol` alone
  flatters the change); no fog visible in front of walls.
- **Expected regression:** beam edges are softer than the march's. Quantify it here;
  raising `r_rtVolFroxelResX/Y` is the first mitigation and its cost is linear.
- ✅ **Landed and measured 2026-09-13. In-game validated; visually better, not just
  cheaper** ("looks kinda incredible, smoother"). Like-for-like adjacent frames
  26700 → 26730, near-identical view:

  | | march | froxel |
  |---|---|---|
  | Vol / FroxelFill | 1.012 | 0.130 |
  | VolTemporal / Integrate | 0.022 | 0.026 |
  | VolBilateral / Resolve | 0.261 | 0.059 |
  | **sum** | **1.295 ms** | **0.215 ms** |

  Median across the capture 1.63 → 0.29 ms (5.6×), worst case 3.61 → 0.49 (7.3×).
  **Variance improved more than the mean**: the march's `Vol` swung 0.73–3.32 ms
  (4.6× spread), the froxel total 0.20–0.49 (2.5×), and `FroxelIntegrate` is
  scene-independent at 0.023–0.026 — 14400 columns regardless of view.
- **The fog got denser, and that is the froxel path being MORE correct.** The march
  takes 8 exponentially-spaced steps over [0,512]; its last step spans 217→512 units
  evaluated at a single point near 364, so a light at ~250 units is missed entirely.
  64 slices over [3,261] makes far cells ~18 units deep and captures it. The march
  was systematically under-integrating the far half of every ray, and
  **`r_rtVolDensity` was tuned against that error** — so the same constant now reads
  denser. Re-tune against the new integrator; this is not a regression to undo. It
  also explains "smoother": fewer lights popping as sparse far steps slide past them.
- **F3 (clustered light lists) is NOT worth doing — see below.**
- **XY resolution is the affordable quality lever.** Fill measures ~0.228 ns/cell:
  240×135×64 costs 0.47 ms fill / 0.55 total / 66 MiB, 320×180×64 costs 0.84 / 0.92 /
  118 MiB. Even the latter stays under the march's *median*. Memory is the binding
  constraint, not time. Note this is an XY problem (beam/fan edge crispness); raising
  `ResZ` addresses the straddle instead.
- **Written 2026-09-13.** As built:
  - The integrate pass anchors its stored value at cell **centres**, not faces. That
    is what makes the straddle weighting fall out of the resolve's trilinear fetch
    for free: sampling at exactly the surface's depth linearly interpolates the
    integral to that depth. Face-anchored values would have needed an explicit
    fractional term.
  - Segment lengths are divided by `cos` to the view axis. Slices are planar, so the
    ray through a cell is longer than the slice's depth extent everywhere but the
    screen centre — skipping this under-integrates the whole periphery by ~30 % at
    the edges of a 90° FOV.
  - `VK_RT_VolFroxelActive()` requires **all three** pipelines, so a shader that
    fails to load falls back to the march rather than standing it down and leaving
    the screen fogless.
  - `volReadView` is claimed at the top of the resolve rather than after its
    dispatch: temporal/bilateral no longer run to update it, so any early-out would
    otherwise leave the composite reading their stale — and differently sized —
    output.
  - `dFar` now ~261 rather than 512 at default density, so **mode 3's blue
    out-of-range region gets noticeably larger**. That is the retune working, not a
    regression.

### F3 — clustered per-cell light lists — ❌ **DROPPED 2026-09-13**
The trigger condition is met but the payoff is gone. F2's capture does show the fill
dominating (60–85 % of froxel cost, swinging 0.115→0.423 ms with light count) — but
the *whole* froxel path is now 0.29 ms median. A perfect cluster cull halving the
fill saves ~0.1 ms against GI at 4.3 ms and Shadows at 1.4–6.1 ms. That budget buys
far more as XY resolution, which is visible on the fan-blade shot; a cluster cull is
not. Revisit only if a future scene pushes the vol light selection far past the ~7
lights these captures saw, or if XY resolution is raised enough to make the fill
dominant again in absolute terms.

Original scope, if it ever returns:
`vol_froxel_cluster.comp`: one thread per cluster (default cluster = 8×8×8 cells →
20×12×8 = 1920 clusters, 16 B each), tests each of the ≤128 vol lights against the
cluster's world AABB, writes a `uvec4` bitmask; fill iterates only set bits.
- **Exit:** `FroxelFill` drops measurably; debug mode 2 (now reading the mask) is
  unchanged from F2's brute-force counts in every test room — a cluster-cull bug
  shows up there as missing lights, which is exactly what that overlay is for.

### F4 — froxel-space temporal + rotation — ❌ **DROPPED 2026-09-14**
F2 shipped without it and looks good enough; the noise F4 exists to suppress was
never observed. Dropping it is not merely "not needed", it **protects a property
F2 turned out to have for free**: with no EMA and no history, `vol_froxel_fill.comp`
re-evaluates the light SSBO from scratch every frame at full grid, so volumetrics
tracks **flickering and moving lights frame-for-frame with zero lag** — which
Doom 3 leans on heavily. F4 would have spent that. `r_rtVolFroxelRotate` is worse
than neutral here: a slice rotation beating against a flicker rate aliases in
depth. Revisit only if a measured noise problem appears, and then with adaptive
blending (see G5) rather than a flat EMA.

Consequence: the two A.9 mitigations that named F4 (cell-centre shadow aliasing,
fast-turn frustum invalidation) now stand unmitigated — neither has been observed,
so they are risks on paper, not open bugs.

Original scope, if it ever returns: EMA against `froxelHistory` (single shared
image, ordered by submission + an explicit barrier — **not** per-slot),
reprojecting the cell centre through `prevViewProj`; a fetch outside the previous
frustum falls back to the current sample. `r_rtVolFroxelRotate`'s index **must**
key off a per-slot counter, not `tr.frameCount`.

### F6 — background attenuation (must land BEFORE F5's decision)
The composite does the additive half of the transport equation and never the
attenuating half: `dst = airlight + dst`, with no `T·L_surface`. Both paths already
compute the path transmittance and write it to `.a` — `vol_march.comp` and
`vol_froxel_resolve.comp` agree on the convention — and `vol_composite.frag` passes it
through, but `dstColorBlendFactor = VK_BLEND_FACTOR_ONE` discards it.

Consequence: light is created rather than redistributed, nothing ever washes out with
distance, and a wall seen through a shaft gains apparent detail instead of losing it
(the added airlight lifts a dark surface off the tonemap toe onto a steeper part of the
curve). The error is first-order — `exp(-0.015*50) = 0.47` at ordinary room distance.
The compensating "medium lights the wall, wall bounces back" term is second-order,
`sigma_s * path * albedo` ~ 5 %, same sign, and does not cancel it.

**Toggle, so the old behaviour stays available until it is tuned out.**
`r_rtVolAttenuateBackground`, default **0**. Both pipelines are built up front, so the
toggle is free at runtime — same pattern as `volCompositeDebugPipeline`.

1. **Blend.** Third pipeline beside the additive and replace ones, identical except
   `dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA`, giving
   `dst = src.rgb + dst*src.a`. Leave the alpha factors and `colorWriteMask` alone.
   `VK_RT_CompositeVolumetrics` selects it; debug modes still win.
2. **Ordering — the real work.** The composite currently runs at vk_backend.cpp:4951,
   BEFORE `VK_RB_DrawInteractions` (4958), so attenuating there would dim only GI and
   ambient and let every direct light land on top unattenuated. Move it, under the
   cvar, to **after `VK_RB_DrawShaderPasses` (4975) and before `VK_RB_FogAllLights`
   (4985)**: interactions and blend stages are surface radiance and must be attenuated;
   Doom 3's own fog lights are a separate artist-placed medium and would be
   double-counted. Keep one `VK_RTPROF_PHASE_VOL_COMPOSITE` bracket at each call site
   (phases accumulate) and add a per-frame guard so it can never composite twice.
3. **Prerequisite, one line, do not skip.** `vol_bilateral.comp:136` initialises
   `bestColor = vec4(0.0)`. If no neighbour passes the bilateral test, alpha reaches
   the composite as 0 — harmless today because alpha is ignored, a **fully black pixel**
   once it multiplies the background. Initialise it to `vec4(0.0, 0.0, 0.0, 1.0)`;
   "clear air" is the correct failsafe everywhere transmittance is unknown.
4. **Retune density.** `r_rtVolDensity 0.015` was tuned by eye against the missing
   attenuation and is absorbing that error — turning this on unchanged will read as
   murk. Same trap as the froxel fog reading denser at identical density. Useful
   synergy: the froxel far plane is `-ln(r_rtVolFroxelFarTransmittance)/density`, so
   lowering density **automatically extends the grid's range**, which is the same fix
   the "shafts only appear near the camera" complaint wants.

- **Exit:** with the cvar on, a wall seen through a shaft LOSES contrast; the scene does
  not get net brighter; distant surfaces wash out. A/B the cvar on the same frame.
- **Check first:** `r_rtVolDebugMode 1` (path transmittance, greyscale) should read
  white in clear air and darken with distance. Uniformly near-black means density is
  crushing everything into a near-camera shell and step 4 is already overdue.
- **Known limitation to accept, not fix:** transmittance comes from the opaque depth
  buffer, so translucent surfaces and glass get attenuated by whatever is behind them.
  Screen-space fog compositing always has this; note it and move on.
- 🟡 **Written 2026-09-16, not yet in-game validated.** As built:
  - `r_rtVolAttenuateBackground` (default 0) drives both the pipeline choice
    (`volCompositeAttenPipeline`) *and* the call site, via one query,
    `VK_RT_VolCompositeAfterSurfaces()`. Backend calls it at exactly one of the two
    sites; `VK_RT_CompositeVolumetrics` additionally self-guards on
    `(tr.frameCount, backEnd.viewDef)` — keyed on the view, not the frame, because a
    frame legitimately draws several.
  - The late site **must re-set viewport and scissor**: the interaction and
    shader-pass loops set both per surface, so the fullscreen triangle would
    otherwise inherit the last drawn surface's rect. Not in the original step list.
  - Debug modes still win over the attenuating pipeline — both want replace blend.
- **Step 4 as written was the wrong knob, and the first build proved it in-game
  (screenshots 2026-09-16: corridor and terminal unreadably dark).** Scaling
  `r_rtVolDensity` cannot work: density drives the *airlight* as well as the
  extinction, so it trades shaft visibility one-for-one against background
  attenuation and has no setting where both are right. It is also the wrong
  magnitude — 0.015 was tuned for visible shafts, and `exp(-0.015 * 500)` is 5e-4,
  i.e. a Doom 3 corridor is fully extinguished long before its far wall.
  Replaced by **`r_rtVolAttenuateStrength` (default 0.1)**, applied as `T^k` in
  `vol_composite.frag` — `T^k == exp(-k*tau)`, so k *is* the ratio of extinction to
  scattering (the single-scatter albedo), and it is the one thing that was missing:
  upstream conflates the two into one `density`. Nothing upstream changes now, so
  the froxel `dFar`, the in-scattering, and an `r_rtVolAttenuateBackground` 0/1 A/B
  are all like-for-like. `r_rtVolDebugMode 1` shows the post-`k` value, so it can be
  tuned against what is actually applied.
- **The 2D GUI/HUD overlay bug, same build:** the late call site sits outside the
  block that gates on `hasRealCamera`, so Doom 3's second per-frame `RC_DRAW_VIEW`
  (zeroed `viewaxis`) got multiplied by stale transmittance — HUD dimmed, settings
  menu black. Additive compositing had always run there harmlessly, which is why
  the gate was never needed before. The late site now carries the same
  `hasRealCamera && !isSubview && !isMirror` test as the dispatches, which also
  retires the stale-subview residual noted below.
- **`r_rtVolAttenuateStrength` is an artistic control, not physics — say so.**
  Review (2026-09-17) correctly flagged two things the original comments claimed
  away: applying `k` only at composite leaves the airlight integrated with the raw
  `density` while the background uses `k*density`, so the two halves of the
  transport equation do not share an extinction coefficient; and `k < 1` puts
  extinction *below* scattering, i.e. an albedo of `1/k` (25 at the tuned 0.04),
  which no medium has. `k = 1` is the only self-consistent setting. Comments
  corrected rather than the code: making it physical means carrying separate
  `sigma_s`/`sigma_t` through `vol_march.comp` and the froxel integrate pass, which
  moves the tuned additive look too and so is a deliberate decision, not a cleanup.
  **Open.**
- **Residual, not fixed:** `gi_temporal_resolve.comp:83` stores `vec4(0.0)` when
  current *and* history are non-finite, which is an alpha-0 (black) pixel under
  attenuation. Reachable only via NaN on the non-default march path, and the shader
  is shared with GI, so it was left alone rather than given a vol-specific
  convention.

### F7 — projected-light cone penumbra — written 2026-09-16
Not planned; found from a user report that a directed light's shaft was invisible
from a step outside it, with no door or gap involved. The point-light branch fades
over a 1.0→1.5 halo shell outside its box; the cone branch was a hard binary
`if (cosAngle < cd.w) continue`, so the two light classes obeyed visibly different
rules. Both `vol_froxel_fill.comp` and `vol_march.comp` now smoothstep over a
penumbra band sized as a fraction of the cone's own angular width
(`VOL_CONE_PENUMBRA 0.35`), kept byte-identical between the two so the
`r_rtVolFroxel` 0/1 A/B stays meaningful.

The penumbra fixes the cone *wall*; it does nothing for viewing *angle*, and the
report was equally about high-angle views (side-on under a fan, or looking down
from above) reading as no scattering at all. That is the phase function, and it is
a real tension rather than a mistuning: `r_rtVolDirectedAnisotropy` was
deliberately raised to 0.6 to sharpen shaft/shadow definition, and HG is
**normalised** — g redistributes a fixed scattered energy rather than scaling it,
so buying forward contrast spends side-on visibility one-for-one (25× between
`cosθ=1` and `cosθ=0` at g=0.6, vs 4× at the point default 0.35). Lowering g would
trade back exactly the definition that was wanted.

Fixed instead with a **two-lobe phase**, `r_rtVolIsotropicMix` (default 0.3):
`PhaseFunction = mix(HG(cosθ, g), 1/4π, isoMix)`. Both lobes integrate to 1, so
the blend does too — this is a redistribution, not a brightness change, and the
anisotropy knobs stay free for shaft definition. At 0.3 it measures ~1.9× side-on
for ~2/3 of the forward peak: real, not dramatic, because the geometry contributes
as much as the phase (inside the cone the whole view ray sweeps the bright
near-apex region; from outside it crosses once).
- Cost nothing in struct size: the march UBO's `_uboPad` at offset 156 and the
  froxel UBO's `strengths.w` (F4's dropped `temporalAlpha`) were both already dead
  slots. `VolParamsUBO` stays 176 bytes, `VolFroxelParamsUBO` stays 304.
- `VOL_CONE_PENUMBRA` and `PhaseFunction` are duplicated verbatim in
  `vol_march.comp` and `vol_froxel_fill.comp`; both dumps now print `isotropicMix`.
- **The penumbra did nothing in-game until the cookie clip was fixed too**, which
  is why the first build read as no change at all. `rt_SampleLightCookie` returns
  black outside the projector's `[0,1]` UV box (Doom 3's own zero-clamp), and
  every cell in the new angular band projects outside it — so the softened edge
  was multiplied by zero on exactly the fan/grate fixtures it was written for.
  Added `rt_SampleLightCookieSoft`, volumetric-only (`VOL_COOKIE_PENUMBRA 0.08`,
  UV units, clamped fetch): the surface paths keep the hard edge, because that is
  what the GL renderer does and softening it would make lit surfaces disagree.
  Caught in review, not in play — the symptom is indistinguishable from "the
  change did nothing".

### F5 — retire decision — **unblocked 2026-09-18, the only Part A item left**
From F1-F4 evidence: keep the march compiled behind `r_rtVolFroxel 0`, flip the
default, or delete `vol_march.comp` + `vol_bilateral.comp` from the froxel path.
Update ROADMAP.md and move this part to `completed/`.

F6 was the blocker and is done; the transport-coefficient work
(`completed/20260917_vol_transport_coefficients.md`) went further and made the
survivor's medium physical. The froxel path has been the default (`r_rtVolFroxel 1`)
throughout, so the march is already only an A/B reference.

Argument for keeping it compiled: every light-model change since F2 — F7's cone
penumbra, the soft cookie edge, the two-lobe phase, the σ_t/albedo split — was written
into *both* shaders precisely so the A/B stays meaningful, and that A/B is what
localised the cookie hard-clip. Deleting the march removes the only independent check
on the froxel sampler. Argument for deleting: that duplication is the maintenance cost
being paid for it, and it has now been paid four times.

**Note before deciding:** `r_rtVolHalfRes`/`r_rtVolBilateral`/`r_rtVolTemporal` and
`vol_bilateral.comp` belong to the march path only. Retiring it retires them too,
including the `bestColor` alpha-1 failsafe added in F6 step 3.

## A.9 Known risks

| Risk | Signal | Mitigation |
|---|---|---|
| Beam edge crispness floors at XY resolution, and side-on shafts are the hero case | F2 A/B screenshots | Raise `ResX/Y` (linear cost, huge freed budget); keep the march selectable; only then consider deleting it |
| Shadow sampling at one cell centre blocks/aliases the shadowed part of a shaft | Blocky shaft boundaries that do not improve with more steps | Per-frame jitter within the cell + F4 EMA is the intended fix; a second occlusion sample per cell is the fallback |
| Cells straddling a wall leak fog through it | Fog visible in front of geometry | Depth-clamped resolve (F2); if it survives, weight the last slice by the fractional depth position |
| Attenuation turned on at a density tuned without it | Scene reads murky and over-dark rather than hazy | F6 step 4: density must come down in the same change. The cvar defaults off so the two can be A/B'd on one frame |
| Transmittance reaching the composite as 0 instead of 1 | Black pixels / black haloes at depth edges once F6 is on | F6 step 3: the bilateral's no-sample fallback must be `a = 1`, not `a = 0`. `r_rtVolDebugMode 1` shows this directly |
| Grid is frustum-shaped, so a fast camera turn invalidates most of it | Flicker on rapid turns | F4's fallback-to-current on reprojection miss; this is why the miss path must be "take current", never "take black" |

---

# Part B — Probe GI

**Do not start until Part A lands.** Part A is a two-shader replacement; this is a
subsystem-scale change.

## B.1 Locked design decisions

| Decision | Choice | Why |
|---|---|---|
| Probe placement | **One camera-anchored uniform grid**, snapped to probe spacing, scrolling with the camera — not per-area grids | Per-area grids need a per-pixel "which grid" lookup before any interpolation can happen. A flat 3D index is one `ivec3`. Per-area placement returns in G5 *if* density proves to be the leak lever |
| Grid defaults | 32×32×16 probes at 64 units → 2048×2048×1024 unit coverage | Doom 3 interiors are small; 64 units ≈ one large step. Cvar-tunable |
| Irradiance storage | Octahedral 8×8 interior + 1-texel border = 10×10 per probe, tiled 128×128 probes → 1280×1280 `rgba16f` (13 MiB) | Standard DDGI; border makes bilinear sampling seamless |
| Visibility storage | 16×16 + border = 18×18, tiled → 2304×2304 `rg16f` (21 MiB), r = mean(d/D), g = mean((d/D)²), **normalised by `r_rtGIProbeMaxRayDist`** | Chebyshev needs both moments; higher res than irradiance because it carries the geometry detail. Raw units overflow `rg16f` — see G1 |
| Ray shading | **Reuse `gi_ray.rchit` unmodified** | It already returns `albedo · irradiance` — outgoing radiance at the hit — which is exactly a probe's incoming radiance. Same P3 stochastic light selection, AR0 admission, `rt_light_eval.glsl`, so a bounce "sees" the same scene as today |
| Pipeline | **Second raygen group in the existing GI RT pipeline**, selected by offsetting the SBT raygen region | Avoids a second pipeline + SBT + material-set duplication. Requires `giDescLayout` to gain bindings 6-8, unused by the per-pixel rgen |
| Ray→atlas path | rgen writes a scratch 2D image (raysPerProbe × probesPerFrame), a separate blend compute scatters into the atlas | No atomics, standard DDGI two-step, and the scratch image is directly inspectable in G1 |
| Resolve target | Compute pass writes `vkRT.giBuffer[slot]`; `gi_albedo_mod.comp` + `gi_composite.frag` unchanged downstream | Probe output is albedo-free irradiance, so receiver-albedo modulation is *more* correct than today |
| Default | `r_rtGIProbes 0` until G6 | Per-pixel GI stays compiled and selectable |

**What dies in probe mode** (still compiled, just not dispatched): `gi_ray.rgen`'s
per-pixel dispatch, checkerboarding and `checkerPhase`, `gi_temporal_resolve.comp`,
`gi_atrous.comp`. **What stays:** all light admission machinery, `gi_albedo_mod`,
`gi_composite`, and the AO pass — AO now carries *all* short-range contact darkening,
which sparse probes structurally cannot represent.

## B.2 Required upstream change: the payload

`gi_payload.glsl` currently carries `vec3 colour` only. Probes need hit distance
(for the visibility moments) and a backface flag (for probe classification):

```glsl
struct GIPayload {
    vec3  colour;    // unchanged
    float hitDist;   // gl_RayTmaxEXT at hit; giRadius on miss
    float backface;  // 1.0 when the ray hit a backface, else 0.0
};
```

`gi_ray.rchit` and `gi_ray.rmiss` must both set the new fields; `gi_ray.rgen` ignores
them. **This edit touches the shipping GI path** — land it on its own, confirm
per-pixel GI is pixel-identical, then build on it.

## B.3 Files

| File | Status | Contents |
|---|---|---|
| `neo/renderer/Vulkan/vk_gi_probe.cpp` | new | CVars, atlas/scratch/SSBO lifecycle, probe scheduling, 4 dispatches |
| `neo/renderer/glsl/gi_probe_common.glsl` | new → **`GLSL_INCLUDES`** | octahedral encode/decode, probe index↔world, atlas UV with border |
| `neo/renderer/glsl/gi_probe_trace.rgen` | new → `GLSL_SHADER_SOURCES` | fires `raysPerProbe` rays per scheduled probe into the scratch image |
| `neo/renderer/glsl/gi_probe_blend.comp` | new → `GLSL_SHADER_SOURCES` | scratch rays → irradiance + distance atlas interiors, EMA |
| `neo/renderer/glsl/gi_probe_border.comp` | new → `GLSL_SHADER_SOURCES` | copies octahedral edges/corners into the 1-texel borders |
| `neo/renderer/glsl/gi_probe_resolve.comp` | new → `GLSL_SHADER_SOURCES` | per-pixel: 8-probe weighted fetch → `giBuffer` |
| `neo/renderer/glsl/gi_payload.glsl` | edit | see B.2 |
| `gi_ray.rchit` / `gi_ray.rmiss` | edit | fill `hitDist` / `backface` |
| `vk_gi.cpp` | edit | `giDescLayout` bindings 6-8, second rgen group + SBT, early-outs when `r_rtGIProbes 1` |
| `vk_raytracing.h`, `vk_backend.cpp`, `neo/CMakeLists.txt` | edit | fields, dispatch order, 3 profiler phases, shader lists |

## B.4 CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_rtGIProbes` | `0` | 0 = per-pixel GI, 1 = probe GI |
| `r_rtGIProbeSpacing` | `64` | world units between probes |
| `r_rtGIProbeCountX/Y/Z` | `32 / 32 / 16` | grid dimensions (realloc on change) |
| `r_rtGIProbeRays` | `128` | rays per probe update |
| `r_rtGIProbeUpdatesPerFrame` | `1024` | probes updated per frame → full refresh every 16 frames at defaults |
| `r_rtGIProbeHysteresis` | `0.97` | atlas EMA: history weight |
| `r_rtGIProbeNormalBias` | `8.0` | receiver offset along the normal before probe lookup |
| `r_rtGIProbeVisibility` | `1` | Chebyshev visibility weighting (G3) |
| `r_rtGIProbeDebug` | `0` | 1 = probe spheres tinted by stored irradiance, 2 = probe weights / Chebyshev rejection, 3 = leak detector (Chebyshev vs ray-traced truth; magenta = no usable probe at all), 4 = probe state verdict (usable/inside/void/never-traced), 5 = distance-atlas mean, 6 = raw backface fraction, 7 = raw miss fraction. Clamped in **two** places — `VK_RT_GIProbeDebugMode` and the `misc[0]` UBO fill; raising only one silently renders a different mode |
| `r_rtGIProbeViewBias` | `8.0` | receiver offset toward the eye, on top of the normal bias (G3) |
| `r_rtGIProbeInsideThreshold` | `0.25` | backface fraction above which a probe is classified buried in geometry (G4); 0 disables |
| `r_rtGIProbeOutsideThreshold` | `0.9` | miss fraction above which a probe is classified as in the void outside the level (G4); 0 disables |
| `r_rtGIProbeDump` | `0` | one-shot: grid origin/dims, memory, active/inactive counts, per-area occupancy, update queue depth |
| `r_rtGIProbeFastBuckets` | `1` | G5b: separately-cached flicker buckets (K). 0 = no split, today's behaviour |
| `r_rtGIFlickerThreshold` | `0.15` | G5b: relative per-frame colour change that classifies a light as fast |
| `r_rtGIFlickerHold` | `2.0` | G5b: seconds a light stays classified fast after its last change |

## B.5 Chunks

### G0 — payload + storage + placement overlay, no tracing ✅ **landed + in-game validated 2026-09-13**
The B.2 payload edit (landed and verified first); allocate atlases, scratch image and
the probe-state SSBO; implement grid anchoring (snap the grid origin to a multiple of
spacing near the camera so probes do not swim); `r_rtGIProbeDebug 1/4` drawing probe
positions; `r_rtGIProbeDump`.
- **Exit:** per-pixel GI unchanged; probe spheres sit on a stable world-space lattice
  that does not slide when the camera moves; dump memory matches B.1's numbers.
- ✅ Mode 4 green and world-locked; dump matches B.1 exactly (16384 probes, 128×128
  tiles, irradiance 1280×1280 / 12.5 MiB, distance 2304×2304 / 20.2 MiB, 35.2 MiB
  total, `sizeof(GIProbeParamsUBO)` 240/240).
- **As built, deviating from B.1/B.5:**
  - **Probe bindings live in their own set 2, not `giDescLayout` bindings 6-8.**
    `gi_ray.rgen` then never sees them, and — the deciding reason — set 0's params
    block is a UNIFORM_BUFFER_**DYNAMIC**, so folding probe params in beside it would
    put two dynamic offsets in one `vkCmdBindDescriptorSets`. One
    `giProbeDescLayout` object is bound at **two different set indices**: set 2 of the
    RT pipeline, set 0 of the blend/border compute pipelines. `gi_probe_common.glsl`
    keys on a `GIPROBE_SET` define so the binding contract is written once.
  - **The lattice is absolute and storage is toroidal.** A probe's world position is
    `cell * spacing` on an infinite lattice — `baseCell` only says which window is
    resident — and its atlas tile is `cell mod gridDim`. Scrolling therefore leaves
    every surviving probe on its own tile and invalidates only the slab that wrapped.
    B.1's "snapped, scrolling" grid without this needs the whole atlas rewritten per
    scroll.
  - The G0 overlay lives in `gi_probe_resolve.comp` (debug modes only) rather than a
    separate shader, mirroring what F1 did for the froxel resolve. It **adds** onto
    the lit scene instead of replacing it: what G0 is checking is where probes sit in
    the world, and a black background makes that harder to judge, not easier. No GI
    composite debug/replace pipeline was needed.
  - `TRACED`/`INSIDE` flags are CPU-owned and uploaded pre-trace, because the blend
    pass needs "did this probe have history *before* this frame" and only the CPU
    knows both the schedule and the scroll.

### G1 — tracing into the atlas ✅ **landed + in-game validated 2026-09-13**
Second rgen group + SBT offset; `gi_probe_trace.rgen` (spherical-Fibonacci directions
with a per-update random rotation); `gi_probe_blend.comp` with EMA; `gi_probe_border.comp`.
Round-robin scheduling only.
- **Exit:** debug mode 1 shows probes converging to plausible room colours within
  ~16 frames; a probe in a red-lit room reads red; composite still untouched. ✅ all three.
- **Scheduling: a SINGLE cursor advanced once per frame, not per-slot.** The
  per-slot rule exists because a `tr.frameCount`-derived index aliases with
  `vk.currentFrame` and pins each slot to one fixed subset of a *per-slot* resource
  forever. The probe atlases are a single shared pair, so there is nothing to pin —
  and two per-slot counters would be actively wrong here, each walking the same
  stride onto the same probes and leaving the rest never updated.
- **Validation (what was actually tested, and what was not):**
  - **Octahedral handedness is proven offline, not by eye.** The question reduces to
    one property: the blend writes texel *i* via `decode`, the resolve reads
    direction *d* via `encode`, so a *globally* mirrored map is harmless and only a
    MISMATCH tilts the stored lighting. A CPU transcription of
    `gip_OctEncode`/`Decode`/`TexelDirection`/`AtlasUV` round-trips to 4.4e-16 over
    the sphere, and texel→dir→texel lands back on the same centre for both sides
    (10 and 18). Re-run it after any edit to that file; it needs no engine build.
  - Also checked: trace and blend regenerate ray directions independently rather
    than storing them, and their `gip_RandomRotation` seed expressions are
    byte-identical.
  - **Not tested at all: the distance/visibility atlas.** Every check above reads
    the irradiance atlas. Its *addressing* is proven (side-18 round-trip, error 0.0)
    and it shares `gip_BorderSource`, but the `pow(dot, sharpness)` lobe and whether
    the stored distances are in a sane range are unverified, and nothing consumes
    them until G3. A debug mode 5 tinting spheres by mean distance / `maxRayDist`
    is the cheap fix, and belongs before G3's Chebyshev work. ✅ **shipped with
    G2 as `r_rtGIProbeDebug 5` and in-game validated 2026-09-14**: blue→red ramp
    of the stored mean, plus magenta where `mean2 < mean²`, which is
    algebraically impossible for a real second moment and so separates
    "mistuned lobe" from "broken moments". No magenta anywhere; red tracks open
    space (and so *appears* to track lights, because fixtures sit in the open
    volume — the moments have no light term at all); scrubbing
    `r_rtGIProbeMaxRayDist` 64/4096 at `r_rtGIProbeHysteresis 0` moves the whole
    picture as it must, which is what proves the normalisation is live.
    **G3 may now rely on the distance atlas.**
  - **Writing that overlay found the bug it was for, before it was ever run.**
    The atlas is `rg16f`, max finite value 65504; the blend pass stored the
    second moment in raw world units, so a miss-dominated probe at the default
    `r_rtGIProbeMaxRayDist 512` stored `512² = 262144` — **`+inf`**. G3's
    Chebyshev test would have been built directly on it. Fixed by storing both
    moments normalised by `maxRayDist`, i.e. `mean(d/D)` and `mean((d/D)²)`,
    each in [0,1]; Chebyshev is scale-invariant provided G3 divides the
    receiver→probe distance by the same `D`. The irradiance atlas was never
    affected — radiance is O(1). Consequence to remember: changing
    `r_rtGIProbeMaxRayDist` now re-scales the atlas and the EMA needs about a
    full refresh to wash the old encoding out.
- **Findings that shape G2/G3 tuning:**
  - **Glass stops a probe ray and returns near-black.** `gi_ray.rahit` discards only
    *alpha-tested* geometry and explicitly accepts everything else; `rchit` then
    returns glass's diffuse albedo × irradiance. So a probe behind a window reads
    dark and a brightly lit room does not bleed through the glass at all. There is
    no specular anywhere in this path — mirrors and polished metal likewise
    contribute only their (usually dark) diffuse albedo.
  - **The shared `rchit` is a high-variance estimator with only the EMA behind it.**
    At `r_rtGIStochasticLights 2` each hit samples 1-2 lights and divides by the
    selection probability; the per-pixel path pays for that with a three-pass
    denoise chain, probes have one EMA. Visible as colour flicker at
    `r_rtGIProbeHysteresis 0`, damped but not gone at 0.97. Probe ray budget and
    stochastic light count trade against each other differently than they do
    per-pixel — that is a G6 retune item, not a bug.
  - **Emissive surfaces short-circuit** in `rchit` at `r_rtGIEmissiveScale`, ignoring
    albedo and lighting, so a glowing panel dominates nearby probes far more than the
    wall beside it. Good for Doom 3 (which lights heavily with emissive panels), but
    it means probe brightness does not track `r_rtGIDirectScale` the way the
    per-pixel path's does.
  - **Constant trap:** `gi_probe_trace.rgen` uses `max(rays.x, 1)` and
    `gi_probe_blend.comp` uses `clamp(rays.x, 1, GIPROBE_MAX_RAYS)` when generating
    spherical-Fibonacci directions. They agree only because the CPU clamps
    `r_rtGIProbeRays` to `VK_GIPROBE_MAX_RAYS` and both caps are 256. Move one
    without the other and the two shaders silently generate DIFFERENT directions —
    the atlas fills with plausible-looking garbage and nothing errors.
- The scratch ray image is the debugging surface here — dump/visualize it before
  chasing an atlas bug.

### G2 — resolve switch ✅ **landed and measured 2026-09-14**
`gi_probe_resolve.comp`: reconstruct position + normal (G-buffer normal, fall back to
`rt_ReconstructNormal`), offset by `r_rtGIProbeNormalBias`, fetch the 8 surrounding
probes, weight by trilinear × `max(0, dot(n, probeDir))` smoothed, normalize, write
`giBuffer`. `r_rtGIProbes 1` skips the rgen dispatch, temporal, and à-trous.
- **Exit:** A/B screenshots in the ALB corpse room and the AREA doorway/zig-zag
  corridors at matched `r_rtGIStrength`; profiler capture (`GI`, `GITemporal`,
  `GIAtrous` should read ~0; `ProbeTrace/Blend/Resolve` are the new cost).
- **Expected regressions:** contact darkening is gone (AO's job now) and light bleeds
  through thin walls — that is G3's whole purpose. Do not tune constants here.
- ✅ **Measured 2026-09-14, in-game A/B clean.** Medians over 75 probe / 63
  per-pixel in-game frames (menu frames excluded on `TLAS`):

  | | per-pixel | probe |
  |---|---|---|
  | GI | 4.360 | 0.001 |
  | GITemporal | 0.132 | 0.001 |
  | GIAtrous | 0.376 | 0.056 |
  | ProbeTrace | — | 0.048 |
  | ProbeBlend | — | 0.075 |
  | ProbeResolve | — | 0.524 |
  | **GI chain** | **4.906** | **0.705** |

  **7.0×** on the median, 5.0× worst case (6.57 → 1.31). Whole RT block's median
  8.53 → 4.46 ms. Two numbers are confirmations rather than measurements: `GI`
  reads 0.001 not 0, which is the predicted barrier-only residue proving set 0 is
  still built for the probe trace; and `GIAtrous` reads 0.056 not 0.001, which is
  **albedo mod running alone** — the direct evidence the reorder above works.
- **`ProbeResolve` is flat at ~0.52 ms regardless of view.** GI cost no longer
  swings with light count or scene complexity, the same scene-independence
  `FroxelIntegrate` gained in F2, and worth as much as the mean drop.
- **The cost model inverted, which changes what G6 tunes.** B.6 predicted
  `ProbeTrace` would dominate with `r_rtGIProbeUpdatesPerFrame` as the throttle.
  Trace + blend are 0.12 ms combined; the resolve is 0.52, i.e. **74 % of the
  chain sits in the one pass no probe CVar affects.** 131 k rays in 0.048 ms is
  ~2.7 Grays/s, ~3× the per-pixel path's per-ray rate, which says that launch is
  latency-bound not throughput-bound — so raising `r_rtGIProbeRays` /
  `UpdatesPerFrame` for quality should be near-free until it saturates. Test that
  in G6 before trading anything else for probe quality. If the resolve ever needs
  to come down, the cost is 8 scattered bilinear `rgba16f` taps per pixel at full
  res (a +1 in Z is 1024 tiles away in the atlas), not arithmetic — the octahedral
  encode and storage-index recomputation were already hoisted out of the loop.
- **As built:**
  - **`gi_albedo_mod` moved to AFTER the probe resolve** in `vk_backend.cpp`.
    It is the one downstream pass that consumes the resolve's output, and the
    resolve has to stay after temporal/à-trous (G1's rule: an overlay written
    before them lands in their history). It shares the `GIAtrous` profiler
    phase as before — two begin/end pairs on one phase accumulate. The
    per-pixel path is unchanged by the move: albedo mod skips itself whenever a
    probe *overlay* owns `giBuffer`, which is the only case the reorder
    touches.
  - **`VK_RT_DispatchGI` is NOT stood down, only its `vkCmdTraceRaysKHR` is.**
    The probe trace is a second raygen in that same pipeline and reaches
    `gi_ray.rchit`, which reads set 0's GIParams/TLAS/light SSBO — the UBO
    build, `s_giParamsOffset` publication and descriptor refresh are what make
    the probe launch legal. `GI` therefore reports the barriers, not ~0 exactly.
  - The resolve applies **`r_rtGIStrength` *and* `r_rtGIContrast`**, identically
    to `gi_ray.rgen`, so the A/B compares sampling structures rather than two
    tone curves. `r_rtGIContrast` lost its `static` for this; `GIProbeParams`
    grew a `tune2` vec4 (240 → 256 bytes) with three slots reserved for G3.
  - **No-data case: black, deliberately.** An untraced probe's atlas tile still
    belongs to whichever probe last occupied it (toroidal storage), so it is
    another room, not "black" — it is excluded, and if all 8 are excluded the
    pixel gets zero GI. The risk table's "must fall back, not darken" has no
    better option available at this stage: the only in-grid data is wrong-room.
    This is bounded — it is the first ~16 frames after a map load, and the
    scroll-invalidated slab is always at the far edge of the window, never near
    the camera. `r_rtGIProbeDebug 4` locates it if it ever shows up in play.
  - Temporal's probe early-out also clears `giHistoryValid`, so flipping
    `r_rtGIProbes` back to 0 resumes from the current frame instead of blending
    against a history that is arbitrarily many frames old.

### G3 — leak hardening (mandatory, pillar 2) ✅ **gate passed 2026-09-15**
Mode 3 reads mostly green, with red only in corners (residual leak, a bias tune for G6)
and magenta on distant walls — the latter is the grid's finite coverage, not a defect:
those cells fall outside the 2048x2048x1024 window and go green on approach. The gate
only became judgeable after the two G4 defects below were fixed.
Chebyshev visibility weighting from the distance moments; **ship the leak overlay
first**: mode 3 = leak detector, mode 2 = per-pixel probe weights. Then tune
density/bias against the overlay.
- **The moments are normalised by `r_rtGIProbeMaxRayDist` (see G1).** The
  receiver→probe distance is divided by the same `D` before the ratio; the `D²`
  in the variance then cancels the `D²` in the squared difference. Getting this
  wrong is silent — the test either does nothing or blacks everything out.
- **Mode 3 is NOT "GI present where direct light is zero" as B.5 specified.**
  That was a proxy, and a weak one: the resolve has no direct-light buffer, and
  evaluating the light list without shadows *overestimates* lit-ness, so it would
  miss exactly the leaks it exists to find. Instead mode 3 traces a **ray query
  from the receiver to each of the 8 probes** for ground-truth visibility and
  colour-codes the disagreement with Chebyshev's estimate:
  - **green** — they agree, whatever the verdict.
  - **red** — ray says occluded, Chebyshev let it through → **leak**, pillar 2.
  - **blue** — ray says visible, Chebyshev blocked it → over-darkening; lost
    energy, not a pillar violation.

  Weighted by each probe's trilinear share, so the colour says "how much of this
  pixel's GI is wrong", not merely "something disagreed". This is strictly
  stronger than the proxy: it measures the mechanism, and it validates the
  approximation against truth rather than against a correlate.
  **`r_rtGIProbeVisibility 0` shows the raw leak surface** with no mitigation —
  the before-picture to tune against; `1` shows the residual. Costs up to 8 ray
  queries per pixel, overlay only.
- **This put the TLAS in the resolve descriptor set (binding 7).** Only mode 3
  reads it, but the shader references it statically, so it must be a live handle
  on every dispatch — the resolve now requires a valid TLAS, the same
  requirement `VK_RT_DispatchGI` already had, so a frame without one already had
  no GI.
- **Added `r_rtGIProbeViewBias` (default 8.0, `tune2.y`)** alongside
  `r_rtGIProbeNormalBias`. Without a view bias a wall seen at a grazing angle
  reads as its own occluder and Chebyshev over-darkens it. Clamped below half
  spacing: a larger bias walks the sample into the next lattice cell, so the
  trilinear weights would describe a point the receiver is not at. **These two
  biases are the tuning levers** — mode 3 blue along walls means raise the view
  bias, red means it went too far.
- Chebyshev is floored at **0.05, not 0** (`GIPROBE_VIS_FLOOR`). At zero the
  interpolation goes discontinuous across the wall and the seam reads worse than
  the residual leak. Same constant DDGI uses.
- Mode 3's pass/fail threshold is `cheb > 0.5`, a classification for the overlay
  only — it does not appear in the resolve.
- **First run, 2026-09-14 — two fixes, one real and one overlay-only:**
  - **REAL, shipping path.** When no probe ray landed in a distance texel's
    lobe, `gi_probe_blend.comp` wrote `vec2(1.0, 1.0)` — mean = `maxRayDist`,
    variance 0, i.e. "nothing in the way". `gip_Chebyshev` returns 1.0
    unconditionally for that, so **the fallback for missing data was the
    leak-permissive one.** At `r_rtGIProbeRays 128` against a 16×16 interior
    there are *fewer rays than texels* (~3 rays per `pow(dot,50)` lobe, and a
    few per cent of texels get zero), so this fired constantly. Now the texel
    keeps its history instead; an as-yet-unwritten one reads mean 0 → fully
    occluded, which is the conservative direction.
  - **Overlay-only.** `gip_ProbeOccluded` passed `gl_RayFlagsOpaqueEXT` and
    confirmed every candidate, forcing perforated and translucent geometry to
    read solid — but `gi_ray.rahit`, which built the moments, *discards*
    alpha-tested geometry below threshold, so grating holes and decals do not
    occlude a probe ray. Mars City's floors are grating, so the overlay reported
    a screen of "leaks" that were Chebyshev being correct. Now only opaque
    geometry occludes. Residual mismatch, accepted: rahit accepts non-alpha-
    tested non-opaque geometry (glass), which now reads visible — that turns a
    real leak green or blue rather than inventing a red one, and for a leak
    detector under-reporting on rare geometry beats crying wolf on every floor.
  - **Reading rule that falls out of this:** red on *opaque* geometry is a real
    leak; the earlier wall red is real and still open. Blue is always real.
    Green over perforated geometry can now hide a real over-darkening.
- **Exit (unchanged):** the overlay is clean in the sealed-room and closed-door
  cases of the 2026-08-22 test walk. If leaks survive at sane densities, fall
  back to per-area probe isolation (a probe contributes only to pixels in areas
  its own area reaches through open portals — the same BFS set
  `VK_RT_UploadGILights` already walks).
- **Exit:** the overlay is clean in the sealed-room and closed-door cases of the
  2026-08-22 test walk. If leaks survive at sane densities, fall back to per-area
  probe isolation (a probe contributes only to pixels in areas its own area reaches
  through open portals — the same BFS set `VK_RT_UploadGILights` already walks).

### G4 — probe classification ✅ **done 2026-09-15** · relocation dropped
Give probes that cannot see usable geometry zero weight in the resolve. Two separate
populations, two flags, both folded into `GIPROBE_FLAG_UNUSABLE`:
- `INSIDE` — mostly back-face hits. `r_rtGIProbeInsideThreshold` (0.25, matching RTXGI).
- `OUTSIDE` — mostly misses, i.e. adrift in the void. `r_rtGIProbeOutsideThreshold`
  (0.9, deliberately high: a probe under open sky legitimately misses ~half its rays).
  Doom 3 maps are hollow shells, so "not in a room" usually means void, not solid —
  this is the larger population by far, and backface counting structurally cannot see
  it (a miss sets backface 0, reading as wholesome open air).

Both are EMA'd with the map alpha, taken outright when a probe has no history, and
cleared on scroll. Each has a **dead band** (cleared below 0.75x the threshold) so a
probe on the boundary cannot flip-flop. 0 disables either.

**As built:**
- Statistics live in a **shared** `GIProbeStats` SSBO (probe layout binding 5, resolve
  binding 8), with a per-slot readback snapshot the CPU reads after the fence.
  `GIProbeState` stays 16 bytes and CPU-owned. An EMA accumulator must not live in a
  per-slot buffer; see the oscillation below.
- Overlays: mode 4 = verdict (x-rays flagged probes at half brightness, so dim = behind
  geometry, bright = in open air i.e. misclassified); **modes 6/7 = the RAW backface /
  miss fraction** on a ramp. 4 alone cannot separate a bad statistic from a mistuned
  threshold, which is exactly the failure below. Mode 3 paints **magenta** where all 8
  neighbours are excluded — silence there would read as "fine" when it is the worst case.

**Two real defects found, both of which had been corrupting the shipping path:**
1. **Backface was inverted** (`gi_ray.rchit`). Doom 3 winds front faces opposite to the
   GL/Vulkan convention — `GL_Cull` culls `GL_FRONT` for `CT_FRONT_SIDED` — so every
   VISIBLE surface reports as `gl_HitKindBackFacingTriangleEXT`, and
   `TRIANGLE_FACING_CULL_DISABLE` on the TLAS instances does not change that. Now taken
   from the vertex normal (`dot(hitNorm, rayDir) > 0`), which needs no convention.
   **Not only a G4 bug:** `gi_probe_blend.comp` excludes back-face rays from the
   irradiance, so since G1 the blend discarded the rays that hit lit surfaces and
   integrated the leftovers. Probe GI is markedly brighter now and G6's retune is
   mandatory. The per-pixel path never read `backface`, hence only probes looked dark.
2. **2-frame oscillation.** G4 parked its GPU-written statistics in the per-slot
   `GIProbeState`; the CPU read slot N+1, found the older generation it had uploaded
   itself, and copied it back — so the classification alternated at frame rate, for
   every probe at once. A dead band cannot damp that: both values are legitimate.
   Symptom to remember: a **2-frame period** means a per-frame-in-flight resource, never
   an EMA (τ here ≈ 533 frames).

**State after the fix:** `usable=2959 (18.1%) insideGeometry=1088 (6.6%)
outsideLevel=12337 (75.3%)`, `backface mean=0.052 max=0.499`, `miss mean=0.891`.
18% usable is not alarming and 75% void is not a bug — the window is a 2048-unit cube
centred on the camera and most of it is outside the level. What matters is whether
receiving surfaces keep neighbours, and mode 3's green says they do.

**Relocation dropped.** `backface max` never exceeds 0.5, so nothing is genuinely
buried — a probe inside a brush would read ~1.0. The 1088 `INSIDE` probes are near-hull
void probes seeing the shell's outside with about half their rays. Harmless (both flags
mean unusable), but it means there is nothing to relocate; revisit only if a map shows
`backface` approaching 1.0.

### G5 — scheduling, and dynamic-light latency
Priority queue instead of round-robin: probes in areas reached by the portal BFS
update first; a portal-state change (door opens) jumps its areas' probes to the front.
- **Exit:** opening a door relights the room behind it within a few frames rather than
  a full rotation; dump shows queue depth and the priority bumps.

**Probe GI cannot currently track a flickering or moving light, by ~2 orders of
magnitude.** At the defaults, 16384 probes ÷ 1024 per frame = 16 frames between
updates for a given probe, and `r_rtGIProbeHysteresis 0.97` is alpha 0.03 per
*update*, so τ = (1/0.03) × 16 ≈ **533 frames ≈ 8.9 s at 60 fps**. A 10 Hz
flicker converges to its mean. Direct lighting still flickers correctly (pillar 1
is untouched), so the symptom is that a light dropping to black leaves a steady
bounce glow behind — a **pillar-2** problem, not just fidelity. The same wall
applies to a moving light: the EMA lags in *world* space, so the glow stays where
the light was and decays over seconds. World-space reuse did not remove ghosting,
it changed its coordinate system — screen-space ghosts follow the camera, these
follow the world.

The hysteresis is doing **two jobs and only one is wanted**: it suppresses
*estimator* noise (128 rays, `r_rtGIStochasticLights 2` sampling 1-2 lights per
hit and dividing by the selection probability — G1 measured the colour flicker
that appears without it) and low-passes the *scene* purely as a side effect. So
"lower the hysteresis" is not the lever. **Two fixes, in this order:**

1. **Make each update low-variance enough that heavy smoothing is unnecessary.**
   **No code** — `r_rtGIProbeRays 256`, `r_rtGIProbeUpdatesPerFrame 4096`,
   `r_rtGIProbeHysteresis 0.5`. Both clamps already accept those values
   (`vk_gi_probe.cpp:320`, `:325`); only the scratch realloc follows. A 4-frame
   rotation at alpha 0.5 is τ ≈ 8 frames ≈ 130 ms — a door or a slow pulse, not a
   10 Hz strobe (4096/16384 is a 15 Hz sample rate, Nyquist 7.5 Hz).

   ✅ **Measured 2026-09-19**, medians over 62-82 in-game frames per config, moving
   through Mars City. `Shadows` swings 0.08-3.8 ms in the same capture, so only the
   Probe rows are readable from it.

   | Name | G2 | Expected | 128×1024 | 256×1024 | **256×4096** |
   |---|---|---|---|---|---|
   | ProbeTrace | 0.048 | ~0.4 | 0.052 | 0.070 | **0.146** |
   | ProbeBlend | 0.075 | ~0.6 | 0.087 | 0.158 | **0.499** |
   | ProbeResolve | 0.524 | 0.524 | 0.609 | 0.662 | **0.629** |
   | scratch | 2 MiB | 16 MiB | 2 MiB | 4 MiB | **16 MiB** |

   **+0.51 ms for 8× the rays** — cheaper than expected. GI chain 0.75 → 1.27 ms,
   still ~4× under the per-pixel path's 4.9. Three things this settles:

   - **The trace is latency-bound, confirmed.** 2× rays costs 1.35×, then 4× probes
     costs 2.1× — 8× the work for 2.8× the time.
   - **The blend is now the dominant probe pass** (0.499 vs trace 0.146) and scales
     nearly linearly: 8× work, 5.7× time. Ray count is paid for in the blend, not the
     trace — inverting B.6's prediction a second time, in the other direction.
   - **Resolve is flat** at 0.609/0.662/0.629 across all three, as G2 said. That ±0.05
     spread is view noise and sets the floor: smaller differences are not real.
2. **Adaptive hysteresis in `gi_probe_blend.comp`** — the standard DDGI answer
   and a few lines. Compare the new value against `prev` and boost alpha when the
   relative change is large, so a probe *snaps* to a genuine lighting change while
   still smoothing small noise. RTXGI ships this. The tuning risk is that
   stochastic light selection also produces occasional large spikes, so the
   threshold must sit above the estimator's own tail — which is why fix 1 comes
   first.

Trigger-wise this is the same mechanism as the portal bump above: a light whose
colour changed this frame should jump its neighbouring probes to the front of the
queue, so it belongs here rather than in G6.

❌ **Fix 1 does not reach flicker — confirmed in play 2026-09-19.** At
`256 × 4096 × hysteresis 0.5` a 5 Hz light's GI still does not keep up. 
Fix 1 is a door and moving-light fix only.

**A third fix was proposed 2026-09-18 and supersedes fix 2 for the flicker case
specifically — see G5b.** Fix 2 (adaptive hysteresis) remains the right answer for
*doors and moving lights*, which G5b cannot help, and **fix 1 gates fix 2** — its
threshold is a GPU-side comparison against a noisy estimate.

**Fix 1 does not gate G5b** (corrected 2026-09-19). G5b's classifier is a CPU-side
colour delta on exact `idRenderLightLocal` data, no estimator involved, and the
bucket split is energy-conserving ray-for-ray: at `s = 1` the two buckets sum to
today's value from the same reservoir picks, at the same alpha. Per-bucket relative
variance rises; the total does not. Build G5b first; run fix 1's CVar sweep beside
it as a separate A/B.

### G5b — flicker factorization  🟡 written 2026-09-19, not yet run

A probe is re-traced every 16 frames and blended at alpha 0.03, so τ ≈ 8.9 s. A flickering
light's bounce converges to its mean and a dark room keeps a glow. Pillar 2. Shipping since
G6 flipped probes on.

**Two EMA rates does not fix it.** 16384/1024 = 16 frames per probe = a 3.75 Hz sample rate,
Nyquist 1.875 Hz. Doom 3 flicker runs above that, so a fast bucket with high alpha aliases
rather than tracks. Raising the update rate enough to clear it costs 4-8× the probe budget
and re-imports the estimator noise G1 measured.

**The mechanism.** A flicker is a scalar on a static light, and the CPU knows it exactly
every frame. Split the stored irradiance:

```
E_p = Σ_stable L_j·G_pj  +  s(t) · Σ_fast L̂_k·G_pk
                          └─ cached NORMALISED (s=1) ─┘
```

`G` is geometric transport, constant while nothing moves. Caching the fast bucket with the
flicker divided out makes it time-invariant, so it uses the **same slow hysteresis**, no
extra rays, and the resolve multiplies by the current `s(t)`. 

`L̂` has to be what the shader *reads*, not a correction applied after: the stochastic
reservoir weights each light by its current contribution luminance
(`rt_light_eval.glsl:321`) and `continue`s at `w <= 0`. A fast light in its dark phase
would never be picked — the bucket starves — or be picked at tiny `p` and spike through
the `wSum/w0` division. Storing `L̂` makes the selection time-invariant too, which reduces
the shader change to pure routing by flag.

Covers flicker/pulse/strobe in place and full switch-off. A light switching *on* costs one
refresh (~0.27 s) to build `G`. A **moving** light changes `G`, not `s` — not factorizable,
stays in the stable bucket, and the classifier must reject it on the origin test or it will
smear it confidently.

**Changes**

| File | Change |
|---|---|
| `vk_gi.cpp` classify | per-lightDef: last colour, ~2 s running max, last origin/axis. Fast if relative colour change > threshold within the hold window **and** origin/axis unchanged. `L̂` = running max, `s` = current/max, clamp `s ≤ 1`. Key off `idRenderLightLocal::index` like `s_lightSelectedFrame` (`vk_gi.cpp:1287`) |
| `GILightEntry` | `colorIntensity.rgb` = `L̂`; `s` and bucket index go in the **existing** `uint32_t pad[2]` (`vk_gi.cpp:187`) + flag bit `GI_LIGHT_FAST`. **No size change** |
| `GIProbeParams` | `vec4 fastGain[K]` — the *resolve* reads `gp`, not set 0's `GIParams`. `tune2.zw` is free for K ≤ 2 |
| `rt_light_eval.glsl` | route contribution to accumulator 0/1 by the flag — **bucket selector is a parameter**, not hardcoded. Apply `* s` inside `rt_LightContribAt` for every non-probe caller |
| `vol_froxel_fill.comp:146`, `vol_march.comp:333` | own light loops, not via `rt_LightContribAt` — need `* s` by hand |
| `gi_payload.glsl` | fold `backface` into `sign(hitDist)` (the scratch already encodes it that way) → 16 B, then +1 packed `uint` for the fast radiance. GLSL has no RGB9E5 intrinsic; hand-roll (~8 lines) for 20 B, or `uvec2`/`packHalf2x16` for 24 B |
| `gi_probe_blend.comp` | blend both buckets at the same hysteresis; cache fast rays as packed `uint` (`s_fast[256]`, +1 KB shared, not +3 KB); distance atlas **not** duplicated |
| `gi_probe_resolve.comp` | `E = E_stable + Σ_k E_fast[k]·fastGain[k]`, sharing the tap weights |

**Address bucket `k` at `idx + k*probeCount` and double `s_tilesY`** — not a layer array.
`gip_AtlasUVOct` is then unchanged and no `image2DArray`/`sampler2DArray` retype crosses
blend, border and resolve. The border pass grows its `gl_WorkGroupID.y` range instead.

**Cost** (16384 probes, K=1). Rays unchanged; that is the point.

| | today | after |
|---|---|---|
| Irradiance atlas | 13.1 MB | 26.2 MB |
| Distance / light SSBO | 21.2 MB / 20.5 KB | unchanged |
| Scratch | 1.0 MiB/slot | +0.5 MiB/slot (`rg11b10f`) |
| **ProbeResolve** | **0.609 ms** (2026-09-19) | **~0.9-1.2 ms** |
| ProbeBlend | 0.087 ms | ~0.13 ms — only the irradiance loop doubles; the `pow()` distance loop is untouched |

The resolve is the real cost, and the earlier "capacity only" claim was wrong on its own
terms: G2 measured it **issue-rate bound on 8 scattered bilinear taps**, and K=1 makes it
16 (`gi_probe_resolve.comp:395`). That, not the 13 MB, is the argument against K=2. The
payload is no longer the risk — packing it keeps 20 bytes.

Deriving `L̂` from a running max makes this self-calibrating — works for `flicker`,
`pdflicker`, hand tables and script `setShaderParm` with no material parsing.

**Transients, by design**

- **First classification costs a ~9 s crossfade, not a pop.** At the flip the stable
  bucket still holds that light steadily (today's bug) while the fast bucket is empty;
  both EMAs converge at τ 8.9 s. The dark-room check below cannot be read until then,
  and classifier flapping re-triggers it — that is what `r_rtGIFlickerHold` buys.
- **A rising `L̂` rescales everything already cached in the fast bucket.** Bounded to a
  light's first flicker cycle (~0.2 s) and corrected over one refresh.
- `s` pins at 1 while `L̂` is still below peak, so the first peak reads under-bright.

**CVars**

| CVar | Default | Meaning |
|---|---|---|
| `r_rtGIProbeFastBuckets` | `1` | K separately-cached buckets; `0` = today's behaviour, the off switch. ~~One gain is shared per bucket, so K=1 is approximate when two lights flicker out of phase within a probe's reach: the gain is the `L̂`-weighted mean of `s` over the bucket's lights, which is exact for one fast light and degrades smoothly.~~ **Corrected in the as-built table: that mean is wrong, not approximate. One light per bucket** |
| `r_rtGIFlickerThreshold` | `0.15` | relative colour change that classifies fast |
| `r_rtGIFlickerHold` | `2.0` | ~~seconds a light stays fast after its last change~~ **as built: the `L̂` decay time constant. The classification latches instead** |
| `r_rtGIProbeDebug 8` | — | **ships first:** fast-bucket fraction per probe (blue→red) + count of distinct fast lights reaching each probe. The count says whether K=1 suffices |

🟡 **Written 2026-09-19, not yet in-game validated.** As built, deviating from the above:

| Item | As built |
|---|---|
| K | **Clamped to 0..1**, not 0..2. The argument against K=2 is the resolve's tap count, and mode 8 is the measurement that settles it — so K=2 is not written speculatively. `VK_GI_MAX_FAST_BUCKETS` is the one place to raise |
| Lights per bucket | **Exactly one**, and this corrects the plan. "`L̂`-weighted mean of `s` over the bucket's lights" is not a smooth degradation, it is wrong in the pillar-2 direction: two out-of-phase flickering lights in *different rooms* each receive the mean, so the lit room's bounce halves and the dark room's doubles. A single scalar can only be exact for a single light. The highest-importance flickering light is bucketed; the rest keep their gain everywhere else and their probe GI reverts to pre-G5b behaviour — no regression, just no fix. Lifting this needs a per-**probe** gain (one more float in `GIProbeStateEntry`), which mode 8's red channel is now the evidence for |
| Two flags, not one | `GI_LIGHT_FLAG_FAST` = "rgb is `L̂`, multiply by `fastScale`" and is read by *everything*; `GI_LIGHT_FLAG_FAST_BUCKET` = "this one's transport is cached normalised". Conflating them would have left every non-bucketed flickering light rendering at its **peak** in volumetrics and reflections — permanently bright shafts, the exact regression the plan flagged as highest-visibility |
| Classification latches | It does not expire after `r_rtGIFlickerHold`. A light falling back to the stable bucket hides an already-converged fast cache and reveals an empty stable one: a visible drop plus the ~9 s rebuild this chunk exists to remove. It latches until the light changes shape, and a light that stops flickering simply sits at `s = 1`. `r_rtGIFlickerHold` now governs only the `L̂` decay |
| Re-seed triggers | A transport-field **shape hash** (mirroring `UpdateLightDef`'s own test — origin, axis, lightCenter, lightRadius, target/right/up, start/end, type, shader) rather than origin+axis, which missed radius and target changes; plus a `renderView.time` **rollback** test, which is what actually catches a lightDef index reused by a new map (`tr.frameCount` does not reset across a load, and `VK_RT_InitGI` is renderer init, not a map hook). A gap in sightings refreshes timestamps but **keeps** the verdict — dropping it made re-entering a room cost a full re-earn |
| `L̂` and its luminance | One field. The first cut decayed a `peakLum` scalar while leaving `peakColor` at the old peak, which breaks `L̂ · s == current colour` — at peak luminance 1, decayed denominator 0.5 and current 0.1 the shaders reconstruct 0.2. The decay is applied to the colour and the luminance is derived |
| Bucket routing | Not a bucket-selector parameter. `rt_LightContribAt` returns the **unscaled** contribution; both eval loops gained a `...Buckets` variant returning `stable` / `fastNorm` / `fastScaled`, and the old signatures survive as wrappers returning `stable + fastScaled`. Reflections and the froxel/march loops are therefore behaviour-unchanged by construction |
| Probe vs per-pixel | `gi_ray.rchit` is shared, so the caller has to say which it is: `GIPayload.fast` is read as a mode bit on the way **in** (`GI_PAYLOAD_PROBE_MODE`, set only by `gi_probe_trace.rgen`) and written as packed radiance on the way **out**. One word, no payload growth |
| Payload | 20 B: `vec3 colour` + `float hitDist` (back-face in its sign, as planned) + `uint fast`. Pack is a hand-rolled RGB9E5 in `gi_payload.glsl` |
| Fast scratch | **`rgba16f`, not `r11f_g11f_b10f`.** `B10G11R11` storage-image support is optional in Vulkan and a GLSL format qualifier must match its view, so a runtime fallback would mean two shader variants. Costs 1 MiB/slot rather than 0.5 |
| Blend's ray cache | `shared vec3 s_fast[256]` (+3 KB), not the planned packed `uint`. With an `rgba16f` scratch, packing would buy 3 KB of shared memory at the price of an unpack per (ray × texel) in the hot loop |
| Atlas | As planned — bucket k at tile `idx + k*probeCount`, extra rows not an image array. But `atlas.y` had to stay the **per-bucket** tile count, because the distance atlas is not bucketed and shares the field. `gip_IrrAtlasUVOct`/`gip_IrrAtlasSize` are separate from the distance versions rather than keyed off `side` |
| Mode 8 | Green = fast share of resolved irradiance; **red = flickering lights whose volume contains the receiver, /3** — a containment test against the light SSBO, not a per-probe distinct count, which would have needed the light index carried through the payload. With one light per bucket, red now reads as "what G5b is still leaving EMA-smeared here", which is the evidence for per-probe gains. Needed a new resolve binding 9 (the GI light SSBO) |
| Realloc | K joins `s_probeDim`/rays/updates in `VK_RT_GIProbeGeometryChanged`, and `s_probeFastBuckets` initialises to **-1**, not 0 — K=0 is legal, so 0 would read as "already built" |
| Border dispatch | y extent is now `s_probeFastBuckets + 2`. The old constant 2 would leave the fast bucket's borders unwritten |

**Not bit-identical at `r_rtGIProbeFastBuckets 1`, only at 0.** The reservoir now weights
by `L̂` rather than the current colour, which is the intended fix for starvation but does
change the per-pixel estimator. The off switch is the A/B handle.

**Checks**

- Room lit only by a flickering light goes fully dark on the dark half, no residual glow.
  **Wait ~9 s after the light is first seen** — see the transients above.
- Flicker shape matches direct lighting frame for frame at `r_rtGIProbeUpdatesPerFrame 1024`
  — i.e. no faster sampling was needed.
- Non-flickering rooms identical to `r_rtGIProbeFastBuckets 0`.
- **Fan-blade light shafts still strobe, and reflections of flickering lights still
  flicker.** Both read the light SSBO outside `rt_light_eval.glsl` and will silently go
  steady if the `* s` is missed — the highest-visibility regression here.
- **`r_rtGIProbes 0` is bit-identical**: `gi_ray.rgen` leaves the bucket selector at 0, so
  all energy lands in bucket 0 and the per-pixel path is untouched.
- A light on a moving entity is not classified fast, behaviour unchanged.
- Resolve cost before/after, `r_vkRTProfile 1`, recorded here — expect it to roughly
  double, and decide K=1 vs K=2 on that number plus `r_rtGIProbeDebug 8`.

Same factorization would apply to the froxel vol cache if F4-style reprojection is revived.
Not scheduled.

### Known limitation — the per-pixel overlays are additive
Modes 2 and 3 are composited additively (`VK_RT_CompositeGI` has no replace pipeline)
and the direct-light interactions are drawn after it, so the lit scene tints them.
Read hue *ratios*, not absolute colour, and distrust them on bright surfaces. A proper
fix needs a replace composite plus suppression of the later scene draws — worth doing
only if G6's corner tuning turns out to be ambiguous because of it.

### G6 — default flipped 2026-09-19 · retire deferred · retune owed

**`r_rtGIProbes` now defaults to 1** (`vk_gi_probe.cpp:58`, commit 5d07e756). Decided on
look: probe GI reads better than the per-pixel path. Both prerequisites below were
**consciously waived, not met** — recorded so the artifacts they predict read as known and
scheduled rather than as new bugs.

- **G5b did not pass.** The veto stood on Doom 3 using flickering lights constantly as an
  atmosphere device; that is still true and G5b is still to be built. It is now an open
  defect against the shipped default instead of a gate in front of it.
- **Decided at native**, not at the intended shipping render resolution. The two candidates
  do not scale together: per-pixel GI is entirely screen-resolution work (4.41 → ~1.95 ms
  at 0.67 scale) while the probe trace is probe-count-bound and barely moves (G2: 0.048 ms),
  so lower render resolution *shrinks* the probe path's advantage. This only bites if the
  choice is made on cost; it was made on appearance, so `20260918_fsr_upscaling.md`'s U0 no
  longer has to precede this chunk.

**The per-pixel path stays.** No retire. `r_rtGIProbes 0` remains the A/B handle, the
fallback, and the instrument the retune below is measured with.

**Retune owed — cause confirmed 2026-09-19.** GI/vol read over-bright, and it tracks
`r_rtGIProbes 1`. The defaults still carry the per-pixel path's tuning; `r_rtGIStrength`,
`r_rtGIContrast` and the `r_rtGIAutoDirectScale` coupling all change meaning under probes.
Overlaps `rt_optimization_tuning.md` T4-T6.

**Exit:** brightness under control with the new constants recorded here.

## B.6 Known risks

| Risk | Signal | Mitigation |
|---|---|---|
| Light leaks through thin walls (pillar 2) | Mode-3 overlay lights up in sealed rooms | G3 Chebyshev → G4 relocation → per-area isolation, in that order |
| Loss of contact darkening | Corners and creases read flat vs today | AO carries it; retune AO strength in G6, not earlier |
| Camera-anchored grid pops as it scrolls | Visible lighting shift when the grid origin snaps | Snap to `spacing` multiples and clear only the newly-entered slab; the cleared probes are "never traced", not black — the resolve must fall back, not darken |
| Second rgen in a shared pipeline mis-indexes the SBT | Device lost, or probes get per-pixel GI's behaviour | Raygen region = `sbtBase + groupIndex·handleAlignedSize`, `size = handleAlignedSize`. Verify with a probe rgen that writes a constant before wiring the real one |
| Probe count × ray count blows the budget on large maps | `ProbeTrace` dominates | `r_rtGIProbeUpdatesPerFrame` is the throttle; convergence time degrades gracefully, cost does not. **Measured false in G2** — the trace is 0.048 ms and latency-bound; the resolve is 74 % of the chain and no probe CVar affects it |
| Flickering lights: the EMA low-passes them away (τ ≈ 8.9 s at defaults, 20 s to 90 %), leaving a bounce glow where a light went dark | Room does not go fully black when its light flickers off | **G5b — amplitude factorization.** Cache the flicker light's transport with its gain normalised out, rescale at resolve time. Zero latency, zero extra rays. Note that G5 fix 2 (adaptive hysteresis) does *not* solve this case: a probe is re-traced at 3.75 Hz, below the flicker's own frequency, so alpha cannot help. **Pillar 2. Was a blocker on G6; G6 shipped without it 2026-09-19. Written 2026-09-19, awaiting G5b's in-game checks** |
| Moving lights: the EMA lags in *world* space, so the glow stays where the light was and decays over seconds | A moving light drags a lagging world-space glow | G5's two fixes — narrow the estimator first, then adaptive hysteresis. **G5b explicitly cannot help here** (movement changes the transport, not just the amplitude), and its classifier must exclude moving lights or it will smear them confidently |
| Volumetrics | — | Does *not* have either problem: F4 was dropped and the froxel fill has no history at all |

---

## Rejected alternative — analytic shadow-volume integration

The originating idea (2026-08-23): per light, track its shadow volume and analytically
integrate the *lit intervals* along each view ray — zero sampling, zero noise, exact
edges (Tóth/Umenhoffer-style). Set aside because:

1. Stencil shadow volume generation is **disabled when RT shadows are on**
   (`useStencilShadows = !VK_RTShadowsEnabled()`, `vk_backend.cpp`; volume *creation*
   is skipped too) — this would re-add the CPU cost RT removed, per vol-admitted light.
2. Per-ray interval clipping against arbitrary (non-convex, capped) volume meshes needs
   the additive front/back-face trick, which yields shadowed *length* cheaply but not
   the distance-attenuated, phase-weighted integral we need — closed forms exist only
   for simplified phase/attenuation, and our per-class HG + Cauchy model doesn't reduce.
3. It scales per-light per-pixel; froxels scale per-cell with clustered lights.

Worth a second look for exactly ONE light if a future hero shot demands razor-sharp
noise-free shafts beyond froxel resolution. That's the niche.

---

## Hygiene

- New shaders: `.comp`/`.rgen` → `GLSL_SHADER_SOURCES`; shared `.glsl` → `GLSL_INCLUDES`.
- New files carry the dhewm3-rt GenAI copyright block.
- Old paths stay compiled and CVar-selectable until their replacement is validated
  in-game on the standard test rooms — delete in F5/G6, not before.
- Breadcrumb logging goes in when each mechanism is written, not after it breaks.
- Every chunk's overlay ships before anything it enables is tuned.
