# Vulkan Pipeline Comparison: dhewm3_rtx vs vkDOOM3

Comparison as of 2026-03-18.  Reference: `vkDoom3.md`.  Current state: `current_dhewm3_rtx.md`.

---

## Side-by-Side Summary

| Aspect | dhewm3_rtx | vkDOOM3 |
|--------|-----------|---------|
| **Depth prepass** | Implemented — opaque + alpha-clip (MC_PERFORATED) variants | Full depth prepass with alpha-test clip in shader |
| **Shadow volumes** | Implemented (Z-fail and Z-pass variants); per-light stencil clear | Same; also has per-shadow scissor refinement and depth bounds test |
| **Interaction ordering** | global+local shadows → all interactions → translucent | Per-light: global shadows → local interactions → local shadows → global interactions → translucent |
| **Stencil clear per light** | Yes — `vkCmdClearAttachments(128)` in light scissor | Yes — equivalent stencil reset before each light |
| **Translucent interactions** | `interactionPipelineNoStencil` (stencil disabled) | Stencil ALWAYS via state bits; no-stencil pipeline variant |
| **Fog/blend lights** | Pipelines + shaders defined; dispatch being iterated | `FogAllLights()` post-interaction pass fully implemented |
| **Pipeline management** | Fixed pipelines created at startup (11 total) | Dynamic pipeline cache keyed by `(renderProg, stateBits)` — creates on demand |
| **Blend modes** | Multiple modes via `drawStateBits` evaluation (opaque, src-alpha, additive, modulate) | Full `GLS_*` → VkPipelineColorBlendAttachmentState mapping |
| **Shader parameter system** | Single UBO struct per interaction/GUI/shadow; texture matrices in interaction UBO | `RENDERPARM_*` enum (64 params), `CommitCurrent()` writes separate vertex+fragment UBOs on each draw |
| **Texture coordinate transforms** | Evaluated and uploaded per stage in interaction and shader passes | Handled via `RENDERPARM_TEXTUREMATRIX_*` params in shaders |
| **Render pass design** | Two passes: initial (CLEAR) + resume (LOAD) | Two passes: `renderPass` (DONT_CARE load) + `renderPassResume` (LOAD) — identical concept |
| **Deferred image deletion** | Implemented — `imageGarbage[frame]` ring, drained after fence | `m_imageGarbage[frame]` — freed when frame slot recycles |
| **MSAA** | Not supported | Resolve attachment wired up; configurable sample count |
| **GPU timing** | Not implemented | `vkCmdWriteTimestamp` bracketing passes |
| **Vertex layouts** | 1 layout (idDrawVert for all geometry) | 3 layouts: `DRAW_VERT`, `SHADOW_VERT`, `SHADOW_VERT_SKINNED` |
| **Index buffers** | Always copied to data ring per draw | Static GPU index buffers; dynamic ring for temp geometry |
| **Skinned geometry** | Not verified | `SHADOW_VERT_SKINNED` layout + joint UBO |
| **Buffering** | Double-buffered (`VK_MAX_FRAMES_IN_FLIGHT = 2`) | Triple-buffered (`NUM_FRAME_DATA = 3`) |
| **Memory allocator** | Direct `vkAllocateMemory` per buffer | Custom block allocator (`idVulkanAllocator`) or AMD VMA |
| **Staging manager** | Manual staging per upload | `idVulkanStagingManager` — ring buffer, fence-synced, batched uploads |
| **Overbright handling** | Implemented — scale uploaded to interaction UBO | Via `RENDERPARM_DIFFUSEMODIFIER` / `RENDERPARM_SPECULARMODIFIER` |

---

## Resolved / Previously Missing

The following items were missing in the 2026-03-17 snapshot and have since been addressed:

| Item | Previous State | Current State |
|------|---------------|---------------|
| Deferred image deletion | Images freed immediately (GPU hazard) | `imageGarbage[frame]` ring + `VK_Image_DrainGarbage` after fence |
| Resume render pass | Missing; RT reopen used CLEAR (discarded prior draws) | `renderPassResume` (LOAD op) created; used when reopening after RT dispatch |
| Alpha-clip depth prepass | Missing (all perforated surfaces wrote wrong depth) | `depthClipPipeline` + `depth_clip.frag` — samples diffuse, discards on alpha < threshold |
| Texture coordinate transforms | Not implemented | Evaluated per stage and uploaded to interaction UBO; applied in shaders |
| Blend mode support | Only 2 hardcoded modes | `drawStateBits` evaluated per material stage; multiple blend modes mapped |
| Overbright light handling | Missing | Scale uploaded to interaction UBO and applied in fragment shader |
| Fog/blend light pipelines | No pipelines or shaders | `fogPipeline`, `fogFrustumPipeline`, `blendlightPipeline` + shaders added |
| Z-pass shadow variant | Missing | `shadowPipelineZPass` added alongside Z-fail |

---

## Remaining Gaps (Prioritized)

### P1 — Critical for correctness

