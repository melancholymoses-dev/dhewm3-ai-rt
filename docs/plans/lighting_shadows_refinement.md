# Phase 9 — Fresnel-Correct Reflections, Shape-Aware Soft Shadows, and Pseudo-Emissive Surfaces

**Date:** 2026-07-28
**Branch:** TBD

---

## Overview

Reflections are the strongest RT effect shipped so far, but the general opaque-surface
reflection blend is currently **disabled** (`interaction.frag:132-146`) because raw
specular-map values were used directly as reflectance with no normalization or Fresnel
term, producing a "hall of mirrors" look on ordinary metal/specular surfaces. Volumetric
lighting and GI landed and work, but most of Doom 3's "light-looking" surfaces (console
panels, screens, warning lights) were never authored with a true emissive material stage,
so they contribute nothing to GI or volumetrics even though they visually read as light
sources.

This phase closes both gaps and adds one independent shadow-quality fix:

1. Shape-aware soft shadow radius (independent, cheap, do first).
2. Specular/Fresnel normalization (foundation — fixes the root cause of the hall-of-mirrors bug).
3. Re-enable opaque RT reflections on top of (2).
4. Pseudo-emissive surfaces, split into a cheap GI-only win (4a) and a harder
   volumetric-shaft win (4b), because the two features consume the emissive signal
   through entirely different pipelines.

All four are independent enough to land as separate PRs/commits, but 2→3 and 4a→4b are
hard dependencies — see **Order of Operations** at the end.

---

## Current State Audit

| Component | File | Status |
|---|---|---|
| Opaque reflection blend | `neo/renderer/glsl/interaction.frag:132-146` | **Disabled** — commented out, raw specular-map multiply, no Fresnel |
| Glass reflection Fresnel | `neo/renderer/glsl/reflect_ray.rgen:149-158` | Implemented — fixed `F0=0.1`, Schlick power-5, glass only |
| RT reflection ray/dispatch | `reflect_ray.rgen/.rchit/.rahit/.rmiss`, `vk_reflections.cpp` | Implemented, mirror-sharp only (no roughness — out of scope here) |
| Roughness from material | `vk_material_table.cpp` (SL_SPECULAR comment) | **Not implemented** — "Roughness stays at 1.0 ... revisit in Phase 6" |
| Soft shadow radius | `vk_shadows.cpp:756-765`, `shadow_ray.rgen:92-113` | Implemented, **isotropic scalar only** (`r_rtShadowSoftRadiusScale`) |
| Material emissive flag | `vk_material_table.cpp:489-535` | Implemented, narrow — only additive `SL_AMBIENT` stages, cinematics, GUI screens |
| GI emissive contribution | `vk_gi.cpp:119` (`r_rtGIEmissiveScale`), `gi_ray.rchit` | Implemented — evaluated **per ray hit**, consumes the material flag above |
| GI light list (discrete lights) | `vk_gi.cpp:101-120` (`GILightEntry`/`GILightBuffer`), populated `vk_gi.cpp:972+` | Implemented, **real Doom 3 lights only** — no synthetic entries |
| Volumetric march | `vol_march.comp`, driven by the same `GILightEntry` list | Implemented — cannot see anything not in the discrete light list |

The key structural fact driving the design below: **GI reads emission per-surface at ray
hit time; volumetrics reads emission only from the discrete light list.** A texture-based
emissive flag alone benefits GI for free but is invisible to volumetrics until a real
`GILightEntry` is synthesized.

---

## Stage 1 — Shape-aware soft shadow radius

**Status: Implemented** (`vk_shadows.cpp`, `shadow_ray.rgen`) — see below for what landed
vs. what's still worth tuning in-game.

**Do first** — independent of everything else, no dependency risk, quick "moody" win.

