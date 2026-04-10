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
| 5.4b | Basic glass reflections (flat Fresnel) | **Done** |
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

### Step 5.4b — Basic Glass Reflections (Done)

#### Original Goal

`MC_TRANSLUCENT` geometry (windows, viewports) does not write to the depth
buffer, so `rgen` fires from the opaque wall *behind* the glass and never
sees the glass surface.  The goal was to make glass surfaces visible to
reflection rays and give them a flat Fresnel reflectance.

Split probe and reflection appearance to allow the player to appear in reflections, without their geometry
occluding and causing strange reflections.  Can mainly control scale with r_rtreflectionblend.  F0 does not seem to have much effect.  

#### Debug notes. 
(Have found it necessary to force the LLMs to actual fix the bugs and not just disable features that aren't working)

Found it useful to split apart the submission queue to allow per-stage debugging to find which batch of operations 
poisoned the submission queue.

Then debugging by bisecting bad shaders and color coding potential bad shapes/vertexes allowed us to see what we going wrong on the GPU without print statements. 

---

##### Crash fix: Stale dynamic geometry addresses + vertex bounds check (implemented 2026-04-09)

**Root cause (fully diagnosed):** Dynamic model geometry (player, weapons, monsters)
is skinned and uploaded to the vertex cache ring buffer each frame at a new offset.
`vkBLAS_t::geomVertAddrs[g]` was set once at BLAS build time and never refreshed,
so it held a stale `(baseAddr + oldOffset)` from a previous frame.  Once the ring
buffer wrapped, `rt_InterpolateUV` dereferenced an address pointing into unrelated
or freed memory → GPU page fault → `VK_ERROR_DEVICE_LOST`.


**Fix implemented (commit `8e39f41`):**

1. **Address refresh in TLAS rebuild** — for each dynamic instance geometry
   slot `g`, `VK_RT_RebuildTLAS` now walks the model's surfaces and calls `VK_VertexCache_GetBuffer`
   to obtain the current `(VkBuffer, offset)` pair.  The device address is
   recomputed and written each frame.  

2. **`maxVertex` bounds guard** — `vkBLAS_t` now stores `geomVertSizes[g]` and
   `geomIdxSizes[g]` at build time.  The TLAS rebuild loop checks fetched indices are allowed.
   Default is `0xFFFFFFFFu` (no check) for static geometry.

3. **`VkMaterialEntry` layout update** — `pad0` renamed to `maxVertex`
   
**`rt_material.glsl` guard (added):**
```glsl
uint maxVtx = mat.maxVertex;
if (maxVtx != 0xFFFFFFFFu && (i0 > maxVtx || i1 > maxVtx || i2 > maxVtx))
    return vec2(0.0);
```

**Result:** Reflections now run stably with full material/texture sampling on
both world and dynamic (player/weapon) geometry.  The `gl_GeometryIndexEXT != 0`
early-out guard added previously remains as a fallback for multi-surface BLASes
not yet covered by the per-geometry address table.

**Remaining issue:** An occasional `VK_ERROR_DEVICE_LOST` crash occurs when all
split submits are disabled (i.e. the full frame in a single command buffer with
no intermediate queue submissions).  This is not yet diagnosed; it may be a
missing pipeline barrier between AS build and ray dispatch when both occur in
the same submit.  Split submits remain the default and keep the game stable.

---

#### Known limitations (unchanged)

- Player weapon does not reflect.  The model movements also differ between 1st person and third person.  See shotgun reload.  This includes flashlights and projectiles.  (e.g plasma gun)
- Models behind the player do not render.  

- No refraction (straight-through approximation). Fine for Doom 3's flat window
  panels.  - One-sided: only the front glass face gets the Fresnel split. Both Imperceptible
  for thin single-pane geometry.  

- Angle-independent reflectance. Schlick can be added in `rchit` with no
  structural changes.  WOuld be noticeable to have more reflection at more grazing angles. 


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
| 7 | Step 5.4b Glass reflections | **Done** |
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

### 2026-04-09

- Identified root cause of `VK_ERROR_DEVICE_LOST` crash in reflection hit shaders:
  dynamic geometry vertex/index addresses were stale (captured at BLAS build time;
  vertex cache ring buffer advances each frame).
- Added per-frame address refresh in `VK_RT_RebuildTLAS`: walks model surfaces,
  calls `VK_VertexCache_GetBuffer` for each dynamic geometry slot, recomputes
  device address from current `(VkBuffer, offset)` each frame.
- Added `geomVertSizes`/`geomIdxSizes` arrays to `vkBLAS_t` (stored at build time).
- Added `maxVertex` field to `VkMaterialEntry` (was `pad0`); populated in TLAS
  rebuild as `numVerts - 1`; default `0xFFFFFFFFu` (no check) for static geometry.
- Added `maxVertex` bounds guard in `rt_InterpolateUV` (`rt_material.glsl`):
  any fetched index exceeding `maxVertex` returns `vec2(0.0)`.
- Reflections now stable with full material/texture sampling on world and dynamic
  (player/weapon) geometry.
- `r_rtReflectionBlend` default raised from 0.1 → 0.5 now that reflections are
  stable.
- **Known remaining issue:** occasional crash when all split submits are disabled;
  likely a missing barrier between AS build and ray dispatch in a unified submit.
  Split submits remain the safe default.

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
