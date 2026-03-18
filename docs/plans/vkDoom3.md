# vkDoom3 Rendering Pipeline

Current state of the Vulkan rendering pipeline in `neo/renderer` (BFG Edition base).
Last updated: 2026-03-18.

---

## Architecture Overview

The renderer uses the same frontend/backend split as Doom 3, but the BFG edition reorganized it significantly. The backend is a class (`idRenderBackend`) rather than a set of free functions, and shader programs are first-class objects managed by `renderProgManager`.

```
idRenderSystem::RenderScene(world, renderView)   [Frontend]
  → Builds viewDef_t (drawSurfs, viewLights, viewEntitys)
  → Queues RC_DRAW_VIEW command

idRenderBackend::Execute(renderCommands)          [Backend]
  → DrawView(RC_DRAW_VIEW)
    ├─ FillDepthBufferFast()
    ├─ DrawInteractions()
    ├─ DrawShaderPasses()
    └─ FogAllLights()
  → GL_EndFrame() → present
```

---

## Vulkan Source Files

| File | Role |
|------|------|
| `Vulkan/RenderBackend_VK.cpp` | Instance/device creation, swapchain, command buffers, render pass, frame sync (~1956 lines) |
| `Vulkan/RenderProgs_VK.cpp` | Shader module compilation, pipeline creation/caching, descriptor set management (~1083 lines) |
| `Vulkan/BufferObject_VK.cpp` | Vertex, index, and uniform buffer allocation (~725 lines) |
| `Vulkan/Image_VK.cpp` | Texture upload, format conversion, sampler creation, per-frame garbage collection (~479 lines) |
| `Vulkan/Allocator_VK.h/.cpp` | Custom block allocator; optional VMA (`ID_USE_AMD_ALLOCATOR`) — (~164 + 671 lines) |
| `Vulkan/Staging_VK.h/.cpp` | Staging buffers for GPU uploads, fence-based sync (~82 + 248 lines) |
| `Vulkan/qvk.h` | `ID_VK_CHECK`, `ID_VK_VALIDATE`, platform surface macros, VK error string mapping |

Core (platform-agnostic) structures:

| File | Role |
|------|------|
| `RenderBackend.h/.cpp` | `idRenderBackend` class, pass orchestration, `vulkanContext_t` global |
| `RenderProgs.h` | `renderProg_t`, `shader_t`, `renderProgManager` interface, `RENDERPARM_*` enum |
| `RenderCommon.h` | `viewDef_t`, `viewLight_t`, `drawSurf_t`, `drawInteraction_t` |
| `BufferObject.h` | `idVertexBuffer`, `idIndexBuffer`, `idUniformBuffer` |
| `VertexCache.h` | Per-frame dynamic + static buffer management |
| `Image.h` | `idImage` with Vulkan members |

---

## Swapchain

- **Color format:** `VK_FORMAT_B8G8R8A8_UNORM` with SRGB colorspace
- **Present mode:** IMMEDIATE or MAILBOX when vsync off; FIFO when vsync on
- **Buffering:** `NUM_FRAME_DATA = 3` (triple-buffered command buffers, descriptor pools, fences)
- **Sync:** per-frame `m_acquireSemaphores`, `m_renderCompleteSemaphores`, command buffer fences

---

## Render Passes

Two render passes are created:

| Pass | Load Op | Initial Layout | Use |
|------|---------|---------------|-----|
| `vkcontext.renderPass` | DONT_CARE | UNDEFINED | Fresh frame start |
| `vkcontext.renderPassResume` | LOAD | SHADER_READ_ONLY_OPTIMAL | Continue after a pass break |

**Attachments:**
```
0: Swapchain color (B8G8R8A8_UNORM)
     Store: STORE  Final: GENERAL
1: Depth/stencil (D24_UNORM_S8 or device-selected)
     MSAA sample count: vkcontext.sampleCount
2: (optional) MSAA resolve target when sampleCount > 1
```

The two-pass design allows interrupting the render pass (e.g. for copies or compute) and resuming without clearing.

---

## Rendering Passes (Draw Order)

### 1. `FillDepthBufferFast`

Renders all opaque surfaces to the depth buffer only.

- Binds `BUILTIN_DEPTH` program
- No color writes, depth function LESS
- Populates depth buffer for early-Z rejection in later passes
- Handles alpha-tested surfaces via clip in shader

### 2. `DrawInteractions`

Per-light lit rendering with full Phong shading.

For each `viewLight_t`:
1. **Stencil shadow pass** — shadow volume geometry rendered with Carmack's Reverse (depth-fail stencil)
2. **Local interactions** — surfaces near the light, no shadow test
3. **Local shadow surfaces** — shadow-only geometry pass
4. **Global interactions** — surfaces with shadow stencil test
5. **Translucent interactions** — blended lit surfaces

