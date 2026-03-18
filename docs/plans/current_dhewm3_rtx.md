# dhewm3_rtx Rendering Pipeline

Current state of the Vulkan rendering pipeline in `neo/renderer`.
Last updated: 2026-03-18 (after blend support, tex coord transforms, alpha-clip depth prepass, fog/blend light pipelines, resume render pass, deferred image deletion).

---

## Architecture Overview

The renderer follows a frontend/backend split inherited from original Doom 3. The frontend traverses the scene graph and builds a list of draw surfaces; the backend consumes those surfaces and issues Vulkan commands.

```
idRenderWorld::RenderView()         [Frontend]
  → RC_DRAW_VIEW command queued
  → RB_ExecuteBackEndCommands()     [Backend dispatcher]
    → VKBackend::DrawView()
      → VK_RB_DrawView()
  → RC_SWAP_BUFFERS
    → VK_RB_SwapBuffers()
```

A polymorphic `IBackend` interface (`RendererBackend.h`) allows swapping the GL backend for the Vulkan one at startup.

---

## Vulkan Source Files

| File | Role |
|------|------|
| `Vulkan/VKBackend.h/.cpp` | `IBackend` implementation; Init, Shutdown, resource entry points |
| `Vulkan/vk_backend.cpp` | Main frame loop: `VK_RB_DrawView`, `VK_RB_SwapBuffers`, draw state |
| `Vulkan/vk_instance.cpp` | Physical/logical device, queue families, extension/layer negotiation, RT capability probe |
| `Vulkan/vk_swapchain.cpp` | Swapchain, render passes (initial + resume), framebuffers, synchronization primitives |
| `Vulkan/vk_pipeline.cpp` | All graphics pipelines, descriptor set layouts, per-frame descriptor pools |
| `Vulkan/vk_buffer.cpp` | Buffer allocation, vertex cache integration, per-frame UBO/data rings |
| `Vulkan/vk_image.cpp` | Texture upload, mipmap generation via blit, sampler creation, deferred deletion garbage ring |
| `Vulkan/vk_shader.cpp` | SPIR-V shader module loading from disk |
| `Vulkan/vk_common.h` | Global `vkState_t`, pipeline state `vkPipelines_t`, helper macros (`VK_CHECK`) |
| `Vulkan/vk_raytracing.h` | RT type declarations (BLAS, TLAS, shadow mask) |
| `Vulkan/vk_accelstruct.cpp` | BLAS/TLAS construction, device address queries (`#ifdef DHEWM3_RAYTRACING`) |
| `Vulkan/vk_shadows.cpp` | RT shadow pipeline setup, shadow ray dispatch (`#ifdef DHEWM3_RAYTRACING`) |

---

## Swapchain

- **Color format:** prefers `VK_FORMAT_B8G8R8A8_SRGB`, falls back to first available
- **Depth format:** auto-detected: D32F+S8 → D24+S8 → D32F
- **Present mode:** prefers Mailbox (triple-buffer), falls back to FIFO
- **Image count:** `minImageCount + 1`, capped at `VK_MAX_SWAPCHAIN_IMAGES = 8`
- **In-flight frames:** `VK_MAX_FRAMES_IN_FLIGHT = 2` (double-buffered command buffers + fences)
- **Sync:** per-frame `imageAvailableSemaphore`, `renderFinishedSemaphore`, `inFlightFence`

---

## Render Passes

Two render passes are created:

| Pass | Load Op | Use |
|------|---------|-----|
| `vkState.renderPass` | CLEAR | Fresh frame / first open |
| `vkState.renderPassResume` | LOAD | Reopen after RT dispatch (preserves prior draws) |

- Stencil clear value: 128
- Color clear: (0, 0, 0, 1)
- Depth clear: 1.0
- Both passes share the same attachment formats so all pipelines are compatible with both.

---

## Rendering Passes (Draw Order)

```
VK_RB_DrawView()
  1. VK_RB_FillDepthBuffer()
  2. VK_RB_DrawInteractions()
  3. VK_RB_DrawShaderPasses()
  4. VK_RB_FogAllLights()     [pipelines defined; dispatch partially implemented]
```

### 1. Depth Prepass (`VK_RB_FillDepthBuffer`)

Renders opaque and perforated surfaces to depth buffer only.

- **`depthPipeline`** — `MC_OPAQUE` surfaces: depth LESS, no color write
- **`depthClipPipeline`** — `MC_PERFORATED` (alpha-tested) surfaces: same, but fragment shader samples diffuse and discards on `alpha < threshold`
- Populates depth buffer for early-Z rejection in the interaction pass

### 2. Interaction Pass (`VK_RB_DrawInteractions`)

Per-light lit rendering with Phong + bump + specular.

- Calls `RB_DetermineLightScale()` for `backEnd.lightScale`
- For each `viewLight`:
  1. Sets scissor rect to light bounds
  2. **Clears stencil to 128** within the light scissor (`vkCmdClearAttachments`) — prevents cross-light stencil bleed
  3. **Shadow volume pass** — draw calls for `globalShadows` and `localShadows` using shadow pipeline; skipped when RT shadows are active
  4. Binds `interactionPipeline` (stencil EQUAL 128)
  5. Draws `localInteractions` and `globalInteractions`
  6. For `translucentInteractions`: switches to `interactionPipelineNoStencil`, draws, then restores

