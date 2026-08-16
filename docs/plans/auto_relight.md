# Auto-Relight — Engine-Synthesized Lights from Emissive Surfaces

**Date:** 2026-08-10 (§0 light classifier added 2026-08-15)
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

## §0 — Shared light classifier (land FIRST, independent of synthesis)

Added 2026-08-15 after the "boring uniform GI lift" design review. This stage has no
dependency on clustering/synthesis and directly fixes a live visual bug, so it lands
before everything else in this doc — it can even ship inside Wave 4's tail.

### The discovery: the GI/vol light filter is inverted

`VK_RT_UploadGILights` (`vk_gi.cpp`, noShadows skip at ~line 1057 — the SSBO shared by
GI bounce lighting *and* the volumetric march) filters only **entity-level**
`p.noShadows`. It never consults the light *material*
(`idMaterial::LightCastsShadows()` = not fogLight / not ambientLight / not blendLight /
not MF_NOSHADOWS — see `Material.h`). Net effect, backwards in both directions:

- **Colored accent lights** (alarm reds, monitor ambers — designers habitually flag
  them entity-noShadows) are **excluded** from GI and volumetrics.
- **Material-ambient white washes** (`ambientLight` light shaders — no falloff origin,
  pure paint-bucket fill; used all over alphalabs/comm maps) are **included**, and GI
  re-integrates them as if they were physical emitters → double-counted ambient →
  the uniform white hallway lift.

The importance formula compounds it: `intensity · lum · r² / max(distSq, r²)` scores
any light whose radius reaches the camera at pure `intensity · lum`, so big white
fills near the camera permanently max out, and `lum` (Rec.601 luma) rates a pure red
light at 0.3× an equal-intensity white one. The ranking is "biggest, whitest wins" —
the exact opposite of pillar 2 (GI exists for color bleed, not luminance lift).

Note the P1b shadow batch already uses the correct full test
(`!p.noShadows && lightShader->LightCastsShadows()`) — GI/vol simply never got it.

### Design: one classifier, four consumers

Do **not** patch the collector inline. Write one function and make GI collection, the
volumetric march list, the shadow batch, and the §6 unlock all call it, so the
real-vs-fake judgment can't drift between passes:

```
VK_RT_ClassifyLight(lightDef) →
  FOG_BLEND     fogLight || blendLight               → reject everywhere (RT-wise)
  AMBIENT_FILL  ambientLight material, or noShadows with radius ≥ accent threshold
                                                     → reject from GI/vol; never unlock
  ACCENT        noShadows with radius < accent threshold (placed colored accents)
                                                     → admit to GI/vol; §6 unlock pool
  REAL          everything else (shadow-casting map lights)
```

Layered admission for GI/vol, replacing the single `p.noShadows` test:

1. **Material class, hard reject:** `IsAmbientLight() || IsFogLight() || IsBlendLight()`
   → out. Semantically fake; no threshold, no false positives.
2. **noShadows admission by radius:** admit noShadows lights when
   `radius < r_rtLightAccentMaxRadius` (default **300** — the SAME threshold and cvar
   as §6's unlock rule; big noShadows = semantic ambient fill, small = accent).
3. **Saturation-weighted importance (ranking only, not admission):**
   ```
   sat = (maxChan - minChan) / max(maxChan, 1e-4)
   importance *= mix(r_rtGIWhiteWeight, 1.0, sat)   // default 0.25
   ```
   Legit white hallway lights stay in the list (they really do light bounce
   surfaces) but lose slot/stochastic-selection fights to saturated accents. This is
   NOT what `r_rtGIContrast` does — that subtracts min channel post-hoc but rescales
   to preserve average brightness (tints the lift without reducing it); per-light
   weighting reduces the white contribution before any ray is traced.

Failure mode of over-filtering is *darkening* (GI is additive-only) — which is
pillar 2's stated aesthetic, so the risk profile is asymmetric in our favor.

### Log before thresholds (procedure rule)

Before trusting 300/0.25: `r_rtGILightDump` prints every in-view lightDef — name,
color, saturation, radius, entity noShadows, material class, admitted/rejected +
which layer decided, importance before/after weighting. One alphalabs2 run answers
whether the radius threshold cleanly separates fills from accents. A debug tint mode
(color GI contribution by classifier class) makes the wash source visually obvious.

### Deliverables

- `VK_RT_ClassifyLight()` + enum (suggest `vk_raytracing.h` / a small shared cpp).
- GI/vol collector switched to layered admission + saturation weighting.
- `r_rtGILightDump`, classifier debug tint.
- Expected visible result: white hallway wash drops, colored accents START appearing
  in GI and volumetrics (they are currently filtered out entirely).

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
RT path inherits (skipped at `vk_gi.cpp` ~1057 for GI/vol; the interaction path honors
the flag too). Engine-side rule, cvar-gated (`r_rtUnlockNoShadows`, default 0):
lights the §0 classifier rates **ACCENT** (noShadows, radius <
`r_rtLightAccentMaxRadius`, default 300 — same threshold/cvar as GI/vol admission;
the giant ambient fills that use noShadows *semantically* classify AMBIENT_FILL and
are never unlocked) get RT shadows anyway. This recovers drama from lights that
already exist, at zero placement risk. Dedupe (§4) can feed it: a skipped cluster
whose paired map light is noShadows is a strong unlock candidate.

Perf note: every unlocked light becomes a shadow-caster in the interaction loop
(cheap post-P1b — that's what batching was for) *and* a volumetric candidate (NOT
cheap — per-step ray queries per light, and P8's half-res doesn't change the per-light
scaling). Unlocked accents ride the normal `r_rtVolMaxLights` cap; they do not get
automatic shafts.

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
| `r_rtLightAccentMaxRadius` | 300 | §0/§6 shared: noShadows radius below which a light is ACCENT (admit to GI/vol, unlockable) vs AMBIENT_FILL |
| `r_rtGIWhiteWeight` | 0.25 | §0: importance multiplier floor for fully desaturated lights (1.0 = no saturation weighting) |
| `r_rtGILightDump` | 0 | §0: console dump of every in-view lightDef with classification + admission verdict |

## Debug / validation workflow (do before any tuning)

0. §0 first: `r_rtGILightDump 1` on alphalabs2 → confirm the radius threshold cleanly
   separates fills from accents *before* trusting the 300/0.25 defaults; then verify
   the white wash drops and colored accents appear in GI/vol with the classifier on.
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
