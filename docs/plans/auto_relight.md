# Auto-Relight — Engine-Synthesized Lights from Emissive Surfaces

**Date:** 2026-08-10
**Branch:** shaped-shadows
**Replaces:** Stages 4a/4b of `completed/lighting_shadows_refinement.md`.
**Companion docs:** `rt_optimization_tuning.md` (P1b shadow batching is the perf
enabler; L1 stratified light list is the selection layer), `gbuffer_normal_pass.md`.

---

## Goal

Make Doom 3's glowing fixtures — LED screens, light panels, strip lights, consoles —
behave as **real, shadow-casting light sources**, with **zero map editing**. Target
shot: a zombie walks in front of a bank of LED screens and casts a soft moving shadow
on the floor *and* a silhouette through the volumetric glow.

Design constraints (from project owner):

- No per-map hand authoring. Original assets only; small def-file mods are acceptable
  (weapons/particles), map/geometry edits are not.
- Doom 3's look must survive: darkness stays black; scarcity of light is the aesthetic.
  Budgeted hero lights, not global illumination-by-a-thousand-panels.

## Why this works end-to-end (verified against current code)

The entire consumption side already exists. A synthesized light that becomes a real
`idRenderLight` def flows automatically through:

1. **Interaction pass** — real diffuse/specular pools on floors (per-light raster).
2. **RT shadow dispatch** — per-light shadow mask; a zombie between panel and floor
   blocks the rays. Stage 1's anisotropic soft radius (`vk_shadows.cpp`) accepts the
   panel's physical extent, so the penumbra is correctly wide and soft for an LED wall.
3. **Volumetrics** — `vol_march.comp:267-286` fires **ray-query occlusion tests toward
   each light at every march step**, and monsters are in the TLAS as dynamic BLAS
   instances. The zombie's volumetric silhouette in the panel glow is therefore not a
   new feature — it falls out of feeding the existing march a new light.
4. **GI bounce + reflections** — via `VK_RT_UploadGILights` collecting from
   `lightDefs` like any other light.

No new GPU code paths. The whole feature is a CPU-side loader pass plus budget/dedupe
logic.

## Non-goals

- **No texture-content classification** (old Stage 4a's brightness/saturation
  heuristic). The reliable emissive signal is the *material structure*: the stage-based
  `MAT_FLAG_EMISSIVE` walk in `vk_material_table.cpp:489-535` (additive `SL_AMBIENT`
  stage, cinematic, GUI surface). Panels that glow in Doom 3 are authored with those
  stages. If specific wanted materials are missed, extend by material-name pattern
  (`textures/base_light/*`, `lights/*`) — a rule, not a per-map list.
- **No per-frame GPU light synthesis** (old Stage 4b). Load-time CPU synthesis is
  simpler, cheaper, and feeds every pipeline at once.

---

## Architecture

```
Map load complete (tr.primaryWorld populated)
  └─ VK_AutoRelight_Generate()              [new: vk_auto_relight.cpp]
       1. Walk static world surfaces; keep those whose material passes the
          emissive test (same logic as the material-table stage walk — extract
          a shared helper so the two can't drift).
       2. Cluster emissive triangles into physical fixtures.
       3. Score clusters; apply budget.
       4. Dedupe against existing map lights.
       5. AddLightDef() one synthesized light per surviving cluster.
       6. Log a summary table (accepted / deduped / dropped-by-budget).
```

### 1. Surface harvest

Walk the world model surfaces (portal-area models, same geometry the static BLAS
build already walks in `vk_accelstruct.cpp` — reuse that iteration pattern). For each
surface whose material is emissive, collect triangles: world-space centroid, area,
normal, and UV centroid (for color sampling later).

### 2. Clustering (one fixture = one light)

A single surface can span several physical panels (a strip of six ceiling lights is
often one draw surface). v1 algorithm — deliberately simple:

- Bucket emissive triangle centroids into a world-space grid (cell ≈ 48 units).
- Union adjacent occupied cells (6-connectivity) into clusters.
- Per cluster: area-weighted centroid, area-weighted average normal, total emissive
  area, AABB extents projected onto the normal's tangent plane (the fixture's physical
  width/height — feeds the soft-shadow aperture).
- Reject clusters with wildly disagreeing normals (|avg normal| < 0.7 after
  normalization → a wraparound glow strip; skip, log it).

### 3. Scoring and budget

```
score = totalArea × emissiveLuminance × styleWeight
```

- `emissiveLuminance`: average luminance of the emissive stage's image over the
  cluster's UV range. Implementation note: sample the idImage's CPU-side data if
  retained; otherwise compute a per-image average once at load (small images, cheap)
  and cache it. Fallback: 1.0 (white).
- `styleWeight`: 1.0 default; GUI/videomap surfaces ×1.5 (screens are the money shot).
- Keep the top `r_rtAutoRelightMax` (default **16**) clusters per map. Log what was
  dropped and its score — never truncate silently.

### 4. Dedupe (critical — prevents the grey washout returning)

For each cluster, search existing `lightDefs` within `r_rtAutoRelightDedupeDist`
(default **96** units) of the proposed origin. If a map light exists there and its
color is broadly similar (hue distance below threshold, or either is near-white), the
mappers already lit this fixture → **skip synthesis**, but optionally tag that existing
light for the noShadows unlock (see §6). This rule is what makes auto-relight additive
instead of double-bright.

### 5. Light synthesis

Per surviving cluster, `tr.primaryWorld->AddLightDef()`:

