# dhewm3_rtx Rendering Pipeline

Current state of the Vulkan rendering pipeline in `neo/renderer`.

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
- The stencil is initialized to 128 so the interaction pass (which tests `stencil == 128`) always draws — a placeholder for missing shadow volumes.

---

## Rendering Passes (Draw Order)

### 1. Shader Passes (`VK_RB_DrawShaderPasses`)

Renders unlit / 2D surfaces using the GUI pipeline.

- Iterates `viewDef->drawSurfs`
- Evaluates material registers to determine blend mode (opaque vs alpha)
- Allocates a GUI UBO slot and descriptor set per draw
- Issues `vkCmdDrawIndexed`

### 2. Interaction Pass (`VK_RB_DrawInteractions`)

Renders per-light lit surfaces (Phong + bump + specular).

- Calls `RB_DetermineLightScale()` for `backEnd.lightScale`
- For each `viewLight`:
  - Sets scissor rect to light bounds
  - **Shadow volume pass: STUBBED** — pipeline bound, no draw calls issued
  - For each surface in `localInteractions`, `globalInteractions`, `translucentInteractions`:
    - Calls `RB_CreateSingleDrawInteractions()` → `VK_RB_DrawInteraction(din)`
    - Fills UBO: light params, matrices, colors
    - Allocates descriptor set; binds bump, falloff, light projection, diffuse, specular, spec table, RT shadow mask
    - Issues `vkCmdDrawIndexed`

**No depth prepass.** The interaction pipeline uses depth test `LESS_OR_EQUAL` as a workaround.

---

## Pipelines

| Pipeline | Shaders | Key State |
|----------|---------|-----------|
| **Interaction** | `interaction.vert/frag` | Depth LESS_OR_EQUAL, Stencil EQUAL 128, Additive blend (ONE+ONE) |
| **Shadow** | `shadow.vert/frag` | Depth LESS, no color write, Stencil depth-fail (Carmack's Reverse) |
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

When disabled: stencil shadow volumes are the intended fallback (currently unimplemented).

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

## Known Missing / Incomplete Features

| Feature | Status | Impact |
|---------|--------|--------|
| Depth prepass | Missing | Z-fighting, overdraw not rejected |
| Stencil shadow volumes | Stubbed (pipeline exists, no draw calls) | Shadows not rendered; stencil always 128 |
| Multiple blend modes | Only 2 supported (opaque, src-alpha) | Particles, glows, additive effects broken |
| Texture coordinate transforms | Missing | Animated materials (scroll, rotate) broken |
| Fog / blend lights | Missing | Atmospheric effects absent |
| Per-stage depth function | Missing | Some material stages depth-tested incorrectly |
| Per-surface cull mode | Missing | Two-sided geometry may render incorrectly |
| Post-process passes | Missing (`CopyRender` is a no-op stub) | No bloom, no screen-space effects |
| Light scale / overbright | Partially implemented | Lighting intensity may be off |
| Dynamic pipeline variant cache | Missing | Pipelines are fixed; cannot adapt to material state bits |
| GPU index buffer usage | Missing | Indexes always copied to data ring per draw |
| Skinned/joint geometry | Not verified | May not render correctly |