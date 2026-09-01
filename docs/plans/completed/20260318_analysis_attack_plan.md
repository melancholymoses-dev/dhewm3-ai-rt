# Depth / Ordering / Triangle Artifact Analysis Plan

As of 2026-03-18. Screenshots: Mars City Hangar (shots 1-3), interior corridor (shot 4), Departure Lounge (shot 5).

---

## Visual Symptom Inventory

| Shot | Area | Symptom |
|------|------|---------|
| 1-3 | Outdoor Hangar | Brown/amber fill + black triangle wireframe lines + white/cyan overlit edges. Geometry appears flat, no diffuse lighting. |
| 4 | Interior corridor | Mostly correct textures. Window renders white instead of transparent. Far corridor visible through wall. Black triangle seam artifacts. |
| 5 | Departure Lounge | Lighting mostly correct. Red vending machine (far) appears in front of nearer counter/bench. |

---

## Hypotheses (ranked by likelihood)

### H1 — LIKELY PRIMARY: GUI pipeline has no depth test
**Where:** `VK_CreateGuiPipelineEx` → `depthTestEnable = VK_FALSE`, `depthWriteEnable = VK_FALSE`
**What it causes:** Every surface drawn via `VK_RB_DrawShaderPasses` (which uses the GUI pipeline) renders without depth testing. It floats in front of everything already written to the framebuffer by the interaction pass. Draw order is submission order, not depth order.

**GL reference:** `RB_STD_DrawShaderPasses` calls `GL_State(pStage->drawStateBits)` — the depth function is encoded in `drawStateBits`, so 3D surfaces get `GLS_DEPTHFUNC_LESS`. Our `VK_GetOrCreateGuiBlendPipeline(drawStateBits)` only keys on blend bits and ignores depth bits.

**Explains:**
- Shot 5: Vending machine (far, drawn by shader pass) appears over counter (near, drawn by interaction pass).
- Shot 4: Far wall visible through near wall — geometry drawn by shader pass ignores near wall's depth.
- Shot 4: Window renders white — sky/exterior surface drawn on top of everything, at alpha 1.0 (incorrect blend order).
- Shots 1-3: If interaction pass draws nothing (see H2), only shader pass geometry is visible, all floating with no depth ordering, producing the layered triangle pattern.

**How to distinguish from H2:** Temporarily disable `VK_RB_DrawShaderPasses` call entirely.
- If shots 4-5 depth ordering fixes but shots 1-3 are now black/empty → H1 is the shader pass depth issue; H2 is a separate stencil issue.
- If shots 1-3 are now correctly lit → both problems caused by shader pass.

---

### H2 — LIKELY CONTRIBUTING: Shadow stencil zeros interaction pass in outdoor areas

**Where:** `VK_RB_DrawShadowSurface` selecting Z-pass vs Z-fail via `DSF_VIEW_INSIDE_SHADOW`.

**What it causes:** If Z-pass is used when the camera is actually inside a shadow volume, the stencil arithmetic is wrong. Front faces that pass depth increment stencil (128→129), but back faces that are occluded by geometry don't fire. Net result: stencil 129 at pixels inside the shadow volume. Since interaction uses EQUAL 128, those pixels are skipped → nothing lit.

In a large outdoor area like the Hangar, the player may be inside many light shadow volumes simultaneously. If DSF_VIEW_INSIDE_SHADOW is not set for those volumes (or is set wrong), Z-pass fires when Z-fail should, and the stencil leaves no pixels at 128 → interaction draws nothing → only DrawShaderPasses visible.

**Explains shots 1-3:** The "wireframe" is DrawShaderPasses geometry with no depth test (H1 effect) but with the interaction contribution completely absent (H2 effect).

**How to distinguish:** In `VK_RB_DrawInteractions`, temporarily skip all shadow surface draws (comment out the call to `VK_RB_DrawShadowSurface` for all lights):
- If shots 1-3 now show correct lit geometry → shadow stencil is poisoning the entire scene; H2 confirmed.
- If shots 1-3 still show wireframe only → stencil is not the issue; look at H3/H4.

If H2 is confirmed, then check:
1. Is `DSF_VIEW_INSIDE_SHADOW` being set correctly? See `tr_light.cpp:755` — it's set if the viewpoint is inside the shadow extrusion. Check whether the detection logic works for the Doom 3 version of `srfTriangles_t::shadowCache`.
2. Are shadow volumes actually being submitted and filling the stencil, or are they silently skipped?

