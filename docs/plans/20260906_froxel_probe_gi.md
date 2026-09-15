# Froxel Volumetrics + Probe GI — world-space caching arc

**Date:** 2026-08-23 · **Detailed for implementation:** 2026-09-12
**Status:** Arc #2 in ROADMAP.md. The owed profiler checkpoint is taken
(Mars City 2026-09-11: GI 4.41 / Refl 3.24 / Vol 1.53 / AO 1.17 / denoise ~0.65 ms).
**Part A: F0-F2 landed and validated, F3 dropped, F4/F5 open.
Part B: G0/G1 landed and validated 2026-09-13. G2 written 2026-09-13, not yet
run. Next is in-game A/B + profiler for G2, then G3 (leak hardening).**

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

### F4 — froxel-space temporal + rotation
EMA against `froxelHistory` (single shared image, ordered by submission + an explicit
barrier — **not** per-slot), reprojecting the cell centre through `prevViewProj` into
the previous grid; a fetch outside the previous frustum falls back to the current
sample (no ghost trail, unlike screen-space). Per-cell jitter already exists from F0.
Optional `r_rtVolFroxelRotate`: update 1/N of Z slices per frame — its index **must**
key off a per-slot counter, not `tr.frameCount`.
- **Exit:** debug mode 4 (cell age) shows every cell refreshing at the expected rate;
  walking through a doorway produces no visible trail; the disocclusion case (spin
  180°) converges within a few frames.

### F5 — retire decision
From F1-F4 evidence: keep the march compiled behind `r_rtVolFroxel 0`, flip the
default, or delete `vol_march.comp` + `vol_bilateral.comp` from the froxel path.
Update ROADMAP.md and move this part to `completed/`.

## A.9 Known risks

| Risk | Signal | Mitigation |
|---|---|---|
| Beam edge crispness floors at XY resolution, and side-on shafts are the hero case | F2 A/B screenshots | Raise `ResX/Y` (linear cost, huge freed budget); keep the march selectable; only then consider deleting it |
| Shadow sampling at one cell centre blocks/aliases the shadowed part of a shaft | Blocky shaft boundaries that do not improve with more steps | Per-frame jitter within the cell + F4 EMA is the intended fix; a second occlusion sample per cell is the fallback |
| Cells straddling a wall leak fog through it | Fog visible in front of geometry | Depth-clamped resolve (F2); if it survives, weight the last slice by the fractional depth position |
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
| `r_rtGIProbeDebug` | `0` | 1 = probe spheres tinted by stored irradiance, 2 = per-pixel probe weights, 3 = leak detector, 4 = probe state (active/inside-geometry/never-traced), 5 = distance-atlas mean (G2) |
| `r_rtGIProbeDump` | `0` | one-shot: grid origin/dims, memory, active/inactive counts, per-area occupancy, update queue depth |

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
    G2 as `r_rtGIProbeDebug 5`**: blue→red ramp of the stored mean, plus
    magenta where `mean2 < mean²`, which is algebraically impossible for a real
    second moment and so separates "mistuned lobe" from "broken moments".
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

### G2 — resolve switch ✅ **written 2026-09-13, not yet run**
`gi_probe_resolve.comp`: reconstruct position + normal (G-buffer normal, fall back to
`rt_ReconstructNormal`), offset by `r_rtGIProbeNormalBias`, fetch the 8 surrounding
probes, weight by trilinear × `max(0, dot(n, probeDir))` smoothed, normalize, write
`giBuffer`. `r_rtGIProbes 1` skips the rgen dispatch, temporal, and à-trous.
- **Exit:** A/B screenshots in the ALB corpse room and the AREA doorway/zig-zag
  corridors at matched `r_rtGIStrength`; profiler capture (`GI`, `GITemporal`,
  `GIAtrous` should read ~0; `ProbeTrace/Blend/Resolve` are the new cost).
