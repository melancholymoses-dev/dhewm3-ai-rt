# RTX Refactor — Phase 4 Fix & Phase 5 Plan

**Document:** `rtx_refactor_plan3.md`
**Date:** 2026-03-21
**Branch:** `improve_vulkan_rt`

This document records what has been done, identifies the current gaps that
must be fixed to make Phase 4 (RT shadows) actually correct, and lays out
the detailed steps for Phase 5 (RTAO + reflections).

---

## Status Summary

| Phase | Description | State |
|-------|-------------|-------|
| 1 | GLSL shaders (ARB → GLSL/SPIR-V) | **Done** |
| 2 | Vulkan rasterization backend | **Done** (minor visual gaps remain — see §Rendering Gaps) |
| 3 | Acceleration structures (BLAS/TLAS) | **Done structurally; performance issues remain** |
| 4 | RT shadow pass | **Structurally done; critical correctness bug; performance issues** |
| 5 | RTAO + RT reflections | **Not started** |
| 6 | One-bounce global illumination | **Not started — scope expansion, see §Phase 6** |

The Stencil shadow fallback path (`VK_RB_DrawShadowSurface`, z-fail / Carmack's Reverse)
was a source of debugging pain but is now structurally complete. It can be left as-is for
the non-RT fallback. **The RT path should be verified independently of the stencil path
by running with `r_useRayTracing 1 r_rtShadows 1`.**

---

## Phase 4 — Fixes Required

### Fix 1 (Critical): Per-Light Shadow Mask — Design Flaw

**File:** `neo/renderer/Vulkan/vk_shadows.cpp` — `VK_RT_DispatchShadowRays()`
**File:** `neo/renderer/Vulkan/vk_backend.cpp` — `VK_RB_DrawInteractions()`

#### The Problem

`VK_RT_DispatchShadowRays()` loops over `viewDef->viewLights` and for each
light it dispatches shadow rays and **overwrites the same `vkRT.shadowMask`
image**. At the end of the loop only the last light's shadows are in the
texture.

Then `VK_RB_DrawInteractions()` runs the per-light interaction pass inside a
render pass, and every light samples `u_ShadowMask` — but the mask now only
reflects the shadows of whichever light was processed last by the dispatch
loop. The result: most lights have incorrect shadows (either all shadowed or
all lit, depending on which light ended up last).

This mirrors what stencil shadows do correctly: clear-and-fill stencil
**per light**, then draw that light's interactions against that light's
stencil state.

#### The Fix — Interleaved Per-Light RT Dispatch

Restructure `VK_RB_DrawInteractions` so that for each light:

1. **End the current render pass** (`vkCmdEndRenderPass`)
2. **Dispatch shadow rays for this light only** — call a new
   `VK_RT_DispatchShadowRaysForLight(cmd, viewDef, vLight)` that only writes
   the shadow mask for the given `viewLight_t *`.
3. **Reopen the render pass** using the `renderPassResume` (LOAD_OP_LOAD so
   prior colour/depth is preserved), restore viewport and scissor.
4. **Draw this light's interactions** (local + global, as today).
5. Before closing the render pass again: transition the shadow mask back to
   `VK_IMAGE_LAYOUT_GENERAL` ready for the next light's dispatch.

```
for each vLight:
  vkCmdEndRenderPass(cmd)
  → VK_RT_DispatchShadowRaysForLight(cmd, vLight)   // writes shadow mask
  → pipeline barrier: GENERAL→SHADER_READ_ONLY_OPTIMAL
  vkCmdBeginRenderPass(cmd, renderPassResume, ...)
  → restore viewport/scissor
  → VK_RB_DrawShadowVolumes (stencil path, skipped when RT active)
  → draw localInteractions / globalInteractions sampling shadow mask
  → pipeline barrier: SHADER_READ_ONLY→GENERAL  (on shadow mask)
```

This exactly mirrors the stencil approach (clear stencil per light, draw
shadow volumes per light, draw interactions per light) but with RT dispatch
replacing the stencil steps.

The outer TLAS rebuild (`VK_RT_RebuildTLAS`) stays where it is — called once
before the first light, outside the render pass, before the loop starts.

#### Refactoring `VK_RT_DispatchShadowRays`

Extract the per-light work into a new internal function:

```cpp
// vk_shadows.cpp
static void VK_RT_DispatchShadowRaysForLight(
    VkCommandBuffer cmd,
    const viewDef_t *viewDef,
    const viewLight_t *vLight);
```

The public `VK_RT_DispatchShadowRays(cmd, viewDef)` can be removed or kept
as a batch-all convenience (for debugging: verify the last-light shadow mask
appears in the frame to confirm the RT pipeline is alive).

Declare the new function in `vk_raytracing.h`:

```cpp
void VK_RT_DispatchShadowRaysForLight(VkCommandBuffer cmd,
                                      const viewDef_t *viewDef,
                                      const viewLight_t *vLight);
```

---

### Fix 2 (Performance): Remove `vkQueueWaitIdle` Inside Per-Light Loop

**File:** `neo/renderer/Vulkan/vk_shadows.cpp` line ~450

```cpp
// CURRENT — catastrophically slow:
vkQueueWaitIdle(vk.graphicsQueue);
vkDestroySampler(vk.device, depthSampler, NULL);
vkDestroyBuffer(vk.device, uboBuf, NULL);
vkFreeMemory(vk.device, uboMem, NULL);
```

`vkQueueWaitIdle` serialises every light dispatch with CPU + GPU sync. With
20+ lights per frame this makes the RT path effectively unusable.

#### The Fix — UBO Ring Buffer for Shadow Params

The shadow dispatch already uses a per-frame data ring for vertex/index data.
Apply the same pattern here:

1. Allocate a small host-visible ring buffer sized for
   `MAX_LIGHTS_PER_FRAME * sizeof(ShadowParamsUBO)` (e.g. 256 lights × 80
   bytes = 20 KB), one per frame-in-flight.  Call it `shadowUBORing`.
2. At the start of each frame, reset the ring offset to 0.
3. Each per-light dispatch suballocates `sizeof(ShadowParamsUBO)` from the
   ring — no per-dispatch alloc/free needed.
4. The depth sampler (`VkSampler`) is constant — create it once in
   `VK_RT_InitShadows()` and store in `vkRTState_t`. No per-dispatch sampler
   creation/destruction.
5. Remove the `vkQueueWaitIdle` entirely. The UBO ring memory is only
   reused next frame, by which time the fence for that frame-in-flight has
   been signalled.

---

### Fix 3 (Correctness): BLAS Only Built from CPU-Side Vertices

**File:** `neo/renderer/Vulkan/vk_accelstruct.cpp` — `VK_RT_BuildBLAS()`

`tri->verts` is only guaranteed non-NULL while the model data is loaded. If
mesh data has been freed to GPU-only (possible after `ambientCache` upload),
the BLAS build silently returns NULL and that geometry is absent from the
TLAS, casting no shadow.

#### The Fix

Before the BLAS is built: check `tri->verts != NULL`. If the CPU data is
gone, log a warning (once per unique mesh) and skip. Separately, hook the
BLAS build at the point where `ambientCache` is populated in
`VertexCache.cpp` (`R_CreateAmbientCache`) — at that moment `tri->verts` is
still valid. Store the built `vkBLAS_t *` in `srfTriangles_t` (add a new
field `vkBLAS_t *blasHandle = NULL`).

Longer term: use the GPU vertex buffer already uploaded to
`ambientCache` directly as the BLAS geometry source, eliminating the staging
copy entirely. This requires the vertex cache buffer to be created with
`VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`.

---

### Fix 4 (Performance): Batch BLAS Uploads

**File:** `neo/renderer/Vulkan/vk_accelstruct.cpp`

Currently each BLAS build does two `VK_BeginSingleTimeCommands()` calls
(vertex upload + index upload), each submitting and waiting synchronously.
This serialises every BLAS build at startup.

**Fix:** Batch all BLAS uploads and builds into a single command buffer
using `VK_CmdBuildAccelerationStructures` with an array of geometry
descriptors. Record the full batch, submit once, wait once. This is a
single-submission fence wait at level-load time, not per-BLAS.

---

### Fix 5 (Minor): Review Notes from Code Review

These are lower priority but should be addressed before shipping:

| Issue | File | Fix |
|-------|------|-----|
| `depthClamp` enabled without checking support | `vk_instance.cpp:285` | Query `VkPhysicalDeviceFeatures` first; enable only if supported |
| `vk.swapchainFormat` stale after `VK_RecreateSwapchain` | `vk_swapchain.cpp:400` | Assign `vk.swapchainFormat = surfaceFormat.format` in recreate path |
| Depth prepass opaque variant samples `gui.frag` (has texture fetch) | `vk_pipeline.cpp:704` | Create a trivial no-output frag shader for the opaque depth variant |
| Readback buffer not resized when swapchain grows | `vk_backend.cpp` | Track allocated size; destroy+recreate when `vk.swapchainExtent` is larger |
| `vkDeviceWaitIdle` per vertex cache buffer free | `vk_buffer.cpp` | Defer destruction to per-frame garbage list; wait once on fence |

---

## Rendering Correctness Gaps (Phase 2 carry-overs)

These affect visual quality of the rasterized output that RT shadows are
applied to. They do not block RT verification but affect correctness.

| # | Gap | Impact | Plan ref |
|---|-----|--------|----------|
| 1 | Multiple blend modes | Particles, glows, muzzle flashes invisible | `missing_pieces.md §2` |
| 2 | Depth prepass uses `LESS_OR_EQUAL` not `EQUAL` | No early-Z rejection savings | `missing_pieces.md §1` |
| 3 | Texture coord transforms missing | Animated surfaces (water, fire) are static | `missing_pieces.md §3` |
| 4 | LightScale not applied | Scene brightness wrong | `missing_pieces.md §6` |
| 5 | Two-sided cull mode not selected | Vegetation/fences clipped | `missing_pieces.md §9` |
| 6 | Fog/blend lights absent | No atmospheric effects | `missing_pieces.md §5` |

These are documented in detail in `missing_pieces.md`. They should be
addressed in parallel with (or after) Phase 4 fixes to produce a visually
correct rasterised base that RT is applied on top of.

---

## Phase 5 — RTAO + RT Reflections

Phase 5 adds two more RT effects on top of the working shadow pass from
Phase 4. Each is a new ray-generation shader dispatched after the depth
prepass, writing results into its own storage image sampled during the
interaction/ambient pass.

---

### Step 5.1 — Ray Traced Ambient Occlusion (RTAO)

#### Goal

Replace the flat ambient term in Doom 3 with per-pixel AO: shoot N
hemispherical rays from each surface point; fraction of misses → AO factor.
Apply the AO factor to the ambient lighting stage (surfaces lit by
`SL_AMBIENT` or the unlit ambient pass).

#### New Files

```
neo/renderer/glsl/ao_ray.rgen      — ray generation shader
neo/renderer/Vulkan/vk_ao.cpp      — AO pipeline, dispatch, and AO buffer lifecycle
```

#### AO Image

- Format: `VK_FORMAT_R8_UNORM` (one byte per pixel, 0=fully occluded, 1=clear)
- One image per frame-in-flight (same pattern as shadow mask)
- Add `vkAO_t aoBuffer[VK_MAX_FRAMES_IN_FLIGHT]` to `vkRTState_t`
- Add `VkSampler aoSampler` (nearest-clamp) to `vkRTState_t`

#### `ao_ray.rgen` Shader Design

```glsl
#version 460
#extension GL_EXT_ray_tracing : require

layout(set=0, binding=0) uniform accelerationStructureEXT topLevelAS;
layout(set=0, binding=1, r8) uniform image2D aoImage;
layout(set=0, binding=2) uniform sampler2D depthSampler;
layout(set=0, binding=3) uniform sampler2D normalSampler;   // world-space normals

layout(set=0, binding=4) uniform AOParams {
    mat4  invViewProj;
    float aoRadius;        // max AO ray length (Doom 3 units, ~64–128)
    int   numSamples;      // rays per pixel (4–16)
    uint  frameIndex;
    float pad;
};

layout(location = 0) rayPayloadEXT float aoPayload;

// Cosine-weighted hemisphere sample around n
vec3 cosineSampleHemisphere(vec3 n, uint seed);

void main() {
    ivec2 coord = ivec2(gl_LaunchIDEXT.xy);
    vec3 worldPos = reconstructWorldPos(coord, ...);
    vec3 normal   = sampleWorldNormal(coord);

    float occlusion = 0.0;
    for (int i = 0; i < numSamples; i++) {
        vec3 dir = cosineSampleHemisphere(normal, seed + i);
        aoPayload = 1.0;   // miss shader writes 1.0 (unoccluded)
        traceRayEXT(topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
            0xFF, 0, 0, 0,
            worldPos + normal * 0.5,   // offset along normal
            0.0, dir, aoRadius, 0);
        occlusion += (1.0 - aoPayload);
    }
    float ao = 1.0 - (occlusion / float(numSamples));
    imageStore(aoImage, coord, vec4(ao));
}
```

The miss shader writes `aoPayload = 1.0` (same pattern as shadow miss).
The any-hit shader writes `aoPayload = 0.0` and terminates (same pattern as
shadow any-hit). These can be **reused** from the shadow RT pipeline by
adding a second hit/miss group to the same SBT, or by creating a separate
AO pipeline.

**Separate pipeline is simpler to start** (avoids SBT complexity).

#### World-Space Normal Source

AO requires surface normals at each pixel. Options (in order of difficulty):

1. **Read from the G-buffer normal map** — sample the bump map at the
   surface's UV and transform from tangent space to world space using the
   TBN from the G-buffer. Requires adding a normal render target.
2. **Reconstruct from depth** — compute normal from cross-product of depth
   gradients (`dFdx`/`dFdy` equivalent via finite differences in the rgen
   shader). Lower quality but requires no extra render target.
3. **Use the geometric normal from the TLAS hit** — only available in
   closest-hit shaders, not in the rgen shader.

**Recommended start:** depth-derived normals (option 2). Add a dedicated
G-buffer normal pass later when it becomes important for quality.

Depth-derived normal in the rgen shader:
```glsl
vec3 p0 = reconstructWorldPos(coord, size);
vec3 p1 = reconstructWorldPos(coord + ivec2(1, 0), size);
vec3 p2 = reconstructWorldPos(coord + ivec2(0, 1), size);
vec3 normal = normalize(cross(p1 - p0, p2 - p0));
```

#### C++ Dispatch (`vk_ao.cpp`)

```cpp
void VK_RT_DispatchAO(VkCommandBuffer cmd, const viewDef_t *viewDef);
```

Call this from `VK_RB_DrawView` (in `vk_backend.cpp`) once per frame, after
`VK_RT_RebuildTLAS` and before `VK_RB_FillDepthBuffer`.  Must be outside a
render pass.

#### Integration in the Interaction Shader

Add `u_AOFactor` sampling to `interaction.frag`:

```glsl
layout(set=0, binding=8) uniform sampler2D u_AOMap;
// in UBO: int u_UseAO;
```

Sample at `gl_FragCoord.xy / vec2(u_ScreenWidth, u_ScreenHeight)` and
multiply into the ambient/diffuse term:

```glsl
float ao = (u_UseAO != 0) ? texture(u_AOMap, screenUV).r : 1.0;
diffuseLight *= ao;
```

Also add binding 8 to `VK_CreateInteractionDescLayout()` in `vk_pipeline.cpp`
and upload the AO sampler/view in `VK_UpdateInteractionDescriptors()`.

#### New CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtAO` | `0` | Enable RTAO (requires `r_useRayTracing 1`) |
| `r_rtAOSamples` | `4` | Rays per pixel (1–32) |
| `r_rtAORadius` | `64.0` | Max AO ray length in world units |

---

### Step 5.2 — Temporal Denoising for AO and Shadows

Shooting 1–4 rays per pixel produces noisy results. A simple temporal
accumulation (exponential moving average, EMA) dramatically improves quality
at near-zero cost.

#### Design

- Maintain a **history image** (`VK_FORMAT_R8_UNORM`) per effect per
  frame-in-flight. (Two more images: `shadowHistory`, `aoHistory`.)
- After each dispatch, run a **resolve pass** (a compute shader or a
  full-screen graphics pass) that blends the new frame's result into the
  history image:
  ```glsl
  float blended = mix(history, current, alpha);  // alpha ≈ 0.1–0.2
  ```
- The interaction shader samples the history image rather than the raw
  dispatch output.

#### Camera-Cut Reset

When the camera cuts (e.g. loading a new area), the history is stale.
Detect this by comparing the current `invViewProj` against the stored one
from last frame. If they differ by more than a threshold (translation >
some units or rotation > some degrees), write `alpha = 1.0` (no
accumulation, use current frame directly) for that frame.

This can be tracked in `vkRTState_t`:
```cpp
float prevInvViewProj[16];
bool historyValid;
```

#### New Files

```
neo/renderer/glsl/temporal_resolve.comp  — compute shader for EMA blend
neo/renderer/Vulkan/vk_temporal.cpp      — pipeline + dispatch for resolve
```

The resolve pass must run **after** the RT dispatch and **before** the
render pass opens (so the interaction shader sees the blended result).

---

### Step 5.3 — RT Reflections

#### Goal

For specular/metallic surfaces, shoot a reflection ray from the camera hit
point along the specular reflection direction and sample the radiance of
whatever the ray hits. Blend the reflected colour into the specular term.

This is optional and expensive. Only enable for highly-specular surfaces
(metal floors, mirrors, polished panels).

#### New Files

```
neo/renderer/glsl/reflect_ray.rgen      — reflection ray generation shader
neo/renderer/Vulkan/vk_reflections.cpp  — reflection pipeline and dispatch
```

#### Reflection Image

- Format: `VK_FORMAT_R16G16B16A16_SFLOAT` (HDR colour)
- One image per frame-in-flight: `vkReflection_t reflBuffer[VK_MAX_FRAMES_IN_FLIGHT]`
- The 4th channel (alpha) stores a confidence/blend weight

#### `reflect_ray.rgen` Shader Design

```glsl
// Per-pixel:
vec3 worldPos = reconstructWorldPos(coord, ...);
vec3 normal   = reconstructNormal(coord, ...);    // depth-derived (same as AO)
vec3 viewDir  = normalize(worldPos - cameraPos);
vec3 reflDir  = reflect(viewDir, normal);

// Roughness gate: skip rough surfaces (material roughness > threshold)
// For now, use a single global threshold CVar.
// Later: read from a roughness G-buffer channel.
float roughness = sampleRoughness(coord);          // placeholder: constant 0
if (roughness > r_rtReflectionRoughnessThreshold)
{
    imageStore(reflImage, coord, vec4(0.0));
    return;
}

// Shoot reflection ray
reflPayload = vec4(0.0);    // miss = environment colour
traceRayEXT(topLevelAS,
    gl_RayFlagsNoneEXT,     // need closest-hit for colour
    0xFF, 0, 0, 0,
    worldPos + normal * 0.5,
    0.01, reflDir, 10000.0, 0);

imageStore(reflImage, coord, reflPayload);
```

Unlike shadow/AO rays, reflection rays need a **closest-hit shader** to
shade the hit point (simple diffuse evaluation, or a lookup into the
lightmap, or just the material's diffuse colour). This is more complex than
the shadow/AO any-hit pattern.

**Simplified closest-hit:** look up the material's diffuse texture at the
hit barycentric UV, multiply by the light colour at that point. This
requires passing texture/material index via TLAS custom instance data — a
significant but one-time investment in TLAS instance metadata.

**Fallback for misses:** Sample a cubemap (use `_currentRender` or a
pre-captured environment cube). This covers sky/background reflections.

#### Integration in `interaction.frag`

Sample the reflection buffer and add to the specular term:

```glsl
layout(set=0, binding=9) uniform sampler2D u_ReflectionMap;
// in UBO: int u_UseReflections;

vec3 reflColor = (u_UseReflections != 0)
    ? texture(u_ReflectionMap, screenUV).rgb
    : vec3(0.0);
specularLight += reflColor * specularMask;
```

#### New CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtReflections` | `0` | Enable RT reflections |
| `r_rtReflectionRoughnessThreshold` | `0.3` | Skip surfaces rougher than this |

---

### Step 5.4 — TLAS Instance Metadata for Material Lookup

**Note: this step is now a shared prerequisite for Phase 6 (GI) and the
deferred alpha-test fix (shadow_ray.rahit). Implementing it once here
unlocks all three. See §Phase 6 for the full design.**

Both the AO closest-hit and the reflection closest-hit need to know what
material was hit so they can look up diffuse colour, alpha mask, and
roughness. This requires storing material data in the TLAS.

#### Design

Each `VkAccelerationStructureInstanceKHR` has a 24-bit `instanceCustomIndex`
field. Use this to index into a **material data SSBO** bound to all RT
shaders:

```glsl
// set=2, binding=0
layout(std430, set=2, binding=0) readonly buffer MaterialTable {
    MaterialEntry materials[];
};

struct MaterialEntry {
    uint  diffuseTexIndex;    // index into a bindless texture array
    uint  normalTexIndex;
    float roughness;          // derived from Doom3 specularExponent (see §Phase 6)
    uint  flags;              // MATERIAL_FLAG_ALPHA_TESTED, MATERIAL_FLAG_TWO_SIDED, etc.
    uint  vtxBufInstance;     // index into VertexBufferTable for UV interpolation
    uint  idxBufInstance;     // index into IndexBufferTable for UV interpolation
    float alphaThreshold;     // from alphaTestRegister (MC_PERFORATED only)
    uint  pad;
};
```

The SSBO is built once per scene load and updated when new geometry is
registered. For Phase 5 start: only `diffuseTexIndex` and `roughness` are
needed.

A **bindless texture array** (`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`
with a large `descriptorCount`) stores all loaded textures, indexed from the
material table. This requires the
`VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing` feature
(widely supported on all RT-capable GPUs).

A **vertex buffer table SSBO** and **index buffer table SSBO** (one entry
per BLAS, pointing to device addresses of `geomVtxBuf` and `geomIdxBuf`)
allow any-hit and closest-hit shaders to interpolate UVs at secondary hit
points using `gl_PrimitiveID` and `gl_HitBarycentricsEXT`. These are the
same buffers already retained in `vkBLAS_t`; no new uploads are needed.

This is a substantial infrastructure change and can be deferred until
reflection hit shading or GI is needed. For AO (which only needs 0/1
occlusion), no material data is required.

---

---

## Phase 6 — One-Bounce Global Illumination

### Goal

Add indirect diffuse lighting: light that bounces off geometry and
illuminates surfaces not directly lit by a source. This produces the
"light leaking into shadows" quality typically missing from direct-only
rendering — corners brighten slightly from nearby lit walls, coloured
surfaces tint adjacent geometry.

This is a scope expansion beyond Phase 5. It builds directly on the
material infrastructure from Step 5.4 (SSBO + bindless textures + vertex
buffer table).

---

### Albedo and Roughness from Existing Doom3 Materials

**No new art or modeling is required.**

Doom3's material system already contains everything needed:

| GI Property | Doom3 Source | Notes |
|-------------|-------------|-------|
| Albedo | `diffuse` stage texture (`diffuseTexIndex` in MaterialEntry) | Exactly what GI needs — base colour at secondary hit |
| Roughness | `specularExponent` on the specular stage | Map to GGX roughness: `roughness = sqrt(2.0 / (specExp + 2.0))`. Store the result in `MaterialEntry.roughness` at SSBO build time. |
| Emissive | Materials with a `blend add` stage and no `diffuse` | These are lights/glows; their contribution naturally appears as GI source geometry |
| Alpha mask | `MC_PERFORATED` + `alphaTestRegister` | Already in `MaterialEntry.flags` + `alphaThreshold` |

The roughness conversion maps Doom3's Blinn-Phong exponent to GGX:
- `specExp = 64` → `roughness ≈ 0.17` (polished metal, floor panels)
- `specExp = 16` → `roughness ≈ 0.33` (painted surfaces)
- `specExp = 4`  → `roughness ≈ 0.58` (rough rock, concrete)
- No specular stage → `roughness = 1.0` (fully diffuse)

This is computed once per material at SSBO build time
(`VK_RT_BuildMaterialTable()`) — no per-frame cost.

---

### Approach: Albedo-Weighted Irradiance (Coloured AO)

The simplest one-bounce GI that produces the desired visual result:

1. From each camera-visible pixel, shoot one (or a small number of)
   cosine-weighted hemisphere rays.
2. At the secondary hit: interpolate the UV using `gl_HitBarycentricsEXT`
   + vertex/index buffer SSBOs, sample the albedo texture.
3. Weight the albedo by a simple irradiance estimate at the hit point.
4. Accumulate and write to a `VK_FORMAT_R16G16B16A16_SFLOAT` GI buffer.
5. The interaction shader adds the GI buffer contribution to the diffuse term.

For the irradiance estimate (step 3), two options in ascending cost:

**Option A — Ambient-only (start here):**
`gi_colour = albedo * r_giAmbientScale`

The secondary hit geometry's colour tints the bounce light. No additional
shadow rays. Gives colour bleeding and contact brightening in corners.
Essentially coloured AO. Very cheap: ~1 ray + 1 texture sample per pixel.

**Option B — Single light evaluation at secondary hit:**
For each secondary hit, fire a shadow ray toward the nearest/brightest
light from `viewDef->viewLights`. Multiply by `albedo * lightColour /
lightAttenuation`. Gives directional bounce light (e.g. a red corridor
wall catching the key light and bouncing red into the floor). ~2 rays per
pixel. Does not require a G-buffer.

Start with Option A and promote to B once the infrastructure is stable.

---

### Step 6.1 — Material SSBO + Bindless Textures (Shared Foundation)

This is Step 5.4 promoted and expanded. Implement once; shared by:
- Alpha-test in `shadow_ray.rahit` (deferred from Phase 4)
- GI secondary-hit albedo lookup
- Reflection closest-hit shading (Phase 5.3)

#### C++ Side (`vk_material_table.cpp`, new file)

```cpp
// Called once at scene load / material registration change.
// Iterates all registered idMaterials, fills MaterialEntry SSBO.
void VK_RT_BuildMaterialTable();

// Per-frame update: only needed if dynamic materials changed.
void VK_RT_UpdateMaterialTable();
```

`MaterialEntry` (as defined in Step 5.4, reproduced for reference):
```cpp
struct VkMaterialEntry {
    uint32_t diffuseTexIndex;
    uint32_t normalTexIndex;
    float    roughness;       // pre-computed from specularExponent
    uint32_t flags;
    uint32_t vtxBufInstance;  // index into vertex buffer device-address table
    uint32_t idxBufInstance;  // index into index buffer device-address table
    float    alphaThreshold;
    uint32_t pad;
};
```

The vertex/index buffer device address tables are simple `uint64_t[]` arrays
(one entry per BLAS) that map `gl_InstanceID` → buffer device address.
These are populated when `VK_RT_BuildBLAS` is called and persist in
`vkBLAS_t`. The tables are rebuilt into a SSBO each time the TLAS is rebuilt.

#### Shader Side

All RT shaders that need material data bind:
```glsl
layout(set=2, binding=0) readonly buffer MaterialTable  { MaterialEntry materials[]; };
layout(set=2, binding=1) readonly buffer VtxAddrTable   { uint64_t vtxAddrs[]; };
layout(set=2, binding=2) readonly buffer IdxAddrTable   { uint64_t idxAddrs[]; };
layout(set=2, binding=3) uniform sampler2D textures[];  // bindless array
```

UV interpolation helper (shared across shaders via `#include`):
```glsl
vec2 interpolateUV(uint instanceID, uint primitiveID, vec2 bary) {
    // fetch 3 indices
    uint base = primitiveID * 3;
    uint i0 = fetchIndex(instanceID, base + 0);
    uint i1 = fetchIndex(instanceID, base + 1);
    uint i2 = fetchIndex(instanceID, base + 2);
    // idDrawVert: xyz(12) + st(8) → UV at byte offset 12, stride 60
    vec2 uv0 = fetchVertexUV(instanceID, i0);
    vec2 uv1 = fetchVertexUV(instanceID, i1);
    vec2 uv2 = fetchVertexUV(instanceID, i2);
    vec3 b = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    return b.x * uv0 + b.y * uv1 + b.z * uv2;
}
```

---

### Step 6.2 — GI Ray Generation Shader (`gi_ray.rgen`)

```
neo/renderer/glsl/gi_ray.rgen    — GI ray generation
neo/renderer/glsl/gi_ray.rchit  — GI closest-hit (albedo lookup)
neo/renderer/glsl/gi_ray.rmiss  — GI miss (sky/environment colour)
neo/renderer/Vulkan/vk_gi.cpp   — GI pipeline, dispatch, image lifecycle
```

#### `gi_ray.rgen` Sketch

```glsl
#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set=0, binding=0) uniform accelerationStructureEXT topLevelAS;
layout(set=0, binding=1, rgba16f) uniform image2D giImage;
layout(set=0, binding=2) uniform sampler2D depthSampler;
layout(set=0, binding=3) uniform GIParams {
    mat4  invViewProj;
    vec4  ambientColour;   // fallback irradiance scale (Option A)
    float giRadius;        // max bounce ray distance (default: 512 units)
    int   numSamples;      // rays per pixel (1 for Option A, 1-2 for Option B)
    uint  frameIndex;
    float pad;
};

layout(location = 0) rayPayloadEXT vec3 giPayload;

void main() {
    ivec2 coord = ivec2(gl_LaunchIDEXT.xy);
    vec3 worldPos = reconstructWorldPos(coord, ...);
    vec3 normal   = reconstructNormal(coord, ...);  // depth-derived (same as AO)

    vec3 indirect = vec3(0.0);
    for (int i = 0; i < numSamples; i++) {
        vec3 dir = cosineSampleHemisphere(normal, seed + uint(i));
        giPayload = ambientColour.rgb;  // miss shader returns ambient
        traceRayEXT(topLevelAS,
            gl_RayFlagsNoneEXT,   // need closest-hit for albedo
            0xFF, 0, 0, 0,
            worldPos + normal * 0.5,
            0.01, dir, giRadius, 0);
        indirect += giPayload;
    }
    vec3 gi = indirect / float(numSamples);
    imageStore(giImage, coord, vec4(gi, 1.0));
}
```

#### `gi_ray.rchit` — Albedo Lookup at Secondary Hit

```glsl
void main() {
    MaterialEntry mat = materials[gl_InstanceID];

    // Option A: albedo * ambient scale (no shadow ray)
    vec2 uv = interpolateUV(gl_InstanceID, gl_PrimitiveID, gl_HitBarycentricsEXT);
    vec3 albedo = texture(textures[nonuniformEXT(mat.diffuseTexIndex)], uv).rgb;
    giPayload = albedo * ambientScale;  // ambient scale from GI UBO

    // Option B (upgrade path): fire a shadow ray toward nearest light here,
    // multiply albedo * lightColour * shadowFactor / attenuation.
}
```

The miss shader writes `ambientColour.rgb` directly (sky/open-air areas
bounce ambient light).

#### GI Image

- Format: `VK_FORMAT_R16G16B16A16_SFLOAT` (HDR colour bounce)
- One image per frame-in-flight, same lifecycle as shadow mask
- Add `vkGI_t giBuffer[VK_MAX_FRAMES_IN_FLIGHT]` to `vkRTState_t`

---

### Step 6.3 — Temporal Accumulation for GI

Use the same EMA resolve infrastructure from Step 5.2 (`vk_temporal.cpp`).
GI is noisy at 1 sample/pixel; temporal accumulation across 8–16 frames
brings it to an acceptable level with no additional ray cost per frame.
Camera-cut detection (existing `historyValid` mechanism) also resets GI
history.

---

### Step 6.4 — Integration in `interaction.frag`

Add GI buffer binding (binding 10) and sample it in the ambient/diffuse term:

```glsl
layout(set=0, binding=10) uniform sampler2D u_GIMap;
// in UBO: int u_UseGI; float u_GIStrength;

vec3 gi = (u_UseGI != 0)
    ? texture(u_GIMap, screenUV).rgb * u_GIStrength
    : vec3(0.0);
diffuseLight += gi;
```

`u_GIStrength` (default 1.0, tweakable at runtime) lets the player tune the
intensity of the bounce contribution without recompiling.

---

### Phase 6 New CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtGI` | `0` | Enable one-bounce GI (requires `r_useRayTracing 1`) |
| `r_rtGISamples` | `1` | Bounce rays per pixel (1 for Option A, 1-2 for Option B) |
| `r_rtGIRadius` | `512.0` | Max bounce ray distance in world units |
| `r_rtGIStrength` | `1.0` | Scale applied to GI contribution in interaction shader |
| `r_rtGILightBounce` | `0` | Enable Option B: evaluate nearest light at secondary hit |

---

## Implementation Order

```
Phase 4 fixes (do these first — they make RT shadows actually correct)
  Fix 1: Per-light shadow mask — interleave dispatch with interaction loop
  Fix 2: Remove vkQueueWaitIdle per light — use UBO ring
  Fix 3: BLAS CPU-side vertex dependency
  Fix 4: Batch BLAS uploads

Verify RT shadows are working visually with r_useRayTracing 1 r_rtShadows 1

Phase 5 steps
  5.1: RTAO (ao_ray.rgen + vk_ao.cpp + interaction.frag binding)
  5.2: Temporal denoising for shadows and AO
  5.4: Material SSBO + bindless textures + vertex/index buffer tables
       (promoted: prerequisite for alpha-test fix, GI, and reflections)
  5.3: RT Reflections (reflect_ray.rgen + vk_reflections.cpp)
       (uses 5.4 infrastructure for hit shading)

Phase 6 steps
  6.1: (covered by 5.4 — no extra work if 5.4 is done first)
  6.2: GI ray generation + closest-hit shaders + vk_gi.cpp
  6.3: Temporal accumulation for GI (extend vk_temporal.cpp from 5.2)
  6.4: GI binding in interaction.frag

Deferred alpha-test fix (shadow_ray.rahit texture sampling)
  - Implement once Step 5.4 is done; all infrastructure is already present

Rendering correctness (can be done in parallel with Phase 4 fixes)
  - Multiple blend modes
  - LightScale
  - Texture coord transforms
  - Fog/blend lights
```

---

## File Change Summary

| File | Change |
|------|--------|
| `neo/renderer/Vulkan/vk_shadows.cpp` | Refactor `VK_RT_DispatchShadowRays` → add `VK_RT_DispatchShadowRaysForLight`; add shadow UBO ring; remove `vkQueueWaitIdle`; create depth sampler once at init |
| `neo/renderer/Vulkan/vk_raytracing.h` | Declare `VK_RT_DispatchShadowRaysForLight`; add AO/history/reflection/GI image fields to `vkRTState_t`; add `shadowUBORing` |
| `neo/renderer/Vulkan/vk_backend.cpp` | Move RT dispatch inside per-light loop in `VK_RB_DrawInteractions`: end RP → dispatch → reopen RP per light; call `VK_RT_DispatchAO` and `VK_RT_DispatchGI` before FillDepthBuffer |
| `neo/renderer/Vulkan/vk_accelstruct.cpp` | Fix CPU-verts guard; batch BLAS submissions; expose `geomVtxBuf`/`geomIdxBuf` device addresses for vtx/idx buffer tables |
| `neo/renderer/Vulkan/vk_material_table.cpp` | New: build and update MaterialEntry SSBO + bindless texture array + vtx/idx device-address tables; `VK_RT_BuildMaterialTable()` |
| `neo/renderer/Vulkan/vk_ao.cpp` | New: AO pipeline init, dispatch, image lifecycle |
| `neo/renderer/Vulkan/vk_temporal.cpp` | New: EMA resolve pipeline for shadow + AO + GI history |
| `neo/renderer/Vulkan/vk_reflections.cpp` | New: reflection pipeline init and dispatch |
| `neo/renderer/Vulkan/vk_gi.cpp` | New: GI pipeline init, dispatch, image lifecycle |
| `neo/renderer/glsl/rt_material.glsl` | New: shared include — `MaterialEntry` struct, UV interpolation helper, bindless texture/vtx/idx bindings |
| `neo/renderer/glsl/ao_ray.rgen` | New: AO ray generation shader |
| `neo/renderer/glsl/temporal_resolve.comp` | New: compute EMA blend into history image |
| `neo/renderer/glsl/reflect_ray.rgen` | New: reflection ray generation shader |
| `neo/renderer/glsl/reflect_ray.rchit` | New: reflection closest-hit shader (uses rt_material.glsl) |
| `neo/renderer/glsl/gi_ray.rgen` | New: GI bounce ray generation shader |
| `neo/renderer/glsl/gi_ray.rchit` | New: GI closest-hit — albedo lookup + optional light evaluation (uses rt_material.glsl) |
| `neo/renderer/glsl/gi_ray.rmiss` | New: GI miss — returns ambient sky colour |
| `neo/renderer/glsl/shadow_ray.rahit` | Extend with alpha-test (MC_PERFORATED): UV interpolation + texture sample; `ignoreIntersectionEXT` if alpha < threshold |
| `neo/renderer/glsl/interaction.frag` | Add AO (binding 8), reflection (binding 9), GI (binding 10); multiply AO into diffuse; add reflection to specular; add GI to diffuse |
| `neo/renderer/glsl/interaction.vert` | No change |
| `neo/renderer/Vulkan/vk_pipeline.cpp` | Add AO, reflection, temporal, GI pipeline descriptors; extend interaction desc layout to binding 10; add material table descriptors |
| `neo/CMakeLists.txt` | Add new `.comp`/`.rgen`/`.rchit`/`.rmiss`/`.glsl` to GLSL_SHADER_SOURCES |
