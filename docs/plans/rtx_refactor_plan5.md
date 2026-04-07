# RTX Refactor — Phase 4 Optimization, Phase 5 and Phase 6 Plan

**Document:** `rtx_refactor_plan4.md`
**Date:** 2026-04-04
**Branch:** `feature/rt_reflections`

---

## Status Summary

| Phase | Description | State |
|-------|-------------|-------|
| 1 | GLSL shaders (ARB → GLSL/SPIR-V) | **Done** |
| 2 | Vulkan rasterization backend | **Done** |
| 3 | Acceleration structures (BLAS/TLAS) | **Done** |
| 4 | RT shadow pass | **Done** |
| 5.1 | RTAO | **Done** |
| 5.2 | Temporal EMA denoising | **Done** |
| 5.2b | Atrous spatial filter | **Done** |
| 5.3 | RT reflections (cheap approximation) | **Done** |
| 5.4 | Material table — real diffuse in reflections, alpha-test in shadows | **Done** |
| 5.4b | Basic glass reflections (flat Fresnel) | **Blocked — see investigation** |
| 6 | One-bounce global illumination | **Planned** |

---

## Phase 4 — RT Shadows (Complete)

Correctness and core optimizations are done.  Remaining items are polish:

| Fix | Status |
|-----|--------|
| Per-light RT shadow mask dispatch | Done |
| Remove per-light queue idle stalls | Done |
| GPU-native BLAS geometry inputs | Done (CPU fallback retained) |
| TLAS static/dynamic instance split | Done (model-keyed BLAS cache) |
| Frontend shadow volume skip when RT active | Done |
| Batched upload (remove N × vkQueueWaitIdle) | Done |

**Remaining:** finer per-instance static-change detection; per-light-type shadow
softness controls (separate cvars for point vs projected lights).

---

## Phase 5 — RTAO + RT Reflections

---

### Step 5.1 — Ray Traced Ambient Occlusion (Done)

Shoots cosine-weighted hemisphere rays from each visible surface point.
Fraction of misses → AO factor applied to the diffuse term in `interaction.frag`.

**Key files:**
- `neo/renderer/glsl/ao_ray.rgen` — ray generation
- `neo/renderer/glsl/ao_ray.rahit` / `ao_ray.rmiss` — hit/miss shaders
- `neo/renderer/glsl/rt_indirect.glsl` — shared cosine sampling + world-pos reconstruction
- `neo/renderer/Vulkan/vk_ao.cpp` — pipeline, dispatch, image lifecycle

**CVars:** `r_rtAO`, `r_rtAOSamples`, `r_rtAORadius`

**Known limitation:** Normals are depth-derived (finite differences). Causes
minor halos at depth discontinuities. A dedicated G-buffer normal pass would fix
this; deferred until the artifact is the primary quality complaint.

---

### Step 5.2 — Temporal EMA Denoising (Done)

Exponential moving average blends each frame's raw RT output into a history
image. The interaction shader samples the history rather than the noisy raw
result.

Camera-cut detection compares `invViewProj` against the previous frame — a
large change resets accumulation (sets alpha = 1.0 for that frame).

**Key files:**
- `neo/renderer/glsl/temporal_resolve.comp` — EMA blend compute shader
- `neo/renderer/Vulkan/vk_temporal.cpp` — pipeline + dispatch

**Known limitation:** Ghosting during continuous camera movement. Acceptable at
Doom 3 camera speeds. Motion vectors would fix this properly; deferred.

---

### Step 5.2b — Atrous Spatial Filter (Done)

Edge-stopped à-trous wavelet filter runs after the EMA resolve, producing
acceptable quality at 4 rays/pixel without additional ray cost. Inputs are
depth and depth-derived normals — no extra G-buffer required.

**Key file:** `neo/renderer/glsl/atrous_filter.comp`

**CVars:** `r_rtAtrousIterations` (0 = off, 4–5 = typical), `r_rtAtrousSigmaDepth`,
`r_rtAtrousSigmaNormal`

---

### Step 5.3 — RT Reflections (Done)

One mirror-reflect ray per non-sky pixel. Result written to an RGBA16F buffer
sampled by the interaction fragment shader and blended into the specular term.

**Key files:**
- `neo/renderer/glsl/reflect_ray.rgen` — ray generation (depth reconstruct, reflect, trace)
- `neo/renderer/glsl/reflect_ray.rmiss` — miss: dim ambient sky colour
- `neo/renderer/glsl/reflect_ray.rchit` — closest-hit: samples real diffuse (Phase 5.4)
- `neo/renderer/glsl/reflect_ray.rahit` — any-hit: alpha-discard for perforated geometry
- `neo/renderer/Vulkan/vk_reflections.cpp` — pipeline, SBT, dispatch

