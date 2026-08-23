# RT Performance Optimization & Tuning Plan

**Date:** 2026-08-08
**Branch:** shaped-shadows
**Companion docs:** `gbuffer_normal_pass.md` (G-buffer/F0 pass — several items below
depend on it), `completed/lighting_shadows_refinement.md` (Phase 9 record).

**Goal:** free enough GPU budget to raise sample counts (1→4 GI samples is a huge
noise win per in-game testing) while fixing the brightness/tuning problems whose root
causes are structural, not constant-twiddling.

---

## Guiding rule

**Measure → restructure → tune, in that order.** Several items below change the
*meaning* of existing tuning constants (the reflection composite changes brightness
math; stochastic GI changes noise character). Any constant tuned before those land
will need retuning after. Don't polish CVar values on top of math that's about to move.

The per-phase GPU profiler already exists (`VK_RTProfile_*`, phases TLAS / AO /
REFLECTIONS / GI / GI_TEMPORAL / GI_ATROUS / VOL / VOL_TEMPORAL). Before and after each
change below, capture the same test spot (e.g. the Administration hallway in the
screenshot) and record the phase table. Add a phase for the per-light shadow work if
one doesn't exist — it's currently the least-visible large cost.

---

## Part 1 — Structural performance issues (largest first)

### P1. Per-light shadow dispatch serializes the whole GPU pipeline

`vk_backend.cpp:3436-3446`: for **every shadow-casting light**, the interaction loop does:

```
vkCmdEndRenderPass → depth barriers → shadow trace dispatch → barrier
→ blur compute dispatch (vk_shadows.cpp:951-1053) → barrier → vkCmdBeginRenderPass(resume)
```

With L visible lights that is L render-pass breaks and 2L+ full pipeline barriers per
frame, forcing raster→RT→compute→raster serialization with zero overlap. This is very
likely the largest *structural* cost after raw ray counts.

**Fix (staged):**
- **P1a (cheap, do first):** skip the blur dispatch when the light's scissor rect is
  small (< ~64px on a side) — tiny lights don't visibly benefit — and clamp
  `r_rtShadowBlur` cost by running blur at the dispatch rect only (already done) but with
  one barrier pair instead of two where possible.
  **✅ Implemented (Wave 2, 2026-08-14):** `r_rtShadowBlurMinRect` (default 64, 0 = always
  blur) in vk_shadows.cpp; skips when *both* rect sides are under the threshold. Skip
  logged at `r_vkLogRT 2`. The barrier-pair consolidation was not done (P1b subsumes it).
- **P1b (the real fix):** batch shadow masks. Pack up to 4 lights per RGBA8 mask image
  (one light per channel), or use an R8 texture-array with one layer per light
  (`VK_MAX_SHADOWED_LIGHTS` ≈ 8-16 covers Doom 3 scenes). Trace **all** lights in one
  dispatch (rgen loops lights, or one dispatch per mask image), blur once, then run the
  entire interaction loop with **zero** render-pass breaks — each light's draw samples
  its channel/layer. The interaction UBO gains a `shadowMaskChannel`/layer index.
  This removes ~L render-pass breaks + ~2L barriers per frame.