---

### H3 — POSSIBLE: Per-light stencil clear scissor uses wrong Y coordinates

**Where:** `VK_RB_DrawInteractions` → `vkCmdClearAttachments` using the light's scissor rect.

**What it causes:** The per-light stencil clear resets the stencil to 128 within the light's screen-space bounds. The scissor rect from `viewLight->scissorRect` is in OpenGL convention (Y=0 at bottom). Vulkan's `vkCmdClearAttachments` uses `VkClearRect.rect` which is in framebuffer coordinates (Y=0 at top). If Y is not flipped, the stencil is cleared at the wrong pixels — the wrong area of stencil stays modified from prior lights, causing incorrect shadow culling or no-shadow where there should be.

**GL reference:** Not applicable — GL uses `glScissor` which takes bottom-left origin like OpenGL NDC.

**How to check:** Search where `lightScissor` is constructed before `vkCmdClearAttachments`. Compare the Y computation to the viewport setup (Y origin at `swapchainExtent.height`, negative height). The light scissor Y should be inverted: `vulkan_y = extent.height - gl_y - gl_height`.

**To distinguish from H2:** If stencil clear is hitting wrong pixels, you'd see patches of unshadowed areas where shadows should be, or vice versa — and it would be position-dependent (related to screen position of lights). H2 would be uniform (entire scene unshadowed or shadow everywhere).

---

### H4 — POSSIBLE: Depth prepass and interaction pass produce inconsistent Z values

**Where:** `VK_RB_FillDepthBuffer` uses `gui.vert.spv` with `VkGuiUBO::modelViewProjection`. `VK_RB_DrawInteraction` uses `interaction.vert.spv` with `VkInteractionUBO::u_ModelViewProjection`. Both MVPs come from `VK_BuildSurfMVP(surf->space, mvp)` which uses `s_projVk * modelViewMatrix`.

