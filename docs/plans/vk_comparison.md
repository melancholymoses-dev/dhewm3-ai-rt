# Vulkan Rendering Pipeline Comparison

Comparing `dhewm3_rtx` (classic Doom 3 base, raytracing target) vs `vkDoom3` (BFG Edition base, reference Vulkan implementation).

---

## High-Level Summary

| Aspect | dhewm3_rtx | vkDoom3 |
|--------|-----------|---------|
| Engine base | Classic Doom 3 / dhewm3 | Doom 3 BFG Edition |
| Backend structure | Free functions + `IBackend` interface | `idRenderBackend` class |
| Rendering completeness | Partial (loads maps, no textures on surfaces) | Full production quality |
| Shadow rendering | Stubbed (unimplemented) | Complete (stencil volumes) |
| Depth prepass | Missing | Present |
| Blend modes | 2 (opaque, src-alpha) | Full `GLS_*` set |
| Dynamic pipeline cache | No (fixed pipelines) | Yes (on-demand per state) |
| Fog/blend lights | Missing | Present |
| Raytracing hooks | Present (`#ifdef DHEWM3_RAYTRACING`) | Absent |
| MSAA | No | Yes |
| GPU timing | No | Yes |

---

## Render Pass Architecture

| Feature | dhewm3_rtx | vkDoom3 |
|---------|-----------|---------|
| Number of render passes | 1 (monolithic) | 2 (primary + resume) |
| Frame clear | CLEAR at pass open | DONT_CARE (preserves nothing, but LOAD pass allows resume) |
| RT shadow integration | Closes/reopens render pass mid-frame | N/A |
| MSAA resolve attachment | No | Yes |

**Missing in dhewm3_rtx:** The resume render pass (`renderPassResume` with `LOAD` op) is essential for any mid-frame pass breaks — the RT shadow dispatch currently closes and reopens the single pass with a CLEAR, which discards prior draws on tile-based GPUs.

---

## Rendering Passes

