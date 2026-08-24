# Auto-Relight — Engine-Synthesized Lights from Emissive Surfaces

**Date:** 2026-08-10 (AR0 light classifier added 2026-08-15)
**Branch:** shaped-shadows
**Replaces:** Stages 4a/4b of `completed/lighting_shadows_refinement.md`.
**Companion docs:** `rt_optimization_tuning.md` (P1b shadow batching is the perf
enabler; L1 stratified light list is the selection layer),
`completed/gbuffer_normal_pass.md`, `completed/gi_albedo_target.md` (GI
receiver-albedo fix surfaced by this doc's AR0/AREA validation, 2026-08-16 — landed
before AR1-5, since synthesized panel lights would amplify the unmodulated-GI wash
it fixes), `completed/portal_area_lights.md` (the collector AR0 admits into —
BFS/area-walk gathering, done 2026-08-22; also landed the vol-specific light
selection this doc's AR6/volumetric-budget-interaction sections below now assume,
see the 2026-08-22 note in each).

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
3. **Volumetrics** — `vol_march.comp` fires **ray-query occlusion tests toward each
   light at every march step**, and monsters are in the TLAS as dynamic BLAS
   instances. The zombie's volumetric silhouette in the panel glow is therefore not a
   new feature — it falls out of feeding the existing march a new light. **Updated
   2026-08-22:** volumetrics no longer reads GI's own light buffer — it has its own
   dedicated selection (`vkRT.volLightSsbo`, built in `VK_RT_UploadGILights` from the
   same admitted-candidate pool as GI, filtered to lights whose sphere/cone can reach
   within `r_rtVolMaxDist` — default 512 — of the camera). A synthesized light still
   needs no new code to reach the march, but see the Volumetric budget interaction
   section below — this filter is a real constraint on which synthesized "hero"
   lights actually get shafts, separate from the score-based `r_rtAutoRelightVolMin`
   budget this doc already plans for.
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

## AR0 — Shared light classifier (land FIRST, independent of synthesis)

Added 2026-08-15 after the "boring uniform GI lift" design review. This stage has no
dependency on clustering/synthesis and directly fixes a live visual bug, so it lands
before everything else in this doc — it can even ship inside Wave 4's tail.

**Status: implemented 2026-08-15, validated in-game 2026-08-15/16** (dump runs in ACO
Offices / ACO Access Junction: ACCENT admissions confirmed, saturation weighting
verified against hand-computed values; no AMBIENT_FILL in those halls — that class
is rarer than assumed, see the map-grep note in the session log). Landed as
`neo/renderer/Vulkan/vk_light_classify.cpp` (new file, `vkRTLightClass_t` enum +
`VK_RT_ClassifyLight()` + `VK_RT_LightClassName()`, declared in `vk_raytracing.h`,
registered in CMakeLists.txt). `VK_RT_UploadGILights` (`vk_gi.cpp`) now calls it for
admission (replacing the old `if (p.noShadows) continue;`) and applies saturation
weighting to importance. AR6 (noShadows unlock) is a separate, not-yet-implemented
consumer of the same classifier. **Runtime validation confirmed done (2026-08-22):**
the 300-unit accent/fill split holds up against real level data, and the white wash
drops with colored accents appearing in GI/vol as intended.

### The discovery: the GI/vol light filter is inverted

`VK_RT_UploadGILights` (`vk_gi.cpp`) filters only **entity-level** `p.noShadows` at
admission — the single gate every downstream consumer (GI bounce, reflections, and,
since `portal_area_lights.md`'s 2026-08-22 fix, volumetrics' own derived selection —
see the note below) inherits. It never consults the light *material*
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
volumetric march list, the shadow batch, and the AR6 unlock all call it, so the
real-vs-fake judgment can't drift between passes:

```
VK_RT_ClassifyLight(lightDef) →
  FOG_BLEND     fogLight || blendLight               → reject everywhere (RT-wise)
  AMBIENT_FILL  ambientLight material, or noShadows with radius ≥ accent threshold
                                                     → reject from GI/vol; never unlock
  ACCENT        noShadows with radius < accent threshold (placed colored accents)
                                                     → admit to GI/vol; AR6 unlock pool
  REAL          everything else (shadow-casting map lights)
```

