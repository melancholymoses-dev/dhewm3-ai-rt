# dhewm3_rtx Rendering Pipeline

Current state of the Vulkan rendering pipeline in `neo/renderer`.
Last updated: 2026-03-17 (after fixing 1: stencil bleed, 2: fog/blend skip, 3: translucent stencil).

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
| `Vulkan/vk_swapchain.cpp` | Swapchain, render pass, framebuffers, synchronization primitives |
| `Vulkan/vk_pipeline.cpp` | All graphics pipelines, descriptor set layouts, per-frame descriptor pools |
| `Vulkan/vk_buffer.cpp` | Buffer allocation, vertex cache integration, per-frame UBO/data rings |
| `Vulkan/vk_image.cpp` | Texture upload, mipmap generation via blit, sampler creation |
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

## Render Pass

A **single monolithic render pass** covers the entire frame. All views in one `EndFrame` (3D world + GUI overlay) share it.

```
Subpass 0:
  Color attachment  – swapchain image   – Load: CLEAR,  Store: STORE
  Depth attachment  – dedicated buffer  – Load: CLEAR,  Store: DONT_CARE
  Clear values: color=(0,0,0,1)  depth=1.0  stencil=128
```

- Opened on the first `RC_DRAW_VIEW` of the frame.
- Closed just before `RC_SWAP_BUFFERS`.
- No MSAA, no input attachments, no subpass dependencies.

---

## Rendering Passes (Draw Order)

```
VK_RB_DrawView()
  1. VK_RB_FillDepthBuffer()
  2. VK_RB_DrawInteractions()
  3. VK_RB_DrawShaderPasses()
  [FogAllLights — NOT YET IMPLEMENTED]
```

### 1. Depth Prepass (`VK_RB_FillDepthBuffer`)

Renders opaque (`MC_OPAQUE`) surfaces to depth buffer only.

- Uses the depth pipeline: depth LESS, color write mask = 0, no blend, no stencil
- Populates depth buffer for early-Z rejection in the interaction pass
- Does **not** handle alpha-tested surfaces via clip (clip not implemented in depth shader)

### 2. Interaction Pass (`VK_RB_DrawInteractions`)

Per-light lit rendering with Phong + bump + specular.

- Calls `RB_DetermineLightScale()` for `backEnd.lightScale`
- For each `viewLight`:
  1. Sets scissor rect to light bounds
  2. **Clears stencil to 128** within the light scissor (`vkCmdClearAttachments`) — prevents cross-light stencil bleed
  3. **Shadow volume pass** — Carmack's Reverse (depth-fail stencil) draw calls for `globalShadows` and `localShadows`; skipped when RT shadows are active
  4. Binds `interactionPipeline` (stencil EQUAL 128)
  5. Draws `localInteractions` and `globalInteractions`
  6. For `translucentInteractions`: switches to `interactionPipelineNoStencil` (stencil disabled), draws, then restores opaque pipeline

- Interaction UBO holds: light params, matrices, colors
- Descriptor set per draw: bump, falloff, light projection, diffuse, specular, spec table, RT shadow mask (binding 7)

### 3. Shader Passes (`VK_RB_DrawShaderPasses`)

Renders unlit / 2D surfaces using the GUI pipeline.

- Iterates `viewDef->drawSurfs`
- **Skips fog/blend light volume geometry** (`mat->IsFogLight() || mat->IsBlendLight()`) — prevents depth-test-disabled GUI pipeline from rendering volumes through floors
- Evaluates material registers to determine blend mode (opaque vs alpha)
- Allocates a GUI UBO slot and descriptor set per draw
- Issues `vkCmdDrawIndexed`

### 4. Fog/Blend Lights — NOT IMPLEMENTED

`FogAllLights` is absent. Fog and blend light volumes are currently skipped entirely.

---

## Pipelines

