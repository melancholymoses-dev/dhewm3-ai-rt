# Missing GL Pipeline Pieces — Vulkan Implementation Plan

This document lists the rendering features present in `RB_STD_DrawView` (the GL backend,
`draw_common.cpp`) that are absent or incomplete in `VK_RB_DrawView` (`vk_backend.cpp`).
Ordered by impact on visual correctness.

---

## 1. Depth Prepass (`RB_STD_FillDepthBuffer`)

**GL equivalent:** `draw_common.cpp:581`
**Impact:** Without a depth prepass, z-fighting and incorrect overdraw occur on 3D geometry.
The interaction pass in GL uses `GLS_DEPTHFUNC_EQUAL` (draw only where depth exactly matches),
which requires the depth buffer to already be filled. The Vulkan interaction pipeline currently
uses `VK_COMPARE_OP_LESS_OR_EQUAL` as a workaround.

**What GL does:**
- Only runs for 3D views (`viewDef->viewEntitys != NULL`). 2D/menu views skip it entirely.
- Renders all opaque surfaces with depth write enabled, `GL_LESS`, color write disabled.
- Handles alpha-tested surfaces (cutouts like grates/foliage) by binding their diffuse texture
  and using `GL_ALPHA_TEST` in the depth pass so holes are cut correctly.
- Handles `weaponDepthHack` and `modelDepthHack` (depth range remapping for viewmodel/particle
  z-fighting prevention).

**Vulkan plan:**
1. Add a new Vulkan pipeline: `depthPipeline` — depth write ON, `VK_COMPARE_OP_LESS`,
   color write mask = 0 (no color output).
   - Two variants: opaque (no alpha test) and alpha-tested.
   - For alpha-tested: the fragment shader discards fragments where `texture.a < 0.5`;
     needs the diffuse texture bound.
2. Add `VK_RB_FillDepthBuffer(VkCommandBuffer cmd)` in `vk_backend.cpp`:
   - Skip if `!backEnd.viewDef->viewEntitys` (2D views).
   - Iterate `viewDef->drawSurfs[]`.
   - For each surface, check `mat->Coverage()`: skip `MC_TRANSLUCENT`; use alpha-test variant
     for `MC_PERFORATED`; use opaque variant for `MC_OPAQUE`.
3. Call it at the top of `VK_RB_DrawView`, before `VK_RB_DrawShaderPasses`.
4. After the depth prepass is in place, change the interaction pipeline back to
   `VK_COMPARE_OP_EQUAL` (strict equality match, same as GL).

**Note on `depthPipeline`:** The depth pipeline already exists in `vkPipelines_t` and
`VK_InitPipelines` (it was stubbed earlier). Wire up the actual draw calls.

---

## 2. Multiple Blend Modes in Shader Pass (`VK_RB_DrawShaderPasses`)

**GL equivalent:** `draw_common.cpp:915`, `GL_State(pStage->drawStateBits)`
**Impact:** Many materials use blend modes other than SRC_ALPHA/ONE_MINUS_SRC_ALPHA — notably
additive blending (`GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE`) used by glows, lens flares,
weapon muzzle flashes, and most particle effects. Currently those surfaces either don't draw
or draw with the wrong blend.

**GL blend mode → Vulkan factor mapping:**

| GLS flag | Vulkan factor |
|---|---|
| `GLS_SRCBLEND_ONE` | `VK_BLEND_FACTOR_ONE` |
| `GLS_SRCBLEND_ZERO` | `VK_BLEND_FACTOR_ZERO` |
| `GLS_SRCBLEND_DST_COLOR` | `VK_BLEND_FACTOR_DST_COLOR` |
| `GLS_SRCBLEND_ONE_MINUS_DST_COLOR` | `VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR` |
| `GLS_SRCBLEND_SRC_ALPHA` | `VK_BLEND_FACTOR_SRC_ALPHA` |
| `GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA` | `VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA` |
| `GLS_DSTBLEND_ONE` | `VK_BLEND_FACTOR_ONE` |
| `GLS_DSTBLEND_ZERO` | `VK_BLEND_FACTOR_ZERO` |
| `GLS_DSTBLEND_SRC_COLOR` | `VK_BLEND_FACTOR_SRC_COLOR` |
| `GLS_DSTBLEND_ONE_MINUS_SRC_COLOR` | `VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR` |
| `GLS_DSTBLEND_SRC_ALPHA` | `VK_BLEND_FACTOR_SRC_ALPHA` |
| `GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA` | `VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA` |
| `GLS_DSTBLEND_DST_ALPHA` | `VK_BLEND_FACTOR_DST_ALPHA` |
| `GLS_DSTBLEND_ONE_MINUS_DST_ALPHA` | `VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA` |