### Current state
`vk_shadows.cpp:756-765` derives a single scalar `softRadius` from
`minLightRadius * r_rtShadowSoftRadiusScale` (clamped by `r_rtShadowSoftRadiusMin/Max`),
passed as `lightOrigin.w` into `shadow_ray.rgen`. `jitterDirection()`
(`shadow_ray.rgen:92`) uses it to build a **circular** cone (`sinHalfAngle = radius/dist`)
around the light direction. This already gives correct distance-based penumbra growth —
it is not "no soft shadows," as an earlier pass at this survey mis-stated. The gap is
narrower: every Doom 3 light volume is treated as an isotropic sphere, but
`renderLight_t::lightRadius` is a **vector** (`p.lightRadius.x/y/z`) — Doom 3 models
elongated fixtures (fluorescent tubes, vents, wall strips) as non-uniform point-light
volumes, and projected/spot lights have a frustum shape entirely.

### Fix
1. Pass the full `lightRadius.xyz` (not `.Length()`/`Max()`) through the shadow UBO
   instead of the single scalar.
2. In `shadow_ray.rgen`, project the light's local radius axes onto the tangent plane
   perpendicular to `lightDir` at the shading point, producing two half-angles
   (`sinHalfAngleU`, `sinHalfAngleV`) instead of one. Build the cone sample as an
   ellipse: `cosTheta = mix(cosHalfAngle(u,v,phi), 1.0, r1)` where the effective
   half-angle at azimuth `phi` interpolates between the two axis half-angles.
3. For projected/spot lights (`lt==1` equivalent in the shadow path), derive the
   soft-shadow aperture from the light's **near-plane** extent (the fixture's physical
   emitting area) rather than the far-reach of the cone — a spotlight's penumbra size is
   set by the bulb/lens size, not how far the beam travels.
4. Keep `r_rtShadowSoftRadiusScale/Min/Max` as the overall magnitude controls; add the
   anisotropic projection as a multiplier on top, not a replacement.

### Debug tooling
`r_rtShadowDebugMode 7` and `8` output the two projected cone half-angles (`sinConeU`,
`sinConeV`) directly to the shadow mask, one per mode (the mask is a single-channel R8
image, so they can't be packed into R/G as originally sketched — two modes instead of
one two-channel mode). A spherical point light shows identical output for 7 and 8; an
elongated fixture or a projected-light aperture should visibly differ between them.

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
  formula `r(dir) = 1/sqrt(Σ dir_i²/a_i²)` — this is computed once per pixel (outside the
  sample loop) since it doesn't depend on the per-sample random jitter.
  `sinU == sinV` reproduces the old isotropic behavior exactly, so spherical point lights
  are visually unchanged.
- Not attempted: true axis-rotated point-light ellipsoids (would break from the
  established world-axis-aligned convention used elsewhere) and physically exact
  frustum-aperture projection (the `start`-plane scaling is an approximation, not a
  rendering of the true near-clip cross-section under perspective).

### Effort / Impact
- **Effort:** Medium — vector math in 2 files (`vk_shadows.cpp` UBO fill,
  `shadow_ray.rgen` cone construction), no new files. ~1 day.
- **Impact:** Medium-high specifically for the "moody" goal. This is a parallax-bearing
  effect — unlike volumetrics or bloom, the shadow shape visibly tracks light-fixture
  geometry as the player moves, which is exactly the kind of cue you said was missing
  from the volumetric pass.
- **Risk:** Low. Purely additive to existing correct behavior; degrades gracefully to the
  current isotropic result if the two axes are equal.

---

## Stage 2 — Specular/Fresnel normalization (foundation)

**Must land before Stage 3.** This is the actual fix for the original hall-of-mirrors
regression — re-enabling reflections without it will reproduce the same bug.

### Current state
Fresnel exists today, but **only** for glass (`reflect_ray.rgen:149-158`): a fixed
`F0 = 0.1` (or `0.05` for thin glass, `reflect_ray.rchit:171-179`) run through a proper
Schlick power-5 curve. The opaque path has no Fresnel term at all — the disabled
`interaction.frag` code multiplied the reflection sample directly by raw specular-map
luminance (`specBase`) with a single flat `specweight = 0.1` fudge factor. Doom 3's
specular maps were authored for cheap Blinn-Phong highlight shaping, not physically
plausible reflectance, so they run bright almost everywhere — any surface with a nonzero
specular texel got a strong mirror-sharp reflection, hence "everything looks like chrome."

