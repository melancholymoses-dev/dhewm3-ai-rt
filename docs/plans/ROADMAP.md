# RT Roadmap

**Status reviewed:** 2026-09-19
**This is the entry point.** If you're wondering what to work on or which plan doc
is authoritative, start here. This file owns *ordering* and *status*; detailed
designs live in the linked docs. Prior cycle: `completed/202608_ROADMAP.md`.

---

## Design pillars (the taste contract)

Every stage below serves these; anything that fights them gets cut or demoted.

1. **Shadows are the feature.** Soft, shaped, from more lights. This is where
   "dynamic lighting" actually lives.
2. **Darkness stays black.** GI may tint lit regions; it must never lift the noise
   floor of dark ones.
3. **Light the air sparingly.** Volumetrics from hero lights only.
4. **Reflections are set dressing.** Glass and mirrors, plus the odd hero surface by
   explicit opt-in.  The assets carry no PBR data; don't pretend they do.
5. **No map editing.** Engine-side rules + budgets + debug overlays. Small def-file
   mods are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.

---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 1 | **Reflection gating rework** — reflections were charging a flat per-pixel rate over the whole screen for sub-1% radiance. Now glass-only. | `completed/20260911_reflection_gating.md` | ✅ **Done.** R1/R2/R4/R6 landed 2026-09-12, validated in-game 2026-09-18. R5 (what the surviving pixels are worth) moved to 1b and closed there |
| 1b | **Reflection brightness** — R5 split out and expanded after in-game validation. Reflections are dim and the player self-shadows in glass. | `completed/20260918_reflection_brightness.md` | ✅ **Closed 2026-09-19.** B0/B1 landed; **B2 dropped** (premise inverted — GI/vol are over-bright, not dark; handed to arc 2's G6 retune); **B3 delivered by constants** (`r_rtReflectionBlend` 2.0 × `GLASS_F0` 0.15 = 0.30, vs the 0.4 the plumbing was going to buy); B4 deferred; B5 decided = accept the dark-room regime, bloom later, never the toe |
| 1c | **Dynamic model normals are stale in RT** — skinned md5 meshes reach the TLAS with current positions and bind-pose normals, because `R_DeriveTangents` is gated on the raster frontend's decision to draw and RT consumes geometry the frontend culled. Found under 1b; affects GI and volumetrics too, not just reflections. | `completed/20260919_dynamic_model_normals.md` | ✅ **Closed 2026-09-19.** N1 landed + measured: the deferral is overridden for deformed surfaces whenever the TLAS is live, 0.16 ms/frame with 12 characters on screen. N2 and N3 dropped — the cost doesn't justify the surgery, and the fix is unconditional so there is no frustum edge left to test |
| 2 | **Froxel volumetrics + probe GI** — move vol/GI sampling out of screen space into world-space caches; deletes most of the GI noise-fighting chain structurally. | `20260906_froxel_probe_gi.md` | 🟡 **Part A: only F5 (retire decision) left.** F0-F2 landed + validated (vol 1.63 → 0.29 ms median, 5.6×, and visually better); F3/F4 dropped; F6 (background attenuation) and F7 (cone penumbra + soft cookie edge) landed; transport coefficients made physical and tuned in play — see completed doc. **Part B: G0-G4 landed + validated. G6 flipped `r_rtGIProbes` to 1 on 2026-09-19** — probe GI is the shipped default, decided on appearance, with G5b's flicker veto consciously waived and the per-pixel path *kept*, not retired. **Left: G5, G5b and G6's retune.** G5b is a live defect in the default path (flickering lights smear, τ ≈ 8.9 s), is the highest-value open item here, and is **not gated by G5's fix 1** — its classifier is CPU-side on exact light data, so the two are independent. **G5 fix 1 measured + closed 2026-09-19:** `256 rays × 4096 updates` costs +0.51 ms (chain 0.75 → 1.27 ms, still ~4× under per-pixel) but **does not reach a 5 Hz flicker** — τ ≈ 8 frames against a 6-frame half-cycle, so it is a door/moving-light fix only, as claimed. Two cost models inverted in the process: the trace is latency-bound (8× work, 2.8× time) and the **blend**, not the trace, is now the dominant probe pass. The retune is owed because the defaults still carry the per-pixel path's tuning — GI/vol reported over-bright right after the flip. |
| 3 | **FSR upscaling** — every RT pass is screen-resolution, so decoupling render res from display res is worth ~6.6 ms of the 11.93 ms RT budget. U0 (resolution split + bilinear resolve) delivers the whole perf win with no third-party code; U1-U5 buy the image quality back. | `20260918_fsr_upscaling.md` | 🔴 **Not started, design only.** SDK: standalone FidelityFX-FSR2 2.2.1, Vulkan backend, MIT. **Now unblocked** — arc 1 is closed, and U0 no longer needs to precede G6 (the probe-vs-per-pixel choice was made on appearance, not cost, so the resolution-scaling argument below no longer gates it) |

### Sequencing note — revised 2026-09-19

**Two of the three reasons FSR went last have expired.** Arc 1 is closed, so reason 1
(don't debug glass underneath a reconstruction filter) no longer applies. Reason 2 is gone
in a different way than expected: G6 kept the per-pixel path rather than retiring it, so
U4's denoiser-chain retune is not tuning something scheduled for demolition — but it is now
tuning a path that is no longer the default, which lowers its value rather than its risk.

**The U0-before-G6 exception is withdrawn.** It rested on G6 being a cost/quality decision
that render scale would tilt: per-pixel GI is entirely screen-resolution work (4.41 →
~1.95 ms at 0.67) while the probe trace is probe-count-bound and barely moves (G2:
0.048 ms), so lower render resolution shrinks the probe path's advantage and the decision
could end up made twice. G6 was decided on appearance instead, so there is no cost decision
left for U0 to tilt. U0 can go wherever it fits.

**What still holds — reason 3, pillar 6.** Arc 2's G6 retune is open and GI/vol are being
tuned right now. Adding sub-pixel jitter and a reconstruction filter underneath a live
tuning loop destroys the ability to attribute a change. Land the constants first, then
change the sampling. That puts FSR after G6's retune, but nothing further back than that.

### Measured RT budget (Mars City, 2026-09-11)

`r_vkRTProfile 1`, median ms per phase. This is the checkpoint the froxel arc was
waiting on, and it sets the ordering.

| GI | Refl (before) | Vol | AO | denoise chain | TLAS | **RT total** |
|---|---|---|---|---|---|---|
| 4.41 | 3.24 | 1.53 | 1.17 | ~0.65 | 0.15 | **11.93** |

**GI is the largest single cost**, which is why the froxel/probe arc is next and why
it is the bigger perf prize (GI + Vol + denoise ≈ 6.6 ms). Reflections were second at
27 % of the budget.

### Decisions taken 2026-09-12

- **Reflections are glass-only.** `r_rtReflectionMode 1` (default) dispatches rays only
  over the union screen rect of the view's `SURFTYPE_GLASS` surfaces, and skips
  `vkCmdTraceRaysKHR` entirely when the view holds no glass. Mode 2 keeps the old
  full-screen path for A/B.
- **Per-material F0 (tuning item T3) is dropped, not deferred.** With opaque geometry
  never reflecting there is nothing for a material F0 table to classify. If a specific
  hero surface is wanted later it returns as a small opt-in list.
- **Threshold tuning cannot fix reflection looks.** Max achievable F0 under the spec-map
  remap is 0.2, and the worst artifacts (mirror specks on rack edges, unlit fixture
  housings) are the *highest*-F0 pixels in the scene while any weight threshold culls
  from the bottom. There is no setting between "all on with artifacts" and "all off".
- **Grazing-angle tuning has a narrow reach.** The Schlick tail `pow(1-NdotV,5)` is
  ≤ 0.0007 until ~40° off-normal, so grazing knobs only affect near-silhouette pixels —
  useful for floors viewed along, a no-op on wall panels.
- **Engine-wide rule: any ray origin built from `rt_ReconstructWorldPos` must floor its
  bias at `d^2*ulp/znear`.** A fixed bias is a distance-limited bias. This caused A12 in
  both AO and shadows; GI and volumetrics share the same reconstruction and the same
  latent exposure. Check the bias before reaching for a new G-buffer target — and note
  `r_znear` is game-owned and drops to 1.0 in cinematics, cutting every safe distance by
  sqrt(3).

### Findings 2026-09-18 (reflections) — see `completed/20260918_reflection_brightness.md`

- **RT direct lighting is half the raster path's, everywhere.** `RB_DetermineLightScale`
  applies `r_lightScale` (=2) to every raster light colour; the RT light upload takes raw
  `shaderParms` and pins `intensity = 1.0f`. This affects reflections, GI bounce *and*
  volumetrics — all three were tuned by eye around the shortfall, so fixing it requires
  retuning them, not just landing it.
  **Superseded 2026-09-19 — do not action this.** True at the source, but the downstream
  gains did not merely absorb the 2×, they overshot it: GI and volumetrics read *too
  bright*. B2 was dropped for that reason. The over-brightness belongs to G6's retune.
- **The tonemap toe has two regimes, not one.** At `r_rtTonemapToe 2.7` the blended curve's
  slope is <0.33 below base luminance 0.07 but *exceeds 1.0* between 0.12 and 0.3 — the
  toe→linear `smoothstep` is steeper than either section. So it crushes additive effects
  only in already-dark scenes and amplifies them in mid-dark ones. Quoting the toe branch
  alone overstates the crush; evaluate the whole curve before blaming tonemapping.
- **Reflected surfaces get no indirect light.** GI is screen-space and modulates by the
  *primary* G-buffer albedo, so geometry seen in a mirror is direct-lit only, plus a
  hardcoded 0.01 floor. Structural, not a tuning miss.
- **Engine-wide rule: a shadow ray's cull mask is not a constant.** `noSelfShadow` gets
  instance mask `0x01` and every consumer must choose — `shadow_ray.rgen` and the glass
  probe use `0xFE`, but `rt_light_eval.glsl` hardcoded `0xFF`, so the player self-shadowed
  in reflections and nowhere else. Fixed in B1. Any new shared ray helper takes the mask as
  a parameter. Note B1 only removed *half* the vertical banding — the rest was stale
  dynamic-model normals (1c), which the cull-mask fix exposed underneath.

### Decisions taken 2026-09-18 (volumetrics)

- **A gain on the light is legitimate; a gain on a medium coefficient is not.** The
  medium has exactly two constants (σ_t, albedo) and they are global — extinction is
  not a property of any one light. Per-class punch belongs on the light's radiance,
  where it conserves energy by construction. Only albedo > 1 creates energy; a large
  gain does not, and a diagnostic that conflates them will cry wolf during tuning.
- **Pillar 3 has a cost that tuning cannot remove.** A participating medium compresses
  dynamic range from both ends — airlight raises the black floor, extinction lowers the
  highlight ceiling — and Doom 3's art direction is built on the opposite. **Albedo is
  the lever**, not extinction or the tonemap toe: it cuts in-scatter while leaving
  attenuation intact, so the medium darkens more than it glows and lit-vs-unlit contrast
  goes *up*. Reach for it before the toe, which is global and also crushes real shadow
  detail.
- **Separating general haze from shaft punch is the return on the refactor.** Point
  lights fill rooms with veil, directed lights only reach where they point, so a large
  gain spread between the classes (6:1 in play) buys drama locally without paying for it
  everywhere. The old single-density parameterisation could not express this.

---

## Live documents

| Doc | Owns |
|---|---|
| `rt_optimization_tuning.md` | Perf items P1-P10, light-list L1, tuning items T1-T6. Waves 2-4 done; **T3-T6 not started** (T3 is absorbed into the reflection rework above). |
| `20260906_froxel_probe_gi.md` | World-space caching arc (arc #2), including G5b flicker factorization. |
| `20260918_fsr_upscaling.md` | Render-resolution decoupling and FSR upscaling (arc #3). Also owns motion vectors and jitter, which any future upscaler/TAA would share. |
| `20260906_bloom_plan.md` | Bloom post-process — unimplemented; the tonemapped HDR pipeline it needs now exists. |
| `see_first_person_player_model.md` | First-person player body; orthogonal to the lighting arc. |
| `../vulkan_debugging.md` | Not a plan — the reference for getting Vulkan validation/GPU-AV output out of this engine. Load it before chasing any AMD-vs-NVIDIA or device-lost bug. |

---

## Completed

All in `completed/`. Waves 1-7 of the original roadmap are done.

| Doc | Owns |
|---|---|
| `20260826_amd_vulkan_cleanup.md` | AMD-vs-NVIDIA RT correctness. A1/A3/A5/A8/A11 landed; **A12 (far-field shadow flicker) has one zero-code experiment left** — `r_rtShadowSoftRadiusScale 0` picks between a cheap fix and a reversed-Z projection change. A2/A4/A6/A7 minor/latent. |
| `20260831_rt_temporal_cut_detection.md` | Camera-cut detection rewritten to test camera position/orientation instead of ill-conditioned matrix elements; also fixed the GUI/HUD overlay's degenerate second `RC_DRAW_VIEW` re-running AO/Refl/GI/Vol every frame. |
| `20260905_rt_projected_light_cookies.md` | Projected-light cookie/gobo textures in direct lighting, reflections, GI and volumetrics. All 4 stages, in-game validated. |
| `20260917_vol_transport_coefficients.md` | Split the volumetric medium into σ_t + albedo + per-class radiance gains, replacing one `density` cvar that was extinction, scattering and brightness at once. Both integrators were already structurally correct — this was a semantics fix, zero resource change. Tuned in play. |
| `20260810_auto_relight.md` | Synthesized shadow-casting lights from emissive panels. |
| `20260808_gbuffer_normal_pass.md` | G-buffer normal/F0 prepass. |
| `20260816_portal_area_lights.md` | Stage 2 (transition blend) was shelved on the temporal bug, now unblocked — not currently scheduled. |
| `20260831_rt_parallel_sun_lights.md` | Sun/parallel lights in GI/vol/reflections. **Not pursuing.** |

---

## Backlog (not in the current arc, not dead)

| Item | Doc | Note |
|---|---|---|
| Tuning items T4-T6 | `rt_optimization_tuning.md` | Falloff-mode A/B, emissive floor, final constants pass. Stable base; polish. **T3 is dropped** — see decisions above. |
| ~~A12 far-field flicker~~ | `completed/20260826_amd_vulkan_cleanup.md` | ✅ **Fixed 2026-09-12, in-game validated.** Ray-origin bias was swamped by depth-reconstruction error (`d^2*ulp/znear`). Shadow bias now floors at the error term; AO fades out, band scaled by `sqrt(znear/3)`. Linear-depth G-buffer **not needed** and deferred. |
| Projectiles in reflections | `completed/20260423_reflection_enhancements.md` AR3 | Sprite attempt reverted (`f37f071b`); needs a new approach. |
| Roughness-blurred reflections | — | Now the *only* route to reflective non-glass surfaces: sharp mirror reflection is why opaque geometry looks wrong, so "dimmer" can't fix it. Affordable for the first time now the traced pixel set is tiny. Not scheduled. |
| Runtime emissive-state lights | `completed/20260810_auto_relight.md` | v2 of auto-relight. |
| Translucent square borders over reflections | `completed/20260423_reflection_enhancements.md` | Polish. |
