# Missing GL Pipeline Pieces — Vulkan Implementation Status

Last updated: 2026-03-21

This document tracks GL features that were absent from `VK_RB_DrawView` at the start of the
Vulkan refactor, and the current implementation status of each.

---
## Rendering Correctness Status (Phase 2 carry-overs)

Updates: 2026-03-25
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


## Status Summary

| # | Feature | Status |
|---|---------|--------|
| 1 | Depth prepass | **Done** |
| 2 | Multiple blend modes | **Done** |
| 3 | Texture coord transforms | **Done** |
| 4 | Stencil shadow volumes | **Done** |
| 5 | Fog and blend lights | **Done** |
| 6 | LightScale / overbright | **Done** |
| 7 | Post-process (`_currentRender`) | **Partial** |
| 8 | Per-stage depth function | **Partial** |
| 9 | Per-surface cull mode | **Partial** |

---

## 1. Depth Prepass — DONE

**Implemented in:** `neo/renderer/Vulkan/vk_backend.cpp` — `VK_RB_FillDepthBuffer`

Draws all opaque and alpha-tested surfaces to the depth buffer before the interaction pass.
Two pipeline variants:
- `vkPipes.depthPipeline` — opaque surfaces, depth LESS, color write disabled.
- `vkPipes.depthClipPipeline` — MC_PERFORATED (alpha-cut) surfaces; fragment shader discards
  where alpha < threshold.

The interaction pipeline uses `VK_COMPARE_OP_LESS_OR_EQUAL` (not EQUAL) intentionally:
MC_PERFORATED surfaces may not have written exact depth values during the prepass, so EQUAL
would reject them. This is a known trade-off noted in the pipeline creation comment.

---

## 2. Multiple Blend Modes — DONE

**Implemented in:** `neo/renderer/Vulkan/vk_pipeline.cpp` — `VK_GetOrCreateGuiBlendPipeline`

All GLS blend mode combinations are mapped to Vulkan blend factors in `VK_GlsSrcBlendToVk` /
`VK_GlsDstBlendToVk`. `VK_GetOrCreateGuiBlendPipeline` lazily creates and caches pipelines
keyed on `(blendBits | depthTest | twoSided)` so every blend mode encountered at runtime gets
a correct pipeline on first use.

---

## 3. Texture Coord Transforms — DONE

**Implemented in:** `neo/renderer/Vulkan/vk_backend.cpp` — `VK_SetGuiTexCoordParams`

When `pStage->texture.hasMatrix` is true, the 2×3 affine UV transform is evaluated from
`regs[]` and written to `VkGuiUBO::texMatrixS/T`. `gui.vert` applies the transform before
writing `vary_TexCoord`. When no matrix, identity rows are uploaded.

Scroll clamping (`[-40, 40]`) mirrors the GL path to avoid precision loss.

Environment map (`TG_REFLECT_CUBE`) and skybox (`TG_SKYBOX_MAP`) texgens are not yet
handled and fall through to the identity path.

---

## 4. Stencil Shadow Volumes — DONE

**Implemented in:** `neo/renderer/Vulkan/vk_backend.cpp` — `VK_RB_DrawShadowSurface`

