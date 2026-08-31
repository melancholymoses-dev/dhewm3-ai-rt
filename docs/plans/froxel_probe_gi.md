# Froxel Volumetrics + Probe GI — world-space caching arc

**Date:** 2026-08-23
**Status:** Future arc — NOT scheduled. Post-Wave 6 candidate, alongside (or instead
of) the sun/sky arc in ROADMAP.md. Written up while the reasoning was fresh; do not
start before Wave 6 lands and its profiler checkpoints exist, because both stages
here are justified by perf numbers we have not actually captured yet.

---

## Thesis

The two most expensive RT features (volumetrics, GI) currently recompute their
sampling **per screen pixel, per frame**, and then fight the resulting noise with
screen-space machinery (checkerboard, temporal EMA, à-trous, bilateral upsample).
Screen-space temporal reuse is fragile — it reprojects, ghosts, and resets on
camera cuts (the GI/Vol camera-cut detection bug is still open precisely because
of this).

Both features can instead cache their expensive result in **world space**, where
temporal reuse is trivially valid (a world position doesn't move when the camera
does), and reduce the per-pixel screen-space work to a cheap interpolated lookup:

| Feature | Today (screen space) | Proposed (world space) |
|---|---|---|
| Volumetrics | per-pixel ray march, ray query per step per light (`vol_march.comp`) | froxel grid: fill once per cell, integrate per column, trilinear resolve |
| GI | per-pixel hemisphere rays + denoise chain (`gi_ray.rgen` → temporal → à-trous → albedo mod) | irradiance probes: fixed ray budget updates rotating probe subset, per-pixel = ~8 texture fetches |

This is the standard industry structure (Frostbite/id Tech froxel fog since ~2015;
DDGI probes since 2019). The novelty here is only that our visibility tests stay
**RT ray queries against the TLAS** instead of shadow maps — we keep the "no map
editing, real occlusion" premise, we just stop paying for it once per screen pixel.

Cost sketch (1080p, current defaults): vol today ≈ 960×540 px × 8 steps ×
(pre-culled lights) ray queries every frame ≈ 4M+ step evaluations. A 160×90×64
froxel grid is ~920k cells, but cells are world-anchored so a ¼-per-frame update
rotation is valid → ~230k cell fills/frame, each against a *clustered* (per-cell)
light list. GI today ≈ 1M px ÷ 2 (checker) × 4 samples ≈ 2M rays/frame + three
denoise passes; ~4k active probes × 256 rays, updating ~1k probes/frame ≈ 256k
rays/frame, and the denoise chain is deleted rather than tuned.

## Pillar check

- **Pillar 1 (shadows are the feature):** untouched — this arc does not modify the
  direct shadow path.
- **Pillar 2 (darkness stays black):** the *main risk* of probe GI. Sparse probes
  interpolate, and interpolation leaks light through thin walls into dark rooms.
  This is why Stage G3 (visibility-weighted probes) is mandatory, not polish.
- **Pillar 3 (light the air sparingly):** froxels make each admitted vol light
  cheaper; admission policy (AR0 classifier + dedicated vol selection) is unchanged.
  Per user direction 2026-08-23: **flashlight volumetrics are deprioritized** — the
  beam axis is nearly parallel to every view ray (no parallax), so shaft structure
  is invisible from the wielder's POV; you only see a general forward-scatter haze.
  The hero case for beam *shape* is side-on fixture/panel shafts (the auto-relight
  zombie-vs-LED-wall shot). Optional enhancement, v2: offset the flashlight light
  origin from the eye (weapon-mounted offset) to create parallax — def-file change,
  allowed under pillar 5, but check it doesn't break the raster flashlight feel.
- **Pillar 6 (overlays before tuning):** every stage below ships its overlay first.

---

## Part A — Froxel volumetrics (replaces per-pixel march)

### Design

A 3D texture ("froxel grid") in **camera-frustum space**: X/Y follow screen
position, Z slices are exponentially spaced (same distribution the march's
`exp(alpha*logFac)` step spacing already uses — this is the same idea, made into
storage). Suggested start: 160×90×64 rgba16f, `r_rtVolFroxelRes[XYZ]` to tune.

Three passes replace `vol_march.comp`:

1. **Fill** (`vol_froxel_fill.comp`): for each froxel cell, compute in-scattering
   at the cell's world-space center — same light loop as today's march step
   (containment tests, HG phase per light class, Cauchy attenuation, `rayQueryEXT`
   occlusion against the TLAS). Reads the existing dedicated vol light selection
   (`vkRT.volLightSsbo`) — that machinery carries over unchanged and its
   distance filter (`r_rtVolMaxDist`) now bounds the grid depth naturally.
   Output: per-cell scattering RGB + extinction A.