| Pass | dhewm3_rtx | vkDoom3 |
|------|-----------|---------|
| **Depth prepass** | ❌ Missing | ✅ `FillDepthBufferFast()` |
| **Stencil shadow volumes** | ❌ Stubbed | ✅ `StencilShadowPass()` (Carmack's Reverse) |
| **Interaction / lit surfaces** | ✅ Present | ✅ Present |
| **Shader passes (unlit/2D)** | ✅ Present (GUI pipeline) | ✅ Present |
| **Fog / blend lights** | ❌ Missing | ✅ `FogAllLights()` |
| **Post-process / copy render** | ❌ No-op stub | ✅ Handled via resume pass |
| **Translucent interactions** | ⚠️ Listed but blend modes incomplete | ✅ Full blend state |

### Impact of Missing Depth Prepass

In dhewm3_rtx the interaction pipeline uses depth test `LESS_OR_EQUAL` as a workaround. Without a depth prepass:

- Surfaces are not depth-rejected before the expensive interaction shader runs → overdraw
- Z-fighting is more likely when surfaces coincide
- The stencil value of 128 (never modified since shadows are unimplemented) means the stencil equal-128 test in the interaction pass always passes, which happens to work only because there are no real shadows yet

---

## Shadow Rendering

| Aspect | dhewm3_rtx | vkDoom3 |
|--------|-----------|---------|
| Shadow pipeline | Created (vert/frag shaders loaded) | Created |
| Draw calls issued | ❌ None (TODO comment) | ✅ Full depth-fail stencil |
| Stencil reference | Hard-coded 128 (never changes) | Dynamically set per pass |
| Shadow volumes (geometry) | `geo->shadowCache` exists but unused | Used in `StencilShadowPass()` |
| RT shadows | ✅ Optional via `#ifdef DHEWM3_RAYTRACING` | ❌ Not present |

**What to implement:** Mirror `vkDoom3`'s `RenderInteractions` loop — before issuing interaction draws for a light, first render `vLight->globalShadows` and `vLight->localShadows` through the shadow pipeline using two-sided stencil (increment back faces, decrement front faces on depth fail). Then re-enable color writes and test `stencil == 0` for lit regions.

---

## Pipeline Management

| Feature | dhewm3_rtx | vkDoom3 |
|---------|-----------|---------|
| Pipeline cache | ❌ Fixed set of 4 pipelines | ✅ Dynamic cache keyed by `(program, stateBits)` |
| Blend mode variants | 2 only | All GLS_* combinations |
| Depth function variants | Fixed per pipeline | Any `GLS_DEPTH_TEST_*` |
| Cull mode variants | Fixed | `GLS_CULL_*` |
| Stencil op variants | Fixed | Dynamic |
| Pipeline layout | Per pipeline (3 distinct) | Per render program |

**Impact:** Many Doom 3 material effects require specific blend modes (additive, multiply, etc.) and per-stage depth functions. Without a dynamic pipeline cache these materials render incorrectly or are silently skipped.

---

## Shader / Render Program System

| Feature | dhewm3_rtx | vkDoom3 |
|---------|-----------|---------|
| Shader loading | SPIR-V from disk (`glprogs/glsl/*.spv`) | SPIR-V compiled + cached |
| Program abstraction | Direct `VkPipeline` handles in `vkPipelines_t` | `renderProg_t` objects in `renderProgManager` |
| Shader parameters | Per-UBO struct, manually filled | `RENDERPARM_*` enum, 60+ params, `SetRenderParm()` |
| Parameter commit | Manual UBO fill per draw | `CommitCurrent(stateBits, cmdBuf)` |
| Built-in shaders | 4 programs | 20+ built-in programs |
| Skinning / joints | Not verified | Supported (`usesJoints`, joint cache binding) |

---

## Descriptor Set Management

| Feature | dhewm3_rtx | vkDoom3 |
|---------|-----------|---------|
| Pool size | 4096 sets/frame | 16 384 sets/frame |
| Pool reset | Per frame | Per frame |
| Texture bindings | 7 fixed slots (interaction) | Dynamic per `rpBinding_t` list |
| Uniform binding | Offset into ring buffer | Offset into per-frame param buffer |
| Descriptor layout | 3 fixed layouts | 1 per render program, generated from binding list |

---

## Buffer Management

| Feature | dhewm3_rtx | vkDoom3 |
|---------|-----------|---------|
| Vertex buffer | GPU-cached (`VK_VertexCache_Alloc`) + 32 MB temp ring | 31 MB dynamic/frame + 31 MB static |
| Index buffer | Always copied to temp ring | Static GPU buffers; dynamic ring for temp |
| Uniform buffer | Per-frame UBO ring (~1.5 MB/frame) | Per-frame param buffer via `CommitCurrent` |
| Staging manager | Inline (staging in upload functions) | Dedicated `idVulkanStagingManager` |
| Memory allocator | Manual `vkAllocateMemory` | Custom block allocator or VMA |
| Joint/bone buffers | Not verified | Dedicated joint buffer per frame |

---

## Texture Handling

| Feature | dhewm3_rtx | vkDoom3 |
|---------|-----------|---------|
| Mipmap generation | ✅ vkCmdBlitImage loop | Loaded from DDS or generated |
| Compressed formats | ❌ RGBA8 only | ✅ BC1/BC3 (DXT1/DXT5) + others |
| Cube maps | Not verified | ✅ TT_CUBIC supported |
| Deferred deletion | ❌ Immediate | ✅ Per-frame garbage list |
| Component swizzle | Not present | ✅ Format-specific swizzle on image view |

**Impact:** Without deferred deletion, destroying a texture that is still referenced by in-flight GPU commands causes undefined behavior. Without compressed format support, memory usage is significantly higher.

---

## Vertex Layouts

| Layout | dhewm3_rtx | vkDoom3 |
|--------|-----------|---------|
| Standard geometry | ✅ `idDrawVert` (60 bytes) | ✅ `LAYOUT_DRAW_VERT` |
| Shadow volumes | ⚠️ Pipeline exists, unused | ✅ `LAYOUT_DRAW_SHADOW_VERT` |
| Skinned shadow volumes | ❌ Not present | ✅ `LAYOUT_DRAW_SHADOW_VERT_SKINNED` |
| Skeletal animation | Not verified | ✅ Joint cache + skinning shader |

---

## Missing Features in dhewm3_rtx — Priority Order

### Critical (visible rendering bugs right now)

1. **Textures not rendering on surfaces**
   - Likely cause: interaction descriptor sets not binding correctly, or material stage evaluation not reaching the draw path
   - Reference: vkDoom3 `DrawSingleInteraction()` + `CommitCurrent()` flow

2. **No depth prepass**
   - Add `VK_RB_FillDepthBuffer()` mirroring `FillDepthBufferFast()` in vkDoom3
   - Bind a depth-only pipeline (no color writes, depth LESS)
   - Required before interaction pass for correct stencil-based shadow masking

3. **Stencil shadow volumes not drawn**
   - The geometry exists (`vLight->globalShadows`, `vLight->localShadows`)
   - The pipeline exists
   - Need to issue draw calls mirroring `StencilShadowPass()` in vkDoom3

### High (material system incomplete)

4. **Dynamic pipeline cache**
   - Create pipelines on demand keyed by `(program index, stateBits)`
   - Required for any material that uses non-standard blend/depth/cull state

5. **Blend mode coverage**
   - Map all `GLS_SRCBLEND_*` / `GLS_DSTBLEND_*` bits to `VkBlendFactor`
   - Currently only opaque and src-alpha are handled

6. **Texture coordinate transforms**
   - Material stages with `texgen`, scroll, rotate are passed as shader registers
   - These register values need to be evaluated and fed to the shaders

### Medium (visual correctness)

7. **Fog / blend lights** — `FogAllLights()` equivalent
8. **Compressed texture formats** (BC1/BC3) — memory and upload performance
9. **Deferred texture deletion** — correctness under GPU pipelining
10. **Per-stage depth function** — some material stages require explicit depth ops

### Low (robustness / future)

11. **Resume render pass** — needed for RT shadow dispatch not to clear prior draws on mobile/tile GPUs
12. **MSAA support** — not needed for raytracing target but useful for rasterization fallback
13. **GPU timing queries** — profiling aid
14. **Skinned shadow volumes** — `LAYOUT_DRAW_SHADOW_VERT_SKINNED`

---

## Recommended Investigation for Current Texture Bug

The most likely causes for "no textures" given the current state:

1. **Material stage not reaching draw** — `VK_RB_DrawShaderPasses` only handles GUI/2D surfaces; 3D surfaces go through `VK_RB_DrawInteractions`. If surfaces are not in any `viewLight->*Interactions` list (e.g., ambient-only surfaces) they may never be drawn.

2. **Descriptor set binding wrong texture** — Check that `vkImageData_t` is non-null and the fallback 1×1 white texture is not being substituted for every slot.

3. **Shader register evaluation** — Material diffuse color registers may evaluate to zero if `EvaluateRegisters()` is not called or the result is not passed to the UBO.

4. **Missing ambient pass** — vkDoom3 renders ambient surfaces in `DrawShaderPasses` using `BUILTIN_INTERACTION` with ambient light. dhewm3_rtx may not have an equivalent ambient lighting pass, leaving surfaces black even when the interaction pass runs.

Compare specifically:
- `vkDoom3/neo/renderer/RenderBackend.cpp` `DrawShaderPasses()` — how ambient / non-lit surfaces are handled
- `dhewm3_rtx/neo/renderer/Vulkan/vk_backend.cpp` `VK_RB_DrawShaderPasses()` — what surfaces are actually drawn
