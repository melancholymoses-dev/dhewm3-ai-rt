# Stencil Shadow Symbol Map (Math <-> Code)

Audience: companion for explain_pipeline.md and explain_pipeline_math.md.

Goal:
1. Give a paper-style symbol glossary for the stencil shadow solver.
2. Map each symbol to concrete Vulkan/C++/GLSL variables in this repo.
3. Make it easy to debug by following one per-light stencil pass end to end.

---

## 1) The Quantity Being Computed

For each light $l$ and shaded point $x$, stencil shadows approximate a binary visibility:

$$
V_l(x) \in \{0,1\}
$$

Interpretation in this renderer:
- $V_l(x)=1$: interaction pass is allowed to add light contribution.
- $V_l(x)=0$: interaction pass is rejected by stencil test.

In Doom3-style counting form, we keep a stencil counter $S_l(p)$ per pixel $p$.
The interaction pass uses threshold test $S_l(p) \ge S_0$ where $S_0 = 128$.

---

## 2) Symbol Table (Paper Notation)

| Symbol | Type | Meaning |
|---|---|---|
| $x_m$ | vec4 | Local/model-space shadow volume vertex |
| $l_m$ | vec4 | Light origin in model space |
| $PVM$ | mat4 | Model-view-projection matrix for shadow pass |
| $x'_m$ | vec4 | Extruded shadow vertex (possibly at infinity) |
| $d_{sv}$ | scalar | Depth of shadow volume fragment |
| $d_{scene}$ | scalar | Scene depth from prepass |
| $S_l(p)$ | uint8 | Per-light stencil value at pixel |
| $S_0$ | uint8 | Stencil baseline reference value (128) |
| $\Delta_f, \Delta_b$ | op | Front/back-face stencil increments/decrements |
| $V_l(x)$ | binary | Per-light visibility used by interaction pass |

---

## 3) Shader-Level Mapping (Extrusion Equation)

Shadow volume extrusion lives in `neo/renderer/glsl/shadow.vert`.

Math form:

$$
x'_m = x_m - l_m
$$
$$
x'_m = x'_m + w(x'_m) \cdot l_m
$$

where the encoded input $w \in \{0,1\}$ chooses:
- $w=1$: keep cap vertex near original position.
- $w=0$: extrude to infinity direction from light.

Code mapping:
- $x_m$ -> `in_Position`
- $l_m$ -> `u_LightOrigin`
- $PVM$ -> `u_ModelViewProjection`
- $x'_m$ -> `vPos`
- clip output -> `gl_Position = u_ModelViewProjection * vPos`

CPU-side upload backing this UBO is in `VkShadowUBO` and `VK_RB_DrawShadowSurface` in `neo/renderer/Vulkan/vk_backend.cpp`.

---

## 4) C++ Mapping For Shadow UBO Inputs

Inside `VK_RB_DrawShadowSurface` (`neo/renderer/Vulkan/vk_backend.cpp`):

1. Compute model-local light origin:
- `R_GlobalPointToLocal(..., backEnd.vLight->globalLightOrigin, localLight)`

2. Compute MVP used by shadow vertex shader:
- `VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp)`

3. Upload to `VkShadowUBO`:
- `ubo->lightOrigin[0..2] = localLight`
- `memcpy(ubo->mvp, mvp, 64)`

So the symbol mapping is:
- $l_m$ -> `localLight` -> `ubo->lightOrigin`
- $PVM$ -> `mvp` -> `ubo->mvp`

---

## 5) Per-Light Stencil Lifecycle (Runtime Flow)

The per-light orchestration is in `VK_RB_DrawInteractions` (`neo/renderer/Vulkan/vk_backend.cpp`).

For each `vLight`:

1. Clear stencil in light scissor to baseline:
- `vkCmdClearAttachments`
- `clearAtt.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT`
- `clearAtt.clearValue.depthStencil.stencil = 128`

2. If stencil shadows active (`!VK_RTShadowsEnabled()`), draw shadow volumes:
- global shadows
- local interactions
- local shadows
- global interactions

3. Draw interaction pipeline with stencil comparison enabled.

This gives the threshold classifier:

$$
V_l(x) = \mathbf{1}[S_l(p) \ge 128]
$$

where $p$ is the pixel containing $x$.

---

## 6) Stencil Compare In Interaction Pipeline

In `VK_CreateInteractionPipeline` (`neo/renderer/Vulkan/vk_pipeline.cpp`):

- `depthStencil.stencilTestEnable = VK_TRUE` (opaque path)
- `depthStencil.front.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL`
- `depthStencil.front.reference = 128`
- `depthStencil.back = depthStencil.front`

So interaction shading only contributes where current stencil is at least the baseline.
Translucent interactions use `interactionPipelineNoStencil` by design.

---

## 7) Z-Fail (Carmack's Reverse) Operator Mapping

Pipeline constructor: `VK_CreateShadowPipelineZFail` in `neo/renderer/Vulkan/vk_pipeline.cpp`.

Key configuration:
- depth test enabled, depth write disabled
- `depthCompareOp = VK_COMPARE_OP_LESS`
- stencil enabled
- color writes disabled (stencil-only pass)

Face operators (non-mirror):
- front `depthFailOp = DECREMENT_AND_WRAP`
- back  `depthFailOp = INCREMENT_AND_WRAP`

Mirror variant swaps these.

Math interpretation:
- $\Delta_f, \Delta_b$ act only on depth-fail events.
- Net signed count relative to $S_0$ indicates inside/outside shadow volume at scene depth.

---

## 8) Z-Pass Operator Mapping

Pipeline constructor: `VK_CreateShadowPipelineZPass` in `neo/renderer/Vulkan/vk_pipeline.cpp`.

Key configuration:
- depth test enabled, depth write disabled
- `depthCompareOp = VK_COMPARE_OP_LESS`
- stencil enabled
- color writes disabled

Face operators (non-mirror):
- front `passOp = DECREMENT_AND_WRAP`
- back  `passOp = INCREMENT_AND_WRAP`
- `depthFailOp = KEEP`

Mirror variant swaps front/back sign.

Math interpretation:
- updates happen on depth-pass crossings instead of depth-fail crossings.
- used when camera is outside volume (`external` branch in `VK_RB_DrawShadowSurface`).

---

## 9) Branch Selection Symbols -> Code

Selection of Z-fail vs Z-pass and index sets is in `VK_RB_DrawShadowSurface` (`neo/renderer/Vulkan/vk_backend.cpp`).

Symbolized decision variables:
- $I_{inside}$ (viewer inside shadow projection) -> `(surf->dsFlags & DSF_VIEW_INSIDE_SHADOW)`
- $I_{light}$ (viewer inside light) -> `backEnd.vLight->viewInsideLight`
- cap visibility bits -> `backEnd.vLight->viewSeesShadowPlaneBits`, `tri->shadowCapPlaneBits`
- index subset selector -> `numIndexes`, `numShadowIndexesNoFrontCaps`, `numShadowIndexesNoCaps`
- mode flag (`external`) -> picks Z-pass when true, Z-fail when false

Pipeline pick:
- `external ? shadowPipelineZPass : shadowPipelineZFail`
- mirror-adjusted variants selected with `effectiveMirrorOps`

---

## 10) Why Stencil Baseline Is 128 (Not 0)

This Vulkan path mirrors Doom3 GL semantics using midpoint reference.

Practical behavior:
- clear to 128 per light means "initially lit".
- shadow volumes decrement/increment around this baseline.
- interaction uses `>=128` lit test.

This avoids cross-light contamination and preserves signed counting room with wrap ops.

---

## 11) Debugging Checklist (Symbol-Driven)

If shadows look wrong, check in this order:

1. Baseline clear:
- Is stencil cleared to 128 each light and within intended scissor?

2. Interaction compare:
- Is interaction pipeline using `GEQUAL 128` (or intentionally disabled)?

3. Face convention:
- Is front-face winding consistent with Y-flip (`VK_FRONT_FACE_CLOCKWISE`)?
- Are mirror variants selected correctly?

4. Z-fail/Z-pass mode:
- Is `external` branch selection stable with camera movement?
- Are index subsets (`no caps`, `no front caps`, `full`) coherent for the current geometry?

5. Geometry source consistency:
- Are shadow vertices/indices coming from coherent cache/CPU sources for the draw?

---

## 12) Minimal End-to-End Equation Summary

Per light and pixel:

$$
S_l(p) \leftarrow S_0 = 128
$$

Apply oriented stencil updates from rasterized shadow volume crossings:

$$
S_l(p) \leftarrow S_l(p) + \sum \Delta_{face,event}
$$

Then interaction contribution gate:

$$
V_l(x) = \mathbf{1}[S_l(p) \ge 128], \quad L_l(x) \leftarrow V_l(x) \cdot L_l^{raw}(x)
$$

This is the stencil analogue of the RT visibility test, expressed as rasterized topological counting instead of ray-segment intersection.