**CVars:** `r_rtReflections`, `r_rtReflectionDistance`, `r_rtReflectionBlend`

---

### Step 5.4 — Material Table (Done)

Per-TLAS-instance material data (diffuse texture index, alpha flags, vertex/index
buffer addresses) uploaded as persistently-mapped SSBOs.  A bindless sampler2D
array (4096 slots) holds all loaded textures.

This infrastructure unlocks:
- `reflect_ray.rchit`: barycentric UV interpolation + real diffuse texture sample
  instead of the direction-tint approximation used in 5.3.
- `shadow_ray.rahit`: alpha-test for perforated geometry (grates, foliage).

**Key files:**
- `neo/renderer/Vulkan/vk_material_table.cpp` — init, shutdown, per-frame SSBO upload
- `neo/renderer/glsl/rt_material.glsl` — shared GLSL include: `MaterialEntry` struct,
  set=1 SSBO bindings, `rt_InterpolateUV()`, `rt_SampleDiffuse()`

**`VkMaterialEntry` layout (32 bytes, std430):**

```cpp
struct VkMaterialEntry {
    uint32_t diffuseTexIndex;   // bindless slot (0 = white fallback)
    uint32_t normalTexIndex;    // bindless slot (1 = flat normal fallback)
    float    roughness;         // default 1.0 (fully diffuse)
    uint32_t flags;             // VK_MAT_FLAG_ALPHA_TESTED | VK_MAT_FLAG_TWO_SIDED | VK_MAT_FLAG_GLASS
    uint32_t vtxBufInstance;    // → VtxAddrTable[n] for UV interpolation
    uint32_t idxBufInstance;    // → IdxAddrTable[n] for UV interpolation
    float    alphaThreshold;    // default 0.5
    uint32_t pad;
};
```

**`idDrawVert` UV layout (GLSL):** stride = 15 floats (60 bytes); UV at float[3..4].

**Known limitation:** Multi-surface BLAS models use `geomVertAddrs[0]` only — UV
correct for surface 0 of each model, off for surfaces 1+.  Not a practical
issue for world geometry; deferred to Phase 6.

---

### Step 5.4b — Basic Glass Reflections (Blocked — see investigation)

#### Original Goal

`MC_TRANSLUCENT` geometry (windows, viewports) does not write to the depth
buffer, so `rgen` fires from the opaque wall *behind* the glass and never
sees the glass surface.  The goal was to make glass surfaces visible to
reflection rays and give them a flat Fresnel reflectance.

---

#### Investigation Findings (2026-04-06)

Implementation was attempted but blocked by two compounding problems: a crash
in the reflection shader caused by multi-geometry BLASes, and a more
fundamental problem with how Doom 3 renders glass.

---

##### Crash fix: Multi-surface BLAS OOB (implemented)

`VK_RT_MakeMaterialEntry` always writes `geomVertAddrs[0]` / `geomIdxAddrs[0]`
— surface 0's geometry addresses — into the material table for every TLAS
instance.  When a reflection ray hits surface 1+ of a multi-geometry BLAS,
`gl_PrimitiveID` is local to that surface but indexes into surface 0's buffer
→ out-of-bounds GPU read → `VK_ERROR_DEVICE_LOST`.

**Quick fix applied:** `rt_material.glsl` `rt_InterpolateUV()` now returns
`vec2(0.0)` early when `gl_GeometryIndexEXT != 0`:

```glsl
// Guard: address table stores only surface 0's geometry per TLAS instance.
// Hits on surface 1+ have a local gl_PrimitiveID that would OOB-read
// surface 0's buffers.  Return zero UVs until the table is refactored.
if (gl_GeometryIndexEXT != 0)
    return vec2(0.0);
```

**Proper fix (deferred):** Add `baseGeomIdx` to `VkMaterialEntry`; restructure
`vtxAddrs` / `idxAddrs` as per-geometry arrays indexed by
`baseGeomIdx + gl_GeometryIndexEXT`.  Touches `vk_raytracing.h`,
`vk_material_table.cpp`, TLAS rebuild, `rt_material.glsl`, and all three
shaders that call `rt_InterpolateUV`.

---

##### Doom 3 glass is screen-space, not geometry-based

Inspection of `materials/glass.mtr` (and alpha-era materials) shows every
standard glass surface uses a two-or-three stage setup:

| Stage | Technique |
|-------|-----------|
| 0 | `Program heatHaze.vfp` + `fragmentMap _currentRender` — screen-space distortion |
| 1 | `maskcolor` + `makealpha(texture)` — alpha mask |
| 2 | `cubeMap env/gen2` + `texgen reflect` — pre-baked environment cube reflection |
| 3 *(alpha only)* | `mirrorRenderMap 512 512` — id's intended real mirror render, cut from shipped game |

The `heatHaze` program is a custom vertex/fragment program that sources
`_currentRender` (the framebuffer capture) and applies normal-map distortion.
It is a pure screen-space effect with no RT-accessible geometry.

Consequences for the RT pipeline:

1. **`VK_DrawShaderPasses` skips stage 0** — the `heatHaze` stage has no
   `pStage->texture.image` (image is a program parameter via `fragmentMap`,
   not a standard `map`).  The code falls into the `!img` branch, doesn't
   match TG_SCREEN / TG_GLASSWARP / cinematic, and hits the `continue` path.
   This is correct behaviour for now; the stage is genuinely unrenderable
   without a proper heatHaze replacement.

2. **Glass surfaces do not appear in the BLAS** — world glass surfaces are
   never processed by the ambient lighting pass (`R_CreateAmbientCache` is
   not called for purely translucent surfaces), so `geo->ambientCache` is
   null.  `geo->verts` may also be null if the BSP compiler released CPU
   memory after static vertex-cache upload.  `VK_RT_BuildBLASForModel`'s
   geometry check (`haveGpuGeom || haveCpuGeom`) fails silently, and the
   surface is excluded.

3. **`MC_TRANSLUCENT` is not a glass proxy** — enabling translucent surfaces
   in the BLAS (commit 411c275) caused the multi-surface crash above.  Debug
   logging (`VK_RT GLASS:`) confirmed the surfaces flagged were decals
   (`textures/decals/splat6`), muzzle flashes (`machinegun_mflash*`), and
   particle effects (`smokepuff`) — not actual glass.  The real
   `textures/glass/glass1` surfaces were absent from the log entirely.

4. **`drawSurf_t` path is equivalent** — `drawSurf_t->geo` is the same
   `srfTriangles_t *` as the model surface.  Walking `viewDef->drawSurfs`
   for MC_TRANSLUCENT entries gives the same geometry pointer and the same
   null-cache problem; it does not provide access to geometry the entity path
   cannot see.

---

##### What the alpha materials reveal

The pre-release `glass.mtr` (recovered externally) adds a `mirrorRenderMap
512 512` stage to every glass material.  This was id Software's intended
real-reflection path: a recursive `R_RenderView` from the mirror's
perspective, captured to a scratch image.  The implementation exists in
`neo/renderer/tr_subview.cpp` (`R_MirrorRender`, `R_MirrorViewBySurface`).
The feature was cut from the shipped game, presumably for performance.

`R_MirrorViewBySurface` accesses `tri->verts` directly to compute the mirror
plane, confirming CPU verts are present on the `drawSurf_t` when this path is
active — but this path is not triggered by any shipped material.

---

#### Routes Forward

Three credible paths exist, in order of increasing scope:

**Route A — Fix ambientCache for glass (medium effort)**

Identify why world glass surfaces lack `ambientCache` (or `geo->verts`) and
ensure they are uploaded to the static vertex cache so `haveGpuGeom` is true
in `VK_RT_BuildBLASForModel`.  This is the narrowest change: glass geometry
enters the BLAS via the existing entity path and the GLSL changes in the
original Step 5.4b design (Fresnel split, bounce loop) proceed as planned.

The investigation point: trace whether `R_CreateAmbientCache` is called for
MC_TRANSLUCENT surfaces in the world area model setup path, and whether the
vertex cache block is accessible via `VK_VertexCache_GetBuffer`.

**Route B — Hook the `mirrorRenderMap` subview path (medium effort)**

`R_MirrorRender` in `tr_subview.cpp` already computes a mirrored viewDef and
has access to `tri->verts`.  Instead of doing a full recursive scene render,
intercept `mirrorRenderMap` stages in the Vulkan backend and substitute the
RT reflection buffer.  This would:
- Give glass surfaces a physically-plausible RT reflection without needing to
  solve the ambientCache problem
- Require shipping glass materials with the `mirrorRenderMap` stage re-enabled
  (either via a patched `.mtr` or a mod-style override in `base/materials/`)
- Still require glass geometry in the BLAS (same root problem, different
  trigger point)

**Route C — Global illumination naturally handles reflections (high effort)**

