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

Done 2026-10-17, verified in-game alongside 2b.

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

Done 2026-10-17, verified by eye: smoothing visibly better across scuffed panels.

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

Retuned window (Slow, 2026-10-18): `r_rtReflMaxLuminance` 6, `r_rtReflRoughLumExp` 2.5,
`r_rtSpecF0Scale` 0.332 / `Gamma` 2.46. The 6 vs the new 8.0 default is exactly the
0.332/0.25 F0 ratio, so the rescale's sizing held. Defaults left at 8.0, which is
correct for the stock remap.

`r_rtReflectionMinWeight` is now a coarse-F0 threshold, so a value tuned against
per-pixel F0 reads far stricter — a hand-tuned 0.2 culled whole panels and had to come
back to 0.04. Flag this when Stage 5 touches the knob again.

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

Split into three after reading the code 2026-10-18: the resolution change needs glass and
opaque in separate images with separate denoiser chains, and that separation is most of
the work. 4a is a parity-gated refactor, 4b the dispatch split, 4c the resolution change.

Glass and opaque **cannot** share one buffer once their resolutions differ. The cheap
alternative — one full-res buffer, one ray per 2×2 block, incident light replicated to all
4 pixels with 2b's exact per-pixel alpha — fails only on glass: it must stay full-res and
sharp, the two populations are told apart solely by alpha sign in that buffer, and the
opaque pass would need a per-pixel glass probe to know where not to write, which spends
the rays it just saved.

### 4a — One chain struct, one instance (no image change) — verified in-game 2026-10-18

All reflection denoiser state was local to `vk_reflections.cpp` (only two `reflBuffer`
null-checks leaked, both in `vk_backend.cpp` beside a `VK_RT_GetReflReadView` call), so the
refactor stayed contained.

| File | Change |
|---|---|
| `vk_raytracing.h` | `vkReflChain_t`: `trace[]`, `history[2]`, `moments[2]`, `atrousA/B`, `atrousReadView`, `pingPong`, `historyValid`, `prevCamPos/Fwd`, trace + temporal + à-trous desc sets and their `lastUpdated` counters, `dispatchRect`/`Frame`, the three duplicate-dispatch guards, the 8 cached bound views. `VK_REFL_CHAIN_COUNT` sizes the pools. New `VK_RT_ReflReadReady()` |
| `vk_reflections.cpp` | `VK_RT_Dispatch{Refl,ReflTemporal,ReflAtrous}Chain` + `VK_RT_ReflChainReadView` take a `vkReflChain_t *`; the public entry points are one-line wrappers over `VK_RT_ReflChain()`. Image create/destroy/resize and all three desc-set allocations loop over chains. `VK_RT_InitReflChainState` seeds the guards to -1 (vkRT is zero-init and frame 0 is real) |
| `vk_backend.cpp`, `vk_accelstruct.cpp` | Two `reflBuffer` null-checks → `VK_RT_ReflReadReady`; the level-load desc-counter reset loops over chains |

Two deviations from the sketch above, both deliberate: the **trace** desc sets moved into
the chain as well (4b's chains bind different storage images, so splitting them later
would be a second pass over the same code), and `VK_RT_ResizeReflections` now clears
`dispatchRectFrame` — a rect recorded against images it just freed is never valid.

**Check:** screenshot parity — the image must not change at all. Glass mirrors, debug 11,
14, 15 all as before. Log lines now carry a `[opaque]` chain tag.

### 4b — Split the populations — code written 2026-10-18, unvalidated

| File | Change |
|---|---|
| `vk_raytracing.h` | `VK_REFL_CHAIN_COUNT` 2. `vkReflChain_t::spatialFilter` (false for glass). New `VK_RT_GetReflGlassReadView` |
| `vk_reflections.cpp` | `REFL_POP_*`. `VK_RT_DispatchReflections` resolves the glass rect once, then routes: mode 1 → glass only, mode 2 and every debug mode → one `BOTH` dispatch into the opaque chain, mode 3 → glass over the rect + opaque full-screen. Temporal runs per chain; à-trous skips glass. `ReflParamsUBO` 288 → 304 |
| `reflect_ray.rgen` | `params.population` + `params.glassRect*`. `GLASS` keeps only glass pixels, `OPAQUE` zeroes them, `BOTH` is unchanged |
| `vk_backend.cpp` | Glass overlay binds `VK_RT_GetReflGlassReadView` |

Three findings that shaped it:

