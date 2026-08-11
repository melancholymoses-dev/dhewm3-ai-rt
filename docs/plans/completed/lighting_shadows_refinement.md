# Phase 9 — Shape-Aware Soft Shadows & Fresnel Normalization (RECORD)

**Date:** 2026-07-28 (slimmed 2026-08-10)
**Status:** Stages 1–2 landed; all remaining stages migrated to newer docs.
**Where the live work went:** see `docs/plans/ROADMAP.md`.

This doc is now a **record of what shipped** in Phase 9 plus pointers for the stages
that were redesigned. The original Stage 3.5 / 4a / 4b designs were removed:

- **Stage 3.5 (G-buffer normal pass)** → superseded by `gbuffer_normal_pass.md`.
  The original design wrote normals during the interaction pass, but reflection rays
  dispatch *before* the interaction pass each frame (`vk_backend.cpp` ~4228-4373), so
  the normals would always be one frame stale. The corrected plan writes normals + F0
  from the depth prepass.
- **Stage 3 (re-enable opaque reflections)** → absorbed into `gbuffer_normal_pass.md`
  Step 8. The design changed: reflections are no longer re-enabled inside
  `interaction.frag` (which multiplies the contribution once per light); they composite
  once per frame in a dedicated fullscreen pass. Current state: the `interaction.frag`
  reflection block is disabled pending that work.
- **Stages 4a/4b (auto-emissive / synthesized volumetric shafts)** → superseded by
  `auto_relight.md`. The texture-content classifier (4a) was judged fragile — the
  stage-based `MAT_FLAG_EMISSIVE` flag is the reliable signal; per-frame GPU
  `GILightEntry` synthesis (4b) was replaced by CPU-side real-light creation at map
  load (`AddLightDef`), which feeds interaction, RT shadows, and volumetrics through
  existing pipelines.

---

## Stage 1 — Shape-aware soft shadow radius  ✅ LANDED

Every Doom 3 light volume was treated as an isotropic sphere for soft-shadow purposes,
but `renderLight_t::lightRadius` is a vector — elongated fixtures (fluorescent tubes,
wall strips) and projected-light apertures deserve anisotropic penumbrae.

### Implementation notes (what actually landed)
- CPU side (`vk_shadows.cpp`): per-light, computes three world-space axis/half-extent
  pairs instead of one scalar. Point lights use the axis-aligned ellipsoid semi-axes
  (`light.lightRadius.xyz` directly — axis rotation intentionally ignored, matching the
  existing convention in `vol_march.comp`/`vk_gi.cpp`). Projected lights use
  `light.right`/`light.up` rotated into world space by `light.axis`, scaled by
  `start.Length()/target.Length()` to approximate the near-clip aperture size.
  `r_rtShadowSoftRadiusScale/Min/Max` are applied per-axis, unchanged in meaning.
- GPU side (`shadow_ray.rgen`): the old isotropic `jitterDirection()` was replaced with
  `jitterDirectionAniso()`, which builds an elliptical cone from two tangent-plane
  half-angles instead of one. The half-angles come from projecting the light's ellipsoid
  (or flat aperture, for projected lights) onto the tangent plane via the implicit-surface
  formula `r(dir) = 1/sqrt(Σ dir_i²/a_i²)` — computed once per pixel (outside the
  sample loop). `sinU == sinV` reproduces the old isotropic behavior exactly.
- Debug: `r_rtShadowDebugMode 7`/`8` output the two projected cone half-angles to the
  shadow mask (one per mode; the mask is single-channel R8). Spherical lights show
  identical output for 7 and 8; elongated fixtures visibly differ.
- Not attempted: true axis-rotated point-light ellipsoids (would break the established
  world-axis-aligned convention) and physically exact frustum-aperture projection (the
  `start`-plane scaling is an approximation).

**Relevance going forward:** this machinery is what gives auto-relight's synthesized
panel lights (`auto_relight.md` §5) correctly wide, soft penumbrae — the `lightRadius`
ellipsoid feeds the anisotropic aperture directly.

---

## Stage 2 — Specular/Fresnel normalization  ✅ LANDED

Doom 3 specular maps are Blinn-Phong highlight masks, not reflectance data — used raw,
any nonzero specular texel produced a mirror ("hall of mirrors" bug). Landed:

- Power-curve remap in `interaction.frag:118-120`:
  `normF0 = clamp(pow(specLum, r_rtSpecF0Gamma) * r_rtSpecF0Scale, 0, 1)` followed by
  a Schlick power-5 Fresnel term.
- CVars `r_rtSpecF0Scale` (0.3), `r_rtSpecF0Gamma` (2.5) in `vk_reflections.cpp`.
- Debug: `r_rtReflectionDebugMode 1` outputs the Fresnel term as greyscale — tune from
  the visualization, not by eye on the composite.

**Post-landing findings** (drove the redesigns tracked in `ROADMAP.md`):
- The remap alone cannot fix grate floors: at grazing NdotV, Schlick → 1.0 regardless
  of F0, and the depth-reconstructed geometric normal gives the wrong reflection
  *direction* → G-buffer plan.
- The per-light additive accumulation of `reflColor` (N lights = N× reflection) is a
  brightness bug independent of Fresnel → reflection composite pass
  (`gbuffer_normal_pass.md` Step 8).
- Grazing blowout on rough surfaces needs a roughness-aware clamp
  (`rt_optimization_tuning.md` T2).

---

## Final status

| Stage | Status | Live continuation |
|---|---|---|
| 1 — Shape-aware shadows | **Done** | tuning only |
| 2 — Fresnel/specular normalization | **Done** | constants revisited in `rt_optimization_tuning.md` T6 |
| 3.5 — G-buffer normal pass | Superseded | `gbuffer_normal_pass.md` |
| 3 — Re-enable reflections | Superseded (reverted in code) | `gbuffer_normal_pass.md` Step 8 |
| 4a — GI auto-emissive | Superseded | `auto_relight.md` |
| 4b — Volumetric light shafts | Superseded | `auto_relight.md` |