- **Origin:** centroid + normal × `r_rtAutoRelightOffset` (default 8).
- **Type:** point light with `lightRadius` shaped as a flattened ellipsoid — tangent
  extents ≈ cluster half-extents × 2.5, normal extent ≈ `r_rtAutoRelightReach`
  (default 160). Rationale: Doom 3 projected-light frustums can't express a ~180°
  panel wash; an offset point light can, and **backward leakage through the wall is
  killed by the RT shadow mask** (the panel's own wall occludes the rays).
  ⇒ Synthesized lights therefore MUST have RT shadows enabled — they are never
  `noShadows`. Force `r_rtShadows`-path inclusion regardless of other heuristics.
- **Color/intensity:** average bright-texel color of the emissive image;
  `SHADERPARM_ALPHA` = `r_rtAutoRelightIntensity` (default 0.5 — start dim; these are
  accents, not primary illumination).
- **Soft-shadow shape:** `lightRadius` ellipsoid already feeds Stage 1's anisotropic
  aperture — the LED wall gets a wide penumbra for free.
- Mark the def (e.g. a flag in `renderLight_t` parms or a registry of defHandles owned
  by auto-relight) so cleanup on map change is exact and so debug tooling can identify
  them.

### 6. noShadows unlock for existing fixture lights (companion rule)

Many mapper-placed fixture lights are `noShadows` — a 2004 stencil-budget decision the
RT path inherits (skipped at `vk_gi.cpp:993` for GI/vol; the interaction path honors
the flag too). Engine-side rule, cvar-gated (`r_rtUnlockNoShadows`, default 0):
lights with `noShadows` whose radius < threshold (default 300 — skip the giant ambient
fills, which use noShadows *semantically*) get RT shadows anyway. This recovers drama
from lights that already exist, at zero placement risk. Dedupe (§4) can feed it: a
skipped cluster whose paired map light is noShadows is a strong unlock candidate.

### 7. Weapons / projectiles / particles (def-mod scope, allowed)

Muzzle flash and projectile lights are already real dynamic lights flowing through the
RT shadow dispatch; where they're `noShadows` in defs, a small def patch (allowed per
project owner) or the §6 unlock rule turns them into moving shadow casters — plasma
bolts strobing shadows down a corridor. No engine work beyond §6. Particle-emitted
lights, if wanted later, are also def-side (`emitLight` style additions), not engine.

---

## Volumetric budget interaction

Each synthesized light the volumetric march sees costs ray queries per step. Rules:

- Synthesized lights participate in volumetrics only above a score threshold
  (`r_rtAutoRelightVolMin`, default: top 4 by score per map). The rest light surfaces
  but don't get shafts. This is the "hero light" budget from the design discussion.
- They count against the existing `r_rtVolMaxLights` cap and the stratified list
  quotas (see `rt_optimization_tuning.md` L1) like any other light — no special path.

## CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_rtAutoRelight` | 0 (flip to 1 after validation) | master toggle; regenerates on map load |
| `r_rtAutoRelightMax` | 16 | cluster budget per map |
| `r_rtAutoRelightIntensity` | 0.5 | SHADERPARM_ALPHA for synthesized lights |
| `r_rtAutoRelightReach` | 160 | normal-direction radius (units) |
| `r_rtAutoRelightOffset` | 8 | origin offset off the surface |
| `r_rtAutoRelightDedupeDist` | 96 | existing-light suppression radius |
| `r_rtAutoRelightVolMin` | 4 | top-N clusters eligible for volumetrics |
| `r_rtUnlockNoShadows` | 0 | §6 rule, independent toggle |
| `r_rtAutoRelightDebug` | 0 | 1 = console table; 2 = also tint lit clusters (reuse an interaction debug mode) |

## Debug / validation workflow (do before any tuning)

1. `r_rtAutoRelight 1; r_rtAutoRelightDebug 1` → console table: per cluster, material
   name, centroid, area, score, verdict (LIT / DEDUPED-vs-light-N / BUDGET-DROPPED).
2. **`r_showLights 1` works natively** — synthesized lights are real lightDefs and
   render their volumes in the existing debug view. Walk Mars City reception: expect
   the big wall screens and ceiling strips lit, no light inside geometry, no doubles.
3. The zombie test: `testmodel` a zombie in front of the Admin LED wall, confirm
   (a) floor shadow moves with it, (b) shaft silhouette in the volumetric glow,
   (c) turning the panel dark via script (if scripted screen) kills the light — **not
   supported in v1**, note below.

## Known v1 limitations (accepted)

- **Static only.** Screens that change state (broken/off via script) keep their
  load-time light. Fix later by keying light on the material's active stage at runtime
  — requires a per-frame check; out of scope.
- One light per fixture cluster — a 6-tube strip becomes one soft source, not six.
  Visually fine at Doom 3 fixture scale; revisit only if a specific fixture reads wrong.
- Emissive *texture* areas that are dark texels within a bright-flagged material still
  count toward area (scoring uses average luminance to compensate).
- Load-time cost: one world-surface walk + small qsorts; negligible (< 50 ms expected).

## Effort / risk

- **Effort:** ~2-3 days including the debug table and dedupe iteration.
- **Risk:** Medium-low. Everything downstream is existing, tested machinery; the new
  logic is load-time CPU code with a full audit trail. The classic failure mode
  (double-lighting → grey washout) has a dedicated rule (§4) and a debug view.
- **Perf dependency:** adds up to `r_rtAutoRelightMax` interaction lights + shadow
  dispatches. Land **after** P1b (batched shadow masks) in
  `rt_optimization_tuning.md`, or start with `r_rtAutoRelightMax 6` before P1b.