### Fix
1. **Remap, don't reuse, the specular map.** Add a tunable curve
   (`normF0 = pow(specLuminance, r_rtSpecF0Gamma) * r_rtSpecF0Scale`) that maps the raw
   `[0,1]` specular intensity into a physically plausible F0 range (~0.02–0.08 for most
   Doom 3 surfaces, higher only where the source texel is genuinely near-white). Start
   with `r_rtSpecF0Gamma ≈ 2.5-3.0` so only the brightest specular texels (actual metal
   highlights) end up with non-trivial F0 — this is the gate that keeps ordinary
   "somewhat shiny" Doom 3 surfaces from becoming mirrors.
2. **Add a real Schlick term for opaque surfaces**, reusing `N` (`interaction.frag:79`)
   and `V` (`interaction.frag:83`, tangent-space view dir, already computed for the
   existing Blinn-Phong path):
   ```glsl
   float NdotV = clamp(dot(N, V), 0.0, 1.0);
   float fresnel = normF0 + (1.0 - normF0) * pow(1.0 - NdotV, 5.0);
   ```
3. Re-enable the reflection block (`interaction.frag:134-146`) using
   `reflColor = reflSample * fresnel` instead of `specBase * specweight`.
4. New CVars: `r_rtSpecF0Scale` (default ~0.3), `r_rtSpecF0Gamma` (default ~2.5).
5. **Debug tooling** (per project convention — visualize before tuning blind): add
   `r_rtReflectionDebugMode 1` that outputs `fresnel` as grayscale over the full scene.
   Walk a level (server room panels, weapon models, wet floors, plain concrete) and
   confirm only intended surfaces light up — concrete/cloth/skin should read near-black,
   metal/wet/glass should read bright at grazing angles and dim head-on.

### Effort / Impact
- **Effort:** Medium — shader math is small, but expect a real tuning pass across a few
  representative areas; budget ~1 day including playtesting.
- **Impact:** High — this is the prerequisite for Stage 3, and the debug-overlay step
  directly answers whether "blinding metal" is fixed before it ever reaches gameplay.
- **Risk:** Medium. The main failure mode is picking a gamma/scale that still lets too
  much of the (unreliable) specular-map data through — this is why the debug overlay is
  not optional here, tune from the visualization, not by eye on the final composite.

---

## Stage 3 — Re-enable opaque RT reflections

**Depends on Stage 2 and Stage 3.5.**

### Fix
Once Stage 3.5's G-buffer normal pass exists, re-enable the reflection blend in
`interaction.frag`: uncomment the `reflColor` block, swap in `fresnel` for the old
`specBase * specweight`, leave the ray dispatch (`vk_reflections.cpp`,
`reflect_ray.rgen`) untouched. Preserve the existing behavior where reflection is added
independently of `attenuation`/`shadow`.

**Explicitly out of scope:** glossy/roughness-blurred reflections. Reflections stay
mirror-sharp after this stage. If sharp-but-correctly-gated reflections still look too
"clean" on rough surfaces, that's a follow-up (cone-jitter or roughness blur reusing
the volumetric/AO denoiser infrastructure), not part of this phase.

### Current state
Stage 2 (Fresnel) is implemented and the reflection block was wired up, but **reverted**
after in-game testing showed that `rt_ReconstructNormal` (depth-gradient normal
reconstruction) produces mirror reflections from the geometric surface normal, not the
bump-mapped shading normal. Flat-polygon grate floors showed the ceiling reflected
back at the player. Increasing `r_rtSpecF0Gamma` does not fix this: at grazing NdotV
the Schlick term approaches 1.0 regardless of the specular map, so the wrong reflection
direction is always composited at near-silhouette angles. Stage 3 stays disabled until
Stage 3.5 is complete.

### Effort / Impact
- **Effort:** Small — a few hours, given Stage 2 and 3.5 exist.
- **Impact:** Same payoff as Stage 2, listed separately only for review-ability.
- **Risk:** Low, contingent on Stage 3.5 being validated first.

---

