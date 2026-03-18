# Vulkan Pipeline Comparison: dhewm3_rtx vs vkDOOM3

Comparison as of 2026-03-17.  Reference: `vkDoom3.md`.  Current state: `current_dhewm3_rtx.md`.

---

## Side-by-Side Summary

| Aspect | dhewm3_rtx | vkDOOM3 |
|--------|-----------|---------|
| **Depth prepass** | Implemented (`VK_RB_FillDepthBuffer`) — no alpha-clip | Full depth prepass with alpha-test clip in shader |
| **Shadow volumes** | Implemented (Carmack's Reverse, depth-fail stencil) — draw calls issued per light | Same; also has per-shadow scissor refinement and depth bounds test |
| **Interaction ordering** | global+local shadows → all local interactions → all global interactions → translucent | Per-light: global shadows → local interactions → local shadows → global interactions → translucent |
| **Stencil clear per light** | Yes — `vkCmdClearAttachments(128)` in light scissor | Yes — equivalent stencil reset before each light |
| **Translucent interactions** | `interactionPipelineNoStencil` (stencil disabled) | Stencil ALWAYS via state bits; no-stencil pipeline variant |
| **Fog/blend lights** | SKIPPED in shader passes; no FogAllLights pass | `FogAllLights()` post-interaction pass using `BUILTIN_FOG`/`BUILTIN_BLEND` |
| **Pipeline management** | Fixed pipelines created at startup (6 total) | Dynamic pipeline cache keyed by `(renderProg, stateBits)` — creates on demand |
| **Blend modes** | 2 hardcoded (opaque, src-alpha) | Full `GLS_*` → VkPipelineColorBlendAttachmentState mapping |
| **Shader parameter system** | Single UBO struct per interaction/GUI/shadow | `RENDERPARM_*` enum (60+ params), `CommitCurrent()` writes uniform buffer on each draw |
| **Render pass design** | Single monolithic pass; RT shadow hack closes/reopens with CLEAR | Two passes: `renderPass` (DONT_CARE load) + `renderPassResume` (LOAD) — can suspend without clearing |
| **MSAA** | Not supported | Resolve attachment wired up; configurable sample count |
| **Deferred image deletion** | No — images freed immediately (potential GPU hazard) | `m_imageGarbage[frame]` — freed when frame slot recycles |
| **GPU timing** | Not implemented | `vkCmdWriteTimestamp` bracketing passes |
| **Vertex layouts** | 1 layout (idDrawVert for all geometry) | 3 layouts: `DRAW_VERT`, `SHADOW_VERT`, `SHADOW_VERT_SKINNED` |
| **Index buffers** | Always copied to data ring per draw | Static GPU index buffers; dynamic ring for temp geometry |
| **Skinned geometry** | Not verified | `SHADOW_VERT_SKINNED` layout + joint UBO |
| **Texture coordinate transforms** | Not implemented | Handled via `RENDERPARM_*` matrix params in shaders |

---

## Highest Value Fixes (Prioritized)

### P1 — Critical for correctness

#### 1. Implement `VK_RB_FogAllLights()`
**Gap:** Fog and blend light volumes are currently skipped entirely (both in `DrawShaderPasses` and never rendered otherwise). Atmospheric effects (underwater, fire glow, etc.) are invisible.

**What to do:**
- Add `VK_RB_FogAllLights(cmdBuf)` called after `DrawShaderPasses` in `VK_RB_DrawView`.
- For each `viewLight` where `vLight->lightShader->IsFogLight()` or `IsBlendLight()`: render the proxy volume geometry with depth test EQUAL (only where world geometry exists at that pixel), additive or fog blend.
- Reference: `RB_STD_FogAllLights()` / `RB_FogPass()` / `RB_BlendLight()` in `draw_common.cpp`.
- Requires a new fog pipeline with `depthTestEnable=true`, `depthCompareOp=EQUAL`, appropriate blend state.

**Impact:** Restores all fog and colored blend light effects (significant for atmosphere in many maps).

---

#### 2. Fix Interaction Shadow Ordering to Match GL Path
**Gap:** dhewm3_rtx draws all global+local shadows together, then all interactions. The correct GL ordering is: global shadows → local interactions → local shadows → global interactions.

**What to do:** In `VK_RB_DrawInteractions`, reorder the loop body:
```
1. vLight->globalShadows → shadow pipeline
2. vLight->localInteractions → interaction pipeline (stencil EQUAL 128)
3. vLight->localShadows → shadow pipeline
4. vLight->globalInteractions → interaction pipeline (stencil EQUAL 128)
5. vLight->translucentInteractions → no-stencil pipeline
```
This matches `RB_STD_DrawInteractions` in `draw_interaction.cpp`.

**Impact:** Prevents local light surfaces from being incorrectly shadowed by global shadow volumes that were drawn before local interactions got a chance to render.

---

### P2 — Important for visual quality

#### 3. Full Blend Mode Support in `DrawShaderPasses`
**Gap:** `VK_RB_DrawShaderPasses` only selects between two hardcoded pipelines (opaque / src-alpha). Many material stages use additive, modulate, or other `GLS_*` blend modes. These currently render wrong (as opaque or wrong alpha).

**What to do:**
- Parse `pStage->drawStateBits` (or equivalent) per stage.
- Map `GLS_SRCBLEND_*` / `GLS_DSTBLEND_*` bits to `VkBlendFactor`.
- Create a small set of common pipeline variants (opaque, src-alpha, additive ONE+ONE, additive ONE+ONE with alpha, modulate) or implement a mini pipeline cache.

**Impact:** Particles, decals, glows, lens flares, and many environmental effects render correctly.

---

#### 4. Texture Coordinate Transforms in Interaction Shader
**Gap:** Animated material stages (scroll, rotate, shear) use texture matrix transforms stored in shader registers. These are not passed to the shaders and are silently ignored.

**What to do:**
- Add `texMat0` and `texMat1` (or equivalent) to the interaction UBO.
- In `VK_RB_DrawInteraction`, evaluate and upload `pStage->texture.matrix` registers.
- Apply in `interaction.frag` before sampling.

**Impact:** Many in-game surfaces with animated textures (computer screens, water, fire) will animate correctly.

---

#### 5. Alpha-Test / Alpha-Clip in Depth Prepass
**Gap:** `VK_RB_FillDepthBuffer` draws all `MC_OPAQUE` surfaces unconditionally. Alpha-tested surfaces (fences, grates) need a clip test in the depth shader to avoid incorrect depth writes through transparent pixels.

**What to do:**
- Add `alphaTest` uniform to depth pipeline UBO.
- In depth shader, sample diffuse texture and `discard` if alpha < threshold.
- Only bind the diffuse texture for alpha-tested surfaces; skip the texture bind for fully opaque ones.

**Impact:** Fences, grates, and foliage no longer incorrectly occlude geometry behind them.

---

### P3 — Polish / robustness

#### 6. Resume Render Pass for RT Shadow Interop
**Gap:** When RT shadows are enabled, the code closes the render pass, dispatches RT, then reopens it with `vkCmdBeginRenderPass` using the CLEAR load op — discarding any prior color draws in that frame.

**What to do:**
- Create a second `renderPassResume` with `loadOp=LOAD`, `initialLayout=COLOR_ATTACHMENT_OPTIMAL`.
- Use `renderPassResume` when reopening the pass after the RT dispatch.
- All pipelines must be compatible with both render pass objects (same attachment formats/layouts — they are).

**Impact:** Removes visual flash/clear when RT shadows are active; matches vkDOOM3's resume-pass design.

---

#### 7. Deferred Image Deletion
**Gap:** `VK_Image_Purge` destroys the `VkImage`/`VkImageView`/`VkSampler` immediately. If the GPU is still reading that image (e.g. in a descriptor set from the previous frame), this is a GPU hazard.

**What to do:**
- Add a `imageGarbage[VK_MAX_FRAMES_IN_FLIGHT]` ring similar to vkDOOM3.
- Move destroy calls into the garbage ring; drain when the corresponding frame fence fires.

**Impact:** Prevents potential validation errors and crashes on texture streaming / map loads.

---

#### 8. Per-Shadow Scissor Refinement
**Gap:** Shadow volumes are drawn with the full light scissor. vkDOOM3 sets a tighter scissor per shadow surface.

**What to do:** Before each shadow draw call in `VK_RB_DrawShadowSurface`, compute and set a scissor from the shadow surface's `scissorRect` field (clamped to the light scissor).

**Impact:** GPU perf improvement — fragments outside the shadow surface scissor are culled earlier.

---

## Features Not Applicable to dhewm3_rtx

| vkDOOM3 Feature | Reason Not Needed |
|-----------------|-------------------|
| `RENDERPARM_*` system (60+ params) | dhewm3_rtx uses structured UBOs per pipeline; acceptable for the current fixed pipeline set |
| AMD VMA allocator | GPU memory pressure is currently low; the simple `vkAllocateMemory` path is fine until performance becomes a concern |
| MSAA | RT path doesn't benefit from MSAA; can be added later |
| GPU timing queries | Nice-to-have profiling; low priority |
