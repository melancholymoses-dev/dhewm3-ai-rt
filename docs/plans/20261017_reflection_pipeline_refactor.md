# Reflection pipeline refactor — Arc 8g

Opaque reflections now carry the "wow" (glossy sheen, LED glow, wall wash in metal floors),
but they cost up to ~10 fps in heavy scenes, and the pipeline is Stage 1's glass denoiser
stretched over opaque surfaces. This arc makes it cheaper and gives its knobs one meaning each.

## Measured cost

Slow machine (Ryzen 5 2600 / RX 9070 XT), mchq, 1440p, 2577 frames, 2026-10-17, opaque
reflections on (`mchq.csv`) vs the 2026-10-14 glass-only capture (`mchq_r3.csv`).
`python tools/vk_profile_report.py mchq.csv --baseline mchq_r3.csv`.

| p50 ms | 10-14 (glass only) | 10-17 (opaque on) |
|---|---|---|
| `gpu.Refl` (trace + hit shading + refl volumetrics) | 0.00 (p95 2.64 on 10-13) | **10.09** (p95 13.39) |
| `gpu.ReflTemporal` + `gpu.ReflAtrous` | — | 0.29 |
| GPU frame (RT + raster) | 12.0 | 17.2 |
| `frame.backend` | 12.1 (83 fps) | 18.0 (56 fps) |

Reflections are ~60% of the GPU frame; the filters are 3% of the reflection cost. The trace
and its hit shading are the target. Both captures at native 1440p, no upscaling. The 10-17
run had AO at 2 samples, `r_rtShadowSamples 3` and bloom off, which accounts for the AO
−1.9 ms; probe resolve (−0.9) and froxel fill (−0.5) also dropped, unexplained. The
`gpu.Refl` delta stands on its own; the frame totals are indicative.

## Current pipeline, and what's wrong with it

| # | Stage | Problem |
|---|---|---|
| 1 | G-buffer: F0 from full-res specular map | F0 is pixel-scale scuff detail |
| 2 | rgen cull: `fresnel × blend < minWeight` → store 0 | F0 decides which pixels exist: culled scuffs become holes the filters can't cross (fixed, 2b) |
| 3 | One GGX ray per pixel, full render res | Every opaque pixel pays a ray, then gets blurred anyway |
| 4 | rchit: up to 8 shadow rays per hit (`REFL_MAX_SHADOW_LIGHTS`) | Most of the cost. Lights past the budget are added unshadowed (leak) |
| 5 | `× blend × fresnel`, then ceiling | Filters blur an F0-patterned signal |
| 6 | Temporal, then à-trous on raw NDC depth | Depth stop dead past ~20 units; no normal stop, so floor/rail bleed |
| 7 | Composite adds as stored | Glass and opaque share buffers, told apart by alpha sentinels |

## Expected gains

Estimates until Stage 0 measures them.

| Change | Rays saved | Stage |
|---|---|---|
| Stochastic lights at hits (1–2 shadow rays, not ≤8) | ~3–6× fewer shadow rays per hit in multi-light rooms | 1 |
| Half-res opaque trace + bilateral upsample | 4× fewer opaque reflection rays *and* their shadow rays | 4 |
| Combined | Opaque reflection trace plausibly ≤ ¼ of today | 1 + 4 |
| Demodulation + coarse F0 mask | Costs rays (holes get traced); offset by the above | 2 |

## Settled inputs (`../rt_engine_rules.md`)

- Firefly clamp: before temporal and every à-trous pass, no floor. Opaque temporal alpha fixed, no variance boost.
- Taste contract #7: half-res and stochastic lights change the image. Each gets its own cvar;
  it becomes the default only if converged screenshots are indistinguishable.

---

## Stage 0 — Baseline

Started: mchq on Slow (above). Still needed — the splits say which stage pays most.

| Capture | Detail |
|---|---|
| Phases | `REFLECTIONS`, `REFL_TEMPORAL`, `REFL_ATROUS` p50/p95 (`../vulkan_debugging.md` recipe) |
| Scenes | mchq, r2r, commfire + one opaque-heavy spot (ACO offices LED wall) |
| Machines | Fast and Slow |
| Splits | `r_rtReflectionMode 1` vs `3` (opaque cost), `r_rtReflVol 0` vs `1` |

**Check:** numbers in a `notes/` file; the trace phase is the majority of the reflection cost.
If not, re-order the stages to match.

---

## Stage 1 — Stochastic lights at reflection hits

