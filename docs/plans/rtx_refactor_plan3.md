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
    float roughness;
    uint  flags;              // alpha-tested, two-sided, etc.
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

This is a substantial infrastructure change and can be deferred until
reflection hit shading is needed. For AO (which only needs 0/1 occlusion),
no material data is required.

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
  5.3: RT Reflections (reflect_ray.rgen + vk_reflections.cpp)
  5.4: TLAS material metadata (SSBO + bindless textures) for reflection hits

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
| `neo/renderer/Vulkan/vk_raytracing.h` | Declare `VK_RT_DispatchShadowRaysForLight`; add AO/history/reflection image fields to `vkRTState_t`; add `shadowUBORing` |
| `neo/renderer/Vulkan/vk_backend.cpp` | Move RT dispatch inside per-light loop in `VK_RB_DrawInteractions`: end RP → dispatch → reopen RP per light; call `VK_RT_DispatchAO` before FillDepthBuffer |
| `neo/renderer/Vulkan/vk_accelstruct.cpp` | Fix CPU-verts guard; batch BLAS submissions |
| `neo/renderer/Vulkan/vk_ao.cpp` | New: AO pipeline init, dispatch, image lifecycle |
| `neo/renderer/Vulkan/vk_temporal.cpp` | New: EMA resolve pipeline for shadow + AO history |
| `neo/renderer/Vulkan/vk_reflections.cpp` | New: reflection pipeline init and dispatch |
| `neo/renderer/glsl/ao_ray.rgen` | New: AO ray generation shader |
| `neo/renderer/glsl/temporal_resolve.comp` | New: compute EMA blend into history image |
| `neo/renderer/glsl/reflect_ray.rgen` | New: reflection ray generation shader |
| `neo/renderer/glsl/reflect_ray.rchit` | New: reflection closest-hit shader (diffuse lookup) |
| `neo/renderer/glsl/interaction.frag` | Add AO and reflection bindings (8, 9); multiply AO into diffuse; add reflection to specular |
| `neo/renderer/glsl/interaction.vert` | No change |
| `neo/renderer/Vulkan/vk_pipeline.cpp` | Add AO, reflection, temporal pipeline descriptors; extend interaction desc layout to binding 9 |
| `neo/CMakeLists.txt` | Add new `.comp`/`.rgen`/`.rchit` to GLSL_SHADER_SOURCES |