With a full path-traced GI pass (Phase 6), reflections are a degenerate case:
a roughness=0 surface bounces its single GI ray as a pure mirror reflection.
Glass becomes a transmissive BSDF material.  The `R_MirrorRender` subview path
becomes entirely redundant.

GI does **not** eliminate the BLAS geometry requirement — glass still needs to
be in the TLAS for rays to intersect it.  Route A (fixing ambientCache) is a
prerequisite for Route C as well.

---

#### Current State of the Shader Design

The GLSL changes described in the original plan (Fresnel branch in `rchit`,
bounce loop in `rgen`, glass flag in `rahit`) remain valid and unchanged.
They can be applied once the geometry pipeline problem is resolved via any
of the routes above.

**Flag definition (already in header, not yet in use):**
```cpp
#define VK_MAT_FLAG_GLASS 0x04u   // MC_TRANSLUCENT — thin glass, flat Fresnel
```

#### Known limitations (unchanged)

- No refraction (straight-through approximation). Fine for Doom 3's flat window
  panels.
- Angle-independent reflectance. Schlick can be added in `rchit` with no
  structural changes.
- One-sided: only the front glass face gets the Fresnel split. Imperceptible
  for thin single-pane geometry.

---

## Phase 6 — One-Bounce Global Illumination

### Goal

Add indirect diffuse lighting: light bouncing off surfaces illuminates geometry
not directly lit by a source.  Corners brighten slightly from nearby lit walls;
coloured surfaces tint adjacent geometry.

Builds directly on the Step 5.4 material infrastructure (SSBO + bindless
textures + vertex/index address tables).

---

### Albedo and Roughness from Existing Doom3 Materials

No new art required.  Doom3's material system has everything needed:

| GI Property | Doom3 Source |
|-------------|-------------|
| Albedo | `diffuse` stage texture (`diffuseTexIndex` in `MaterialEntry`) |
| Roughness | `specularExponent` → GGX: `roughness = sqrt(2.0 / (specExp + 2.0))` |
| Emissive | `blend add` stages — naturally appear as GI source geometry |
| Alpha mask | `MC_PERFORATED` + `alphaThreshold` — already in `MaterialEntry` |

Roughness is computed once at SSBO build time; no per-frame cost.

---

### Approach: Albedo-Weighted Irradiance (Coloured AO)

1. Shoot one cosine-weighted hemisphere ray per pixel from each visible surface.
2. At secondary hit: interpolate UV, sample albedo texture.
3. Weight albedo by a simple irradiance estimate.
4. Accumulate to a `VK_FORMAT_R16G16B16A16_SFLOAT` GI buffer.
5. Add GI buffer to the diffuse term in `interaction.frag`.

**Option A — Ambient-only (start here):**
`gi_colour = albedo * r_giAmbientScale`
~1 ray + 1 texture sample per pixel. Gives colour bleeding and contact brightening.

**Option B — Single light evaluation at secondary hit:**
Fire a shadow ray toward the nearest/brightest light from `viewDef->viewLights`.
`gi_colour = albedo * lightColour / attenuation * shadowFactor`
~2 rays per pixel. Gives directional bounce (red wall bouncing red onto the floor).

Start with A; promote to B once the infrastructure is stable.

---

### Step 6.1 — GI Pipeline (`gi_ray.rgen`, `gi_ray.rchit`, `vk_gi.cpp`)

**New files:**
```
neo/renderer/glsl/gi_ray.rgen   — cosine hemisphere ray generation
neo/renderer/glsl/gi_ray.rchit  — albedo lookup at secondary hit
neo/renderer/glsl/gi_ray.rmiss  — miss: sky/ambient colour
neo/renderer/Vulkan/vk_gi.cpp   — pipeline, dispatch, image lifecycle
```

The rgen shader reuses `rt_indirect.glsl` (same cosine sampling and
world-pos reconstruction as AO).  The rchit shader uses `rt_material.glsl`
(same UV interpolation and texture sampling as the reflection pipeline).

**GI image:** `VK_FORMAT_R16G16B16A16_SFLOAT`, one per frame-in-flight,
added as `vkGI_t giBuffer[VK_MAX_FRAMES_IN_FLIGHT]` in `vkRTState_t`.

---

### Step 6.2 — Temporal Accumulation for GI

Reuse the existing EMA resolve infrastructure from Step 5.2 (`vk_temporal.cpp`).
GI at 1 sample/pixel is substantially noisier than AO at 4; temporal
accumulation across 8–16 frames brings it to an acceptable level at no
additional ray cost.  Camera-cut detection resets GI history via the existing
`historyValid` mechanism.

---

