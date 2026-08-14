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

### P2. Reflection rays traced for every pixel, reflective or not

`reflect_ray.rgen` traces a glass-probe ray + a mirror ray per non-sky pixel, and each
opaque hit (`reflect_ray.rchit:98-149`) loops ≤128 lights firing ≤8 shadow rays. On the
vast majority of pixels (concrete/cloth, F0 ≈ 0) the result is then multiplied to
~nothing by Fresnel.

**Fix:** the G-buffer F0 early-out (`gbuffer_normal_pass.md` Step 7). Pixels with
`F0 < 1/255` and no glass skip the reflection ray *and* its shadow rays entirely.
In typical Doom 3 scenes this culls well over half the reflection workload.

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

**✅ Implemented (Wave 3, 2026-08-14):** `rt_EvalDirectLightingStochastic` in
rt_light_eval.glsl — two passes over the light list (total weight, then a CDF walk
per draw) so no per-light array is stored; weights are unshadowed contribution
luminance via the shared `rt_LightContribution` helper; a repeated winner reuses the
previous draw's shadow result. Selection seed is world-anchored to the hit position
(`rt_LightSelectionSeed`) so temporal accumulation converges. GI wiring:
`r_rtGIStochasticLights` (default 2, 0 = legacy full loop) through a new GIParams
field; the stochastic candidate pool is ALL uploaded lights (not
`r_rtGIMaxBounceLights`, which now only caps the legacy path) — composing with L1 as
designed. `r_rtGISamples` default raised 4 → 8 with the freed budget.
**Reflections intentionally not switched:** they have no temporal denoiser (see P10),
so stochastic noise there would flicker; the shared function is ready when that lands.

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

### P9. Depth-reconstruction ALU in three rgens (after G-buffer)

`rt_ReconstructNormal` costs 5 depth fetches + 4 `invViewProj` matrix multiplies per
pixel and runs in AO, GI, and reflections. Commit 3 of the G-buffer plan replaces it
with one G-buffer fetch in all three — modest ALU win, plus bump-mapped AO/GI
directions (quality).
**✅ Implemented (Wave 3, 2026-08-14):** ao_ray.rgen and gi_ray.rgen sample
gbufNormalSampler at binding 5 (same slot as reflections; AO skips binding 4 for
parity). Validity test is the **rgb ≠ 0.5-clear-sentinel**, not alpha — a == 0 only
means F0 == 0, and AO/GI want the bump normal on non-specular surfaces too (unlike
reflections, which early-out on those pixels anyway). Fallback to
rt_ReconstructNormal per pixel when the prepass didn't cover it; the 1×1 null image
(exported as `VK_RT_GetNullGbufView()`) binds when the G-buffer is unsupported. The
existing whole-RT-block barrier in vk_backend.cpp already covered AO/GI dispatch
order, so no new barriers were needed.

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