**What it causes:** If the two vertex shaders produce different clip-space Z for the same vertex (e.g., the UBO field layout in the shader doesn't match the C struct), the interaction pass depth test (LEQUAL) would compare against wrong prepass depths. If prepass Z is smaller (nearer) than interaction Z for the same point, interaction fails → nothing draws.

**Why unlikely:** Both shaders read `mat4` at byte offset 0 of their respective UBOs, and both UBOs start with a 64-byte float[16] MVP. Unless there's an std140 padding difference, the values should agree.

**How to check:** Look at whether `gui.vert` and the UBO ring's `VkGuiUBO` are truly aligned. The shader reads `mat4 u_ModelViewProjection` (std140: 64 bytes at offset 0). `VkGuiUBO::modelViewProjection` is `float[16]` at offset 0 = 64 bytes. These match. Low risk.

**To distinguish:** Temporarily disable the depth prepass (skip the `VK_RB_FillDepthBuffer` call). If shots improve or worsen distinctly, depth prepass consistency is the issue.

---

### H5 — UNLIKELY: Z-remap formula is wrong direction or applied twice

**Where:** `VK_RB_DrawView` lines 1606-1611:
```cpp
for (int c = 0; c < 4; c++)
    s_projVk[c * 4 + 2] = 0.5f * src[c * 4 + 2] + 0.5f * src[c * 4 + 3];
```

**What it causes:** If the formula is wrong, clip-space Z from all shaders maps to values outside [0,1] and gets clamped to 1.0 (far plane). Everything appears at max depth → interaction LEQUAL always passes (drawing on top of everything) OR always fails (drawing nothing). Inconsistent with the partial correctness seen in shots 4-5.

**Why unlikely:** The formula is the standard OpenGL→Vulkan Z remap. The partial correctness in shots 4-5 would be hard to explain if all depths were wrong.

**How to check quickly:** Set a `r_showDepth` debug output in the interaction fragment shader: output `gl_FragCoord.z` as a color. If you see a gradient from near (dark) to far (bright) that makes geometric sense → remap is correct.

---

## Ordered Investigation Steps

**Do these in order. Stop and record results at each step before moving on.**

### Step 1: Isolate DrawShaderPasses depth bug (H1)

**Action:** In `VK_RB_DrawView`, log how many surfaces each pass draws (add a `common->Printf` counter). Alternatively, add a cvar `r_skipShaderPasses` and skip `VK_RB_DrawShaderPasses` when set.

**Run with shader passes disabled.** Observe:
- Do shots 4-5 now have correct depth ordering? → H1 confirmed as depth source.
- Does shot 4's white window disappear? → that surface was being drawn by shader pass, not interaction.
- Do shots 1-3 change at all? → tells you whether interaction pass actually draws anything there.

**Then enable shader passes and disable interactions** (`r_skipInteractions`). Observe:
- Are shots 1-3 now just the flat unlit surfaces? → confirms interaction pass is the missing piece.

**Code to check:** `VK_GetOrCreateGuiBlendPipeline` in `vk_pipeline.cpp`. It only keys on `GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS`. It must also key on depth function bits (`GLS_DEPTHFUNC_BITS`) and create pipelines with the correct `depthTestEnable`/`depthCompareOp`.

**GL reference for depth state:**
```
// From draw_common.cpp RB_STD_DrawShaderPasses:
GL_State(pStage->drawStateBits);
// → GLS_DEPTHFUNC_LESS is the default for 3D world surfaces
// → Some stages override to GLS_DEPTHFUNC_ALWAYS (new/program stages)
```
The fix is: when `drawStateBits` has `GLS_DEPTHFUNC_LESS` or `GLS_DEPTHFUNC_LEQUAL`, enable depth test in the pipeline. When it has `GLS_DEPTHFUNC_ALWAYS`, disable depth test (like the current behavior).

---

### Step 2: Isolate shadow stencil issue (H2)

**Action:** In `VK_RB_DrawInteractions`, add a cvar `r_skipShadows`. When set, skip all shadow surface draws (don't call `VK_RB_DrawShadowSurface` for any light). Also skip the per-light stencil clear (leave stencil at its initial 128 everywhere).

**Run with shadows disabled in shots 1-3.**
- If lighting appears correct (surfaces lit) → shadow stencil is the problem (H2 confirmed).
- If still wireframe → shadow is not the issue; look at why interaction draws nothing (H3/H4).

**If H2 confirmed:**
- Check `DSF_VIEW_INSIDE_SHADOW` logic: in `tr_light.cpp:755`, what condition sets it? Compare with the actual Doom 3 original `tr_light.cpp`.
- Check whether Z-pass silhouette-only index count `tri->numShadowIndexesNoCaps` is populated. In dhewm3, this field may not be set, defaulting to 0, causing no shadow geometry to be drawn at all — which would actually mean no stencil modification → stencil stays 128 → interactions pass. That's the opposite of H2. So check the actual value.
- Print `numShadowIndexesNoCaps` vs `numIndexes` for a shadow surface to verify they're non-zero.

---

### Step 3: Check per-light scissor Y-flip (H3)

**Where to look:** Find where `lightScissor` is constructed in `VK_RB_DrawInteractions`.

**Check:** Is the scissor rect Y-flipped for Vulkan coordinates?
- Vulkan framebuffer Y=0 is at top. The dynamic viewport sets `y = height, height = -height` (Y-flip). But `vkCmdSetScissor` and `vkCmdClearAttachments` use framebuffer coordinates (Y=0 at top), unaffected by the viewport Y-flip.
- If `viewLight->scissorRect` is in OpenGL screen space (Y=0 at bottom), converting to Vulkan: `vulkan_y = fb_height - gl_y - gl_height`.
- If this conversion is missing, the stencil clear hits the wrong band of pixels.

**How to distinguish from H2:** Disabling shadows (Step 2) would fix both H2 and H3 symptoms. To isolate H3 specifically, check whether the stencil clear pattern correlates with light positions. H3 causes incorrect stencil *per-light* in a position-dependent way; H2 causes uniform wrong stencil across the whole scene.

---

### Step 4: Verify projection matrix Z-remap (H5)

**Action:** Add a debug mode in `interaction.frag`: output `vec4(gl_FragCoord.z, gl_FragCoord.z, gl_FragCoord.z, 1.0)` as the color. Compile and run.

**Expect:** Near surfaces are dark (z near 0.0), far surfaces are bright (z near 1.0). A smooth continuous gradient makes geometric sense. If all surfaces are white (z=1.0) or all black (z=0.0) → remap is wrong.

**GL reference (`tr_render.cpp` R_SetupProjection):** The Doom 3 projection matrix uses OpenGL convention. The formula `new_row2[c] = 0.5 * old_row2[c] + 0.5 * old_row3[c]` (column-major: `c*4+2` index) converts OpenGL z ∈ [-1,1] to Vulkan z ∈ [0,1]. Verify the column-major indexing: in column-major storage, element at row r, column c is at index `c*4+r`. Row 2 of column c is `c*4+2`. This is correct.

---

### Step 5: Verify idDrawVert attribute offsets

**Check:** Compare `offsetof(idDrawVert, ...)` values against what vkDOOM3 uses.

**In `vk_pipeline.cpp` `VK_GetInteractionVertexInput`:**
```
location 0:  position  VK_FORMAT_R32G32B32_SFLOAT    offsetof(idDrawVert, xyz)
location 3:  color     VK_FORMAT_R8G8B8A8_UNORM       offsetof(idDrawVert, color)
location 8:  texcoord  VK_FORMAT_R32G32_SFLOAT        offsetof(idDrawVert, st)
location 9:  tangent0  VK_FORMAT_R32G32B32_SFLOAT     offsetof(idDrawVert, tangents[0])
location 10: tangent1  VK_FORMAT_R32G32B32_SFLOAT     offsetof(idDrawVert, tangents[1])
location 11: normal    VK_FORMAT_R32G32B32_SFLOAT     offsetof(idDrawVert, normal)
```

**vkDOOM3 reference (`RenderProgs_VK.cpp` `CreateVertexDescriptions` LAYOUT_DRAW_VERT):**
```
Position   R32G32B32_SFLOAT   offset 0
TexCoord   R16G16_SFLOAT      offset 12     ← HALF FLOAT, not R32G32!
Normal     R8G8B8A8_UNORM     offset 16     ← PACKED BYTE NORMAL
Tangent    R8G8B8A8_UNORM     offset 20     ← PACKED BYTE TANGENT
Color1     R8G8B8A8_UNORM     offset 24
Color2     R8G8B8A8_UNORM     offset 28
```

**This is a critical structural difference.** vkDOOM3 uses BFG edition's `idDrawVert` which has packed/half-float fields. dhewm3 uses the original Doom 3 `idDrawVert` with full float tangents and normals. If the attribute formats or offsets are wrong for dhewm3's idDrawVert, the vertex shader reads garbage for normal/tangent → wrong lighting.

**How to check:**
1. Print `sizeof(idDrawVert)` — should be 60 bytes for original Doom 3 (xyz=12, st=8, normal=12, tangents=24, color=4).
2. Print `offsetof(idDrawVert, xyz)`, `offsetof(idDrawVert, st)`, `offsetof(idDrawVert, normal)`, `offsetof(idDrawVert, tangents[0])`, `offsetof(idDrawVert, tangents[1])`, `offsetof(idDrawVert, color)`.
3. Cross-reference with `tr_local.h idDrawVert` struct definition.

**Impact if wrong:** Normal and tangent garbage → dot products with light direction are garbage → per-pixel lighting is wrong. This would cause incorrect coloring but not necessarily depth ordering issues. A separate bug from H1/H2.

---

### Step 6: Check interaction UBO field ordering vs shader layout

**Check:** `VkInteractionUBO` in `vk_pipeline.cpp` vs `InteractionParams` uniform block in `interaction.vert`.

**C struct:**
```
lightOrigin[4]      → vec4  u_LightOrigin       offset 0
viewOrigin[4]       → vec4  u_ViewOrigin         offset 16
lightProjectionS[4] → vec4  u_LightProjectionS   offset 32
lightProjectionT[4] → vec4  u_LightProjectionT   offset 48
lightProjectionQ[4] → vec4  u_LightProjectionQ   offset 64
lightFalloffS[4]    → vec4  u_LightFalloffS      offset 80
bumpMatrixS[4]      → vec4  u_BumpMatrixS        offset 96
bumpMatrixT[4]      → vec4  u_BumpMatrixT        offset 112
diffuseMatrixS[4]   → vec4  u_DiffuseMatrixS     offset 128
diffuseMatrixT[4]   → vec4  u_DiffuseMatrixT     offset 144
specularMatrixS[4]  → vec4  u_SpecularMatrixS    offset 160
specularMatrixT[4]  → vec4  u_SpecularMatrixT    offset 176
colorModulate[4]    → vec4  u_ColorModulate      offset 192
colorAdd[4]         → vec4  u_ColorAdd           offset 208
mvp[16]             → mat4  u_ModelViewProjection offset 224
diffuseColor[4]     → vec4  u_DiffuseColor       offset 288
specularColor[4]    → vec4  u_SpecularColor      offset 304
gammaBrightness[4]  → vec4  u_GammaBrightness    offset 320
applyGamma (int)    → int   u_ApplyGamma         offset 336
screenWidth (float) → float u_ScreenWidth        offset 340
screenHeight (float)→ float u_ScreenHeight       offset 344
useShadowMask (int) → int   u_UseShadowMask      offset 348
lightScale (float)  → float u_LightScale         offset 352
```

**Std140 rules for the GLSL block:** `int` fields after a `mat4` (column-major, base alignment 16) must be aligned to 4 bytes. After `vec4 u_GammaBrightness` at offset 320+16=336, `int u_ApplyGamma` at 336 — 4-byte aligned ✓. Then `float` at 340, `float` at 344, `int` at 348, `float` at 352. These are all 4-byte aligned after 4-byte scalars. Matches C struct layout ✓.

**Risk:** Low — layout looks correct. But worth cross-checking against a Vulkan validation layer output if available.

---

## Decision Tree Summary

```
Observation: shots 1-3 outdoor wireframe + shots 4-5 depth ordering issues

Is DrawShaderPasses the source of depth ordering? (Step 1)
  → Disable shader passes: does shot 5 vending machine order fix?
     YES → H1 confirmed. GUI pipeline needs depth test enabled for 3D stages.
           Fix: VK_GetOrCreateGuiBlendPipeline must also key on GLS_DEPTHFUNC_BITS.
     NO  → Something else is drawing 3D geometry without depth. Investigate further.

Are shots 1-3 an interaction pass failure? (Step 1 continued)
  → Disable interactions: do shots 1-3 go dark/empty?
     YES → Interaction pass draws nothing in the outdoor area.
           Proceed to Step 2 (shadow stencil).
     NO  → Interaction pass works; depth issues from something else.

Is shadow stencil poisoning the outdoor scene? (Step 2)
  → Disable shadows: do shots 1-3 now show lit geometry?
     YES → H2 confirmed. Investigate DSF_VIEW_INSIDE_SHADOW, numShadowIndexesNoCaps.
     NO  → Stencil not the issue. Check H3 (scissor Y-flip) or H4 (prepass Z mismatch).
```

---

## Reference Files

| Question | Where to look |
|----------|--------------|
| DrawShaderPasses GL depth state | `neo/renderer/draw_common.cpp:RB_STD_DrawShaderPasses` — `GL_State(pStage->drawStateBits)` |
| Shadow Z-pass/fail selection (GL) | `neo/renderer/draw_common.cpp:1440` — `if (!(surf->dsFlags & DSF_VIEW_INSIDE_SHADOW))` |
| DSF_VIEW_INSIDE_SHADOW set | `neo/renderer/tr_light.cpp:755` |
| Doom3 idDrawVert layout | `neo/renderer/Model.h` or `tr_local.h` — `struct idDrawVert` |
| vkDOOM3 vertex layout (BFG) | `vkDOOM3/neo/renderer/Vulkan/RenderProgs_VK.cpp:CreateVertexDescriptions` |
| Interaction pipeline depth state | `vk_pipeline.cpp:VK_CreateInteractionPipeline` — `depthCompareOp = LESS_OR_EQUAL` |
| GUI pipeline depth state | `vk_pipeline.cpp:VK_CreateGuiPipelineEx` — `depthTestEnable = VK_FALSE` |
| Per-light stencil clear | `vk_backend.cpp:VK_RB_DrawInteractions` — `vkCmdClearAttachments` |
| Light scissor construction | `vk_backend.cpp:VK_RB_DrawInteractions` — where `lightScissor` is built |
| Z-remap formula | `vk_backend.cpp:VK_RB_DrawView` lines 1606-1611 |
| Shadow vertex count | `vk_backend.cpp:VK_RB_DrawShadowSurface` — `numShadowIndexesNoCaps` |