### Step 6.3 — Integration in `interaction.frag`

```glsl
layout(set=0, binding=10) uniform sampler2D u_GIMap;

vec3 gi = (u_UseGI != 0) ? texture(u_GIMap, screenUV).rgb * u_GIStrength
                          : vec3(0.0);
diffuseLight += gi;
```

---

### Phase 6 CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtGI` | `0` | Enable one-bounce GI |
| `r_rtGISamples` | `1` | Bounce rays per pixel |
| `r_rtGIRadius` | `512.0` | Max bounce ray distance (world units) |
| `r_rtGIStrength` | `1.0` | Scale applied to GI contribution |
| `r_rtGILightBounce` | `0` | Enable Option B: evaluate nearest light at secondary hit |

---


## Implementation Order

| # | Step | Status |
|---|------|--------|
| 1 | Phase 4 correctness + optimizations | **Done** |
| 2 | Step 5.1 RTAO | **Done** |
| 3 | Step 5.2 Temporal EMA | **Done** |
| 4 | Step 5.2b Atrous filter | **Done** |
| 5 | Step 5.3 RT Reflections | **Done** |
| 6 | Step 5.4 Material table | **Done** |
| 7 | Step 5.4b Glass reflections | Blocked (geometry pipeline) |
| 8 | Phase 6 GI pipeline | Planned |
| 9 | Phase 6 Temporal + integration | Planned |

---

## File Summary

| Status | File | Purpose |
|--------|------|---------|
| Done | `vk_backend.cpp` | Per-light RT dispatch; init ordering |
| Done | `vk_shadows.cpp` | Shadow ray pipeline + per-light dispatch |
| Done | `vk_ao.cpp` | AO pipeline, dispatch, image lifecycle |
| Done | `vk_reflections.cpp` | Reflection pipeline, SBT, dispatch |
| Done | `vk_temporal.cpp` | EMA resolve + Atrous filter compute pipelines |
| Done | `vk_accelstruct.cpp` | BLAS/TLAS build; static/dynamic split; geom addr storage |
| Done | `vk_material_table.cpp` | Material SSBO, VtxAddrTable, IdxAddrTable, bindless textures |
| Done | `vk_raytracing.h` | All RT types and declarations |
| Done | `rt_indirect.glsl` | Cosine hemisphere sampling, world-pos reconstruction |
| Done | `rt_material.glsl` | MaterialEntry, set=1 bindings, UV interpolation helpers |
| Done | `ao_ray.rgen/rahit/rmiss` | AO ray shaders |
| Done | `shadow_ray.rgen/rahit/rmiss` | Shadow ray shaders (rahit alpha-tests) |
| Done | `reflect_ray.rgen/rchit/rahit/rmiss` | Reflection ray shaders |
| Done | `temporal_resolve.comp` | EMA blend |
| Done | `atrous_filter.comp` | Spatial denoise |
| Done | `interaction.frag` | Samples shadow mask, AO map, reflection buffer |
| Planned | `vk_gi.cpp` | GI pipeline lifecycle + dispatch |
| Planned | `gi_ray.rgen/rchit/rmiss` | GI ray shaders |

---

## Changelog

### 2026-04-06

- Marked Step 5.4b as blocked; replaced original planned design with full
  investigation findings.
- Documented multi-surface BLAS OOB crash (root cause, quick fix applied in
  `rt_material.glsl`, proper fix deferred).
- Documented why Doom 3 glass is absent from the BLAS: screen-space `heatHaze`
  materials, no `ambientCache`, `MC_TRANSLUCENT` flag matching wrong surfaces.
- Analysed alpha-era `mirrorRenderMap` material stage and `R_MirrorRender` in
  `tr_subview.cpp` as a potential interception point.
- Added three concrete routes forward (A: fix ambientCache, B: hook subview
  path, C: GI).  Route A is prerequisite for all others.
- Updated implementation order table.

### 2026-04-04

- Condensed completed steps 5.1–5.4 to brief done summaries; removed code
  scaffolding that is now superseded by the actual implementation.
- Added Step 5.4b (glass reflections, flat Fresnel) as a planned step.
- Updated status table and implementation order to reflect current reality.
- Trimmed Phase 6 design sketches; kept high-level design, removed redundant
  code that duplicates what rt_material.glsl and rt_indirect.glsl already provide.

### 2026-03-25

- Added current-state snapshot marking Phase 4 correctness as complete.
- Reworked optimization workstream with status labels.
- Rewrote implementation order into a status-driven sequence.

### 2026-03-21 (historical baseline)

- Original Phase 4 and Phase 5 planning draft.