Implemented 2026-10-17. Default `r_rtReflShadowPicks 0` (taste #7) until the converged A/B
below is done. Slow, mchq, vs `mchq.csv` (picks 0):

| ms | picks 0 | picks 1 (`mchq1.csv`) | picks 2 (`mchq2.csv`) |
|---|---|---|---|
| `gpu.Refl` p50 / p95 | 10.09 / 13.39 | **7.47 / 9.35** | 9.07 / 11.81 |
| `frame.backend` p50 | 18.0 | 15.2 | 16.9 |

Picks 2 ran a lighter path (`ring.draws` −16%), so its gain is if anything overstated.
~7.5 ms remains with one shadow ray per hit: the reflection ray itself now dominates (Stage 4).

| File | Change |
|---|---|
| `reflect_payload.glsl` | Add `uint lightSeed`, `int lightPicks` (0 = deterministic; replaces a separate rayClass) |
| `reflect_ray.rgen` | Opaque: `lightPicks = params.shadowPicks`, seed = `rt_WorldAnchoredSeed` × pixel hash. Glass: 0 |
| `reflect_ray.rchit`, `player_reflect.rchit` | `lightPicks > 0` → `rt_EvalDirectLightingStochastic`, else the deterministic loop |
| `rt_light_eval.glsl` | Stochastic loop takes `ambientScale` (occluded pick keeps that fraction, matching the deterministic loop). `gi_ray.rchit` passes 0 |
| `vk_reflections.cpp` | `r_rtReflShadowPicks` (0–2) → `ReflParamsUBO.shadowPicks` (UBO 272 → 288). `[Refl]` log line on change |
| `Dhewm3SettingsMenu.cpp` | Slider |

Side benefit: the stochastic estimator is unbiased, so the "past the 8-ray budget, added
unshadowed" leak disappears for opaque hits.

**Check:** static camera, converged (~60 frames), screenshot A/B picks 0 vs 1 vs 2 at the LED
wall and a multi-light room. `REFLECTIONS` ms drops. No new light leaking through walls.

---

## Stage 2 — Demodulation; trace mask off per-pixel F0

### 2a — Octahedral normal frees a channel (no image change)

Implemented 2026-10-17, not yet verified in-game.

`gbufNormal` (RGBA8) stores a 2-DoF unit normal in 3 channels. Octahedral encoding fits it
in RG (~1° worst case, comparable to today). No spare bit exists elsewhere: `gbufAlbedo`
alpha is full (class + roughness), `motionVectors` is RG16F.

| Channel | Before | After |
|---|---|---|
| RG | normal xy | octahedral normal |
| B | normal z | coarse F0, written ≤ 254/255 |
| A | fine F0 | fine F0 |

"Never written" sentinel moves from rgb = 0.5 (a valid oct code) to **B = 1.0**: clear
value and null image become (0.5, 0.5, 1.0, 0.0). Self-contained, no new bindings.

| File | Change |
|---|---|
| `rt_gbuf_normal_fmt.glsl` | **New.** Owns the format: `GBuf_EncodeNormal`, `GBuf_DecodeNormal`, `GBuf_NormalValid`, `rt_Oct*`. No includer contract (the sampler-based `rt_GbufNormalAt` stays in `rt_gbuf_normal.glsl`, which includes this), so `gi_probe_common.glsl` can share the oct math — `gip_Oct*` are now one-line wrappers |
| `gbuffer.frag`, `gbuffer_clip.frag` | Write via the helper; B = 0 until 2b |
| `reflect_ray.rgen` (decode + debug 2/4), `refl_temporal_resolve.comp`, `refl_atrous.comp`, `rt_gbuf_normal.glsl` | Inline decodes → helper. Debug 2 now decodes before display |
| `vk_backend.cpp` clear value, `vk_reflections.cpp` null image | New sentinel |
| `CMakeLists.txt` | New include |

**Check:** debug 2 (normals) and 4 (coverage) look as before; AO/GI/reflection screenshots
unchanged; `grep -rn "rgb \* 2.0 - 1.0" neo/renderer/glsl` finds only bump-map samples
in the two `gbuffer*.frag`.

### 2b — Coarse F0 and demodulation

Implemented 2026-10-17, not yet verified in-game.

| File | Change |
|---|---|
| `refl_alpha.glsl` | **New.** Owns the alpha convention: glass sentinel, traced test, and the floor below. Replaces `VALID_ALPHA_THRESHOLD` duplicated in both filters |
| `gbuffer.frag`, `gbuffer_clip.frag` | B = the same F0 remap on a fixed mip 4 specular sample, clamped to 254/255 |
| `rt_gbuf_normal_fmt.glsl` | `GBuf_CoarseF0` — returns 0 at the sentinel, which would otherwise read as a mirror |
| `reflect_ray.rgen` | Both culls (`f0 < 1/255` early-out and the `minWeight` test) and debug 13 read coarse F0. `ReflFresnel` helper, called on coarse to admit and on fine for the weight. Stores `light × blend`, alpha = fine Fresnel. Ceiling now bounds incident light |
| `refl_temporal_resolve.comp`, `refl_atrous.comp` | Validity/glass tests → `refl_alpha.glsl` helpers; they now filter incident light only. Moment luminance capped at 128 (fp16 `lum²`, newly reachable) |
| `refl_composite.frag` | `rgb × alpha`, then add. Debug modes all store alpha 1.0, so they pass through |
| `glass_refl_overlay.frag` | Unchanged — already `rgb × abs(alpha)` |
| `vk_reflections.cpp`, `Dhewm3SettingsMenu.cpp` | `r_rtReflMaxLuminance` 0.75 → **8.0**, slider max 2 → 24 |

A traced pixel's alpha is floored at 2/255 so a scuff with fine F0 ≈ 0 still reads as
traced — otherwise the filters see a hole and the demodulation buys nothing.

`r_rtReflMaxLuminance` was rescaled because the ceiling moved to the far side of the
Fresnel multiply: it used to mean "0.75 / this pixel's F0" of incident light and now
means one number for every pixel. 8.0 reproduces the old bound at a typical remapped
F0 (~0.1); it is tighter than before on bright-F0 metal and looser on dark surfaces,
where the final contribution is scaled down anyway.

**Check:** debug 14 on the scuffed wall: no dark-blue blotches inside a panel. Scuff detail
still visible in the final composite (comes back from fine F0). Debug 3 unchanged.
Overall reflection brightness unchanged vs. Stage 2a — if highlights clip or dim, the
`r_rtReflMaxLuminance` rescale is the suspect, not the demodulation.

---

## Stage 3 — Edge stops: linear depth + normal

Worth doing: the raw-depth stop does nothing past ~20 units, and Stage 4's upsample needs
the same stops to stay on one surface.

Done 2026-10-17, verified by eye (debug 15 + final image). Plane distance replaces `|Δz|/z`: a relative depth
stop also blocks grazing floors, where depth changes fast per pixel along one plane.

| File | Change |
|---|---|
| `refl_atrous.comp` | World positions from depth (`invViewProj` push constant). Plane stop: `|dot(pT − pC, nGeo)| / viewDist`, `nGeo` from depth (shading normal would put bump detail into it). Normal stop `dot(nC, nT)^k` on the G-buffer normal. Debug 15: step-1 edge weight |
| `vk_reflections.cpp` | Bind `gbufNormal` (binding 4). PC 48 → 112 bytes. `r_rtReflAtrousSigmaZ` → `r_rtReflAtrousSigmaPlane` (0.02); `r_rtReflAtrousNormalPower` (4 — low because bumps vary the shading normal; 90° creases cut at any k). Shared `VK_RT_ReflInvViewProj` |
| `vk_raytracing.h` | `REFL_DEBUG_MAX_MODE` 15 |

Follow-up, separate change: `gi_atrous.comp` uses the same raw-depth convention.

**Check:** debug 15: floors white out to 100+ units, rails and creases outlined dark, no
bump pattern on rough panels (lower NormalPower if there is). Then the final image: a rail
crossing a reflective floor at 100+ units: no halo of floor reflection on the rail, no rail
colour bleeding into the floor. Floor/wall crease stays split.

---

## Stage 4 — Half-res opaque trace

| File | Change |
|---|---|
| `vk_reflections.cpp` | Opaque buffers at half res: trace, history, moments, à-trous A/B. Glass keeps full-res `reflBuffer` and its own dispatch over the glass rect |
| `reflect_ray.rgen` | Opaque launch: one ray per 2×2 block; sub-pixel rotates over 4 frames on a **per-slot counter**, not `tr.frameCount` (engine rule) |
| `refl_temporal_resolve.comp`, `refl_atrous.comp` | Run at half res; depth/normal fetched at the traced sub-pixel |
| `glsl/refl_upsample.comp` | **New.** Joint bilateral 2×2 → full res, using Stage 3's depth + normal stops |
| `vk_backend.cpp` | Dispatch upsample after à-trous; composite reads its output |
| `CMakeLists.txt` | New shader |
| `vk_reflections.cpp` | `r_rtReflOpaqueHalfRes` (0/1) |

**Check:** converged screenshots 0 vs 1 at the LED wall, a metal floor with wall wash, a rail
over a floor. No 2×2 blockiness at edges, no crawl on a slow strafe. Trace ms ~¼. Debug 14
still meaningful (classifies the half-res grid).

---

## Stage 5 — Knob consolidation

| Today | After |
|---|---|
| `r_rtReflectionBlend`, `r_rtReflMaxLuminance`, `r_rtReflRoughLumExp` | Reflection strength + highlight ceiling (roughness curve fixed or one exponent) |
| `r_rtReflAtrousIterations`, `r_rtReflAtrousRoughnessGain` | Blur radius from roughness (passes run until radius ≥ lobe footprint), one "wash" scale |
| `r_rtReflDenoiseAlpha` / `…Glass` | Unchanged |
| `r_rtReflectionMinWeight` | Coarse-F0 threshold only |

Update menu, `../Dhewm3-AI-RT-Settings.md`, presets. Deleted cvars go with their UBO fields
and menu entries in the same change (cleanup rule).

**Check:** every remaining reflection cvar has a visible, single effect; defaults reproduce
the Stage 4 image.

---

## Stage 6 (optional) — Probe radiance for very rough hits

Above a roughness threshold, rchit reads probe GI at the hit instead of shading it: zero
shadow rays, and reflected surfaces get indirect light (today they get none — settled
conclusion). Overlaps `r_rtReflRoughnessMax`'s GI handoff; decide that interaction here.

**Check:** dark rooms stay black (taste contract #2). Trace ms drops further on rough metal.
