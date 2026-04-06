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
| 5.4b | Basic glass reflections (flat Fresnel) | **Planned** |
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

### Step 5.4b — Basic Glass Reflections (Planned)

#### Goal

`MC_TRANSLUCENT` geometry (windows, viewports) does not write to the depth
buffer, so `rgen` fires from the opaque wall *behind* the glass and never
sees the glass surface.  This step makes glass surfaces visible to reflection
rays and gives them a flat 4 % reflectance (F0 for real glass ≈ 0.04) with
the remaining 96 % transmitted straight through (no refraction).

The result is a visible specular highlight on glass panels at low cost: one
additional `traceRayEXT` only when a reflection ray hits glass, which is rare
in most Doom 3 scenes.

#### Why flat 4 % is acceptable

Real Fresnel ranges from ≈ 4 % at normal incidence to 100 % at grazing angle.
Doom 3 glass is mostly seen at moderate viewing angles where the error is
small.  Angle-dependent Schlick (`F0 + (1−F0) × (1 − dot(N,V))^5`) can be
added later in `rchit` with no structural changes.

#### Changes required

**1. New flag (`vk_raytracing.h`, `rt_material.glsl`)**

```cpp
#define VK_MAT_FLAG_GLASS 0x04u   // MC_TRANSLUCENT — thin glass, F0 = 0.04
```

**2. Set flag in `VK_RT_MakeMaterialEntry` (`vk_material_table.cpp`)**

```cpp
if (shader->Coverage() == MC_TRANSLUCENT)
    entry.flags |= VK_MAT_FLAG_GLASS;
```

**3. Mark glass BLAS geometry non-opaque (`vk_accelstruct.cpp`)**

Translucent surfaces currently receive `VK_GEOMETRY_OPAQUE_BIT_KHR`.  Remove
that flag for glass so `rahit` is invoked.  Requires passing a second bool
`isTranslucent` alongside the existing `isPerforated` parameter.

**4. Extend the reflection payload (`reflect_ray.rgen` / `reflect_ray.rchit`)**

```glsl
layout(location = 0) rayPayloadEXT struct ReflPayload {
    vec3  colour;         // colour for this ray segment
    float transmittance;  // weight to carry through (0 = stop)
    vec3  nextOrigin;     // origin for the continuation ray
    vec3  nextDir;        // direction (straight-through for glass)
} reflPayload;
```

**5. Glass branch in `reflect_ray.rchit`**

```glsl
if ((mat.flags & MAT_FLAG_GLASS) != 0u) {
    const float F0       = 0.04;
    vec4 diffuse         = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
    reflPayload.colour        = F0 * diffuse.rgb;
    reflPayload.transmittance = 1.0 - F0;
    reflPayload.nextOrigin    = gl_WorldRayOriginEXT
                                + gl_WorldRayDirectionEXT * (gl_HitTEXT + 0.01);
    reflPayload.nextDir       = gl_WorldRayDirectionEXT;  // straight-through
    return;
}
// Opaque path unchanged — set transmittance = 0.0 to stop the loop
```

**6. Bounce loop in `reflect_ray.rgen`** (replaces single `traceRayEXT`)

```glsl
vec3  accum  = vec3(0.0);
float weight = 1.0;
vec3  origin = worldPos + normal * 0.5;
vec3  dir    = reflDir;

for (int i = 0; i < 2 && weight > 0.01; i++) {
    reflPayload.transmittance = 0.0;
    traceRayEXT(topLevelAS, gl_RayFlagsNoneEXT, 0xFF,
                0, 0, 0, origin, 0.01, dir, params.maxDist, 0);
    accum  += weight * reflPayload.colour;
    weight *= reflPayload.transmittance;
    origin  = reflPayload.nextOrigin;
    dir     = reflPayload.nextDir;
}
imageStore(reflImage, coord, vec4(accum * params.reflBlend, 1.0));
```

Max 2 iterations covers double-pane windows.

**7. `reflect_ray.rahit` — accept glass hits**

```glsl
if ((mat.flags & MAT_FLAG_ALPHA_TESTED) != 0u) {
    // existing alpha-discard path
} else if ((mat.flags & MAT_FLAG_GLASS) != 0u) {
    // fall through — rchit handles the Fresnel split
} else {
    terminateRayEXT;
}
```

#### Files changed

| File | Change |
|------|--------|
| `vk_raytracing.h` | `VK_MAT_FLAG_GLASS 0x04u` |
| `vk_material_table.cpp` | Set flag for `MC_TRANSLUCENT` |
| `vk_accelstruct.cpp` | Remove opaque flag for translucent geometry |
| `rt_material.glsl` | `MAT_FLAG_GLASS` constant |
| `reflect_ray.rgen` | 2-iteration bounce loop |
| `reflect_ray.rchit` | Glass branch + transmittance fields |
| `reflect_ray.rahit` | Accept glass, fall through to rchit |

#### Known limitations

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

## Open Bugs

### Bug A: Mirrors/Portals Render as Black

`vk_backend.cpp:962` skips shader stages with `DI_MIRROR_RENDER` /
`DI_REMOTE_RENDER`.  The frontend generates subviews correctly (shared with GL)
but the Vulkan backend never binds the captured `scratchImage`.

**Fix:** Remove the skip; bind `scratchImage` for mirror stages.  Verify
`VK_RB_CopyRender` fires and that `isMirror` flips winding order correctly.

**Note:** Once RT reflections are solid, the rasterized mirror subview path can
be retired — mirrors become a degenerate case of roughness = 0 in the
reflection pipeline (no clip plane, no second full-scene render).

**Priority:** Medium-high.

### Bug B: Texture Aliasing / Z-Fighting on Camera Tilt

Likely cause: `vkCmdSetDepthBias` argument mapping may have `constant` and
`slope` swapped relative to GL's `glPolygonOffset` semantics.  Secondary
cause: anisotropic filtering is disabled, causing mip-level oscillation on
oblique surfaces.

**Fix:** Verify depth-bias argument order; enable anisotropic filtering by
default for non-nearest samplers.

**Priority:** Medium.

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
| 7 | Step 5.4b Glass reflections | Planned |
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