## Stage 3.5 — G-buffer normal pass (prerequisite for Stage 3)

**Depends on nothing. Do before Stage 3.**

### Problem
`reflect_ray.rgen` currently reconstructs the surface normal from depth gradients
(`rt_ReconstructNormal`). This returns the **geometric polygon normal**, not the
bump-mapped shading normal. For Doom 3's flat-polygon grate floors, "normal = up"
produces a mirror reflection of the ceiling instead of the floor surface. No Fresnel
tuning can fix this — the issue is the direction of the reflection ray, not its weight.

### Fix
Add a world-space normal render target written during the interaction pass and sampled
by the reflection rgen.

**1. New image (`vk_gbuffer.cpp` or alongside `vk_reflections.cpp`):**
Allocate `vkRT.gbufNormal[VK_MAX_FRAMES_IN_FLIGHT]` as `rgba8snorm`
(`SIGNED_NORMALIZED` so each channel stores `[-1, 1]`), same dimensions as the
swapchain, with `COLOR_ATTACHMENT_BIT | SAMPLED_BIT`. Follow the same
`vkReflBuffer_t`/resize pattern used by `vkRT.reflBuffer`.

**2. Framebuffer attachment:**
Add the normal image as a second color attachment in the main render pass
(`vk_backend.cpp`). The interaction fragment shader already has all vectors needed —
`N` is the bump-mapped tangent-space normal, but must be transformed to world space
first (requires passing the TBN matrix to the fragment stage, or encoding in view
space which avoids the TBN cost).

Simplest approach: encode the **view-space** normal (multiply tangent-space `N` by the
normal matrix, already computable from the existing `u_ModelViewProjection` or a
separate normal matrix uniform). View-space is sufficient for `reflect_ray.rgen` since
it already has the inverse-view-projection to reconstruct world positions.

**3. `interaction.frag`:**
Add `layout(location = 1) out vec4 gbufNormal;` and write the shading normal:
```glsl
gbufNormal = vec4(shadingNormalVS * 0.5 + 0.5, 1.0); // encode [-1,1] → [0,1]
```

**4. `reflect_ray.rgen`:**
Add a `sampler2D gbufNormalSampler` binding. Replace the `rt_ReconstructNormal` call:
```glsl
vec3 reflNormal = normalize(texture(gbufNormalSampler, uv).xyz * 2.0 - 1.0);
// transform from view space to world space using params.invViewProj's rotation block
```

**5. Resize / shutdown:** mirror the pattern already used for `vkRT.reflBuffer` in
`vk_reflections.cpp`.

### Effort / Impact / Risk
- **Effort:** Medium (~1 day) — new image, framebuffer change, one new UBO field or
  normal matrix, two shader edits, resize wiring.
- **Impact:** High — fixes the root cause of the bad grate reflections; also improves
  AO (which currently has the same reconstructed-normal problem) and enables
  roughness-proxy blur in a future stage.
- **Risk:** Low-medium. Adding a second color attachment is a well-understood Vulkan
  operation. The main risk is normal-space confusion (tangent vs. view vs. world) —
  keep a debug mode that outputs the G-buffer normal as colour and walk the level to
  confirm normals look correct before wiring them to the reflection rgen.

---

## Stage 4a — Loosen the emissive material heuristic (GI only, cheap)

**Depends on Stage 2's normalized F0** (used as an exclusion gate below).

### Current state
`vk_material_table.cpp:489-535` only flags `VK_MAT_FLAG_EMISSIVE` for materials with an
explicit additive `SL_AMBIENT` stage, a cinematic/videomap stage, or a GUI surface. GI
already consumes this flag per ray hit (`gi_ray.rchit`, scaled by `r_rtGIEmissiveScale`,
`vk_gi.cpp:119`) — **this pipeline already exists and works**; it's just blind to plain
diffuse textures that merely *look* bright (a console readout baked directly into the
diffuse map, no separate glow stage).

### Fix
Add a second, texture-content-based classification pass evaluated once per unique
diffuse image at material-table build time (same location as the existing stage walk,
`vk_material_table.cpp` ~line 520). Sample the diffuse image (or a precomputed low mip)
and flag it emissive if **all** of:
- A **small fraction** of texels exceed a brightness threshold (a discrete indicator
  light or screen, not a uniformly lit metal panel covering the whole surface).