- Interaction UBO holds: light params, matrices, colors, texture coordinate transforms, overbright scale
- Descriptor set per draw: bump, falloff, light projection, diffuse, specular, spec table, RT shadow mask (binding 7)

### 3. Shader Passes (`VK_RB_DrawShaderPasses`)

Renders unlit / 2D surfaces.

- Iterates `viewDef->drawSurfs`
- Skips fog/blend light volume geometry (handled in pass 4)
- Evaluates material registers and `drawStateBits` per stage to select blend mode
- Supports multiple blend modes: opaque, src-alpha, additive ONE+ONE, and others via state bit mapping
- Evaluates and uploads texture coordinate transform matrices per stage
- Handles overbright color scaling

### 4. Fog / Blend Lights (`VK_RB_FogAllLights`)

- Pipelines defined: `fogPipeline` (depth EQUAL, additive), `fogFrustumPipeline` (depth LESS, back-cull), `blendlightPipeline` (depth EQUAL, DST_COLOR/ZERO modulate)
- Shaders exist: `fog.vert/frag`, `blendlight.vert/frag`
- Render dispatch partially implemented; correctness being iterated

---

## Pipelines

| Pipeline | Shaders | Key State |
|----------|---------|-----------|
| **Interaction** | `interaction.vert/frag` | Depth LESS_OR_EQUAL, no depth write, Stencil EQUAL 128, Additive blend (ONE+ONE) |
| **Interaction (no stencil)** | `interaction.vert/frag` | Same but stencil disabled — translucent interactions |
| **Shadow (Z-fail)** | `shadow.vert/frag` | Depth LESS, no color write, Stencil depth-fail (Carmack's Reverse) |
| **Shadow (Z-pass)** | `shadow.vert/frag` | Depth LESS, no color write, Stencil depth-pass |
| **Depth (opaque)** | `depth.vert/frag` | Depth LESS, color write mask = 0 |
| **Depth (alpha-clip)** | `depth.vert` + `depth_clip.frag` | Depth LESS, color mask = 0, alpha discard |
| **GUI Opaque** | `gui.vert/frag` | No depth/stencil test, no blend |
| **GUI Alpha** | `gui.vert/frag` | No depth/stencil test, SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend |
| **Fog** | `fog.vert/frag` | Depth EQUAL, additive blend |
| **Fog Frustum** | `fog.vert/frag` | Depth LESS, back-face cull |
| **Blend Light** | `blendlight.vert/frag` | Depth EQUAL, DST_COLOR/ZERO modulate |

Pipeline state is **fixed** at creation time — no dynamic pipeline variant cache.

---

## Descriptor Set Layouts

| Set | Binding | Type | Content |
|-----|---------|------|---------|
| Interaction | 0 | UBO | Interaction params (matrices, colors, light, tex coord transforms) |
| Interaction | 1–6 | Sampler | bump, falloff, light proj, diffuse, specular, spec table |
| Interaction | 7 | Sampler | RT shadow mask (optional) |
| Shadow | 0 | UBO | MVP + light origin |
| GUI | 0 | UBO | MVP, color |
| GUI | 1 | Sampler | Diffuse texture |

Per-frame descriptor pools hold up to 4096 sets; reset each frame after fence wait.

---

## Buffer Management

### GPU-Cached Vertex Buffers
- Created once per model load via `VK_VertexCache_Alloc()`
- Usage: `VERTEX_BUFFER_BIT | TRANSFER_DST_BIT | SHADER_DEVICE_ADDRESS_BIT`
- Memory: Device-local
- Vertex format: `idDrawVert` (pos, color, texcoord, 2 tangents, normal = 60 bytes)

### Per-Frame UBO Ring
- One ring per frame (2 rings total)
- ~384 bytes per interaction × 4096 max = ~1.5 MB per frame
- Alignment: `minUniformBufferOffsetAlignment`
- Memory: Host-visible coherent; reset to offset 0 each frame

### Per-Frame Data Ring
- One ring per frame: 32 MB
- Holds temporary vertex/index data for non-cached geometry (TAG_TEMP, GUI)
- Memory: Host-visible coherent; reset each frame

---

## Texture Handling

```
idImage (frontend)
  → VKBackend::Image_Upload()
    → VK_Image_Upload()
      ├─ Create VkImage (RGBA8, optimal tiling, TRANSFER_DST | SAMPLED)
      ├─ Upload via staging buffer
      ├─ Generate mipmaps via vkCmdBlitImage loop
      ├─ Transition to SHADER_READ_ONLY_OPTIMAL
      └─ Store vkImageData_t in idImage->backendData
```

- Fallback: 1×1 white texture used when `idImage` has no GPU backing
- Sampler wrapping: TR_CLAMP → CLAMP_TO_EDGE, TR_CLAMP_TO_ZERO → CLAMP_TO_BORDER, default REPEAT
- Filtering: TF_NEAREST → NEAREST, default LINEAR
- **Deferred deletion:** `VK_Image_Purge` queues into `imageGarbage[frame]` ring; drained after per-frame fence (`VK_Image_DrainGarbage`)

---

## Coordinate System Adjustments

- **Z remap:** OpenGL NDC z ∈ [-1,1] → Vulkan NDC z ∈ [0,1]
  - `new_proj[col*4+2] = 0.5 * old[col*4+2] + 0.5 * old[col*4+3]`
- **Y flip:** Viewport height is set negative to maintain CCW winding

---

## Raytracing (Optional, `#ifdef DHEWM3_RAYTRACING`)

When enabled via compile flag:

1. **BLAS** built per mesh from vertex/index buffers (`vk_accelstruct.cpp`)
2. **TLAS** rebuilt each frame from visible entities (structure defined; population loop incomplete)
3. Shadow rays dispatched before the interaction pass:
   - Render pass closed → TLAS rebuild → `VK_RT_DispatchShadowRays()` → shadow mask image written
   - Render pass reopened using `renderPassResume` (LOAD op — preserves prior draws)
4. Shadow mask sampled as binding 7 in the interaction descriptor set
5. When RT is active, `useStencilShadows = false` — stencil shadow volume pass is skipped

RT pipeline is initialized (`VK_RT_InitShadowPipeline`), SBT built; dispatch loop still under construction.

---

## Frame Submission

```
VK_RB_SwapBuffers()
  ├─ D3::ImGuiHooks::RenderVulkan(cmdBuf)    [ImGui overlay]
  ├─ vkCmdEndRenderPass()
  ├─ vkEndCommandBuffer()
  ├─ vkQueueSubmit(graphicsQueue)
  │   wait:   imageAvailableSemaphore[frame]
  │   signal: renderFinishedSemaphore[frame]
  │           inFlightFence[frame]
  ├─ vkQueuePresentKHR(presentQueue)
  └─ currentFrame = (currentFrame + 1) % 2
```

---

## Recent Fixes Applied

| Fix | Description |
|-----|-------------|
| Per-light stencil clear | `vkCmdClearAttachments(stencil=128)` before each light's scissor rect prevents shadow stencil bleed between lights |
| Fog/blend light skip | `IsFogLight() \|\| IsBlendLight()` skipped in `DrawShaderPasses`; fog/blend handled in dedicated pass |
| Translucent no-stencil pipeline | `interactionPipelineNoStencil` used for `translucentInteractions` |
| Shadow volume draw calls | `VK_RB_DrawShadowSurface` issues real draw calls using the shadow pipeline |
| Depth prepass | `VK_RB_FillDepthBuffer` with opaque + alpha-clip variants |
| Blend mode support | `DrawShaderPasses` evaluates `drawStateBits` per stage for multiple blend modes |
| Texture coord transforms | Material stage texture matrices evaluated and uploaded to interaction UBO |
| Alpha-clip depth prepass | `depthClipPipeline` samples diffuse and discards in fragment shader for perforated surfaces |
| Fog/blend light pipelines | `fogPipeline`, `fogFrustumPipeline`, `blendlightPipeline` created; shaders compiled |
| Resume render pass | `renderPassResume` (LOAD op) created; used when reopening after RT dispatch |
| Deferred image deletion | `VK_Image_Purge` enqueues to garbage ring; freed after frame fence |
| Overbright handling | Light overbright scale uploaded to interaction UBO and applied in shaders |
| Z-pass shadow variant | `shadowPipelineZPass` added alongside Z-fail for camera-outside-volume cases |

---

## Known Missing / Incomplete Features

| Feature | Status | Impact |
|---------|--------|--------|
| FogAllLights dispatch | Pipelines and shaders exist; render loop correctness in progress | Fog/blend light volumes may not render correctly |
| Dynamic pipeline variant cache | Fixed pipeline set; cannot adapt to arbitrary material state bits | Some exotic blend/depth states render wrong |
| Per-stage depth function | Not verified | Some material stages depth-tested incorrectly |
| Per-surface cull mode | Not verified | Two-sided geometry may render incorrectly |
| Post-process passes | `CopyRender` is a no-op stub | No bloom, no screen-space effects |
| GPU index buffer usage | Indexes copied to data ring per draw | Extra host memory traffic |
| Skinned/joint geometry | Not verified | Skeletal animation may not render correctly |
| Shadow volume ordering | Simplified (all global+local shadows together per light) | May differ from vkDOOM3 (global→local→local shadow→global shadow) |
| Per-shadow scissor refinement | Uses full light scissor | Some extra GPU fragment work |
| RT shadow TLAS population | Structure allocated; per-frame populate loop incomplete | RT shadows not yet dispatching |
| RT shadow dispatch loop | Pipeline created, SBT built; dispatch loop incomplete | RT mode does not produce shadow masks yet |
| Geometry rendering correctness | Recent commits show "still triangles" debug state | Possible vertex input or pipeline configuration issue |
