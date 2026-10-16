# Glass reflection normals: geometric instead of smoothed

Mars City's window reflections focus and invert. The panes are `patchDef2` Bezier patches
whose control points are collinear within each quadratic span — two flat facets meeting at
a 26.57° crease. `idSurface_Patch::GenerateNormals` has only an all-or-nothing coplanar
test (`Surface_Patch.cpp:327-348`), so a piecewise-flat patch gets its crease smoothed into
a continuous normal ramp. That ramp is a cylindrical mirror with f ≈ 25-60 units; everything
past arm's length reflects inverted.

Doom 3 never mirrored these panes — `glass1` is `heatHaze.vfp`, pure refraction — so the
curvature was never visible before our glass probe became the first code to treat glass as a
reflector.

Fix: use the geometric (face) normal for the reflection plane where the smoothing is
inventing curvature, and keep the interpolated normal where curvature is real.

## Census (all 27 stock maps, 718 glass patches)

`notes/scan_glass_patches.py` → `notes/20261013_glass_patch_census.txt`. Classifies by span
collinearity, validated against the hand-computed 26.57° on `mars_city1`.

| Class | N | % | Meaning | Fix must |
|---|---|---|---|---|
| `FLAT` | 538 | 75% | planar; normal constant | not regress |
| `MIXED` | 153 | 21% | some spans flat, some curved | pick per-hit, not per-surface |
| `ARC` | 21 | 3% | genuinely curved, up to 90° | keep smooth |
| `CREASE` | 6 | 1% | piecewise-flat + kink — **the bug** | flatten |

The 6 `CREASE` patches are the same 2 panes in `mars_city1` / `mars_city1_autosave` /
`mars_city2`, at `(-3320 -1408 208)` and `(-3320 -1600 208)`, entity `func_static_52993` /
`func_static_52992`. `base/maps/marsmirr.map` carries them over verbatim.

`MIXED` at 21% is why a per-surface plane fit is wrong: one patch can need both answers.

---

## Stage 1 — expose the geometric normal (no behaviour change)

| File | Change |
|---|---|
| `glsl/rt_material.glsl` | `#define RT_VTX_POS_OFF 0u`; add `rt_FaceNormal(matIdx, primId)` — same `geomSlot`/`maxPrimId`/`maxVertex` guards as `rt_InterpolateNormal`, `cross(v1-v0, v2-v0)`, world-space via `transpose(mat3(gl_WorldToObjectEXT))` |
| `glsl/glass_probe_payload.glsl` | `+ vec3 faceNormal`, rename `pad` → `smoothDot`. 32 → 48 bytes |
| `glsl/glass_probe.rchit` | Fetch the 3 positions alongside the 3 normals already read (lines 90-98); write `faceNormal`; `smoothDot = abs(dot(interp, face))` |
| `glsl/reflect_ray.rgen` | Debug modes 9/10 |
| `vk_raytracing.h:811` | `REFL_DEBUG_MAX_MODE` 8 → 10 |
| `vk_reflections.cpp:102` | Document modes 9/10 in the `r_rtReflectionDebugMode` help text |

Doom 3 winds front faces opposite to Vulkan, so `cross(v1-v0, v2-v0)` comes out reversed.
The existing face-forward at `glass_probe.rchit:112-113` fixes the sign — apply the same
rule to `faceNormal` independently, and use `abs()` on the deviation so a sign disagreement
on degenerate geometry can't read as 90°.

Built as planned; modes 9/10 were later folded into a single mode 9 (Stage 2).

**Result:** the deviation angle cannot separate the cases. The `mars_city1` cafeteria pane's
large lower facet ramps 5°→0° across its whole height (one triangle row, crease-averaged
top edge) — that ramp, not the kink, is the inverting lens. The Mars City skylight, a real
arc, reads 3-5°. Same band, so no angle threshold works.

---

## Stage 2 — select the normal