Key functions: `RenderInteractions()`, `StencilShadowPass()`, `DrawSingleInteraction()`

### 3. `DrawShaderPasses`

Non-light-dependent surfaces: decals, particles, sky, unlit effects.

- Evaluates material stages and blend modes
- Supports a full range of `GLS_*` blend state bits
- Calls `renderProgManager.CommitCurrent()` before each draw

### 4. `FogAllLights`

Renders fog volumes and blend lights as post-lighting additive passes.

---

## Pipeline Management

Pipelines are created **on demand** and cached per `(renderProg, stateBits)` pair.

```cpp
renderProg_t {
  name, usesJoints, optionalSkinning
  vertexShaderIndex, fragmentShaderIndex
  vertexLayoutType            // LAYOUT_DRAW_VERT | SHADOW_VERT | SHADOW_VERT_SKINNED
  pipelineLayout: VkPipelineLayout
  descriptorSetLayout: VkDescriptorSetLayout
  bindings: []rpBinding_t
  pipelines: []pipelineState_t   // keyed by stateBits
}
```

`stateBits` encodes blend modes, depth function, depth write, stencil ops, cull mode, polygon mode, fill mode — so each unique rendering state gets its own pipeline automatically.

**State bit mapping:**
- `GLS_SRCBLEND_*` / `GLS_DSTBLEND_*` → `VkBlendFactor`
- `GLS_DEPTHFUNC_*` → `VkCompareOp`
- `GLS_STENCIL_FUNC_*` → `VkStencilOp`
- `GLS_POLYGON_OFFSET` → dynamic depth bias
- `GLS_TWOSIDED` → cull mode NONE

**Built-in programs (subset):**
- `BUILTIN_GUI` — 2D UI
- `BUILTIN_INTERACTION` — main lit surface shading
- `BUILTIN_DEPTH` — depth prepass
- `BUILTIN_SHADOW` — shadow volume stencil
- `BUILTIN_FOG` — fog pass
- 20+ total built-in shaders

---

## Descriptor Set Layout and Management

```
Per renderProg_t:
  Binding 0: Vertex UBO (render parameters used by vertex shader)
  Binding 1: Fragment UBO (render parameters used by fragment shader)
  Binding 2+: Combined image samplers

m_descriptorPools[NUM_FRAME_DATA]  (3 pools)
  MAX_DESC_SETS            = 16 384
  MAX_DESC_UNIFORM_BUFFERS =  8 192
  MAX_DESC_IMAGE_SAMPLERS  = 12 384
```

**Shader parameter flow:**
```
SetRenderParm(RENDERPARM_*, float[4])    → m_uniforms[]
CommitCurrent(stateBits, commandBuffer)
  → Allocate descriptor set from current frame pool
  → Write uniform buffer binding + texture bindings
  → vkCmdBindDescriptorSets()
```

Parameters are accumulated in `m_uniforms[RENDERPARM_TOTAL]` (64 × vec4 = 1 KB) and written to a per-frame uniform buffer on `CommitCurrent`. Vertex and fragment UBOs are written separately.

---

## Buffer Management

| Buffer | Usage Flags | Memory Type | Size |
|--------|------------|-------------|------|
| Dynamic vertex | VERTEX + TRANSFER_DST | CPU-to-GPU | 31 MB/frame |
| Dynamic index | INDEX + TRANSFER_DST | CPU-to-GPU | 31 MB/frame |
| Static vertex | VERTEX | GPU-only | 31 MB total |
| Static index | INDEX | GPU-only | 31 MB total |
| Joint/bone | UNIFORM | CPU-to-GPU | per frame |

**Memory allocator options:**
- Custom block allocator (`idVulkanAllocator`) — linked list of chunks per memory type, configurable budgets via CVars:
  - `r_vkDeviceLocalMemoryMB` (default: 128 MB)
  - `r_vkHostVisibleMemoryMB` (default: 64 MB)
- AMD VMA (`ID_USE_AMD_ALLOCATOR`) — automatic pooling

**Staging:**
- `idVulkanStagingManager` — one staging buffer per frame (`NUM_FRAME_DATA = 3`), fence-synchronized
- `r_vkUploadBufferSizeMB` (default: 64 MB)
- Transfers via `vkCmdCopyBufferToImage`, flushed and waited before reuse

**Vertex cache handle packing:** `size (23 bits) | offset (25 bits) | frame (15 bits) | static flag`

---

## Texture Handling

```cpp
idImage {
  m_image:          VkImage
  m_view:           VkImageView
  m_sampler:        VkSampler
  m_internalFormat: VkFormat
  m_layout:         VkImageLayout
  m_opts: { format, width, height, numLevels, textureType (2D | Cubic) }
}
```

**Format mapping (subset):**

