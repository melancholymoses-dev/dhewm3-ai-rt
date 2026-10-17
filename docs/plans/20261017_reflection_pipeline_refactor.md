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
| 2 | rgen cull: `fresnel × blend < minWeight` → store 0 | F0 decides which pixels exist: culled scuffs become holes the filters can't cross |
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

| File | Change |
|---|---|
| `reflect_payload.glsl` | Add `uint seed` and `uint rayClass` (glass / opaque) |
| `reflect_ray.rgen` | Fill both before `traceRayEXT`; seed per pixel per frame (`rt_WorldAnchoredSeed`) |
| `reflect_ray.rchit` | Opaque rays: `rt_EvalDirectLightingStochastic(..., picks, seed)`. Glass rays keep the deterministic loop (sharp, no à-trous to absorb noise) |
| `vk_reflections.cpp` | `r_rtReflShadowPicks` (0 = deterministic loop, 1–2 = stochastic). Passed via `rtLightBuf` or a reflection UBO field |
| `Dhewm3SettingsMenu.cpp` | Slider |

Side benefit: the stochastic estimator is unbiased, so the "past the 8-ray budget, added
unshadowed" leak disappears for opaque hits.

**Check:** static camera, converged (~60 frames), screenshot A/B picks 0 vs 1 vs 2 at the LED
wall and a multi-light room. `REFLECTIONS` ms drops. No new light leaking through walls.

---

## Stage 2 — Demodulation; trace mask off per-pixel F0

| File | Change |
|---|---|
| `gbuffer.frag`, `gbuffer_clip.frag` | Also write a coarse-mip F0 (`textureLod` at a fixed mip, like the roughness estimate) — where it fits in the G-buffer is decided here |
| `reflect_ray.rgen` | Opaque cull uses coarse F0. Store `light × blend` (no Fresnel); alpha = per-pixel Fresnel weight from fine F0. Ceiling applied to the demodulated value (retune) |
| `refl_temporal_resolve.comp`, `refl_atrous.comp` | Validity = alpha > 0 as today; they now filter incident light only |
| `refl_composite.frag` | `rgb × alpha`, then add |
| `glass_refl_overlay.frag` | Unchanged — glass keeps its negative-alpha path |

**Check:** debug 14 on the scuffed wall: no dark-blue blotches inside a panel. Scuff detail
still visible in the final composite (comes back from fine F0). Debug 3 unchanged.

---

## Stage 3 — Edge stops: linear depth + normal

Worth doing: the raw-depth stop does nothing past ~20 units, and Stage 4's upsample needs
the same stops to stay on one surface.

| File | Change |
|---|---|
| `refl_atrous.comp` | Linearize depth (near plane from a push constant); stop on `|Δz| / z`. Add normal stop `pow(max(dot(nC, nT), 0), k)` |
| `vk_reflections.cpp` | Bind `gbufNormal`; `r_rtReflAtrousSigmaZ` becomes relative (default ~0.02); `k` fixed (~32) |

Follow-up, separate change: `gi_atrous.comp` uses the same raw-depth convention.

**Check:** a rail crossing a reflective floor at 100+ units: no halo of floor reflection on the
rail, no rail colour bleeding into the floor. Floor/wall crease stays split.

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