- Those bright texels have **above-average saturation** (warning lights/screens are
  colored; a plain specular highlight on gray metal is not — this is the check that
  keeps brushed-metal panels from self-classifying as light sources).
- The material's Stage-2 normalized F0 is **not** already high across most of the
  surface — a material that reads as reflective metal shouldn't simultaneously become a
  light source; that combination is what would produce a "blinding" surface.
- Not already flagged `VK_MAT_FLAG_EMISSIVE` by the existing stage-based check (avoid
  double contribution).

When a material qualifies, set `emissiveTexIndex = diffuseTexIndex` (same pattern already
used for the GUI case, `vk_material_table.cpp` ~line 533) and set the flag — no new
runtime shader cost, it flows through the existing `gi_ray.rchit` emissive-hit path.

### Debug tooling
Add a debug overlay mode that tints newly-auto-flagged surfaces solid magenta so you can
walk a level and check for false positives (should catch server screens, warning lights;
should NOT catch brushed metal, concrete, or already-tagged materials) before enabling
`r_rtGIEmissiveScale` contribution from them.

### CVars
`r_rtGIAutoEmissive` (default `0`) gates the whole feature so it's a clean A/B toggle
against the current, narrower classification.

### Effort / Impact
- **Effort:** Medium — CPU-side texture analysis at material-table build (same point
  static entries are already built, no new runtime pass), plus the 3-part gate and debug
  overlay. ~1-2 days.
- **Impact:** Directly answers "most panels aren't actually emissive" — for GI and
  reflections. Does **not** reach volumetrics; see 4b.
- **Risk:** Medium — false positives are the main risk, mitigated by the debug overlay
  and the F0-exclusion gate reusing Stage 2's work.

---

## Stage 4b — Synthesize directed lights for volumetric shafts

**Depends on 4a's classification and on Stage 2/3 being validated in-game** (this stage
reuses the same specular-based exclusion gate, so if that gate is wrong, both GI and
volumetrics inherit the mistake). Do this last.

### Why this is a separate, harder stage
Volumetrics marches only through the discrete `GILightEntry` list
(`vk_gi.cpp:101-120`), populated exclusively from real `idRenderLightLocal` defs
(`vk_gi.cpp:972` loop). A texture flag from 4a never reaches this list on its own — there
is nothing to march toward. This is also directly what you asked earlier: **yes, a naive
point-light synthesis here would reproduce spherical emission** — a floating glow blob in
front of a flat screen, exactly the "floating blob" singularity artifact already fixed for
real point lights in `tonemapping2.md` §7b. A flat panel has no physical basis for
omnidirectional emission.

### Fix
1. For each surface flagged by 4a, compute a world-space placement: surface centroid and
   average outward normal, both derivable from the BLAS vertex data already walked during
   material-table build.
2. Synthesize a `GILightEntry` with **`lightType = 1` (projected/spot), not `0` (point)**.
   Set `coneDir = surface normal`, `cos(halfAngle) ≈ cos(90°)` (hemisphere emission) so
   light exits outward from the panel into the room, not radiating a sphere through the
   wall behind it.
3. Budget: cap total synthetic lights per level (e.g. top 16–32 by bright-cluster score),
   merged into the same nearest-first candidate sort already in the `vk_gi.cpp` collection
   loop (~line 972), so this can't blow `VK_GI_MAX_LIGHTS = 128` or the volumetric march's
   `r_rtVolMaxLights = 32` cost budget. Log how many candidates were dropped by the cap —
   don't truncate silently.
4. New CVar `r_rtVolAutoEmissive` (default `0`), independent of `r_rtGIAutoEmissive` so
   the two contributions can be A/B'd separately (a panel might look right feeding GI but
   wrong feeding a visible light shaft, or vice versa).