| idImageFormat | VkFormat |
|---------------|----------|
| FMT_RGBA8 | VK_FORMAT_R8G8B8A8_UNORM |
| FMT_DXT1 | VK_FORMAT_BC1_RGB_UNORM_BLOCK |
| FMT_DXT5 | VK_FORMAT_BC3_UNORM_BLOCK |
| FMT_DEPTH | `vkcontext.depthFormat` |

- Deferred deletion: images/views/samplers/allocations placed in `m_imageGarbage[frame]` and freed when that frame slot is next reused.
- Component swizzling: formats like green-alpha use `VkComponentMapping` for correct channel mapping.

---

## Vertex Layouts

Three distinct layouts with matching pipeline input descriptions:

| Layout | Contents | Use |
|--------|----------|-----|
| `LAYOUT_DRAW_VERT` | Position (R32G32B32), TexCoord (R16G16), Normal (R8G8B8A8), Tangent, Color1, Color2 | All standard geometry |
| `LAYOUT_DRAW_SHADOW_VERT` | Position XYZW (R32G32B32A32) | Shadow volumes |
| `LAYOUT_DRAW_SHADOW_VERT_SKINNED` | Position XYZW + 2× Color (R8G8B8A8) | Skeletal animation shadow volumes |

---

## Shader Parameters

Shader parameters are addressed by `RENDERPARM_*` enum and passed as `idVec4[RENDERPARM_TOTAL]`. Key parameters:

```
RENDERPARM_LOCALLIGHTORIGIN       – light position in model space
RENDERPARM_LIGHTPROJECTION_S/T/Q  – light projection matrix rows
RENDERPARM_LIGHTFALLOFF_S         – falloff texture coord scale
RENDERPARM_DIFFUSEMODIFIER        – diffuse color / intensity
RENDERPARM_SPECULARMODIFIER       – specular multiplier
RENDERPARM_VIEWORIGIN             – camera position
RENDERPARM_MVPMATRIX_X/Y/Z/W      – model-view-projection matrix rows
RENDERPARM_MODELMATRIX_X/Y/Z/W    – model matrix rows
RENDERPARM_TEXTUREMATRIX_S/T      – texture coordinate transform
```

Total: ~64 `RENDERPARM_*` entries covering all built-in programs.

---

## Frame Submission

```
GL_StartFrame()
  ├─ vkWaitForFences(commandBuffer fence for oldest frame)
  ├─ vkAcquireNextImageKHR() → imageIndex
  ├─ vkBeginCommandBuffer()
  ├─ vkCmdBeginRenderPass(renderPass, framebuffer[imageIndex])
  ├─ Read GPU timestamps from previous frame
  └─ renderProgManager.StartFrame() — reset descriptor pool

[Depth + Interaction + ShaderPasses + Fog]

GL_EndFrame()
  ├─ vkCmdEndRenderPass()
  ├─ vkCmdPipelineBarrier() – GENERAL → PRESENT_SRC_KHR
  ├─ vkEndCommandBuffer()
  ├─ vkQueueSubmit(graphicsQueue)
  │   wait:   m_acquireSemaphores[frame]
  │   signal: m_renderCompleteSemaphores[frame]
  └─ vkQueuePresentKHR(presentQueue)
```

Frame counter cycles: `m_currentFrameData = counter % NUM_FRAME_DATA` (0→1→2→0…)

---

## GPU Timing

- `vkCmdResetQueryPool()` and `vkCmdWriteTimestamp()` bracketing each pass
- Results read back at start of next frame for GPU timing stats
- Driven by `r_showPrimitives` / debug CVars

---

## Notable Strengths vs Basic Vulkan Port

1. **Depth prepass** — eliminates overdraw, enables correct interaction stencil
2. **Dynamic pipeline cache** — one pipeline per state combination, created on demand; full `GLS_*` support
3. **Full blend mode support** — all `GLS_SRCBLEND_*`/`GLS_DSTBLEND_*` bits translate to pipeline variants
4. **Complete shadow volume rendering** — stencil depth-fail (Carmack's Reverse) implemented; correct per-light ordering
5. **Fog/blend lights pass** — `FogAllLights()` atmospheric effects fully present
6. **Resume render pass** — can suspend/resume rendering without clearing (DONT_CARE + LOAD pattern)
7. **Deferred image deletion** — `m_imageGarbage[frame]` prevents GPU hazards on texture streaming
8. **MSAA support** — resolve attachment wired up; configurable sample count + sample shading
9. **GPU timing queries** — profiling infrastructure present and active
10. **Full shader parameter system** — `RENDERPARM_*` enum with 64 parameters, vertex + fragment UBOs
11. **Skinned geometry** — `SHADOW_VERT_SKINNED` layout + joint UBO for skeletal animations
12. **Triple buffering** — `NUM_FRAME_DATA = 3` avoids CPU/GPU stalls between frames
13. **Deferred memory allocator** — custom block allocator or AMD VMA with configurable budgets
14. **Staging manager** — batched CPU→GPU uploads with fence-based synchronization