- Expected win: highly scene-dependent; measure with the profiler. Rooms with 5-8
  lights should see a large drop in total frame time even though traced-ray count is
  unchanged.

  **✅ Implemented (Wave 4, 2026-08-15).** Chose the **R8 texture array**, not RGBA8
  channel packing: it scales past 4 lights, needs no channel-extraction math, and lets
  the blur reuse one descriptor set for every light (the layer rides in a push
  constant). Notes for whoever tunes or extends this:

  - `VK_RT_SHADOW_LAYERS` = 8 in `vk_raytracing.h`: 7 batched + **layer 7 reserved as
    a serial scratch layer**. Lights beyond the batch budget — and *every* light when
    `r_rtShadowBatch 0` — reuse layer 7 via the old per-light path, so they still pay a
    render-pass break. That keeps the A/B switch and the >7-light case correct rather
    than dropping shadows.
  - Per-view flow lives in `vk_backend.cpp`: `VK_RB_AssignShadowLayers()` →
    `VK_RT_ShadowBatchClearLayers()` → (TLAS) → `VK_RT_DispatchShadowBatch()` → the
    interaction loop looks each light's layer up again. The assignment pass walks
    `viewLights` through `VK_RB_LightDrawsInteractions()` and `VK_ComputeLightScissor()`,
    the *same* helpers the interaction loop uses — if those two ever diverge, lights get
    handed layers nothing traced. That is the failure mode to suspect first.
  - Barrier count is now **constant in the light count**: one depth round-trip, one
    RT→compute, one H→V, one →fragment, regardless of L. Traces and blur sweeps for
    different lights need no barriers between them because they touch disjoint layers.
  - **`noShadows` lights no longer get a layer at all** (`layer == -1` →
    `u_UseShadowMask 0`). The single-image version had to clear the mask to white for
    them, costing a clear plus two barriers per such light; now they cost nothing.
  - The mask clear covers **only the layers in use this view**, not all 8 — a full-array
    clear is ~16 MB of pure bandwidth at 1080p when the room has two lights.
  - The clear's barrier now includes `FRAGMENT_SHADER` in its source scope. The old one
    listed only the RT stage, which did not cover the *previous view's* interaction
    reads — latent with one view per frame, a real hazard once a mirror subview renders
    ahead of the main view.
  - Interaction UBO: the old trailing `_pad` int is now `shadowMaskLayer` (offset 376);
    `interaction.frag` binding 7 became `sampler2DArray`. That forced a 2D-**array**
    fallback view (`VK_Image_GetFallbackArrayDescriptorInfo`) — binding the plain 2D
    white texel there is a view-type mismatch, not a cosmetic one.
  - **Memory cost is the tradeoff.** Mask + blur-temp, both 8-layer R8, × 2 frame
    slots ≈ **33 MB at 1080p** (was ~8 MB), ≈ 66 MB at 1440p, ≈ 133 MB at 4K. If that
    ever bites, `VK_RT_SHADOW_LAYERS` is the single knob — lowering it just pushes more
    lights onto the serial path, it doesn't break anything.
  - New profiler phase `Shadows` (`VK_RTPROF_PHASE_SHADOWS`) — the shadow cost was
    previously invisible in the phase table. Use it for the before/after measurement.
  - Debug: `r_vkLogRT 1` prints `VK RT SHADOW BATCH: traced=… blurred=… layers=…
    serialOverflow=…` once per view, and each `VK LIGHT[n]` line carries
    `smLayer=N (batched|serial|noShadows)`.
  - **Not yet measured.** Capture the profiler table with `r_rtShadowBatch` 1 vs 0 in
    the same spot and record it below.

### P2. Reflection rays traced for every pixel, reflective or not

`reflect_ray.rgen` traces a glass-probe ray + a mirror ray per non-sky pixel, and each
opaque hit (`reflect_ray.rchit:98-149`) loops ≤128 lights firing ≤8 shadow rays. On the
vast majority of pixels (concrete/cloth, F0 ≈ 0) the result is then multiplied to
~nothing by Fresnel.

**Fix:** the G-buffer F0 early-out (`gbuffer_normal_pass.md` Step 7). Pixels with
`F0 < 1/255` and no glass skip the reflection ray *and* its shadow rays entirely.
In typical Doom 3 scenes this culls well over half the reflection workload.

### Bug: GI checkerboard left a permanent ghost (fixed 2026-08-15)

Recording it here because the mechanism generalises to any future pass that skips
writing some pixels on some frames.

`gi_ray.rgen` derived its checkerboard parity from `frameIndex` (`tr.frameCount`).
But `giBuffer` is **one image per frame-in-flight slot**, and `vk.currentFrame`
advances once per frame exactly as `tr.frameCount` does — so slot parity is locked to
frame parity. Slot 0 therefore traced only `(x+y)` even, slot 1 only `(x+y)` odd,
*forever*: the complementary half of each slot's image was never written again.
`giBuffer` is cleared only at allocation, so that half kept whatever was last written
to it — a frozen full-res frame from the last time checkerboard was off — and the
temporal EMA re-injected it every frame, converging **to** the ghost rather than
washing it out. Toggling checkerboard on after it had been off is what loaded the
ghost with a recognisable image; moving the camera never cleared it because those
texels were simply never written.