| Pipeline | Shaders | Key State |
|----------|---------|-----------|
| **Interaction** | `interaction.vert/frag` | Depth LESS_OR_EQUAL, no depth write, Stencil EQUAL 128, Additive blend (ONE+ONE) |
| **Interaction (no stencil)** | `interaction.vert/frag` | Same as above but stencil disabled — used for translucent interactions |
| **Shadow** | `shadow.vert/frag` | Depth LESS, no color write, Stencil depth-fail (Carmack's Reverse) |
| **Depth** | `depth.vert/frag` (or gui.vert) | Depth LESS, color write mask = 0 |
| **GUI Opaque** | `gui.vert/frag` | No depth/stencil test, no blend |
| **GUI Alpha** | `gui.vert/frag` | No depth/stencil test, SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend |

Pipeline state is **fixed** at creation time — there is no dynamic pipeline variant cache.

---

## Descriptor Set Layouts

| Set | Binding | Type | Content |
|-----|---------|------|---------|
| Interaction | 0 | UBO | Interaction params (matrices, colors, light) |
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

---

## Coordinate System Adjustments

- **Z remap:** OpenGL NDC z ∈ [-1,1] → Vulkan NDC z ∈ [0,1]
  - `new_proj[col*4+2] = 0.5 * old[col*4+2] + 0.5 * old[col*4+3]`
- **Y flip:** Viewport height is set negative to maintain CCW winding

---

## Raytracing (Optional, `#ifdef DHEWM3_RAYTRACING`)

When enabled via compile flag:

1. **BLAS** built per mesh from vertex/index buffers (`vk_accelstruct.cpp`)
2. **TLAS** rebuilt each frame from visible entities
3. Shadow rays dispatched before the interaction pass:
   - Render pass closed → TLAS rebuild → `VK_RT_DispatchShadowRays()` → shadow mask image written
   - Render pass reopened
4. Shadow mask sampled as binding 7 in the interaction descriptor set
5. When RT is active, `useStencilShadows = false` — stencil shadow volume pass is skipped

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
| Fog/blend light skip | `IsFogLight() \|\| IsBlendLight()` skipped in `DrawShaderPasses` so depth-test-less GUI pipeline doesn't draw volumes through floors |
| Translucent no-stencil pipeline | `interactionPipelineNoStencil` (stencil disabled) used for `translucentInteractions` to avoid shadow volumes incorrectly culling transparent surfaces |
| Shadow volume draw calls | `VK_RB_DrawShadowSurface` now issues real draw calls for `globalShadows`/`localShadows` using the shadow pipeline |
| Depth prepass | `VK_RB_FillDepthBuffer` populates the depth buffer before interactions, enabling early-Z rejection |

---

## Known Missing / Incomplete Features

| Feature | Status | Impact |
|---------|--------|--------|
| FogAllLights pass | Missing | Fog and blend light volumetric effects not rendered |
| Multiple blend modes | Only 2 supported (opaque, src-alpha) | Particles, glows, additive effects may be wrong |
| Texture coordinate transforms | Not verified | Animated materials (scroll, rotate) may be broken |
| Per-stage depth function | Missing | Some material stages depth-tested incorrectly |
| Per-surface cull mode | Missing | Two-sided geometry may render incorrectly |
| Post-process passes | Missing (`CopyRender` is a no-op stub) | No bloom, no screen-space effects |
| Dynamic pipeline variant cache | Missing | Pipelines are fixed; cannot adapt to material state bits |
| GPU index buffer usage | Missing | Indexes always copied to data ring per draw |
| Skinned/joint geometry | Not verified | Skeletal animation may not render correctly |
| Shadow volume ordering | Simplified | All global+local shadows drawn together per light; does not match vkDOOM3 ordering (global→local→local shadow→global shadow) |
| Per-shadow scissor refinement | Missing | Uses full light scissor; vkDOOM3 refines scissor per shadow surface |
| Resume render pass | Missing | RT shadow pass closes/reopens the render pass with CLEAR; prior draws preserved but DONT_CARE reopen is a hack |
| Alpha-tested depth prepass | Missing | Alpha-clip not implemented in depth shader |