Layered admission for GI/vol, replacing the single `p.noShadows` test:

1. **Material class, hard reject:** `IsAmbientLight() || IsFogLight() || IsBlendLight()`
   → out. Semantically fake; no threshold, no false positives.
2. **noShadows admission by radius:** admit noShadows lights when
   `radius < r_rtLightAccentMaxRadius` (default **300** — the SAME threshold and cvar
   as AR6's unlock rule; big noShadows = semantic ambient fill, small = accent).
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

Before trusting 300/0.25: `r_rtGILightDump 1` prints every in-collect-radius lightDef
once (self-clearing) — material name, classifier verdict, entity noShadows, radius,
distance, color, saturation, importance before/after weighting, admit/reject. One
alphalabs2 run answers whether the radius threshold cleanly separates fills from
accents. **Implemented as the console dump only** — a visual debug tint (color GI
contribution by classifier class) was scoped in the original design but not built;
add it later if the console table isn't enough to spot a wash source.

### Deliverables

- ✅ `VK_RT_ClassifyLight()` + enum — `vk_raytracing.h` declares, `vk_light_classify.cpp`
  defines (new file, registered in CMakeLists.txt).
- ✅ GI/vol collector switched to layered admission + saturation weighting
  (`vk_gi.cpp: VK_RT_UploadGILights`).
- ✅ `r_rtGILightDump` (console table, self-clearing). ⬜ Classifier debug tint — not
  built, see note above.
- ✅ **Verified at runtime (2026-08-22).** White hallway wash drops, colored accents
  appear in GI and volumetrics (they were previously filtered out entirely by the
  entity-noShadows-only test).

## Architecture

Below is the `VK_AutoRelight_Generate()` pipeline (new file: `vk_auto_relight.cpp`) —
unlabeled here since the `AR1`-`AR5` headers right after this diagram are the
authoritative numbering; giving the diagram its own 1-6 would just be a second,
colliding count (there is no relation between this diagram's step order and
`portal_area_lights.md`'s unrelated Stage-1/1.5/1.75 numbering, or `AR6`/`AR7` below —
those aren't pipeline steps, they're separate companion rules).

```
Map load complete (tr.primaryWorld populated)
  └─ VK_AutoRelight_Generate()
       - Walk static world surfaces; keep those whose material passes the
         emissive test (same logic as the material-table stage walk — extract
         a shared helper so the two can't drift).                          [AR1]
       - Cluster emissive triangles into physical fixtures.                [AR2]
       - Score clusters; apply budget.                                     [AR3]
       - Dedupe against existing map lights.                               [AR4]
       - AddLightDef() one synthesized light per surviving cluster.        [AR5]
       - Log a summary table (accepted / deduped / dropped-by-budget).
```

### AR1. Surface harvest

Walk the world model surfaces (portal-area models, same geometry the static BLAS
build already walks in `vk_accelstruct.cpp` — reuse that iteration pattern). For each
surface whose material is emissive, collect triangles: world-space centroid, area,
normal, and UV centroid (for color sampling later).

✅ **Implemented 2026-08-23** in the new `vk_auto_relight.cpp` (`AR_HarvestEmissiveSurfaces`).
Walks `world->localModels` (the `_area%i` per-portal-area models — confirmed baked in
world space, identity origin/axis, in `RenderWorld_load.cpp::AddWorldModelEntities`, so
no entity transform is needed). The emissive test was extracted out of
`VK_RT_MakeMaterialEntry` into a new shared `VK_RT_MaterialIsEmissive()`
(`vk_material_table.cpp`, declared in `vk_raytracing.h`) so the material-table and
harvest walks can't drift apart, per this doc's original ask. Mirrors the static-BLAS
walk's surface filters (`MF_POLYGONOFFSET`, particle/sprite/flare deforms). Triangles
are streamed directly into grid-cell accumulators (see AR2) rather than stored
per-triangle, so cost is independent of world triangle count. UV centroid is not
separately tracked — color sampling (AR3/AR5) reads the emissive *image* directly via
`R_LoadImage`, not a UV-windowed region of it (see AR3 note).

### AR2. Clustering (one fixture = one light)

A single surface can span several physical panels (a strip of six ceiling lights is
often one draw surface). v1 algorithm — deliberately simple:

- Bucket emissive triangle centroids into a world-space grid (cell ≈ 48 units).
- Union adjacent occupied cells (6-connectivity) into clusters.
- Per cluster: area-weighted centroid, area-weighted average normal, total emissive
  area, AABB extents projected onto the normal's tangent plane (the fixture's physical
  width/height — feeds the soft-shadow aperture).