**Fix:** the phase is now a per-slot counter (`s_giCheckerPhase` in `vk_gi.cpp`,
passed as `checkerPhase` in the GI UBO) rather than a function of the frame index, so
each slot alternates halves on successive visits and every pixel is refreshed within
two visits to that slot.

The rule to remember: **anything that keys "skip this pixel this frame" off a frame
counter must key off a per-slot counter instead**, or the frame-in-flight count
silently divides the pattern into fixed per-slot classes. Sampling *seeds* keyed off
`frameIndex` (AO/GI/shadow/vol jitter) are fine — every pixel is still written, so
the worst case there is reduced decorrelation, not a stuck image.

### P3. GI worst case: up to 64 traced rays per pixel

`gi_ray.rgen` fires `numSamples` (4) hemisphere rays; each hit (`gi_ray.rchit:137-181`)
loops up to `r_rtGIMaxBounceLights` (16) lights with **one shadow ray each** →
4 × 16 = 64 rays/pixel worst case (checkerboard halves it). This is the dominant GI cost
and the direct obstacle to raising `numSamples`.

**Fix: stochastic light sampling at bounce hits.** Per hit, pick **1-2 lights** with
probability proportional to `intensity × attenuation(dist) × NdotL`, fire the shadow ray
only for the winner(s), and divide the contribution by the selection probability
(standard importance sampling — unbiased). The existing temporal accumulation + à-trous
chain is precisely the denoiser this needs.

- Budget math: 4 samples × 2 stochastic lights = 8 rays/pixel vs. today's 64 —
  an ~8× cut, or equivalently **8 GI samples for a quarter of today's cost**. This is
  the item that converts perf work into the noise reduction you actually want.
- Implementation is contained in `gi_ray.rchit` (CDF over the in-range lights, one
  `randFloat` draw seeded from the existing world-anchored seed).
- The same treatment applies to `reflect_ray.rchit` (shared include — see P5).

**✅ Implemented (Wave 3, 2026-08-15):** `rt_EvalDirectLightingStochastic()` in
`rt_light_eval.glsl`, driven by `r_rtGIStochasticLights` (default 2, 0 = legacy
all-lights path for A/B). Notes for whoever tunes or extends this:

- **Streaming weighted-reservoir sampling, not a CDF.** Two independent
  single-sample reservoirs live in scalars/vectors; there is no 128-float array,
  so the hit shader stays register-resident (a per-invocation CDF array in an
  rchit spills to scratch and is a plausible source of the earlier crashes here).
- Weight = luminance of the light's *unshadowed* contribution. The estimator's
  luminance is therefore exactly `wSum` per pick, i.e. bounded by the total
  unshadowed luminance — the `1/p` division cannot make fireflies. Only the
  binary shadow term is noisy, which is what the temporal + à-trous chain eats.
- `n <= picks` defers to the deterministic loop (same ray count, zero variance) —
  the common case in Doom 3 rooms with 1-3 lights in range. Both reservoirs
  landing on the same light reuse one trace.
- Seed is `launchID ⊕ frameIndex ⊕ floatBitsToUint(gl_HitTEXT)`. All three matter:
  `gl_HitTEXT` decorrelates the rgen's `numSamples` rays (same launch ID),
  `frameIndex` is what lets temporal accumulation average the picks. Drop it and
  the same light wins forever at each point — noise bakes into permanent blotches.
- **Prerequisite bug fixed in the same change:** the GI descriptor set declared
  binding 3 (GIParams UBO) as `RAYGEN` only, while `gi_ray.rchit` has read it
  since Option B landed (`maxBounceLights`). Reading a descriptor from a stage
  absent from `stageFlags` is undefined behaviour — garbage or a fault depending
  on driver. Now `RAYGEN | CLOSEST_HIT`. Any future per-hit knob must ride in
  this UBO (GI) or the light SSBO header; `rt_light_eval.glsl` itself stays
  UBO-free because the *reflection* pipeline's binding 3 is still raygen-only.