- **The probe, not the rect, was the cost.** Mode 3 fired a glass probe at every screen
  pixel. The opaque pass still needs one to know where glass owns the pixel, but only
  *inside* the glass rect — outside, a probe can only miss. A glass-free view now spends
  none. This keeps parity, where simply dropping the probe would have composited the
  behind-glass surface's reflection under the pane's.
- **`refl_atrous.comp` already excluded glass** from both the centre test and the 3×3 taps,
  so a glass à-trous pass is a copy. Skipped, and its scratch images left unallocated.
  The temporal EMA does run on glass — that is what `r_rtReflDenoiseAlphaGlass` feeds.
- **`REFL_POP_BOTH` had to stay.** Mode 2 means "one pass, glass by alpha sign", and every
  debug mode needs one image to land in. Keeping the old path for both left
  `refl_composite.frag`'s glass test in place — the plan expected to delete it.

**Check:** parity on glass mirrors (Mars City windows) and on a metal floor; debug 11/14/15
unchanged. Then `r_vkLogRT 1` in a glass-free room: one `[opaque] pop=2` line, no `[glass]`
line at all. Trace ms should drop most in rooms with no glass.

### 4c — Half res — verified in-game 2026-10-18, `gpu.Refl` −62%

Measured at Mars City HQ, `mchq_reg.csv` vs `mchq_halfres.csv`, p50 ms:

| | full res | half res | |
|---|---|---|---|
| `gpu.Refl` (trace + upsample) | 3.814 | **1.449** | −62% |
| `gpu.ReflTemporal` | 0.187 | 0.187 | unchanged |
| `gpu.ReflAtrous` | 0.378 | 0.370 | unchanged |
| reflection stack total | 4.379 | **2.006** | −54% |
| `gpu.total` | 7.894 | **5.518** | −30% |
| `gpu.total` p95 | 9.648 | 7.211 | −25% |

The two denoisers costing the same in both captures is the direct confirmation that
"upsample early" works as designed — they still run full-res on full-res inputs. It also
means 1.449 includes the reconstruction, so the trace alone is below it. The captures are
not content-matched (`ring.draws` 485 → 550, `lights.view` 16 → 17, `shadow.Mpix` +6%): the
half-res capture carried the *heavier* scene and still won, so −62% is if anything
conservative.

The reflection stack was 55% of RT time and is now 36%. `gpu.AO` (0.962) is now within
reach of `gpu.Refl` as the most expensive RT pass.


**Upsample early, not last.** The plan had the denoisers running on the half-res grid with
the upsample at the end. Reversed: the upsample runs immediately after the trace, so the
temporal EMA and à-trous keep working full-res against exact per-pixel depth, normals and
motion. Reprojection and plane stops are the two things that most dislike a coarse grid,
and the trace is what costs — the à-trous is 1-2 passes of a 3×3. Both existing denoisers
are untouched, and only the rgen's write target moves.

| File | Change |
|---|---|
| `vk_reflections.cpp` | `r_rtReflOpaqueHalfRes` (default **1**). `vkReflChain_t::traceHalf[]` at ½×½, `halfResCapable` (opaque only). Upsample pipeline + dispatch, inside the trace's depth READ_ONLY window. `ReflParamsUBO` 304 (`reflScale`) |
| `glsl/refl_upsample.comp` | **New.** rgb: 2×2 joint bilateral using à-trous's own plane + normal stops. **a: Fresnel recomputed at full res**, never interpolated |
| `glsl/refl_fresnel.glsl` | **New.** `ReflFresnel` shared by the rgen and the upsample so the two can't drift |
| `reflect_ray.rgen` | `gcoord = coord * reflScale` for every full-res fetch; `imageStore` stays on the refl grid |
| `Dhewm3SettingsMenu.cpp`, `CMakeLists.txt` | Checkbox; new shader + include |

**No sub-pixel rotation.** A rotating sub-pixel makes one half-res history pixel accumulate
four *different* full-res positions, which straddle edges — the disocclusion test then
rejects forever and the EMA never converges. Fixed top-left lattice instead; per-frame
variety already comes from the GGX sample's `frameIndex`, and the upsample recovers origin
detail from the full-res G-buffer. (This is also why the per-slot-counter engine rule never
came up: there is no pixel-skipping pattern left to desynchronise.)

**No debug mode for the upsample.** It writes *before* the EMA and à-trous, so any overlay
would be smeared by both and read as a gradient rather than a classification.

**Checked:** no visible quality loss in play at Mars City HQ; specular trim edges stay sharp,
which is the full-res alpha doing its job. Default left at 1.

Not separately measured: the upsample has no profiler span of its own, so it sits inside
`gpu.Refl`. Worth a nested span (like `TLAS.BLAS`) if the trace is ever tuned further.

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