- Reject clusters with wildly disagreeing normals (|avg normal| < 0.7 after
  normalization → a wraparound glow strip; skip, log it).

✅ **Implemented 2026-08-23** (`AR_ClusterCells`). 6-connected flood fill over occupied
grid cells via `idHashIndex`. Tangent-plane half-extents are approximated from the
cluster's *cell-level* AABB corners (not a second per-triangle pass) — slightly
generous, acceptable for v1. Rejected clusters are kept in the output list tagged
`AR_VERDICT_REJECTED_NORMAL` (not dropped silently) so the debug table can show them.

### AR3. Scoring and budget

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

✅ **Implemented 2026-08-23** (`AR_ScoreAndBudget` + `AR_GetImageLumaColor`).
**Finding:** `idImage` does not retain CPU-side texel data after GPU upload (checked
`Image.h`) — the "if retained" fast path never applies. Implementation always reloads
the raw file via `R_LoadImage()` (same canonical-32-bit loader every other image path
uses), stride-sampled for large images (bounded at ~4096 samples), cached by `idImage*`
for the lifetime of one `VK_AutoRelight_Generate()` call only (not across map loads —
same stale-pointer hazard the BLAS cache's `VK_RT_BeginLevelLoad` purge already guards
against, so no cross-load cache was added). `emissiveLuminance` is the plain average
luma of sampled texels; the AR5 "bright-texel color" is computed in the same pass as a
**luminance-weighted** average color (brighter texels dominate) rather than a literal
brightness-threshold filter — cheaper and doesn't need a tuned cutoff. `styleWeight`
1.5× applies to both GUI-emissive and cinematic/videomap stages (`VK_RT_MaterialIsEmissive`
gained a third `outIsCinematic` out-param for this). Sort+budget is an insertion sort
over candidates (cluster counts are tens, not thousands).

**Finding + fix, 2026-08-23 (`r_rtAutoRelightDebug 1` on alphalabs2):** the single
map-wide top-16 cap let two physically large/bright fixture clusters (a pulsing
`floorpgrate` floor section, a `mallight` wall bank — both legitimately emissive,
checked against their `.mtr` defs, not a classification bug) consume **all 16** budget
slots between them (10 `LIT` + 6 `DEDUPED`), while every other room's fixtures —
scattered across a map spanning thousands of units, clearly many rooms — scored lower
by sheer physical size (`floorpgrate` area ~11-12k sq. units vs. a typical wall panel's
500-4000) and all landed `BUDGET-DROP`. Net effect: two rooms got all the budget, every
other room got zero, which isn't "a few hero lights per scene" (the design intent) —
it's "a few hero lights for the entire level, wherever they happen to be biggest."

Fixed with a **two-stage cap**, replacing the single map-wide sort:
1. `world->PointInArea(cluster.centroid)` buckets each candidate by portal area.
2. Within each area, keep the top `r_rtAutoRelightMaxPerArea` (new cvar, default
   **3**) by score — everything else in that area is `AREA-CAP`, a new verdict
   distinct from `BUDGET-DROP` so the debug table shows *why* a cluster lost (its
   own room was oversubscribed, vs. losing to the map-wide ceiling).
3. Everything that survives stage 1, across all areas, is sorted again and capped by
   the existing `r_rtAutoRelightMax` (still 16) as a map-wide safety valve — unchanged
   in spirit, now just applied after the per-area pass instead of being the only pass.