- Reflections were left on the deterministic path (their params UBO is raygen-only,
  and P4's threshold already caps them at 8 rays). Extending P3 to
  `reflect_ray.rchit` needs a seed plumbed through `ReflPayload` first.
- **Not yet done:** raising `r_rtGISamples`. Default stays 4 so the estimator can
  be validated against the legacy path with ray count held constant. Once stable,
  8 samples × 2 picks = 16 rays/px, still a 4× cut from today's 64 worst case.

### P4. Reflection-hit shadow rays go to the wrong lights

`reflect_ray.rchit` shadow budget (first 8 lights **in buffer order**) is spent on
lights sorted nearest-to-*camera*, not nearest-to-hit — reflection hits are often far
from the camera. Cheap interim fix (independent of P3): skip the shadow ray when
`lColor·lInt·NdotL·atten` is below a small threshold, so budget flows to lights that
matter at the hit point. Long-term: P3's stochastic selection subsumes this.
**✅ Implemented (Wave 2, 2026-08-14):** `REFL_SHADOW_MIN_LUM` (0.02) in
reflect_ray.rchit, applied via the shared loop's `minShadowLum` parameter (see P5);
below-threshold lights contribute unshadowed without consuming budget.

### P5. Duplicated light-evaluation code (maintenance, enables P3/P4)

`gi_ray.rchit` and `reflect_ray.rchit` carry near-identical light loops that have
already drifted (GI applies `bounceScale`; reflections don't — one cause of
reflections reading brighter than the surfaces around them). Extract to
`rt_light_eval.glsl` (add to `GLSL_INCLUDES`) before implementing P3, so the
stochastic path lands once.
**✅ Implemented (Wave 2, 2026-08-14):** `rt_light_eval.glsl` owns the light SSBO
declaration, shadow payload, and `rt_EvalDirectLighting(...)` — parameterized by max
lights, shadow budget, bias, contribScale (GI passes bounceScale, reflections 1.0 —
the drift is now an explicit parameter, retune in T6) and P4's minShadowLum. Includers
`#define RT_LIGHT_SHADOW_MISS_INDEX` (GI 1, refl 2) first. Side fix: GI shadow rays
now guard against negative tMax (hit closer to the light than the bias), which the old
inline GI loop didn't. player_reflect.rchit and vol_march.comp keep their own loops
(different semantics: no shadow budget / cone evaluation) — P3 lands only in the
shared file.

### P6. Glass probe ray fired even in glass-free scenes

Every reflection pixel pays a `traceRayEXT` probe (`reflect_ray.rgen:83-93`) whether or
not any glass exists. `vk_material_table.cpp` knows at build time if any
`MAT_FLAG_GLASS` entry exists; `vk_accelstruct.cpp` knows if such an instance is in the
TLAS. Add `int sceneHasGlass` to `ReflParams`; skip the probe when 0. Full-screen ray
dispatch saved in most rooms.
**✅ Implemented (Wave 2, 2026-08-14):** `vkRT.sceneHasGlass` recomputed per TLAS build
in vk_accelstruct.cpp (scan of static+dynamic material entries), plumbed through the
ReflParams pad slot, probe skipped in reflect_ray.rgen. State transitions logged at
`r_vkLogRT 1`.

### P7. TLAS build flags favor the wrong axis

`vk_accelstruct.cpp:515/942`: both BLAS and TLAS use `PREFER_FAST_TRACE`. Standard
practice for a **per-frame rebuilt TLAS** is `PREFER_FAST_BUILD` (trace cost difference
is negligible at TLAS level; build cost is paid every frame). Keep BLAS as
`FAST_TRACE`. One-line change; measure the TLAS profiler phase before/after.
**✅ Implemented (Wave 2, 2026-08-14):** TLAS build flag switched; both BLAS build
sites unchanged. Measure the TLAS profiler phase at the checkpoint.

### P8. Volumetric march at full resolution

`vol_march.comp` marches `r_rtVolSamples` (8) steps × up to `r_rtVolMaxLights` (32)
lights **per full-res pixel** (no half-res path exists in `vk_vol.cpp`). Volumetric
media is inherently low-frequency; industry standard is half or quarter resolution +
joint bilateral upsample. `vol_bilateral.comp` already exists as a blur — extend it
into a depth-aware upsample and march at half res: **4× cost cut** on one of the
heavier passes, with little visible loss.

**✅ Implemented (Wave 4, 2026-08-15):** `r_rtVolHalfRes` (default 1, 0 = full res
for A/B). The march and the temporal EMA history both live at full/`marchScale`;
`vol_bilateral.comp` became the resolve back to full res. Notes:

- **The whole chain below the march moved to half res, not just the march.** The
  temporal EMA runs on the half-res image too, so it is 4× cheaper as well, and the
  bilateral's Gaussian footprint is walked in *source* space — a 4× tap reduction on
  what was a 121-tap full-res kernel. The bilateral is now the only full-res pass.
- **Depth comparison is on linearised view distance, not raw buffer values.** Raw
  depth is wildly non-linear; a fixed threshold would be far too strict near the
  camera and useless in the distance. `linNum`/`linAdd` in the push block are the two
  projection coefficients (`-proj[14]`, `proj[10]`) that invert it, taken per view so
  the weights track `r_znear`/fov changes. `r_rtVolUpsampleDepthSigma` is therefore in
  **world units** (default 24).
- **Fallback when every tap disagrees on depth** (thin geometry no march sample
  landed on): weights collapse, so the shader takes the single depth-nearest tap
  instead of dividing by ~0. Without that you get a bright halo on silhouettes, which
  is the classic half-res-volumetrics artifact.
- The march picks the **top-left texel of each `marchScale²` quad** as its
  representative full-res sample, and the upsample uses the identical mapping — the
  depth a tap is compared against is exactly the depth it was marched from. If those
  two ever disagree the fog will creep across edges; that is the first thing to check.
- **Latent bug fixed alongside:** `vol_composite.frag` derived its UV from
  `textureSize(u_VolMap)`, which was only correct while the vol image was always
  screen-sized. With `r_rtVolBilateral 0` at half res the compositor reads the
  march-res image directly and that would have put `gl_FragCoord` into [0,2] —
  edge-clamped garbage over most of the screen. It now takes the screen size as a
  push constant.
- **A/B levers:** `r_rtVolHalfRes` 1/0 for the whole change (runtime-switchable; costs
  one device-idle stall to reallocate). `r_rtVolBilateral 0` at half res skips the
  upsample entirely and lets the compositor's LINEAR sampler do a plain bilinear —
  the "no filter" reference. `r_rtVolUpsampleDepthSigma` very high ≈ Gaussian upsample
  with no edge stopping, which isolates what the depth weighting alone buys.
- **Measure `Vol` + `VolBilateral` together**, not `Vol` alone — half res moves cost
  from one to the other. `VolBilateral` is a new profiler phase added for this.
  Related: the profile log used to hardcode its phase list and silently omitted every
  Vol phase; it now enumerates them, so volumetric cost is visible at all.
- **Not yet measured.** Expect well under the theoretical 4× on the total: the march
  drops ~4×, but the bilateral goes up and the composite is unchanged.

**Follow-up 2026-08-22 — IGN dither striping.** The march-step jitter switched from
per-pixel `wang_hash` white noise to Interleaved Gradient Noise (Jimenez) at some point
after the initial P8 landing, to fix volumetric beams reading as decorrelated noise
(white noise breaks the spatial coherence a single beam needs across neighbouring
pixels; IGN is spatially correlated within a frame, so a beam's march stays coherent).
Traded one artifact for another: IGN is a fixed, low-discrepancy diagonal grid, and
that same regularity is visible as diagonal striping/fringing along march edges once
the eye locks onto the pattern. Fix: blend a small amount of the original white noise
back into the IGN jitter (`r_rtVolWhiteNoiseMix`) — enough to break up the periodic
grid, not enough to reintroduce the beam-decorrelation problem the switch to IGN was
fixing in the first place. Implementation: repurposed the UBO's unused pad float at
offset 172 (`VolParamsUBO::whiteNoiseMix`, was `_uboPad2`) — no struct size or
descriptor change. `0` = pure IGN (original striping), `1` = pure white noise (original
beam problem). **Tested in-game 2026-08-22: 0.25 was too strong (visible flickering,
i.e. enough white noise to partially reintroduce the beam-decorrelation problem);
0.025 — a full order of magnitude lower — "worked about right."** Default set to
0.025. The gap between the initial guess and the working value suggests the effective
range is narrow/steep near the low end; if striping reappears on a different fog
shaft, retune in small steps from 0.025 rather than jumping back toward 0.25.

### P9. Depth-reconstruction ALU in three rgens (after G-buffer)

`rt_ReconstructNormal` costs 5 depth fetches + 4 `invViewProj` matrix multiplies per
pixel and runs in AO, GI, and reflections. Commit 3 of the G-buffer plan replaces it
with one G-buffer fetch in all three — modest ALU win, plus bump-mapped AO/GI
directions (quality).

**✅ Implemented (Wave 3, 2026-08-15):** `rt_gbuf_normal.glsl` (new shared include,
added to `GLSL_INCLUDES`) provides `rt_GbufNormalAt()`; `ao_ray.rgen` (new binding 4)
and `gi_ray.rgen` (new binding 5) call it and keep `rt_ReconstructNormal` as the
fallback. `r_rtGbufNormals` (default 1) forces both back onto reconstruction for an
A/B; mode transitions log at `r_vkLogRT 1`. Reflections were already on the G-buffer
from Wave 1b and are untouched.

Two things that are easy to get wrong here:

- **The "no data" test is on RGB, not alpha.** `reflect_ray.rgen` tests `gbuf.a > 0.0`,
  which is right for reflections — a pixel with F0 == 0 isn't reflective, so the
  fallback path costs nothing. Copying that test into AO/GI would be a silent no-op:
  most of a Doom 3 scene is matte with a black or absent specular map, so F0 == 0 is
  the *normal* case and nearly every pixel would fall back. The honest test is the
  rgb clear sentinel (0.5, 0.5, 0.5) — the encoding of a zero vector, which no unit
  normal produces. Same test reflection debug mode 4 uses.
- **A back-facing bump normal is rejected, not flipped.** `rt_ReconstructNormal`
  guaranteed `dot(n, toCamera) > 0` and the hemisphere sampling downstream relies on
  that; flipping a bump-mapped normal instead points it into the surface. Those pixels
  take the reconstruction fallback.

Plumbing notes: the `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` barrier in
`vk_backend.cpp` was already issued once for the whole RT block (Step 6 of the G-buffer
plan anticipated this), so no barrier work was needed. The 1x1 null fallback image is
now shared via `VK_RT_GetNullGbufNormalView()` (vk_reflections.cpp); `VK_RT_InitAO` and
`VK_RT_InitGI` call it at init so its one-time submit+wait can't land mid-frame.
AO's `useGbufNormal` reuses the existing `pad0` slot, so `AOParamsUBO` is unchanged at
112 bytes; `GIParamsUBO` grew to 124.

**Debug:** `r_rtReflectionDebugMode 2/3/4` already visualizes exactly the buffer AO/GI
now consume — mode 4 (magenta = prepass never wrote this pixel) is the direct predictor
of where AO/GI still take the fallback. Mode 2 on a grate floor is the quality check:
AO/GI should now pick up that bump detail.

### P10. If reflections are still heavy after P2: checkerboard

GI already has checkerboard + temporal fill. Reflections have no history buffer today,
so this needs a temporal resolve step too — only worth it if the profiler still shows
reflections hot after the F0 early-out. Park it.

### L1. Stratified light list with importance sorting + hysteresis

`VK_RT_UploadGILights` (`vk_gi.cpp:1059-1080`) sorts candidates purely by
distance-to-camera and uploads the nearest 128; hit shaders walk the first N in that
order. Consequences: a bright light 200u away loses to dim fill lights near the camera,
distant sources pop in/out as the player moves, and (with auto-relight) synthesized
panel lights compete on the wrong axis.

**Fix (CPU-side, ~40 lines, improves GI + reflections + volumetrics at zero GPU cost):**

- Bucket candidates into distance tiers: 0-128, 128-320, 320-768, 768+.
- Within each tier sort by perceptual importance, not distance:
  `intensity × luminance(color) × radius²` (optionally `/ max(distSq, radius²)`).
- Fixed quota per tier (e.g. 48 / 40 / 24 / 16 of the 128 slots) so far-tier bright
  sources are guaranteed representation.
- Hysteresis: a light present last frame survives if within ~1.15× of the cut
  threshold — kills popping.
- This is the **candidate pool** layer; P3's stochastic per-hit selection is the
  **usage** layer. They compose: tiers ensure the right lights are in the buffer,
  the per-hit weighted draw ensures the right lights are sampled at each bounce.
  Light-around-the-corner reads only when both are in place (the doorway light must
  win the draw at hit points near the doorway even when the camera is far away).

**✅ Implemented (Wave 2, 2026-08-14)** in `VK_RT_UploadGILights` (vk_gi.cpp):
- Tiers 0-128 / 128-320 / 320-768 / 768+, quotas 48/40/24/16 scaled to
  `r_rtGIMaxLights`, unused quota redistributed near-first.
- Importance = `intensity × luminance(color) × radius² / max(distSq, radius²)`.
- Hysteresis via a 1.15× importance boost for lights uploaded the previous frame
  (equivalent to surviving within ~1.15× of the cut threshold, far simpler).
- Final upload is **importance-ordered** (not tier-grouped): every consumer walks a
  prefix of the buffer (GI `maxBounceLights`, reflection shadow budget, volumetric
  `maxLights`), so ordering is itself a selection layer.
- `r_rtGILightStratify 0` restores the legacy nearest-first sort for A/B popping
  comparisons; tier/take counts logged at `r_vkLogRT 1`.

---

## Part 2 — Brightness / tuning fixes

These are the root causes behind "metal way too bright" and "can't get GI to look
right". Ordered by how much they change the math (do the big movers first, tune last).

### T1. Reflections composited once, not once per light  *(in G-buffer plan, Step 8)*

`interaction.frag` adds `reflColor` in an additive per-light pass → a pixel lit by N
lights gets N× the reflection (documented limitation, `vk_reflections.cpp:16-19`).
The dedicated `refl_composite` fullscreen pass fixes this. **Every reflection-strength
constant tuned before this lands is invalid after.** Drop `r_rtReflectionBlend`
default 1.5 → 1.0 at the same time.

### T2. Grazing-angle suppression  *(in G-buffer plan, Step 8)*

Schlick → 1.0 at grazing regardless of F0 is only correct for polished surfaces; rough
surfaces are held down by shadowing/masking. Without per-pixel roughness, clamp the
grazing lobe by F0:

```glsl
float grazing = mix(r_rtSpecGrazingMax /*≈0.35*/, 1.0, smoothstep(0.3, 0.7, f0));
float fresnel = f0 + (grazing - f0) * pow(1.0 - NdotV, 5.0);
```

Matte surfaces plateau at ~0.35 reflectance at grazing; genuine metal/glass still
reaches ~1.0. Directly addresses the bright silhouette-edge rims in the screenshot
(the rest of that artifact is the wrong reconstructed normal, fixed by the G-buffer).

### T3. Per-material F0 — mirrors too dim, metals too uniform

Reflectance is hardcoded: `GLASS_F0 = 0.1` (`reflect_ray.rgen:156`), `F0 = 0.05`
(`reflect_ray.rchit:175`). A real mirror needs ~0.9+.

- Add `float f0;` to `VkMaterialEntry`/`MaterialEntry` (there is pad space; bump the
  static_asserts).
- Classify in `vk_material_table.cpp`: mirror materials (`SS_SUBVIEW` sort / `mirror`
  keyword) → 0.9; glass (`MAT_FLAG_GLASS`) keeps 0.05-0.1; others → spec-map remap
  default.
- `glass_probe.rchit` returns the hit matIdx so the rgen's `glassWeight` Schlick uses
  the material F0 instead of the constant.

### T4. Falloff-model mismatch between RT lighting and raster lighting

The raster path lights via projection/falloff **textures**; the RT hit shaders use
`atten = 1 − (d/r)²` with `intensity` defaulting to 1.0 when `SHADERPARM_ALPHA` is
unset (`vk_gi.cpp:1004-1009`). RT-lit surfaces (reflection hits, GI bounces) therefore
disagree with the same surface seen directly — one reason GI tuning feels like
whack-a-mole: `r_rtGIBounceScale 4.0` is compensating for systematically-dim RT
falloff in some rooms and overshooting in others.

Pragmatic fix (no texture sampling in hit shaders): fit the falloff curve better.
Doom 3's default falloff is roughly linear-to-zero; try `atten = (1−t)²` or `(1−t²)²`
(smoother tail than the current `1−t²`, which holds ~75% brightness at half radius —
brighter than Doom 3's textures). Add `r_rtGIFalloffMode` (0 = current, 1 = squared,
2 = smooth) and A/B against the raster image on a known room before retuning
`r_rtGIBounceScale` downward.

### T5. Emissive floor in reflections/GI

`rt_EvalEmissiveRadiance` (`rt_material.glsl:222-228`): GUI surfaces get
`max(e*3.0, 0.2)` then × `r_rtGIEmissiveScale` (2.0). The unconditional 0.2 floor makes
every dark screen pixel a light source in reflections — visible as glowing panels in
reflective floors. Make the floor conditional on the sampled texel actually being lit
(`e * 3.0` but floor only where `luminance(e) > 0.05`), or drop the floor for the
reflection path (`reflLightBuf.emissiveScale` is already a separate knob).

### T6. Final tuning pass (only after T1-T5 + P3 land)

Order: `r_rtSpecF0Gamma/Scale` via debug overlay (F0 mode) → `r_rtSpecGrazingMax` in a
grate/metal hallway → `r_rtGIFalloffMode` + `r_rtGIBounceScale` against a raster
reference room → `r_rtGIStrength/Contrast` last. Record final values in this doc.

---

## Part 3 — Recommended order of operations

Rationale: the G-buffer is both the reflection-correctness prerequisite **and** the
biggest reflection perf win; quick perf items buy the sample-count headroom that
improves GI quality immediately; invasive restructures and constant-tuning come after
the math stops moving.

```
Wave 1 — G-buffer (gbuffer_normal_pass.md commits 1+2)
         → fixes reflection direction (grates), N× brightness (T1), grazing clamp (T2),
           F0 early-out (P2). Re-enables Stage 3 reflections.

Wave 2 — Perf quick wins (each independent, small, measurable):
         P7 TLAS FAST_BUILD          (~1 line)
         P6 sceneHasGlass probe skip (~20 lines)
         P4 shadow-ray threshold in reflect rchit (~5 lines)
         P1a per-light blur skip for small rects
         L1 stratified light list + hysteresis (CPU-only)
         P5 shared rt_light_eval.glsl extraction (prep for P3)

Wave 3 — P3 stochastic GI lights, then raise r_rtGISamples with the freed budget
         (this is the noise-reduction payoff), then P9 (G-buffer commit 3: AO/GI
         normals).

Wave 4 — Structural: P1b batched shadow masks; P8 half-res volumetrics.

Wave 5 — Auto-relight (auto_relight.md): synthesized panel lights + noShadows unlock.
         Placed after P1b because each synthesized light is an interaction light +
         shadow dispatch; start with a budget of ~6 if attempted earlier.

Wave 6 — Tuning: T3 per-material F0, T4 falloff mode, T5 emissive floor, then the
         T6 constant pass — last, on top of the stable, fast, relit base.
```

Auto-relight replaces the old Stage 4a/4b plan (see `auto_relight.md`); tuning stays
last because T1/P3/auto-relight each change what the existing constants mean.

---

## Profiler checkpoints

Capture at the same spot each wave (suggest: Administration hallway from the 2026-08-08
screenshot, 2560×1440):

| Wave | TLAS | AO | Refl | GI | GI-denoise | Vol | Shadow (per-light Σ) | Total |
|---|---|---|---|---|---|---|---|---|
| baseline | | | | | | | | |
| after W1 | | | | | | | | |
| after W2 | | | | | | | | |
| after W3 | | | | | | | | |
| after W4 | | | | | | | | |