#### 1. Complete `VK_RB_FogAllLights()` Dispatch
**Gap:** Fog and blend light pipelines and shaders exist, but the render dispatch loop is still being iterated for correctness.

**What to do:**
- For each `viewLight` where `IsFogLight()`: bind `fogPipeline`, draw proxy volume geometry with depth EQUAL (only where world exists at pixel)
- For each `viewLight` where `IsBlendLight()`: bind `blendlightPipeline`, draw with DST_COLOR/ZERO modulate blend
- For fog frustum caps: bind `fogFrustumPipeline` (depth LESS, back-cull)
- Reference: `RB_STD_FogAllLights()` / `RB_FogPass()` / `RB_BlendLight()` in `draw_common.cpp`

**Impact:** Restores all fog and colored blend light effects (significant for atmosphere in many maps).

---

#### 2. Fix Interaction Shadow Ordering to Match GL Path
**Gap:** dhewm3_rtx draws all shadows together, then all interactions. The correct GL ordering is: global shadows → local interactions → local shadows → global interactions.

**What to do:** In `VK_RB_DrawInteractions`, reorder the loop body:
```
1. vLight->globalShadows → shadow pipeline
2. vLight->localInteractions → interaction pipeline (stencil EQUAL 128)
3. vLight->localShadows → shadow pipeline
4. vLight->globalInteractions → interaction pipeline (stencil EQUAL 128)
5. vLight->translucentInteractions → no-stencil pipeline
```
This matches `RB_STD_DrawInteractions` in `draw_interaction.cpp`.

**Impact:** Prevents local light surfaces from being incorrectly shadowed by global shadow volumes drawn before local interactions.

---

#### 3. Geometry Rendering Correctness ("still triangles")
**Gap:** Recent commits indicate geometry is rendering as triangles or in an incorrect state, suggesting a pipeline or vertex input configuration issue.

**What to investigate:**
- Vertex input binding stride vs `sizeof(idDrawVert)` mismatch
- Vertex attribute offsets in `vk_pipeline.cpp` vs actual `idDrawVert` layout
- Index buffer binding type and format (`VK_INDEX_TYPE_UINT32` vs `UINT16`)
- Topology setting in pipeline (`TRIANGLE_LIST` vs `TRIANGLE_STRIP`)

**Impact:** Blocking — nothing renders correctly until resolved.

---

### P2 — Important for visual quality

#### 4. Complete RT Shadow TLAS Population and Dispatch
**Gap:** BLAS building works. TLAS structure is allocated but the per-frame entity→TLAS instance population loop and the shadow ray dispatch are incomplete.

**What to do:**
- In `VK_RT_RebuildTLAS()`: iterate `backEnd.viewDef->viewEntitys`, map entity transforms to `VkAccelerationStructureInstanceKHR`, fill instance buffer, build TLAS
- In `VK_RT_DispatchShadowRays()`: bind RT pipeline + descriptor sets (TLAS, shadow mask, depth sampler, per-light params UBO), call `vkCmdTraceRaysKHR`

**Impact:** Enables RT shadow mode which is the primary goal of the project.

---

#### 5. Per-Shadow Scissor Refinement
**Gap:** Shadow volumes are drawn with the full light scissor. vkDOOM3 sets a tighter scissor per shadow surface.

**What to do:** Before each shadow draw call, compute scissor from the shadow surface's `scissorRect` field (clamped to the light scissor), set via `vkCmdSetScissor`.

**Impact:** GPU perf improvement — fragments outside the shadow surface scissor culled earlier.

---

### P3 — Polish / robustness

#### 6. Per-Stage Depth Function and Cull Mode
**Gap:** Material stages can specify depth compare op and two-sided rendering. These are not read from material state.

**What to do:** Read `pStage->drawStateBits` depth function bits and cull mode bits; select the appropriate pipeline or add pipeline variants.

**Impact:** Some surfaces with non-standard depth ops render incorrectly.

---

#### 7. GPU Index Buffer Usage
**Gap:** Index data is always copied to the per-frame data ring even when the mesh has a static GPU index buffer.

**What to do:** Check if the surface has a cached GPU index buffer (similar to how `VK_VertexCache_GetBuffer` works for vertices); bind it directly instead of copying.

**Impact:** Reduces host memory traffic and copy overhead for static geometry.

---

## Features Not Applicable to dhewm3_rtx

| vkDOOM3 Feature | Reason Not Needed |
|-----------------|-------------------|
| `RENDERPARM_*` system (64 params) | dhewm3_rtx uses structured UBOs per pipeline; acceptable for the current fixed pipeline set |
| AMD VMA allocator | GPU memory pressure is currently low; direct `vkAllocateMemory` is fine until performance becomes a concern |
| MSAA | RT path doesn't benefit from MSAA; can be added later if needed |
| GPU timing queries | Nice-to-have profiling; low priority |
| Triple buffering | Double buffering is sufficient for the current workload |
| Skinned shadow vertex layout | Not yet exercised; can be added when skeletal animation is validated |