This mirrors `portal_area_lights.md`'s AREA fix for GI/vol light *admission* (flat
global candidate pool → per-area walk) — same shape of problem, applied to AR3's
synthesis budget instead. The debug table (`r_rtAutoRelightDebug 1`) gained an `rm`
column (the resolved portal-area number) so the per-room distribution is directly
visible, not just inferred from centroids. **Implemented 2026-08-23, not yet
re-validated in-game against the alphalabs2 run that surfaced it** — re-run
`r_rtAutoRelightDebug 1` there and confirm rooms other than the floorpgrate/mallight
ones now get `LIT` entries.

### AR4. Dedupe (critical — prevents the grey washout returning)

For each cluster, search existing `lightDefs` within `r_rtAutoRelightDedupeDist`
(default **96** units) of the proposed origin. If a map light exists there and its
color is broadly similar (hue distance below threshold, or either is near-white), the
mappers already lit this fixture → **skip synthesis**, but optionally tag that existing
light for the noShadows unlock (see AR6). This rule is what makes auto-relight additive
instead of double-bright.

✅ **Implemented 2026-08-23** (`AR_Dedupe` + `AR_ColorsSimilar`). Color similarity: either
color below 0.15 saturation counts as a wildcard match (near-white always "matches" —
favors skipping synthesis on a borderline call, the safe direction per pillar 2), else
hue compared via normalized-color dot product (>0.85 ≈ within ~32°). Runs unconditionally
(read-only, no `AddLightDef` side effect) even when `r_rtAutoRelight 0`, so
`r_rtAutoRelightDebug 1` alone previews DEDUPED verdicts before the master toggle is
flipped on. Does **not** yet tag the matched existing light for the AR6 unlock — AR6 is
unimplemented (see below), so there's nothing to tag it for yet.

### AR5. Light synthesis

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

