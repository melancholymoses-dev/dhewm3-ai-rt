# RTX Refactor — Phase 4 Optimization, Phase 5 and Phase 6Plan

**Document:** `rtx_refactor_plan4.md`
**Date:** 2026-04-03
**Branch:** `feature/rtao`

This document records what has been done, identifies the current gaps that
must be fixed to make Phase 4 (RT shadows) actually correct, and lays out
the detailed steps for Phase 5 (RTAO + reflections).

---

## Status Summary

| Phase | Description | State |
|-------|-------------|-------|
| 1 | GLSL shaders (ARB → GLSL/SPIR-V) | **Done** |
| 2 | Vulkan rasterization backend | **Done** (core lighting + alpha paths functioning) |
| 3 | Acceleration structures (BLAS/TLAS) | **Done structurally; performance issues remain** |
| 4 | RT shadow pass | **Done for correctness; optimization follow-ups remain** |
| 5 | RTAO + RT reflections | **Not started** |
| 6 | One-bounce global illumination | **Not started — scope expansion, see §Phase 6** |

## Current State Snapshot (2026-04-03)

Brief status for quick return-to-work context:

- Phase 4 correctness is solid. Optimization work has continued since the last snapshot.
- **Profiling finding:** Frame time was bottlenecked by the frontend (RF ~43ms), not the
  backend (BK ~30ms). They run concurrently; total ≈ max(RF, BK). The primary frontend
  cost was CPU shadow volume extrusion happening even when RT shadows were active.
- The following optimizations have been implemented since 2026-03-25:

### Completed Since Last Snapshot

1. **Push descriptors (VK_KHR_push_descriptor)**

2. **Model-keyed BLAS cache** (`vk_accelstruct.cpp`)

3. **Frontend shadow volume extrusion skip** (`Interaction.cpp`)
   - `R_CreateShadowVolume()` is now skipped when `r_useRayTracing && r_rtShadows` are both set.
   - The backend was already discarding stencil volumes when RT is active; this eliminates the
     wasted O(lights × shadow surfaces × silhouette edges) CPU work on the frontend.
   - Expected to bring RF down significantly from the ~43ms baseline.

4. **Batched upload (replace N × vkQueueWaitIdle)** (`vk_instance.cpp`, `vk_buffer.cpp`, `vk_image.cpp`, `vk_backend.cpp`)
   - All `VK_BeginSingleTimeCommands` / `VK_EndSingleTimeCommands` calls accumulate into
     one command buffer per frame instead of submitting and stalling per upload.

5. **RT log trimming** (`vk_accelstruct.cpp`, `vk_backend.cpp`)

## Phase 4 Remaining Work (Brief)

Only items still worth doing before calling this area fully polished:

1. TLAS instance update efficiency:
    - Instance upload/recreate path is still heavier than ideal.
    - Keep reducing per-frame instance-buffer churn and host writes for mostly-static scenes.
2. Optional cleanup:
    - Trim any remaining debug-only cvars/log toggles once confidence is high.

## Phase 4 Fix Checklist (Code Reality)

- Fix 1 (per-light shadow mask design flaw): **Implemented**.
- Fix 2 (remove per-light `vkQueueWaitIdle`): **Implemented**.
- Fix 3 (CPU-vertex dependency gap): **Partially addressed**.
  - Model BLAS path now has fallback access through cache-backed pointers where available.
  - Full GPU-native BLAS input path is still an optimization target.
- Fix 4 (batch/streamline BLAS build work): **Mostly addressed functionally**, further optimization still possible.
- Fix 5 (minor review notes): **Mostly addressed** (depthClamp capability handling, swapchain format refresh, and related robustness fixes are present in current code).