2. **Integrate** (`vol_froxel_integrate.comp`): one thread per X/Y column walks
   the Z slices front-to-back accumulating Beer-Lambert transmittance —
   exactly today's per-pixel loop, done once per column (160×90 = 14k columns).
   Output: per-cell *accumulated* in-scattering + transmittance.
3. **Resolve**: `vol_composite.frag` changes from sampling the marched `volBuf`
   to one trilinear `texture()` of the integrated 3D texture at the pixel's
   (screenUV, depth-slice) coordinate. `vol_bilateral.comp` (the P8 upsample)
   is **deleted from this path** — trilinear froxel interpolation subsumes it.

### Temporal amortization (the actual win)

Cells are world-anchored per frame but the grid is frustum-shaped, so reuse works
as: EMA in froxel space with reprojection of the previous grid (standard froxel
temporal integration — reproject cell center into last frame's grid, blend). This
is far more robust than screen-space EMA because a 3D fetch miss just means "new
territory, take current sample" — no ghosting trails on geometry edges. Jitter the
per-cell sample position within the cell per frame (reuse the IGN +
`r_rtVolWhiteNoiseMix` blend — the striping lesson from P8 transfers directly, and
per [feedback_per_slot_counters] any frame-rotation term must key off a per-slot
counter, not `tr.frameCount`, if grids end up per-frame-in-flight).

Update rotation (e.g. ¼ of Z slices per frame, `r_rtVolFroxelRotate`) is a
Stage F2 option once F1's full-rate cost is measured — don't build it speculatively.

### Known trade-off: beam edge crispness

Froxel XY resolution puts a floor on how sharp a side-on shaft edge can be —
and side-on fixture shafts ARE the hero case (see pillar check). Mitigations, in
order: raise XY res (cost is linear and the budget freed is enormous); keep the
old march path compiled behind `r_rtVolFroxel 0` for A/B comparison until the
shot-level quality is confirmed; only then consider deleting the march.

### Stages

- **F0 — plumbing + overlay, no visual change.** Allocate the grid, fill it,
  add `r_rtVolFroxelDebug`: 1 = render a chosen Z slice as screen overlay,
  2 = per-cell light-count heatmap (color-coded — how many lights survive the
  per-cell cull), 3 = cell-age/update-rotation visualization. Composite still
  uses the march output. Validate the grid's world-position mapping by comparing
  overlay slice N against the march at the same depth.
- **F1 — resolve switch.** `r_rtVolFroxel 1` routes composite to the froxel
  grid. A/B against march. Capture profiler numbers (new `FroxelFill` /
  `FroxelIntegrate` phases; `VolMarch`/`VolBilateral` should read ~0 when on).
- **F2 — amortization + clustering.** Temporal EMA in froxel space; per-cell
  light lists (bin the vol selection into a coarse cluster grid) if the fill
  pass's light loop shows up in the capture; optional update rotation.
- **F3 — retire or keep.** Decide the march path's fate from F1/F2 evidence.

---

## Part B — Probe GI (replaces per-pixel hemisphere GI)

### Design (DDGI-style, reusing our RT plumbing)

A sparse grid of irradiance probes through the playable space. Each probe stores
octahedral-mapped irradiance (8×8 texels) plus mean/mean² ray distance (16×16) for
visibility weighting. Per frame, a **fixed ray budget** updates a rotating subset
of probes (`r_rtGIProbeRaysPerFrame`); rays reuse the existing
`gi_ray.rchit` shading — including P3 stochastic light selection, the AR0
classifier admission, and `rt_light_eval.glsl` — so what a bounce "sees" is
identical to today. Per-pixel GI becomes: fetch the 8 surrounding probes,
weight by trilinear × normal-cosine × Chebyshev visibility, done.

Why probes and not CryEngine-style SVOGI (the Homefront reference): voxel cone
tracing needs a voxelized scene representation maintained alongside the BLAS/TLAS
we already have. Probes spend our existing RT hardware budget directly and store
kilobytes, not a voxel mip chain. Same visual class of result (diffuse bounce +
color bleed), much better fit for this codebase.

### What carries over, what dies

- **Carries over:** `gi_ray.rchit`/`rmiss`/shading, light admission
  (AR0/AREA/portal machinery), `gi_albedo_mod.comp` + `gi_composite.frag`
  (probe output is irradiance — albedo-free by construction, so the
  receiver-albedo modulation composite is actually *cleaner* than today, where
  rchit returns albedo-weighted bounce), the AO pass (`vk_ao.cpp`) which now
  carries all short-range contact darkening that sparse probes can't represent.
- **Dies (in probe mode):** `gi_ray.rgen` per-pixel dispatch, checkerboarding
  and `checkerPhase`, `gi_temporal_resolve.comp`, `gi_atrous.comp` — the entire
  noise-fighting chain, including its open camera-cut bug, which probe GI
  side-steps structurally (probes don't care about camera cuts).

### Probe placement + scheduling — reuse the portal-area work

Placement: uniform grid per **area** (id's areas, the same structure
`portal_area_lights.md` walks), probes culled to cells that intersect the area's
geometry, small surface-offset relaxation to push probes out of walls. Activation:
the existing portal BFS admission set defines which areas' probes are live — the
"which lights reach here" graph and the "which probes matter here" graph are the
same graph. Update priority: probes in areas whose portal state just changed
(a door opened) jump the rotation queue — door-aware relighting falls out of
machinery we already validated in-game.

### Pillar 2 hardening (Stage G3, mandatory)

Chebyshev visibility weighting (the stored ray-distance moments) is what prevents
a probe in a lit hallway from bleeding through a wall into a dark room. Doom 3's
thin-wall, door-heavy layouts are close to the worst case, so: ship the leak
overlay first (per-pixel probe-contribution view, plus a "GI present where direct
light is zero" detector — pillar-2 violations color-coded on screen), then tune
probe density/offset against it. If leaks survive Chebyshev at sane densities,
fall back to per-area probe isolation (a probe only contributes to pixels in
areas its own area reaches through open portals — again the BFS set).

### Stages

- **G0 — placement + overlay, no lighting change.** Probe generation, storage,
  `r_rtGIProbeDebug 1` = probe spheres colored by stored irradiance,
  2 = per-pixel probe-weight visualization, 3 = leak detector. Console dump in
  the `r_rtGILightDump` style (per-area probe counts, memory).
- **G1 — probe tracing.** Rotating-subset update path (`gi_probe_trace.rgen` +
  octahedral border-aware blend into the probe atlas). Validate via overlay 1
  only — composite untouched.
- **G2 — resolve switch.** `r_rtGIProbes 1` replaces the GI buffer's content
  with probe interpolation (small fullscreen compute), albedo-mod + composite
  unchanged downstream. A/B against per-pixel GI; profiler capture.
- **G3 — leak hardening.** Chebyshev weighting, probe offsets, the pillar-2
  overlay pass on the standard test rooms (the ALB corpse room, the AREA
  doorway/zig-zag corridors — reuse the validated 2026-08-22 test walk).
- **G4 — retire decision + retune.** GI constants (`giStrength`, contrast,
  `r_rtGIAutoDirectScale` coupling) all change meaning under probes — this
  stage overlaps ROADMAP Wave 6 T6-style work and must come last.

---

## Rejected alternative — analytic shadow-volume integration (recorded for the record)

The originating idea (2026-08-23): per light, track its shadow volume and
analytically integrate the *lit intervals* along each view ray — zero sampling,
zero noise, exact edges (Tóth/Umenhoffer-style). Assessed and set aside because:

1. Stencil shadow volume generation is **disabled when RT shadows are on**
   (`useStencilShadows = !VK_RTShadowsEnabled()`, vk_backend.cpp; volume
   *creation* is also skipped) — this path would re-add the CPU shadow-volume
   cost that RT removed, for every vol-admitted light.
2. Per-ray interval clipping against arbitrary (non-convex, capped) volume
   meshes needs the additive front/back-face accumulation trick, which yields
   total shadowed *length* cheaply but not the distance-attenuated,
   phase-weighted integral we need — closed forms exist only for simplified
   phase/attenuation, and our per-class HG + Cauchy model doesn't reduce cleanly.
3. It scales per-light per-pixel; froxels scale per-cell with clustered lights.

Might deserve a second look for exactly ONE light if a future hero shot demands
razor-sharp noise-free shafts beyond froxel resolution — that's the niche.

## Ordering, dependencies, hygiene

- **Froxels first** (Part A): smaller, self-contained, replaces a two-shader
  path. Probes (Part B) are a subsystem-scale change; do not start both at once.
- Both parts: capture a profiler checkpoint **before** starting (the Wave 4/6
  captures still owed in ROADMAP) — this arc's justification is quantitative.
- New shaders go into `GLSL_INCLUDES` in neo/CMakeLists.txt; new files carry the
  dhewm3-rt GenAI copyright block (per .claude/CLAUDE.md).
- Old paths stay compiled and CVar-selectable until their replacement is
  validated in-game on the standard test rooms — delete in F3/G4, not before.