**Vulkan plan:**
Vulkan requires blend state baked into the pipeline object at creation time.
Creating one pipeline per blend mode combination would be ≈ 100+ pipelines. Two approaches:

- **Option A (Simple):** Pre-create pipelines for the ~5 common combinations used by Doom 3
  materials:
  - Opaque (blend disabled)
  - `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` — UI transparency
  - `ONE / ONE` — additive (glows, particles, lens flares)
  - `DST_COLOR / ZERO` — modulate/multiply (darkening)
  - `ONE / ONE_MINUS_SRC_ALPHA` — premultiplied alpha

  At draw time, select the nearest matching pipeline. Materials with unusual blend modes fall
  back to the opaque pipeline (wrong but won't crash).

- **Option B (Correct):** Use a pipeline cache keyed on `(srcBlend, dstBlend)` with lazy
  creation. Create the pipeline on first use and cache it.

Option A is simpler to start; Option B is more correct long-term.

**Also add the GL skips to `VK_RB_DrawShaderPasses`:**
- Skip `(GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE)` stages — these are alpha mask passes that
  are a no-op when not using fixed-function alpha masking (`draw_common.cpp:915`).
- Skip stages where additive color is black: `src=ONE, dst=ONE` and all color channels ≤ 0
  (`draw_common.cpp:1206`).
- Skip stages where alpha-blend is fully transparent: `src=SRC_ALPHA, dst=ONE_MINUS_SRC_ALPHA`
  and alpha ≤ 0 (`draw_common.cpp:1213`).

---

## 3. Texture Coordinate Transforms (`pStage->texture.hasMatrix`)

**GL equivalent:** `RB_PrepareStageTexturing` in `draw_common.cpp`
**Impact:** Any material using `scroll`, `rotate`, `scale`, `centerScale`, `shear`, or
`translate` texture transforms will display static/incorrect UVs. This affects animated
surfaces (water, fire, scrolling signs) and environment-mapped surfaces.

**What GL does:** Builds a 2×3 texture matrix from `pStage->texture.matrix[0][1][2]`
(evaluated from shader registers `regs[]`) and applies it as a GL texture matrix.

**Vulkan plan:**
Extend `VkGuiUBO` (or add a new `VkShaderPassUBO`) to include a `texMatrix` field:

```c
struct VkShaderPassUBO {
    float modelViewProjection[16]; // 64 bytes
    float colorModulate[4];        // 16 bytes
    float colorAdd[4];             // 16 bytes
    float texMatrixS[4];           // 16 bytes — row 0 of 2D affine transform
    float texMatrixT[4];           // 16 bytes — row 1
};                                 // 128 bytes total
```

In `gui.vert`, apply the transform:
```glsl
vec2 tc = vec2(
    dot(vec4(in_TexCoord, 0.0, 1.0), u_TexMatrixS),
    dot(vec4(in_TexCoord, 0.0, 1.0), u_TexMatrixT)
);
vary_TexCoord = tc;
```

When `pStage->texture.hasMatrix` is false, upload identity rows: `(1,0,0,0)` and `(0,1,0,0)`.

When true, evaluate from `regs[]` (same logic as `draw_common.cpp:1109`):
```cpp
texS = { regs[matrix[0][0]], regs[matrix[0][1]], 0, regs[matrix[0][2]] };
texT = { regs[matrix[1][0]], regs[matrix[1][1]], 0, regs[matrix[1][2]] };
// clamp scroll to [-40, 40] to avoid precision loss
```

**Note:** Environment map (`texgen == TG_REFLECT_CUBE`) and skybox (`texgen == TG_SKYBOX_MAP`)
texture generation require separate handling — those compute UVs from the view/surface normal,
not from vertex UVs. They are rare and can be deferred.

---

## 4. Stencil Shadow Volumes

**GL equivalent:** `RB_ARB2_DrawInteractions` → `RB_StencilShadowPass`
**Current status:** Shadow pipeline bound, `TODO` comment, no draw calls.

**What GL does (Carmack's Reverse / depth-fail):**
1. For each light: iterate `vLight->globalShadows` and `vLight->localShadows`.
2. Clear stencil to 128 for this light's scissor rect.
3. Depth-fail: front faces decrement, back faces increment stencil.
4. After shadow volumes are drawn, use `stencilFunc(GL_EQUAL, 128, 255)` for interaction pass
   (only lit areas where stencil was not modified).

**Shadow vertex data:** Shadow volumes use `geo->shadowCache` (not `ambientCache`). Each
vertex is `shadowCache_t = vec4` where `w = 0` (finite) or `w = 1` (infinite extrusion at
light). The vertex shader extrudes `w=1` verts to infinity using the light position.

**Vulkan plan:**
- The shadow pipeline (`vkPipes.shadowPipeline`) already exists with correct stencil ops.
- Need a shadow vertex shader that performs the extrusion: if `in_Position.w == 1.0`,
  extrude to light infinity; otherwise use `in_Position.xyz` directly.
- Need a UBO for the shadow pass: at minimum, `modelViewProjection` + `localLightOrigin`.
- Add `VK_RB_DrawShadowVolumes(VkCommandBuffer cmd, viewLight_t *vLight)` that:
  1. Clears stencil for the light's scissor (via `vkCmdClearAttachments` with stencil aspect).
  2. Binds shadow pipeline.
  3. Iterates `vLight->globalShadows` and `vLight->localShadows`, binds their shadow cache
     vertex buffers (upload via data ring), and draws.
- Call this from within the light loop in `VK_RB_DrawInteractions`, between scissor set and
  interaction draw.

**RTX note:** When `r_rtShadows` is enabled, the RT shadow mask replaces stencil shadows
entirely. The stencil shadow path is only needed for the non-RT fallback.

---

## 5. Fog and Blend Lights (`RB_STD_FogAllLights`)

**GL equivalent:** `draw_common.cpp:2013`
**Impact:** Areas with `fog` or `blendLight` lights have no atmospheric effect. Caves, dark
corridors, and the game's characteristic fog effects are all missing.

**What GL does for fog lights:**
- Iterates `viewLight_t` entries where `lightShader->IsFogLight()` or `IsBlendLight()`.
- Projects the light frustum as geometry over the affected surface.
- Blends the fog color with `GL_BLEND`.
- Uses `GL_EQUAL 128` stencil to avoid double-fogging pixels.

**Vulkan plan:**
1. Add `VK_RB_FogAllLights(VkCommandBuffer cmd)` after `VK_RB_DrawInteractions`.
2. For each fog/blend light in `viewDef->viewLights`:
   - Use the interaction pipeline's vertex input (positions only needed).
   - Build a fog pass UBO: light project planes, fog color/density from `lightShader->GetStage(0)`.
   - Iterate `vLight->globalInteractions + localInteractions` (fog covers same geometry as light).
   - Blend: fog typically `(0, SRC_ALPHA)` or material-specified blend.
3. A new `fogPipeline` may be needed, or the GUI alpha pipeline can be reused with appropriate
   UBO inputs if the fog shader is simple enough.

This is non-trivial because the fog geometry projection (TexGen in GL) has to be replicated
in the vertex shader using the light projection planes passed via UBO.

---

## 6. LightScale / Overbright Correction (`RB_STD_LightScale`)

**GL equivalent:** `draw_common.cpp:2084`
**Impact:** Without this, scenes are either darker or brighter than intended depending on
`r_lightScale` and `r_brightness` settings.

**What GL does:** After interaction pass, multiplies the entire framebuffer by `overBright`
using a full-screen quad with `DST_COLOR * SRC_COLOR` blend, repeated in a bit-shift loop
until the desired brightness is reached.

**Vulkan plan:**
The cleanest Vulkan equivalent is to fold the overbright factor into the interaction shader's
final color output instead of doing a separate full-screen pass. The interaction shader already
has a `gammaBrightness` uniform. Simply multiply the output color by `r_lightScale` before
writing `fragColor`.

If a post-pass is preferred (e.g. for correctness with HDR), use a full-screen triangle with
a `DST_COLOR / SRC_COLOR` blend pipeline — but folding into the shader is simpler.

**Recommended:** Add `float u_LightScale` to `VkInteractionUBO` and multiply by it in
`interaction.frag` before writing `fragColor`. Upload from `backEnd.overBright` (set by
`RB_DetermineLightScale()`, which is already called in `VK_RB_DrawInteractions`).

---

## 7. Post-Process Shader Passes and `_currentRender`

**GL equivalent:** `draw_common.cpp:1328` (second call to `RB_STD_DrawShaderPasses` after
fog lights)
**Impact:** Materials with `sort postProcess` (water surfaces, glass refraction, heat haze,
bloom-style effects) read from `_currentRender` — a copy of the framebuffer taken mid-frame.
Without this, those materials either black out or render incorrectly.

**What GL does:**
- After fog lights, calls `RB_STD_DrawShaderPasses` again for the remaining unprocessed surfs
  (those with `GetSort() >= SS_POST_PROCESS`).
- Before drawing, calls `globalImages->currentRenderImage->CopyFramebuffer(...)` to copy
  the rendered scene into a texture that the shader can sample.
- The shader reads from `_currentRender` to distort/blend the background.

**Vulkan plan:**
This requires a mid-frame "image copy to texture" operation, which in Vulkan means:
1. End the current render pass.
2. Transition the swapchain image from `COLOR_ATTACHMENT_OPTIMAL` to `TRANSFER_SRC_OPTIMAL`.
3. Copy (or blit) to a pre-allocated `_currentRender` VkImage.
4. Transition both images back to their required layouts.
5. Re-open the render pass to continue drawing.

This is moderately complex. **Defer until basic rendering is stable.**
For now, detect `mat->GetSort() >= SS_POST_PROCESS` in `VK_RB_DrawShaderPasses` and skip
those surfaces with a `common->DWarning` log message so they fail silently rather than
producing corruption.

---

## 8. Per-Stage Depth Function

**GL equivalent:** `GL_State(pStage->drawStateBits)` maps `GLS_DEPTHFUNC_*` bits
**Impact:** Some stages override the depth function (e.g., `GLS_DEPTHFUNC_ALWAYS` for
decals that must draw regardless of depth, `GLS_DEPTHFUNC_LESS` for additive glows).

**Vulkan plan:**
Like blend modes, depth func is baked into pipeline state. The common cases are:
- Default (LESS_OR_EQUAL) — covers most surfaces
- ALWAYS — used for UI and decals; the GUI pipeline already has depth test disabled

For the shader pass, the existing opaque/alpha pipeline pair likely covers most cases.
If a surface sets `GLS_DEPTHFUNC_ALWAYS` and is not a pure GUI surface, the GUI pipeline
(depth disabled) can be used. Add a check: if `drawStateBits & GLS_DEPTHFUNC_ALWAYS`, select
the GUI pipeline regardless of blend mode.

---

## 9. Per-Stage and Per-Surface Cull Mode

**GL equivalent:** `RB_STD_T_RenderShaderPasses` → `GL_Cull` based on `shader->GetCullType()`
**Impact:** Two-sided surfaces (vegetation, fences, some particle planes) are incorrectly
back-face culled. Surfaces that should cull back faces may leak.

**What GL does:** Sets `GL_CULL_FACE` / `GL_FRONT` / `GL_BACK` / disabled per material,
based on `material->GetCullType()` returning `CT_FRONT_SIDED`, `CT_BACK_SIDED`, or
`CT_TWO_SIDED`.

**Vulkan plan:**
Cull mode is also baked into pipeline state.
- Add three variants of the GUI pipeline: front, back, and none cull.
- At draw time, read `surf->material->GetCullType()` and select the appropriate variant.
- This triples the GUI pipeline count (6 total: 3 cull × 2 blend) but is straightforward.
- Alternatively, use `VK_DYNAMIC_STATE_CULL_MODE` (Vulkan 1.3 / `VK_EXT_extended_dynamic_state`)
  to set cull mode dynamically without extra pipelines. Check device feature support at init.

---

## Implementation Order

| Priority | Feature | Complexity | Visual Impact |
|---|---|---|---|
| 1 | **Blend modes** (§2) | Medium | High — particles, glows, additive effects |
| 2 | **Depth prepass** (§1) | Medium | High — z-fighting on 3D scenes |
| 3 | **Texture coord transforms** (§3) | Low-Medium | Medium — animated surfaces |
| 4 | **LightScale** (§6) | Low | Medium — brightness correctness |
| 5 | **Cull modes** (§9) | Low | Low-Medium — two-sided surfaces |
| 6 | **Stencil shadows** (§4) | High | Medium — shadow correctness (RT path replaces) |
| 7 | **Fog/blend lights** (§5) | High | Medium — atmospheric areas |
| 8 | **Post-process** (§7) | High | Low (rare surfaces) — defer |
| 9 | **Per-stage depth func** (§8) | Low | Low |

---

## Files Involved

| File | Changes |
|---|---|
| `neo/renderer/Vulkan/vk_backend.cpp` | New: `VK_RB_FillDepthBuffer`, `VK_RB_FogAllLights`, shadow draw calls; extend `VK_RB_DrawShaderPasses` for blend/cull/skip logic |
| `neo/renderer/Vulkan/vk_pipeline.cpp` | New pipelines: depth variants, fog, additional blend modes or blend pipeline cache |
| `neo/renderer/Vulkan/vk_common.h` | New pipeline fields in `vkPipelines_t` |
| `neo/renderer/glsl/gui.vert` | Add `u_TexMatrixS/T` and apply transform |
| `neo/renderer/glsl/interaction.frag` | Add `u_LightScale` and multiply output |
| `neo/renderer/glsl/shadow.vert` | Add extrusion logic (`w==1` → infinity) |
| `neo/renderer/glsl/fog.vert/.frag` | New shaders for fog light pass |