✅ **Implemented 2026-08-23** (`AR_Synthesize`). **Important finding, changes the shape
approach above:** `vk_shadows.cpp`'s anisotropic soft-shadow code, `vk_gi.cpp`, and
`vol_march.comp` all **deliberately ignore point-light axis rotation** and read
`lightRadius.xyz` as literal world X/Y/Z half-extents (see the explicit comment in
`vk_shadows.cpp`: "same simplification (axis rotation ignored) ... kept consistent
here"). So a tangent/normal-oriented ellipsoid expressed via `renderLight.axis` would
**not** actually shape the RT penumbra for a non-axis-aligned panel — the consumers
never look at `axis` for point lights. Implementation therefore computes the
world-space-axis-aligned AABB of the true oriented box (±left×halfWidth, ±up×halfHeight,
±normal×reach) and assigns that to `lightRadius` directly, leaving `renderLight.axis`
identity. This is exact when the panel already faces a cardinal direction (the common
case in Doom 3's blocky architecture) and slightly generous otherwise — consistent with,
not a regression from, the simplification the rest of the RT codebase already makes for
every other point light. Cleanup/identification: relies on the normal per-world
`lightDefs` lifecycle (freed generically on map reload) plus the `r_rtAutoRelightDebug 1`
console table (verdict + `AddLightDef` handle per cluster); no separate defHandle
registry was added — nothing yet consumes it (AR6/AR7 are unimplemented).

### AR6. noShadows unlock for existing fixture lights (companion rule)

Many mapper-placed fixture lights are `noShadows` — a 2004 stencil-budget decision the
RT path inherits (skipped in `VK_RT_UploadGILights`'s admission for GI/vol; the
interaction path honors the flag too). Engine-side rule, cvar-gated
(`r_rtUnlockNoShadows`, default 0):
lights the AR0 classifier rates **ACCENT** (noShadows, radius <
`r_rtLightAccentMaxRadius`, default 300 — same threshold/cvar as GI/vol admission;
the giant ambient fills that use noShadows *semantically* classify AMBIENT_FILL and
are never unlocked) get RT shadows anyway. This recovers drama from lights that
already exist, at zero placement risk. Dedupe (AR4) can feed it: a skipped cluster
whose paired map light is noShadows is a strong unlock candidate.

Perf note: every unlocked light becomes a shadow-caster in the interaction loop
(cheap post-P1b — that's what batching was for) *and* a volumetric candidate (NOT
cheap — per-step ray queries per light, and P8's half-res doesn't change the per-light
scaling). Unlocked accents ride the normal `r_rtVolMaxLights` cap *and* the
`r_rtVolMaxDist` reachability gate (see "Volumetric budget interaction" below); they
do not get automatic shafts.

### AR7. Weapons / projectiles / particles (def-mod scope, allowed)

Muzzle flash and projectile lights are already real dynamic lights flowing through the
RT shadow dispatch; where they're `noShadows` in defs, a small def patch (allowed per
project owner) or the AR6 unlock rule turns them into moving shadow casters — plasma
bolts strobing shadows down a corridor. No engine work beyond AR6. Particle-emitted
lights, if wanted later, are also def-side (`emitLight` style additions), not engine.

**Follow-up idea (2026-08-23, not started): third-person self-shadow in volumetrics.**
Once a muzzle flash (e.g. a plasma weapon) is a real shadow-casting light via the above,
it already reaches `vol_march.comp`'s light list like any other light — no extra work
needed for it to light fog/dust in front of the player. What it *won't* do is show the
player's own third-person arm/gun mesh as a silhouette in that glow: `vol_march.comp`
hardcodes its shadow-ray cull mask to `0xFEu` unconditionally, excluding every
`noSelfShadow` (player-body, mask `0x01`) instance from every volumetric occlusion test,
with no distance gate. Contrast the direct-light shadow path
(`vk_shadows.cpp`), which only excludes the player body within `r_rtShadowPlayerExcludeDist`
(default 30 units) of the light — beyond that it self-shadows normally.

Interesting because: unlike the flashlight (mounted ~at the eye, so no parallax — see
`froxel_probe_gi.md`'s pillar-3 note, flashlight volumetrics deprioritized for exactly
this reason), a muzzle-flash light sits away from the camera on the gun itself, so it
has real parallax and could plausibly rim-light the player's own arm/weapon in smoke —
a genuine "hero light" candidate that the flashlight isn't.

Not a trivial flip, though: a muzzle flash sits only a few units from the gun mesh it
would be shadowing against — much closer than the 30-unit threshold the direct-shadow
path uses specifically because a light that close to its own caster is the classic
shadow-acne setup (ray origin bias vs. self-intersection). Naively dropping the `0xFEu`
exclusion for volumetrics would likely reproduce that as flicker in the fog rather than
a clean silhouette. Would need the same kind of distance-aware handling
`r_rtShadowPlayerExcludeDist` already does for direct shadows (or bias tuned
specifically for close-range self-occlusion) — and per this project's usual approach,
should be diagnosed with a debug view of the shadow-ray hit/miss right next to the gun
(watch it flicker) before picking a bias value, not the other way around. Revisit once
AR6/AR7's own noShadows-unlock groundwork exists to make a muzzle flash a real light
in the first place.

---

## Volumetric budget interaction

Each synthesized light the volumetric march sees costs ray queries per step. Rules:

- Synthesized lights participate in volumetrics only above a score threshold
  (`r_rtAutoRelightVolMin`, default: top 4 by score per map). The rest light surfaces
  but don't get shafts. This is the "hero light" budget from the design discussion.
- They count against the existing `r_rtVolMaxLights` cap and the stratified list
  quotas (see `rt_optimization_tuning.md` L1) like any other light — no special path.
- **New gate, 2026-08-22 (`portal_area_lights.md`):** before any of the above, a light
  must be within reach of `r_rtVolMaxDist` (default 512 world units) of the *camera*
  to be selected for volumetrics at all — see "vol-specific light selection" above.
  This is independent of `r_rtAutoRelightVolMin`'s score budget and can silently
  defeat it: a top-scored synthesized panel picked for a shaft by score will still
  get **no shaft** if the player is never within ~512 units of it (e.g. a screen at
  the far end of a large room, seen but not approached). Worth a debug check once
  AR1-5 lands: dump `r_rtgilightdump 1`'s `[vol selection]` block near a synthesized
  hero light and confirm it actually appears there, not just in `[final order]`
  (GI's own list, which has no such distance gate).

**Not implemented (2026-08-23):** `r_rtAutoRelightVolMin` — AR1-5 landed without an
auto-relight-specific volumetric sub-budget. Every synthesized light is a full `REAL`
lightDef via `AddLightDef()`, so it already rides the existing global
`r_rtVolMaxLights`/`r_rtVolMaxDist`/stratified-list machinery like any map light; it just
isn't additionally capped to "top 4 by score" for volumetrics specifically the way this
section originally specified. Enforcing that would need a way for `vk_gi.cpp`'s vol
selection to identify+rank auto-relight lights at selection time (a defHandle registry
this pass deliberately didn't build — see the AR5 note above). Revisit only if too many
synthesized lights competing for vol slots turns out to be a real problem in-game.

## CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_rtAutoRelight` | 0 (flip to 1 after validation) | master toggle; regenerates on map load — ✅ implemented |
| `r_rtAutoRelightMaxPerArea` | 3 | AR3: cluster budget PER PORTAL AREA, applied before the map-wide cap — ✅ implemented 2026-08-23 (primary budget knob now, see AR3's 2026-08-23 finding) |
| `r_rtAutoRelightMax` | 16 | AR3: overall map-wide ceiling, applied after the per-area cap — ✅ implemented (safety valve, not the primary knob anymore) |
| `r_rtAutoRelightIntensity` | 0.5 | SHADERPARM_ALPHA for synthesized lights — ✅ implemented (AR5) |
| `r_rtAutoRelightReach` | 160 | normal-direction radius (units) — ✅ implemented (AR5) |
| `r_rtAutoRelightOffset` | 8 | origin offset off the surface — ✅ implemented (AR5) |
| `r_rtAutoRelightDedupeDist` | 96 | existing-light suppression radius — ✅ implemented (AR4) |
| `r_rtAutoRelightVolMin` | 4 | top-N clusters eligible for volumetrics — ⬜ not implemented, see "Volumetric budget interaction" above |
| `r_rtUnlockNoShadows` | 0 | AR6 rule, independent toggle — ⬜ AR6 not implemented |
| `r_rtAutoRelightDebug` | 0 | ✅ implemented as 0/1 (console table); the doc's "2 = also tint lit clusters" debug-visualization mode was not built, same scope cut AR0's classifier tint took |
| `r_rtLightAccentMaxRadius` | 300 | AR0/AR6 shared: noShadows radius below which a light is ACCENT (admit to GI/vol, unlockable) vs AMBIENT_FILL |
| `r_rtGIWhiteWeight` | 0.25 | AR0: importance multiplier floor for fully desaturated lights (1.0 = no saturation weighting) |
| `r_rtGILightDump` | 0 | AR0: console dump of every in-view lightDef with classification + admission verdict |

## Debug / validation workflow (do before any tuning)

0. ✅ **Done (2026-08-22).** AR0 first: `r_rtGILightDump 1` on alphalabs2 → confirm the
   radius threshold cleanly separates fills from accents before trusting the 300/0.25
   defaults; then verify the white wash drops and colored accents appear in GI/vol
   with the classifier on.
1. ✅ **Implemented 2026-08-23.** `r_rtAutoRelight 1; r_rtAutoRelightDebug 1` → console
   table: per cluster, verdict (`LIT (handle N)` / `DEDUPED (vs light N)` /
   `AREA-CAP` / `BUDGET-DROP` / `REJECTED`), score, surface area, tri/cell counts,
   `rm` (resolved portal-area number, added 2026-08-23 alongside the per-area budget
   fix), normal agreement, centroid, color, material name. `r_rtAutoRelightDebug 1`
   alone (master toggle still 0) previews everything through dedupe — AR5 synthesis is
   the only stage gated on `r_rtAutoRelight`, since it's the only one with a side
   effect (`AddLightDef`). **In-game validated once already** (the 2026-08-23
   alphalabs2 run that surfaced AR3's per-area budget bug — see AR3 above); re-run
   after that fix to confirm the `rm` column now shows lit clusters spread across
   multiple areas instead of concentrated in one or two.
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
  (double-lighting → grey washout) has a dedicated rule (AR4) and a debug view.
- **Perf dependency:** adds up to `r_rtAutoRelightMax` interaction lights + shadow
  dispatches. Land **after** P1b (batched shadow masks) in
  `rt_optimization_tuning.md`, or start with `r_rtAutoRelightMax 6` before P1b.