- **Expected regressions:** contact darkening is gone (AO's job now) and light bleeds
  through thin walls — that is G3's whole purpose. Do not tune constants here.
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

### G3 — leak hardening (mandatory, pillar 2)
Chebyshev visibility weighting from the distance moments; **ship the leak overlay
first**: mode 3 = "GI present where direct light is zero", color-coded per pixel;
mode 2 = per-pixel probe weights. Then tune density/bias against the overlay.
- **The moments are normalised by `r_rtGIProbeMaxRayDist` (see G1).** Divide the
  receiver→probe distance by the same `D` before the Chebyshev ratio, or the
  test is wrong by a factor of 512. G2 reserved `tune2.yzw` in `GIProbeParams`
  for whatever tuning this needs.
- The `tune2` slots and `r_rtGIProbeVisibility` (`misc.z`) are already plumbed
  through to the resolve and unused; the weighting goes in
  `gip_SampleIrradiance`, right after the wrapped-cosine term.
- **Exit:** the overlay is clean in the sealed-room and closed-door cases of the
  2026-08-22 test walk. If leaks survive at sane densities, fall back to per-area
  probe isolation (a probe contributes only to pixels in areas its own area reaches
  through open portals — the same BFS set `VK_RT_UploadGILights` already walks).

### G4 — probe relocation + classification
Offset probes out of walls (small per-probe world offset, driven by the backface
statistics from the blend pass); mark probes whose rays are mostly backface hits as
inside-geometry and give them zero weight in the resolve.
- **Exit:** debug mode 4 shows no active probes buried in geometry in the test rooms;
  mode 3's residual leaks drop further.
- The input already exists: G1's blend pass reads the sign bit of the scratch alpha
  (negative = back face) and currently just *excludes* those rays from the
  irradiance. Counting them per probe is the classification statistic.
- Note mode 4 **cannot** show a buried probe today — the overlay depth-occludes
  spheres behind geometry, so a probe inside a wall is invisible by construction, and
  `insideGeometry` reads 0 only because nothing sets it. Angled geometry (ramps,
  sloped ceilings, the AREA diagonals) buries proportionally more probes than
  axis-aligned rooms do, so those are the rooms to judge G4 in.

### G5 — scheduling
Priority queue instead of round-robin: probes in areas reached by the portal BFS
update first; a portal-state change (door opens) jumps its areas' probes to the front.
- **Exit:** opening a door relights the room behind it within a few frames rather than
  a full rotation; dump shows queue depth and the priority bumps.

### G6 — retire decision + retune
`r_rtGIStrength`, `r_rtGIContrast` and the `r_rtGIAutoDirectScale` coupling all change
meaning under probes. This overlaps `rt_optimization_tuning.md` T4-T6 and **must come
last**. Decide the per-pixel path's fate, flip the default, update ROADMAP.md.

## B.6 Known risks

| Risk | Signal | Mitigation |
|---|---|---|
| Light leaks through thin walls (pillar 2) | Mode-3 overlay lights up in sealed rooms | G3 Chebyshev → G4 relocation → per-area isolation, in that order |
| Loss of contact darkening | Corners and creases read flat vs today | AO carries it; retune AO strength in G6, not earlier |
| Camera-anchored grid pops as it scrolls | Visible lighting shift when the grid origin snaps | Snap to `spacing` multiples and clear only the newly-entered slab; the cleared probes are "never traced", not black — the resolve must fall back, not darken |
| Second rgen in a shared pipeline mis-indexes the SBT | Device lost, or probes get per-pixel GI's behaviour | Raygen region = `sbtBase + groupIndex·handleAlignedSize`, `size = handleAlignedSize`. Verify with a probe rgen that writes a constant before wiring the real one |
| Probe count × ray count blows the budget on large maps | `ProbeTrace` dominates | `r_rtGIProbeUpdatesPerFrame` is the throttle; convergence time degrades gracefully, cost does not |

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