| File | Change |
|---|---|
| `vk_reflections.cpp` | `r_rtGlassNormalMode` (0 interp / 1 face / 2 crease-aware), `r_rtGlassCreaseRatio` (default 0.25) |
| `vk_reflections.cpp` | `ReflParamsUBO` += `int32_t glassNormalMode`, `float glassCreaseRatio` at 252/256. Total 256 → 272 (UBO ring slot is 384) |
| `glsl/glass_probe.rchit` | Per triangle: angle of each vertex normal to the face normal; `devRatio = min/max` (1 when max < 0.5°). Payload `pad` → `devRatio` |
| `glsl/reflect_ray.rgen` | Mode 1 → `faceNormal`; mode 2 → `faceNormal` when `devRatio < glassCreaseRatio`. Debug mode 9 = the split (red face / green interp); old 9/10 dropped |
| `Dhewm3SettingsMenu.cpp` | Combo + ratio slider under the reflection block, gated on `reflOn` |

Discriminator is symmetry, not size: on a real arc every vertex tilts by ~half the facet
angle; on a flat facet beside a smoothed crease the far-edge vertices match the face normal
exactly, so `min/max ≈ 0`. Scale-free, so tessellation density doesn't matter.

Selection lives in the rgen, not the rchit — the rchit has no `ReflParams` binding and
needs none; both normals ride the payload.

`origin = reflOrigin + reflNormal * 0.5` (line 331) picks up the change for free: on a
crease the bias now pushes along the true facet normal.

Mode 1 result: fixes the cafeteria pane, but every arc shows one image per facet (correct
for a faceted mirror, wrong for these assets).

**Check:** debug mode 9 — cafeteria lower facet red, skylight and `enpro` tube green. Mode 2
composite: cafeteria not inverted, skylight matches mode 0. Sweep the ratio 0.1→0.5; arcs
should stay green throughout.

---

## Stage 3 — validate and set the default

**Closed 2026-10-15.** `r_rtGlassNormalMode` defaults to 2; cvars added to
`Dhewm3-AI-RT-Settings.md`. Validated on the `mars_city1` cafeteria pane and skylight only —
the map tour below was skipped. Run it if a curved pane ever looks faceted.

Spots, worst-first. `teleport func_static_52993` reaches the Mars City pane; the others are
worldspawn, so `noclip` + `getviewpos`.

| Map | Coords | Class | Expect |
|---|---|---|---|
| `mars_city1` | -3320 -1408 208 | CREASE 26.6° | **the bug.** Inversion gone, seam appears |
| `admin` | -1152 904 132 | MIXED 90° | curved part stays smooth, flat part stops focusing |
| `alphalabs2` | -1984 1648 400 | MIXED 90° | as above, 9x3 grid |
| `enpro` | 2288 48 -192 | ARC 90° | `enproplasmatubeglass` — a glass tube. Must stay smooth |
| `comm1` | -3974 3968 322 | ARC 90° | 9x7, the densest arc |
| `delta3` | 3648 1900 -672 | ARC 90° | 3x3 — coarsest arc, most likely to facet |
| `mc_underground` | 630 -1282 32 | ARC 90° | several nearby; outdoor glass |
| `recycling2` | 1040 356 432 | FLAT 0° | regression guard — must be pixel-identical to mode 0 |

Then: default `r_rtGlassNormalMode` to 2, update `Dhewm3-AI-RT-Settings.md`, and move
this doc to `completed/`.

**Exit gate:** the Mars City window reads as a flat mirror, `enpro`'s tube is still smooth,
and `recycling2` is unchanged.

---

## Risks

| Risk | Handling |
|---|---|
| Coarse `ARC` patches facet visibly (`delta3` is 3x3) | Symmetric tilt keeps them interpolated regardless of density |
| `MIXED` needs per-hit selection, so the threshold is doing real work on 21% of patches | Debug mode 9 shows the split directly — tune from the overlay, not the composite |
| A facet with a smoothed crease on *both* edges reads symmetric and stays smooth | Accepted; the census `CREASE` panes have one kink each |
| A visible seam at the crease replaces a smooth wrong image | Correct, and it is what the geometry is. Only the 6 `CREASE` patches show it |
| Cost | Three extra vertex position fetches in a shader that already has the addresses and indices loaded |

## Not doing

Per-surface plane fit. It fails the planarity gate on the very patch it would fix (24 units
of deviation over 136), and ungated it collapses a 90° arc to one normal ~45° wrong at both
ends.

Fixing `GenerateNormals` to detect creases engine-wide. It would fix this at the source and
help the raster path too, but it changes normals for every patch in the game, including
lighting. Out of scope for a reflection bug; revisit if creases show up in GI or shadows.