The Stencil shadow fallback path (`VK_RB_DrawShadowSurface`, z-fail / Carmack's Reverse)
was a source of debugging pain but is now structurally complete. It can be left as-is for
the non-RT fallback. **The RT path should be verified independently of the stencil path
by running with `r_useRayTracing 1 r_rtShadows 1`.**

---

## Rendering Correctness Status (Phase 2 carry-overs)

Most previously listed raster correctness gaps are now implemented. Current quick status:

| # | Item | Status | Note |
|---|------|--------|------|
| 1 | Multiple blend modes | Fixed | Shader-pass pipelines are selected from `drawStateBits` blend factors (`GLS_SRCBLEND_BITS`/`GLS_DSTBLEND_BITS`) |
| 2 | Depth prepass parity | Fixed | Current path uses depth prepass + interaction depth equality behavior consistent with Doom 3 flow |
| 3 | Texture coordinate transforms | Fixed | Stage texture matrices are applied for shader/depth-clip paths |
| 4 | LightScale / brightness parity | Fixed | Brightness mismatch was traced to formatting/read-in behavior in prior Vulkan handling; current path applies the expected scale flow |
| 5 | Two-sided cull selection | Fixed | Material cull mode (`CT_TWO_SIDED`, `CT_BACK_SIDED`) is selected dynamically per draw |
| 6 | Fog/blend lights | Fixed | `VK_RB_FogAllLights` + fog and blend-light passes are present in frame execution |

Residual risk is now mostly regression risk across maps/content, not known missing feature blocks.

---

## Phase 4 — Fixes Required

Short version (historical details removed; see your archived copy for full notes):

| Fix | Status | Current Note |
|-----|--------|--------------|
| Fix 1: Per-light RT shadow mask dispatch | Done | Interleaved dispatch in per-light interaction loop (`VK_RT_DispatchShadowRaysForLight`) |
| Fix 2: Remove per-light queue idle | Done | Uses shadow UBO ring allocation + persistent depth sampler; no per-light idle stall |
| Fix 3: CPU-only BLAS dependency | Partial | Multi-surface model BLAS path improved; full GPU-native BLAS input path still pending optimization |
| Fix 4: BLAS build/upload efficiency | Partial | Functionally improved, but still more optimization headroom (especially CPU copy/staging reduction) |
| Fix 5: Minor review-note hardening | Mostly done | depthClamp gating and swapchain format refresh are present; remaining items are polish-level |

Remaining blocker check: no known Phase 4 correctness blocker remains; remaining work is primarily performance/polish.

---

## Optimization Workstream (RT Performance)

### Goal

Minimize avoidable CPU work and CPU↔GPU data movement in the RT path.
RT shadows (and later AO/reflections/GI) are already heavy; we should not add
per-frame CPU staging cost on top.

### Current State (Verified)

1. Per-light queue-idle stalls in RT shadow dispatch have been removed from the active path.
2. Per-light shadow dispatch now uses ring-suballocated UBO data and persistent samplers.
3. BLAS build now prefers zero-copy geometry input from `ambientCache`/`indexCache` Vulkan buffers, with CPU-copy fallback retained.
4. TLAS rebuild and per-light dispatch sequencing are functionally correct; TLAS buffer reuse and static/dynamic instance upload split are implemented in first-pass form.

### Optimization 1: GPU-Side BLAS Geometry Inputs (Vertex/Index)

#### Current Issue

BLAS construction currently depends on CPU-side geometry pointers for many paths,
and can stage/copy mesh data from CPU memory into temporary AS build input buffers.
This causes extra CPU time, memory bandwidth, and synchronization pressure.

#### Target Design

Build BLAS directly from persistent GPU vertex/index buffers already owned by the
renderer cache (`ambientCache` / index cache path), with device addresses used as
AS build inputs.

Required buffer usage flags on geometry buffers:

```
VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
```

Implementation notes:

1. Keep static BLAS persistent across frames and rebuild/refit only for dynamic entities.
2. Remove per-build CPU mesh staging where GPU buffers are available.
3. Restrict CPU-side work to instance list generation + transform updates for TLAS.
4. Preserve a CPU fallback path only for debug/legacy corner cases.

#### Why this matters

This is likely to provide a meaningful performance win because it reduces:

1. CPU memcpy/staging overhead for large meshes.
2. CPU-side cache pressure and allocation churn.
3. Synchronization points needed to hand staged data to GPU builds.

In practice this improves frame-time stability and scalability as light count and
RT effects increase.

#### Status

Implemented (with fallback). BLAS now consumes persistent GPU cache buffers directly
when available, and falls back to CPU-copy AS build inputs only when required.

Validation support added: per-frame lightweight counters/log summary for zero-copy
adoption (`VK RT BLAS SRC` in RT log output).

### Optimization 2: Keep TLAS Updates Lightweight

Even with GPU-side BLAS geometry, TLAS still needs per-frame instance updates.
Keep this path lean:

1. Update only visible/casting instances.
2. Avoid rebuilding unchanged static BLAS.
3. Batch TLAS instance writes/builds and avoid queue-idle style sync points.

#### Status

Implemented (first-pass). TLAS behavior is stable and synchronized correctly, and
first-pass churn/write reduction is implemented:

1. Reuse instance buffer unless required size grows.
2. Reuse TLAS AS storage buffer/handle unless required size grows.
3. Reuse scratch buffer unless required size grows.

Remaining work: improve static-change detection granularity and verify upload-byte
reduction across representative gameplay scenes.

### Optimization 3: Static/Dynamic TLAS Update Efficiency

#### Goal

Reduce per-frame TLAS rebuild cost by splitting instance handling into static and
dynamic buckets, then minimizing update work for unchanged static content.

#### Design

1. Track persistent static instances separately from dynamic instances.
2. Keep a cached static-instance block in the TLAS instance buffer and only rewrite
    it when static visibility/caster membership changes.
3. Rewrite only the dynamic-instance region each frame (moving entities, animated
    objects, and transform updates).
4. Keep deterministic instance ordering (static block first, dynamic block second)
    to simplify debug and reduce accidental churn in instance IDs/masks.
5. Continue doing a single TLAS build per frame, but reduce CPU writes and host-to-device
    transfer volume by shrinking the updated instance range.

#### Implementation Notes

1. First pass implemented: static-instance signature cache + static/dynamic split,
  with static block rewritten only when signature changes.
2. Model-keyed BLAS cache implemented: static entities sharing the same `hModel*` reuse
  one BLAS, keeping device addresses stable and preventing signature churn from entity
  pointer recycling (e.g. doors destroyed/recreated each frame).
3. Next pass: add richer per-instance metadata cache (entity handle + BLAS handle +
  last transform hash + visibility/caster bits) so unchanged static entries are
  detected with finer granularity.
4. Build per-frame dynamic list from visible interactions; avoid scanning unrelated entities.
5. Keep static BLAS handles persistent; dynamic BLAS update policy remains independent.

#### Validation

1. Compare baseline vs optimized logs in scenes with mostly static geometry and
    a few moving actors.
2. Confirm TLAS instance upload bytes/frame drop significantly in those scenes.
3. Confirm no shadow popping/regressions when static visibility changes (doors opening,
    streamed areas, map transitions).
4. Verify mirrored/subview rendering still receives correct TLAS contents.

#### Priority

Medium. Model BLAS cache already eliminates the primary static churn source. Remaining
gain is from reducing per-frame instance buffer writes for truly static scenes.

#### Status

Partially implemented. Model-keyed BLAS cache done. Finer per-instance change detection
not yet implemented.

### Enhancement: Per-Light-Type RT Softness Controls

#### Goal

Allow separate shadow softness tuning for point lights vs projected lights,
so lights like `lights/fanlightgrate` can be tuned independently from omni lights
without compromising either look.

#### Problem

The current RT shadow path uses a single global softness/sample policy.
Projected lights and point lights have different geometric behavior and usually
need different sample counts and effective soft radius to avoid either noisy
or over-blurry shadows.

#### Implementation Outline

1. Add separate cvars for point/projection sample count and soft radius.
2. In `VK_RT_DispatchShadowRaysForLight`, classify by `renderLight_t::pointLight`
    and compute per-light effective RT softness parameters.
3. Extend the shadow UBO payload to pass these effective per-light values.
4. Update `shadow_ray.rgen` to use per-light effective values instead of a
    single global path.
5. Keep safe fallback behavior so existing scenes preserve current visuals when
    new cvars are left at defaults.

#### Validation

1. Test mixed scenes with at least one projected light and one point light.
2. Verify projected-light penumbra can be softened without over-softening point lights.
3. Confirm no regressions in hard-shadow mode (`samples=1`).
4. Confirm no descriptor/UBO layout mismatch across CPU and shader builds.

#### Priority

Medium. This is a quality and control enhancement, not a correctness blocker,
but it improves artistic tuning and reduces pressure to use one-size-fits-all settings.

#### Status

Planned. Current runtime softness tuning is still global.



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
neo/renderer/glsl/ao_ray.rgen        — ray generation shader
neo/renderer/glsl/rt_indirect.glsl   — shared include: cosine hemisphere sampling,
                                        world position reconstruction, seed helpers.
                                        Included by both ao_ray.rgen and gi_ray.rgen
                                        to avoid duplicated code when GI is added.
neo/renderer/Vulkan/vk_ao.cpp        — AO pipeline, dispatch, and AO buffer lifecycle
```

**Design note — shared code with GI:** `ao_ray.rgen` and the later `gi_ray.rgen`
are nearly identical: both shoot cosine-weighted hemisphere rays from the same
reconstructed world position using the same seed/noise helpers. Factor the common
code into `rt_indirect.glsl` from the start. The AO rgen shader then becomes a
thin wrapper that calls `traceRayEXT` with `gl_RayFlagsTerminateOnFirstHitEXT`
and writes an `R8` scalar result. The GI upgrade later replaces the hit logic and
image format (`RGBA16F`) without rewriting the sampling infrastructure.

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

**Known artifact — edge halos:** Depth-derived normals break down at depth
discontinuities (object silhouette edges), where the finite-difference cross-
product spans two surfaces at different depths. This produces incorrect
hemisphere orientations at edges and causes visible AO halos around objects.
The effect is mild at low sample counts but becomes more obvious with temporal
accumulation, which locks in the wrong normal value each frame.

**Upgrade path:** A dedicated G-buffer normal render target (option 1) cleanly
fixes this and also benefits GI secondary-hit sampling. It is worth adding once
basic AO is stable and the artifacts become the primary quality complaint.

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

**Note on Doom 3's ambient term:** Doom 3's ambient is intentionally nearly zero
— the game is designed to be dark. Applying AO only to a near-black ambient
term would be nearly invisible. The `diffuseLight *= ao` integration above is
therefore the correct and important choice: it produces contact darkening
(corners, surface-object contact, crevices) that is visible regardless of
ambient brightness. This is the primary visual payoff of RTAO in Doom 3.

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

#### EMA Ghosting During Camera Movement

**Known limitation:** Camera-cut detection handles hard cuts but not continuous
movement. During any pan, tilt, or strafe, the EMA blends the current frame's
pixel against history from a *different world position*, causing:

- Ghosting/streaking on moving objects (shadows/AO "drag" behind them).
- Smearing of AO/GI at screen edges when panning.

The `alpha` value directly controls the noise/ghosting tradeoff:
- Low alpha (0.05–0.1): smooth but heavy ghosting during movement.
- High alpha (0.2–0.3): less ghosting but noisier result.

**Acceptable for a first pass.** The effect is tolerable at Doom 3 camera
speeds. Start with alpha ≈ 0.1 and tune from play.

**Upgrade path:** A per-pixel velocity/motion-vector buffer would allow proper
reprojection — reproject each pixel's world position into the previous frame's
screen space and sample history there. This eliminates ghosting at the cost
of one additional G-buffer pass (screen-space velocity `RG16F`). Add this
alongside the G-buffer normal pass if ghosting becomes the primary complaint.

#### New Files

```
neo/renderer/glsl/temporal_resolve.comp  — compute shader for EMA blend
neo/renderer/Vulkan/vk_temporal.cpp      — pipeline + dispatch for resolve
```

The resolve pass must run **after** the RT dispatch and **before** the
render pass opens (so the interaction shader sees the blended result).

---

### Step 5.2b — Atrous Spatial Filter (Post-EMA Quality Pass)

**Status:** Planned. Implement after EMA is stable, before GI.

EMA-only denoising reduces temporal noise but cannot eliminate spatial grain
within a single frame. At 4 rays/pixel the AO result is visibly noisy between
camera movements. An Atrous wavelet filter adds a spatial denoise pass on top
of the EMA history image, dramatically improving per-frame quality with no
additional ray cost.

#### Why Before Phase 6

GI at 1 sample/pixel is substantially noisier than AO at 4 samples/pixel.
EMA without any spatial filtering will produce unacceptable GI quality.
Adding Atrous here, while the infrastructure is simple (depth + depth-derived
normals), avoids retrofitting a spatial pass after GI is integrated.

#### Design

Run a series of 4–5 compute passes after the EMA resolve, each doubling the
filter kernel step width (`1, 2, 4, 8, 16` pixels — the "à trous" pattern):

```glsl
// Edge-stopping weights (preserve detail at depth/normal discontinuities)
float wDepth  = exp(-abs(depthCenter - depthSample) / sigmaDepth);
float wNormal = pow(max(0.0, dot(normalCenter, normalSample)), sigmaNormal);
float w = wDepth * wNormal;
```

Inputs required:
- Depth buffer (already available)
- World-space normals (depth-derived, same as AO/GI rgen path)
- No motion vectors required

#### New File

```
neo/renderer/glsl/atrous_filter.comp   — Atrous wavelet filter compute shader
```

The existing `vk_temporal.cpp` can host the Atrous dispatch passes, or they
can be a separate second section of the same file.

#### New CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtAtrousIterations` | `0` | Atrous filter passes (0 = disabled, 4–5 = typical) |
| `r_rtAtrousSigmaDepth` | `1.0` | Depth edge-stop sensitivity |
| `r_rtAtrousSigmaNormal` | `128.0` | Normal edge-stop power |

---

### Future Extension — NRD (NVIDIA RayTracingDenoiser)

**Status:** Post-Phase-6. Depends on G-buffer prerequisites.

NVIDIA's [RayTracingDenoiser (NRD)](https://github.com/NVIDIAGameWorks/RayTracingDenoiser)
(MIT license) is a production-grade spatial-temporal denoiser used in shipped
titles. It produces superior quality to EMA + Atrous, but requires infrastructure
that is not yet in place:

| Prerequisite | Status |
|---|---|
| Screen-space motion/velocity buffer (`RG16F`) | Not yet — requires a G-buffer pass |
| Geometric normals as a proper G-buffer target | Not yet — depth-derived only |
| Hit-distance packed into AO/shadow signal | Minor shader change, low cost |
| CMake subproject integration | Not yet |

Without motion vectors, NRD's REBLUR temporal component ghosts as badly as
naive EMA and offers no meaningful advantage. The right time to evaluate NRD
integration is after the G-buffer normal pass and motion-vector pass are added
(those are already flagged as upgrade paths in Steps 5.1 and 5.2).

At that point NRD can replace both the EMA pass and the Atrous passes entirely
with a single SDK dispatch call, and the result is significantly better on
fast-moving geometry and camera panning.

---

### Step 5.3 — RT Reflections

#### Goal

For specular/metallic surfaces, shoot a reflection ray from the camera hit
point along the specular reflection direction and sample the radiance of
whatever the ray hits. Blend the reflected colour into the specular term.

This is optional and expensive. Only enable for highly-specular surfaces
(metal floors, mirrors, polished panels).

**Mirror replacement:** Once RT reflections are working, the rasterized
mirror subview path (`R_MirrorViewBySurface`, oblique clip plane, dual
`R_RenderView` passes) can be retired entirely. Mirrors become a degenerate
case of perfect specular reflection (roughness = 0). The closest-hit shader
checks for the `mirror` material flag, reflects the ray, and traces again —
no clip planes, no subview, no second full-scene render. This eliminates
roughly 1.5x per-frame cost when a mirror is visible.

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

### Step 5.4b — Basic Glass Reflections (Flat Fresnel)

#### Goal

Translucent surfaces (windows, viewports, bottles) currently let reflection
rays pass straight through them because `MC_TRANSLUCENT` geometry does not
write to the depth buffer — `rgen` reconstructs world position from the
opaque surface *behind* the glass and fires the reflection ray from there.

This step gives glass a cheap fixed 4 % reflectance (F0 for real glass is
≈ 0.04) without angle dependency and without refraction.  The result is a
visible specular highlight on glass panels at minimal extra cost — one
additional `traceRayEXT` call only when a reflection ray hits glass, which
is rare in most Doom 3 scenes.

#### Why flat 4 % is enough for now

- Real Fresnel ranges from ≈ 4 % at normal incidence to 100 % at grazing
  angle.  Doom 3 glass is mostly seen at moderate angles where the
  difference is small.
- No refraction: the transmitted ray continues in the same direction
  (straight-through thin-glass approximation), avoiding snell-law bending
  which would require knowing the IOR and thickness.
- Temporal filtering (EMA already in place) smooths the slight error.

#### Implementation

**1. New material flag and field in `VkMaterialEntry`**

```cpp
#define VK_MAT_FLAG_GLASS 0x04u   // MC_TRANSLUCENT — thin glass, F0 = 0.04
```

No new fields needed — `alphaThreshold` is unused for glass and can carry
the transmittance (1.0 - reflectance = 0.96) if needed later.

**2. Populate flag in `VK_RT_MakeMaterialEntry`**

```cpp
if (shader->Coverage() == MC_TRANSLUCENT)
    entry.flags |= VK_MAT_FLAG_GLASS;
```

**3. Mark glass BLAS geometry as non-opaque**

In `VK_RT_BuildBLAS` / `VK_RT_BuildBLASForModel`, translucent surfaces
currently receive `VK_GEOMETRY_OPAQUE_BIT_KHR`.  Remove that flag when
`isPerforated` is false but the material is translucent, so `rahit` is
invoked for glass hits.

The existing `isPerforated` parameter becomes a tri-state or a second bool
`isTranslucent` is added alongside it.

**4. Extend the reflection payload**

```glsl
// reflect_ray.rgen / reflect_ray.rchit — shared payload
layout(location = 0) rayPayloadEXT struct ReflPayload {
    vec3  colour;         // accumulated colour for this segment
    float transmittance;  // fraction of weight to carry through (0 = stop)
    vec3  nextOrigin;     // origin for the next ray (glass exit point)
    vec3  nextDir;        // direction for the next ray (straight-through)
} reflPayload;
```

**5. `reflect_ray.rchit` — glass branch**

```glsl
MaterialEntry mat = materials[uint(gl_InstanceCustomIndexEXT)];

if ((mat.flags & MAT_FLAG_GLASS) != 0u) {
    const float F0           = 0.04;
    const float transmit     = 1.0 - F0;
    vec4 diffuse             = rt_SampleDiffuse(...);  // tint the reflection
    reflPayload.colour        = F0 * diffuse.rgb;
    reflPayload.transmittance = transmit;
    reflPayload.nextOrigin    = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT
                                + gl_WorldRayDirectionEXT * 0.01; // bias past surface
    reflPayload.nextDir       = gl_WorldRayDirectionEXT;           // straight-through
    return;
}

// Opaque path — same as before
vec4 diffuse  = rt_SampleDiffuse(...);
reflPayload.colour        = diffuse.rgb;
reflPayload.transmittance = 0.0;   // stop
```

**6. `reflect_ray.rgen` — bounce loop**

Replace the single `traceRayEXT` call with a short loop (max 2 iterations
covers double-pane windows):

```glsl
vec3  accum   = vec3(0.0);
float weight  = 1.0;
vec3  origin  = worldPos + normal * 0.5;
vec3  dir     = reflDir;

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

**7. `reflect_ray.rahit` — pass through glass**

Glass is non-opaque so `rahit` is called.  Glass hits should be accepted
(let `rchit` handle the Fresnel split), not discarded:

```glsl
MaterialEntry mat = materials[uint(gl_InstanceCustomIndexEXT)];

if ((mat.flags & MAT_FLAG_ALPHA_TESTED) != 0u) {
    // existing alpha-discard path
} else if ((mat.flags & MAT_FLAG_GLASS) != 0u) {
    // accept the intersection — rchit handles the Fresnel split
    // (do nothing; rahit returning without ignoreIntersectionEXT accepts)
} else {
    terminateRayEXT;
}
```

#### Files changed

| File | Change |
|------|--------|
| `vk_raytracing.h` | `VK_MAT_FLAG_GLASS 0x04u` |
| `vk_material_table.cpp` | Set `MAT_FLAG_GLASS` in `VK_RT_MakeMaterialEntry` |
| `vk_accelstruct.cpp` | Remove opaque flag for translucent geometry |
| `rt_material.glsl` | `MAT_FLAG_GLASS` constant |
| `reflect_ray.rgen` | Replace single `traceRayEXT` with 2-iteration bounce loop |
| `reflect_ray.rchit` | Glass branch: write F0 colour + set transmittance/nextDir |
| `reflect_ray.rahit` | Accept glass hits (fall through to rchit) |

#### Known limitations

- No refraction (straight-through approximation).  Acceptable for Doom 3's
  flat window panels; would look wrong for curved or thick glass.
- Angle-independent reflectance.  At grazing angles real glass is much more
  reflective; this can be added later by computing Schlick from
  `dot(N, V)` in `rchit` at zero extra infrastructure cost.
- One-sided: only the front face of the glass gets the Fresnel split.  The
  back face is an opaque hit and terminates the ray.  For thin single-pane
  glass this is imperceptible.
- Multi-surface BLAS models use `geomVertAddrs[0]` only — same limitation
  as Step 5.4; glass panes are typically single-surface world brushes so
  this is not a practical issue.

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
| Roughness | `specularExponent` on the specular stage | Map to GGX roughness: `roughness = sqrt(2.0 / (specExp + 2.0))`. Store the result in `MaterialEntry.roughness` at SSBO build time. **Default to `1.0` (fully diffuse) when no specular stage is present** — many Doom 3 materials omit specular entirely. |
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

Detailed execution order with status:

1. Phase 4 correctness baseline (completed)
     - Per-light shadow mask dispatch interleaved with per-light interactions.
     - Removed per-light queue-idle synchronization from RT shadow path.
     - Stabilized player/viewmodel shadow behavior and alpha-tested depth-prepass parity.

2. Phase 4 optimization pass (active)
     - 2.1 GPU-native BLAS input path (highest impact):
         - Consume ambient/index cache GPU buffers directly for AS build input.
         - Keep CPU fallback for edge/debug cases.
     - 2.2 TLAS instance upload efficiency:
     - Reuse TLAS/instance/scratch allocations and avoid unnecessary full rewrites.
     - 2.3 Static/dynamic TLAS split:
     - Static block caching + dynamic-only rewrites are implemented (first pass).

3. Validation gate after Phase 4 optimization changes
     - Validate with `r_useRayTracing 1 r_rtShadows 1` across mixed-light maps.
     - Compare frame-time stability in static-heavy and dynamic-heavy scenes.

4. Phase 5 enablement (after optimization baseline is stable)
     - 4.1 RTAO pipeline (`5.1`) — no material data required; can ship early as a quick win.
     - 4.2 Temporal denoise/accumulation for shadow + AO (`5.2`).
     - 4.3 Material SSBO + bindless texture/geometry metadata (`5.4` promoted prerequisite).
     - 4.4 RT reflections (`5.3`) using material metadata path.

     **Note on ordering:** RTAO (4.1) needs zero material infrastructure — it
     only needs a binary hit/miss result. Moving it before the material SSBO work
     gets visible results faster and validates the AO pipeline (descriptor layout,
     image lifecycle, temporal pass) in isolation before adding material complexity.

5. Phase 6 expansion (post-Phase 5 baseline)
     - GI raygen/hit path, temporal accumulation, and interaction integration.

6. Deferred alpha-test in RT any-hit (carried from earlier plan)
     - Implement once material metadata path is online.

7. Rendering correctness carry-overs
     - Now treated as regression checks (not missing feature blocks):
         blend modes, depth-prepass parity, texture transforms, light scale, cull mode,
         fog/blend-light passes.

---

## File Change Summary

Historical + current updates (implemented now) and planned extensions:

| Status | File | Change |
|--------|------|--------|
| Implemented | `neo/renderer/Vulkan/vk_backend.cpp` | Per-light RT dispatch/resume integration in interactions; depth-prepass parity updates for perforated materials; shader-pass blend handling and dynamic cull behavior; fog/blend-light pass execution; light-scale handoff updates |
| Implemented | `neo/renderer/Vulkan/vk_shadows.cpp` | `VK_RT_DispatchShadowRaysForLight` active path; per-light RT dispatch descriptors/UBO ring use; persistent depth-sampler usage |
| Implemented | `neo/renderer/Vulkan/vk_raytracing.h` | Declares per-light dispatch entry point; tracks per-frame shadow descriptor update state and samplers |
| Implemented | `neo/renderer/Vulkan/vk_common.h` | Added `renderPassResume` use for post-dispatch resume and debug-only interaction pipeline variant support |
| Implemented | `neo/renderer/Vulkan/vk_pipeline.cpp` | Blend-state mapping from Doom 3 draw-state bits; depth/depth-clip pipelines; fog/blend-light pipeline support; dynamic cull mode path |
| Implemented | `neo/renderer/Vulkan/vk_accelstruct.cpp` | Multi-surface model BLAS build path; GPU-buffer-preferred BLAS geometry input with CPU fallback; lightweight BLAS source logging; TLAS rebuild flow with buffer reuse and first-pass static/dynamic instance upload split |
| Implemented | `neo/renderer/Vulkan/vk_instance.cpp` | depthClamp feature handling updated to capability-gated behavior |
| Implemented | `neo/renderer/Vulkan/vk_swapchain.cpp` | swapchain format refresh updated in recreate path |
| Implemented | `neo/renderer/glsl/depth_clip.frag` | Depth prepass alpha clip parity update using stage alpha scale + threshold wiring |
| Implemented | `neo/renderer/glsl/interaction.frag` | Uses RT shadow mask modulation in active interaction path |
| Planned | `neo/renderer/Vulkan/vk_material_table.cpp` | Material SSBO + bindless texture/geometry metadata infrastructure |
| Planned | `neo/renderer/Vulkan/vk_ao.cpp` | RTAO pipeline lifecycle + dispatch |
| Planned | `neo/renderer/Vulkan/vk_temporal.cpp` | Temporal denoise/accumulation for shadow/AO/GI buffers |
| Planned | `neo/renderer/Vulkan/vk_reflections.cpp` | RT reflections pipeline + dispatch |
| Planned | `neo/renderer/Vulkan/vk_gi.cpp` | One-bounce GI pipeline + dispatch |
| Planned | `neo/renderer/glsl/rt_material.glsl` | Shared RT material/UV helper include for any-hit/closest-hit paths |
| Planned | `neo/renderer/glsl/rt_indirect.glsl` | Shared include: cosine hemisphere sampling + world-pos reconstruction; used by both AO and GI rgen shaders |
| Planned | `neo/renderer/glsl/ao_ray.rgen` | AO ray generation shader |
| Planned | `neo/renderer/glsl/temporal_resolve.comp` | Temporal resolve compute shader |
| Planned | `neo/renderer/glsl/reflect_ray.rgen` | Reflection ray generation shader |
| Planned | `neo/renderer/glsl/reflect_ray.rchit` | Reflection closest-hit shader |
| Planned | `neo/renderer/glsl/gi_ray.rgen` | GI ray generation shader |
| Planned | `neo/renderer/glsl/gi_ray.rchit` | GI closest-hit shader |
| Planned | `neo/renderer/glsl/gi_ray.rmiss` | GI miss shader |
| Planned | `neo/CMakeLists.txt` | Shader source list updates for new RT stages as they land |

---

## Rendering Bug Investigations

### Bug A: Mirrors/Portals Render as Black

#### Symptom

Mirror surfaces (e.g. the bathroom mirror in Mars City) render as solid black rectangles.
Portal/remote-render textures similarly show nothing.

#### Root Cause Analysis

The Vulkan backend explicitly skips mirror/portal dynamic textures at
`vk_backend.cpp:962`:

```cpp
else {
    // Portal/mirror/other dynamic texture — not yet supported, skip.
    continue;
}
```

When `VK_RB_DrawShaderPasses` encounters a shader stage with
`dynamic == DI_MIRROR_RENDER` (or `DI_REMOTE_RENDER` / `DI_XRAY_RENDER`),
it falls through to this branch and skips the entire draw. The surface
renders with whatever was previously bound (typically black/nothing).

The GL path works because:

1. **Frontend:** `R_GenerateSubViews()` (called from `tr_main.cpp:1234`) scans all
   draw surfaces for subview materials and dispatches to `R_MirrorRender()` /
   `R_RemoteRender()` / `R_XrayRender()` in `tr_subview.cpp`.
2. **Mirror camera:** `R_MirrorViewBySurface()` creates a reflected `viewDef_t`
   (mirrored origin/axis, clip plane, `isMirror=true` for culling flip).
3. **Render + capture:** The mirrored view is rendered via `R_RenderView()`,
   then `CaptureRenderToImage()` copies the framebuffer into `scratchImage`.
4. **Main pass:** The mirror surface samples `scratchImage` via `TG_SCREEN` texgen.

The frontend code (`tr_subview.cpp`) is **shared** between GL and Vulkan, so the
subview generation pipeline *should* run. The issue is that either:
- The subview render never executes on the Vulkan backend (the `RC_COPY_RENDER`
  command never fires or `VK_RB_CopyRender` fails silently), or
- The resulting texture is never bound when the mirror surface draws (the skip
  at line 962 prevents it).

#### Investigation Plan

1. **Add logging to confirm subview dispatch:**
   - In `R_GenerateSubViews()` / `R_GenerateSurfaceSubview()`, log when a
     mirror surface is detected and when `R_MirrorRender()` is called.
   - In `VK_RB_CopyRender()`, log entry with image name, dimensions, and
     whether `s_frameActive` / `s_frameCmdBuf` are valid.
   - This tells us whether the frontend is generating subviews and whether
     the copy command reaches the Vulkan backend.

2. **Confirm `CropRenderSize` / `UnCrop` work in Vulkan:**
   - GL uses `CropRenderSize` to render the mirror view at reduced resolution.
   - Verify that Vulkan's render pass / framebuffer handles the cropped viewport
     correctly (or whether it needs a secondary render pass / offscreen target).

3. **Remove the skip at line 962:**
   - Replace the `continue` with code that binds `scratchImage` (for mirrors)
     or `scratchImage2` (for xray) as the texture for that stage.
   - The image should already contain the captured mirror view if step 1 confirms
     the subview rendered.

4. **Validate face culling flip:**
   - `vk_backend.cpp:1556` checks `backEnd.viewDef->isMirror` — confirm this
     flag is set during the mirror subview render and that the Vulkan pipeline
     flips winding order correctly.

5. **Test with the bathroom mirror in Mars City (`maps/game/mc_underground`):**
   - Stand in front of the mirror and confirm the reflection appears.
   - Check for correct orientation (not upside-down or mirrored incorrectly).
   - Check that the crosshair/HUD is not rendered in the reflection.

6. **Reference vkDOOM3 implementation:**
   - `../vkDOOM3/neo/renderer/tr_frontend_subview.cpp` has a working Vulkan
     mirror path — compare its dispatch and copy logic if our approach stalls.

#### Key Files

| File | Relevance |
|------|-----------|
| `neo/renderer/tr_subview.cpp` | Shared frontend: mirror/portal subview generation |
| `neo/renderer/Vulkan/vk_backend.cpp:962` | **Smoking gun** — skips mirror stages |
| `neo/renderer/Vulkan/vk_backend.cpp:3036` | `VK_RB_CopyRender` — framebuffer blit |
| `neo/renderer/Vulkan/vk_backend.cpp:907-959` | `TG_SCREEN` texgen binding |
| `neo/renderer/RenderSystem.cpp:994` | `CaptureRenderToImage` command creation |
| `neo/renderer/Image_init.cpp:2237` | `scratchImage` / `currentRenderImage` creation |
| `neo/renderer/Material.cpp:1349` | `mirrorRenderMap` material keyword parsing |
| `neo/renderer/tr_local.h:386` | `viewDef_t` — `isMirror`, `isSubview` flags |

#### Priority

Medium-high. Mirrors are visible and expected in early levels. This is a
user-facing visual gap that should be addressed before Phase 5.

---

### Bug B: Texture Aliasing / Z-Fighting on Slight Camera Tilt

#### Symptom

When tilting the camera by very small angles, certain textures (visible in the
Monorail Station / Central Access corridor screenshots) show rapid flickering
or shimmer — as if competing surfaces are fighting for the same depth, or
mipmap levels are oscillating rapidly.

#### Possible Causes

1. **Zero slope factor in depth bias:**
   `vk_backend.cpp` calls `vkCmdSetDepthBias(cmd, biasConstant, 0.0f, r_offsetFactor.GetFloat())`
   where the second argument (clamp) is 0 and the third is the slope factor
   from `r_offsetFactor` (default 0). The GL path uses
   `glPolygonOffset(r_offsetFactor, r_offsetUnits * polygonOffset)` where the
   first arg is the slope factor. With no slope-dependent bias, co-planar
   decals at steep viewing angles get insufficient offset and z-fight.

2. **Mipmap selection instability (no anisotropic filtering):**
   `vk_image.cpp` sets `anisotropyEnable = VK_FALSE` with `mipLodBias = 0.0f`.
   At oblique angles the texture footprint becomes highly anisotropic, causing
   the mip selector to oscillate between levels frame-to-frame, producing
   visible shimmer. Anisotropic filtering smooths this significantly.

3. **Depth format precision:**
   `vk_swapchain.cpp:78` prefers `D32_SFLOAT_S8_UINT` but falls back to
   `D24_UNORM_S8_UINT`. If 24-bit is selected, depth precision at medium
   distances is marginal for the infinite-far-plane projection Doom 3 uses.

4. **Projection matrix precision distribution:**
   `tr_main.cpp:973` uses `projectionMatrix[10] = -0.999f` for the infinite
   far-plane trick. This concentrates precision near the near plane and leaves
   mid-to-far distances with less depth resolution, exacerbating z-fighting on
   co-planar surfaces.

#### Investigation Plan

1. **Identify the specific surfaces:**
   - Use `r_showSurfaceInfo 1` or similar debug cvar to identify the material
     names on the flickering surfaces.
   - Determine whether they are decals, overlays, or genuinely co-planar geometry.

2. **Log depth format at startup:**
   - In `VK_CreateDepthBuffer` / format selection, log which depth format was
     actually selected. If it's `D24_UNORM_S8_UINT`, that's a contributing factor.

3. **Test slope-based depth bias:**
   - Set `r_offsetFactor` to a nonzero value at runtime (e.g. `r_offsetFactor 2`)
     and check whether decal z-fighting improves.
   - If it helps, verify the Vulkan `vkCmdSetDepthBias` argument mapping matches
     GL's `glPolygonOffset` semantics (GL: `factor, units`; VK: `constant, clamp, slope`).
   - The current Vulkan call may have `constant` and `slope` swapped relative to
     GL intent — verify and fix if needed.

4. **Test anisotropic filtering:**
   - Set `r_useAnisotropicFiltering 1` or `image_anisotropy 8` and check
     whether the mip-shimmer on oblique surfaces resolves.
   - If it does, consider enabling anisotropic filtering by default for
     non-nearest samplers.

5. **Test mipmap LOD bias:**
   - Try a small negative `mipLodBias` (e.g. -0.25) to bias toward sharper
     mip levels. This can reduce shimmer on surfaces near mip boundaries.

6. **Compare with GL renderer:**
   - Run the same scene with the GL renderer and note whether the same
     surfaces show aliasing. If GL is clean, diff the depth/filtering setup.

#### Key Files

| File | Relevance |
|------|-----------|
| `neo/renderer/Vulkan/vk_backend.cpp:598-815` | Depth bias application |
| `neo/renderer/Vulkan/vk_pipeline.cpp:248-249` | Pipeline depth bias enable |
| `neo/renderer/Vulkan/vk_image.cpp:135-144, 675` | Sampler/mipmap/anisotropy config |
| `neo/renderer/Vulkan/vk_swapchain.cpp:78-92` | Depth format selection |
| `neo/renderer/tr_main.cpp:973-1041` | Projection matrix setup |
| `neo/renderer/RenderSystem_init.cpp:232-233` | `r_offsetUnits` / `r_offsetFactor` defaults |

#### Priority

Medium. This is a visual quality issue, not a correctness blocker. The fix
is likely a combination of correct depth-bias argument mapping and enabling
anisotropic filtering, both of which are low-risk changes.

---

## Changelog

### 2026-03-25

- Added current-state snapshot marking Phase 4 correctness as complete and clarifying that remaining work is optimization/polish.
- Trimmed the old verbose Phase 4 fixes narrative into a compact status table for quick reference.
- Updated rendering correctness section to current status (blend modes, depth-prepass parity, fog/blend lights, cull behavior, and light-scale parity).
- Reworked Optimization Workstream with explicit status labels (implemented/partial/planned).
- Rewrote Implementation Order into a detailed, status-driven execution sequence.
- Reworked File Change Summary into implemented vs planned items, preserving historical planned work while reflecting current code reality.

### 2026-03-21 (historical baseline)

- Original Phase 4 and Phase 5 planning draft established detailed fix proposals for RT shadow correctness, performance follow-ups, and future AO/reflection/GI workstreams.