The stencil shadow pipeline is fully wired:
- `vkPipes.shadowPipelineZFail` / `shadowPipelineZFailMirror` — depth-fail (Carmack's Reverse).
- `vkPipes.shadowPipelineZPass` / `shadowPipelineZPassMirror` — depth-pass (for external / lit
  interiors where camera is guaranteed outside shadow volumes).
- Stencil is cleared to 128 per light before shadow volumes are drawn.
- The interaction pipeline uses `GREATER_OR_EQUAL 128` stencil to pass only lit pixels.
- `VK_RB_DrawShadowSurface` selects Z-fail vs Z-pass based on `surf->space->isExternal`.

Shadow volumes are skipped automatically when RT shadows are active (`r_rtShadows 1`).

---

## 5. Fog and Blend Lights — DONE

**Implemented in:** `neo/renderer/Vulkan/vk_backend.cpp` — `VK_RB_FogAllLights`

Mirrors `RB_STD_FogAllLights` + `RB_FogPass` + `RB_BlendLight` from `draw_common.cpp`.

Pipelines:
- `vkPipes.fogPipeline` — depth EQUAL, front-cull, SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
- `vkPipes.fogFrustumPipeline` — depth LESS, back-cull (frustum cap).
- `vkPipes.blendlightPipeline` — depth EQUAL, front-cull, DST_COLOR / ZERO (modulate).

Fog volumes in `drawSurfs[]` are skipped by `VK_RB_DrawShaderPasses` and handled
exclusively by `VK_RB_FogAllLights`.

---

## 6. LightScale / Overbright — DONE

**Implemented in:** `neo/renderer/glsl/interaction.frag` — `u_LightScale`

`RB_DetermineLightScale()` sets `backEnd.overBright` to account for lights brighter than
`tr.backEndRendererMaxLight`. This factor is uploaded as `u_LightScale` to the interaction
UBO each frame (from `vk_backend.cpp` around the `VK_RB_DrawInteraction` UBO fill).

The interaction shader multiplies its final output by `u_LightScale` before writing
`fragColor`, replacing the separate `RB_STD_LightScale` full-screen quad pass used by the
GL renderer.

---

## 7. Post-Process Shader Passes — PARTIAL

**Implemented in:** `neo/renderer/Vulkan/vk_backend.cpp` — `VK_RB_DrawShaderPasses`

When a surface with `mat->GetSort() >= SS_POST_PROCESS` is encountered, `VK_RB_CopyRender`
is called to blit the current framebuffer into `globalImages->currentRenderImage`. Subsequent
post-process stages can then sample `_currentRender`.

**What is still missing:**
- The Vulkan image layout transitions around the copy (swapchain → TRANSFER_SRC → copy →
  back to COLOR_ATTACHMENT) may not be fully correct under all resolutions/formats.
- Only the first `SS_POST_PROCESS` surface triggers the copy (guarded by
  `backEnd.currentRenderCopied`). If a second copy is needed mid-frame, it won't happen.
- `TG_GLASSWARP` texgen falls back to sampling `_currentRender` directly without the warp
  distortion pass.

Post-process materials are rare in the base game. Defer full correctness until needed.

---

## 8. Per-Stage Depth Function — PARTIAL

**Implemented:** `GLS_DEPTHFUNC_ALWAYS` → no depth test.

In `VK_RB_DrawShaderPasses`, stages where `pStage->drawStateBits & GLS_DEPTHFUNC_ALWAYS`
is set select a pipeline with depth test disabled (`depthTest=false` in
`VK_GetOrCreateGuiBlendPipeline`). This covers UI and decal cases.

**What is still missing:**
- Per-stage override to `VK_COMPARE_OP_LESS` (some additive glow stages).
- The `GLS_DEPTHMASK` bit (enabling depth write in an otherwise blended stage) is not yet
  honoured per-stage.

Low priority — only matters for unusual material configurations.

---

## 9. Per-Surface Cull Mode — PARTIAL

**Implemented:** In-world GUI surfaces (`surf->space->isGuiSurface`) are rendered two-sided.

`VK_GetOrCreateGuiBlendPipeline(... twoSided=true)` sets `VK_CULL_MODE_NONE`, preventing
back-face rejection for flat GUI quads embedded in 3D space.

**What is still missing:**
- `mat->GetCullType() == CT_TWO_SIDED` is not checked for non-GUI world surfaces.
  Vegetation, fences, and any material with `twoSided` or `backsides` keywords still use
  the default back-cull and will have missing faces.
- `CT_BACK_SIDED` (rare) is also not handled.

**Fix:** In `VK_RB_DrawShaderPasses`, replace the `isGuiSurface` check with:
```cpp
const cullType_t ct = mat->GetCullType();
const bool matTwoSided = (ct == CT_TWO_SIDED);
```
Pass `matTwoSided` to `VK_GetOrCreateGuiBlendPipeline`. This requires the key space to
distinguish front-cull, back-cull, and no-cull separately, which the current high-bit scheme
already supports.

---

## Remaining Work

Only §8 and §9 have meaningful outstanding work. Priority:

| Priority | Feature | Complexity | Visual Impact |
|----------|---------|------------|---------------|
| 1 | **Cull modes** (§9) — CT_TWO_SIDED for world surfaces | Low | Low-Medium — fences, vegetation |
| 2 | **Per-stage depth func** (§8) — `GLS_DEPTHMASK` and `GLS_DEPTHFUNC_LESS` | Low | Low |
| 3 | **Post-process** (§7) — layout correctness, second copy | Medium | Low (rare surfaces) |

---

## Files Involved

| File | Role |
|------|------|
| `neo/renderer/Vulkan/vk_backend.cpp` | `VK_RB_FillDepthBuffer`, `VK_RB_DrawShaderPasses`, `VK_RB_FogAllLights`, `VK_RB_DrawShadowSurface`, `VK_RB_DrawInteractions` |
| `neo/renderer/Vulkan/vk_pipeline.cpp` | Pipeline creation for all variants; `VK_GetOrCreateGuiBlendPipeline` |
| `neo/renderer/Vulkan/vk_common.h` | Pipeline fields in `vkPipelines_t` |
| `neo/renderer/glsl/gui.vert` | `texMatrixS/T` UV transform |
| `neo/renderer/glsl/interaction.frag` | `u_LightScale` final multiply |
| `neo/renderer/glsl/fog.vert/.frag` | Fog and blend light shaders |