### Effort / Impact
- **Effort:** Large — new placement/orientation logic, a new light-type synthesis path
  feeding a pipeline that previously only saw real lights, budget/culling, and by far the
  most likely to need iteration since "does the shaft look right" is a visual judgment
  call, not a formula. ~3-5 days.
- **Impact:** This is the one that actually delivers what you originally asked for —
  visible light shafts from server panels/screens/vents — but it's the riskiest piece in
  this doc.
- **Risk:** High. Recommend not starting this until 4a has been walked through a level and
  Stage 2/3 have shipped without reintroducing blinding surfaces, since 4b's placement
  quality depends entirely on 4a's classification being clean.

---

## Order of Operations

```
1. Stage 1   (shadow shape)           — independent, safe, do anytime
2. Stage 2   (Fresnel/specular)       — foundation, must precede 3 and 4a's F0 gate
3. Stage 3.5 (G-buffer normal pass)   — prerequisite for Stage 3
4. Stage 3   (re-enable reflections)  — depends on 2 + 3.5
5. Stage 4a  (GI auto-emissive)       — depends on 2 (F0 exclusion gate)
6. Stage 4b  (volumetric shafts)      — depends on 4a + validated 2/3
```

Stage 1 has no dependency on the others. Stages 2 and 3.5 can be developed in
parallel — they touch different parts of the pipeline.

---

## Effort / Impact / Risk Summary

| Stage | Effort | Impact | Risk | Depends on | Status |
|---|---|---|---|---|---|
| 1 — Shape-aware shadows | Medium (~1d) | Medium-high, parallax-bearing | Low | none | **Done** |
| 2 — Fresnel/specular normalization | Medium (~1d) | High (unblocks 3) | Medium | none | **Done** |
| 3.5 — G-buffer normal pass | Medium (~1d) | High (fixes reflection direction) | Low-medium | none | Not started |
| 3 — Re-enable reflections | Small (hrs) | High | Low | 2, 3.5 | **Reverted** — needs 3.5 |
| 4a — GI auto-emissive | Medium (~1-2d) | Medium-high (GI/reflections only) | Medium | 2 | Not started |
| 4b — Volumetric light shafts | Large (~3-5d) | High (the actual ask) | High | 4a, 2, 3 | Not started |

---

## File Checklist

| File | Stage | Change |
|---|---|---|
| `neo/renderer/Vulkan/vk_shadows.cpp` | 1 | Pass anisotropic `lightRadius.xyz`; near-plane aperture for spot lights |
| `neo/renderer/glsl/shadow_ray.rgen` | 1 | Elliptical cone jitter; new debug mode |
| `neo/renderer/glsl/interaction.frag` | 2, 3 | Add F0 remap + Schlick term; re-enable reflection blend (after 3.5) |
| `neo/renderer/Vulkan/vk_pipeline.cpp` | 2 | New UBO fields for Fresnel CVars; G-buffer normal attachment (3.5) |
| `neo/renderer/Vulkan/vk_reflections.cpp` | 2 | `r_rtSpecF0Scale`, `r_rtSpecF0Gamma`, `r_rtReflectionDebugMode` CVars |
| `neo/renderer/Vulkan/vk_gbuffer.cpp` (new) | 3.5 | G-buffer normal image alloc, resize, shutdown |
| `neo/renderer/glsl/reflect_ray.rgen` | 3.5 | Read G-buffer normal instead of `rt_ReconstructNormal` |
| `neo/renderer/Vulkan/vk_material_table.cpp` | 4a | Texture-content emissive classification pass + gate |
| `neo/renderer/Vulkan/vk_gi.cpp` | 4b | Synthesize directed `GILightEntry` records, budget/cap, logging |
| `neo/framework/Dhewm3SettingsMenu.cpp` | 1,2,4a,4b | Expose new CVars in the settings menu, matching existing sections |
| `docs/plans/lighting_shadows_refinement.md` | — | This doc |

---

## Notes for whoever (human or LLM) implements this

- Every stage above has an explicit debug-visualization step. Use it before tuning
  constants by eye on the final composited image — that's how the original hall-of-mirrors
  bug should have been caught, and it's the fastest way to confirm each fix actually
  targets the surfaces it's supposed to.
