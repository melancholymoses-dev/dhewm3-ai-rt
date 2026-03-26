# Mirror / Subview Rendering Plan (Vulkan)

**Date:** 2026-03-25
**Branch:** `phase5_rtao`
**Bugs addressed:** Mirrors render as black; mirror surfaces visible through walls

---

## Problem Summary

Two distinct issues:

1. **Black mirrors** — The Vulkan backend at [vk_backend.cpp:975](neo/renderer/Vulkan/vk_backend.cpp#L975) explicitly skips any shader stage with `dynamic == DI_MIRROR_RENDER` (or `DI_REMOTE_RENDER` / `DI_XRAY_RENDER`). The surface draws with no texture bound → black.

2. **Visible through walls** — Mirror surfaces use `sort = SS_SUBVIEW` (value -3), which sorts them *before* opaque geometry (`SS_OPAQUE` = 0). The depth prepass in `VK_RB_DrawShaderPasses` doesn't filter out `SS_SUBVIEW` surfaces, so the mirror quad writes depth and its geometry appears in the TLAS, causing it to occlude or be hit by rays even when behind walls. Additionally, without correct depth/blend state for subview surfaces, the mirror quad renders its (black) color over whatever is behind it.

---

## How Doom3 Mirrors Work (Reference)

The pipeline is a render-to-texture approach shared between GL and Vulkan frontends:

1. **Material** — `mirrorRenderMap` keyword sets `dynamic = DI_MIRROR_RENDER`, `texgen = TG_SCREEN`, `sort = SS_SUBVIEW`, `hasSubview = true`
2. **Frontend** — `R_GenerateSubViews()` → `R_GenerateSurfaceSubview()` finds subview surfaces, calls `R_MirrorRender()` which:
   - Creates a reflected `viewDef_t` via `R_MirrorViewBySurface()` (mirrored camera, clip plane, `isMirror = true`)
   - Calls `R_RenderView(parms)` to render the reflected scene
   - Calls `CaptureRenderToImage(scratchImage)` which emits an `RC_COPY_RENDER` command
   - This command blits the current framebuffer into `scratchImage`
3. **Backend draw** — When the mirror surface draws in the main pass:
   - The GL path sets blend state `GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS` for `SS_SUBVIEW` surfaces
   - The `TG_SCREEN` texgen samples `scratchImage` containing the reflected view
   - Color is down-modulated by `1/overBright`

Key files:
- [tr_subview.cpp](neo/renderer/tr_subview.cpp) — shared frontend, `R_MirrorRender()`, `R_MirrorViewBySurface()`
- [vk_backend.cpp:975](neo/renderer/Vulkan/vk_backend.cpp#L975) — the skip that causes black mirrors
- [vk_backend.cpp:3050](neo/renderer/Vulkan/vk_backend.cpp#L3050) — `VK_RB_CopyRender()` framebuffer blit
- [draw_common.cpp:562](neo/renderer/draw_common.cpp#L562) — GL reference for SS_SUBVIEW blend state
- [Material.cpp:2273](neo/renderer/Material.cpp#L2273) — `mirror` keyword → `SS_SUBVIEW`

---

## Implementation Plan

### Step 1: Add Diagnostic Logging

Before changing behavior, confirm the existing pipeline state.

**1a.** In `R_GenerateSurfaceSubview()` ([tr_subview.cpp:479](neo/renderer/tr_subview.cpp#L479)), add logging when a mirror/subview surface is detected and when `R_MirrorRender()` is called:
```cpp
common->Printf("SUBVIEW: detected mat='%s' sort=%f dynamic=%d\n",
    shader->GetName(), shader->GetSort(), stage->texture.dynamic);
```

**1b.** In `VK_RB_CopyRender()` ([vk_backend.cpp:3050](neo/renderer/Vulkan/vk_backend.cpp#L3050)), log entry to confirm the blit fires:
```cpp
common->Printf("VK COPYRENDER: img='%s' %dx%d frameActive=%d\n",
    cmd->image->imgName.c_str(), cmd->imageWidth, cmd->imageHeight, s_frameActive);
```

**1c.** At the skip site ([vk_backend.cpp:975](neo/renderer/Vulkan/vk_backend.cpp#L975)), log what's being skipped:
```cpp
common->Printf("VK DrawShaderPasses: SKIPPING dynamic=%d mat='%s'\n",
    pStage->texture.dynamic, mat->GetName());
```

**Goal:** Confirm that `R_MirrorRender()` fires, `CaptureRenderToImage` fires, and `VK_RB_CopyRender()` actually blits to `scratchImage`. If all three fire, the image data is there — we just need to bind it.

### Step 2: Fix the Black Mirrors — Remove the Skip and Bind scratchImage

Replace the skip at [vk_backend.cpp:973-977](neo/renderer/Vulkan/vk_backend.cpp#L973-L977):

```cpp
// BEFORE:
else {
    // Portal/mirror/other dynamic texture — not yet supported, skip.
    continue;
}
```

```cpp
// AFTER:
else if (pStage->texture.dynamic == DI_MIRROR_RENDER ||
         pStage->texture.dynamic == DI_REMOTE_RENDER)
{
    // Mirror/portal: subview was rendered and blitted into scratchImage
    // by R_MirrorRender() / R_RemoteRender() + VK_RB_CopyRender().
    idImage *mirrorImg = globalImages->scratchImage;
    if (!VK_Image_GetDescriptorInfo(mirrorImg, &imgInfo))
    {
        VK_Image_GetFallbackDescriptorInfo(&imgInfo);
    }
}
else if (pStage->texture.dynamic == DI_XRAY_RENDER)
{
    idImage *xrayImg = globalImages->scratchImage2;
    if (!VK_Image_GetDescriptorInfo(xrayImg, &imgInfo))
    {
        VK_Image_GetFallbackDescriptorInfo(&imgInfo);
    }
}
else
{
    // Unknown dynamic type — skip safely.
    continue;
}
```

This makes the mirror stage actually draw with the captured reflection texture.

### Step 3: Add SS_SUBVIEW Blend State in VK_RB_DrawShaderPasses

The GL path applies special blend state for `SS_SUBVIEW` surfaces ([draw_common.cpp:562](neo/renderer/draw_common.cpp#L562)):
```cpp
GL_State(GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS);
```

In the Vulkan shader pass draw loop, before binding the pipeline for a surface, check:
```cpp
if (mat->GetSort() == SS_SUBVIEW)
{
    // Use multiplicative blend: dst_color * src_color
    // This down-modulates the reflection by overbright factor.
    // Need a pipeline variant with this blend mode + depth-test-less.
    drawStateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
}
```

This requires either:
- A dedicated Vulkan pipeline variant with this blend state, or
- Using the existing dynamic-state pipeline with appropriate blend mode bits

### Step 4: Fix Visible-Through-Walls — Depth Prepass Filtering

The depth prepass should NOT write depth for `SS_SUBVIEW` surfaces. These surfaces should only draw during the shader pass phase with the correct blend state.

In the depth fill pass (look for the loop that writes depth), add an early skip:
```cpp
if (mat->GetSort() == SS_SUBVIEW)
    continue;  // Subview surfaces don't write depth — they blend over existing content
```

This prevents the mirror quad from writing depth values that would occlude geometry behind it.

### Step 5: TLAS Filtering for Mirror Geometry

Mirror surfaces should not participate in ray tracing (they're flat quads with a render-to-texture approach, not actual reflective geometry for RT). In `VK_RT_BuildBLASForModel` or `VK_RT_RebuildTLAS`, filter out surfaces whose material has `sort == SS_SUBVIEW`:

```cpp
// In the surface iteration loop when building BLAS geometry:
if (surf->shader && surf->shader->GetSort() == SS_SUBVIEW)
    continue;  // Mirror/portal quads are not physical geometry for RT
```

This prevents mirror geometry from appearing in ray intersection tests (which is what causes the "visible through walls" artifact where mirror surfaces cast shadows or occlude ray-traced results from behind walls).

### Step 6: Verify CropRenderSize / UnCrop Works in Vulkan

`R_MirrorRender()` calls `tr.CropRenderSize()` before rendering the subview and `tr.UnCrop()` after. This changes the viewport/scissor for rendering the reflected scene at potentially reduced resolution.

Verify that:
- The Vulkan render pass handles viewport changes mid-frame correctly
- The framebuffer blit in `VK_RB_CopyRender()` reads from the cropped region
- The render pass resume after `VK_RB_CopyRender()` restores the full viewport

If `CropRenderSize` doesn't work properly with Vulkan, the reflected view may render at wrong size or not at all. Check the vkDOOM3 reference at `../vkDOOM3/neo/renderer/tr_frontend_subview.cpp` for how they handle this.

### Step 7: Validate Face Culling in Mirror Views

The mirror view flips winding order. Code at [vk_backend.cpp:1570](neo/renderer/Vulkan/vk_backend.cpp#L1570) already handles this:
```cpp
const bool mirrorView = backEnd.viewDef && backEnd.viewDef->isMirror;
```

Confirm that:
- `isMirror` is correctly set during the subview render
- The shadow pipeline variants (`shadowPipelineZFailMirror`, `shadowPipelineZPassMirror`) are selected
- Stencil ops are flipped correctly for mirror views

### Step 8: Test

Test with the bathroom mirror in Mars City (`maps/game/mc_underground`):
1. Stand in front of the mirror — reflection should appear
2. Verify orientation (not upside-down or reversed)
3. Walk behind the mirror — it should NOT be visible through walls
4. Check that player body is visible in reflection but view weapons are not
5. Check no RT shadow artifacts from mirror geometry

---

## Execution Order & Dependencies

```
Step 1 (logging) — no deps, do first
    ↓
Step 2 (remove skip, bind scratchImage) — core fix for black mirrors
    ↓
Step 3 (SS_SUBVIEW blend state) — needed for correct visual appearance
    ↓
Step 4 (depth prepass filter) — fixes visibility through walls (rasterization)
    ↓
Step 5 (TLAS filter) — fixes visibility through walls (ray tracing)
    ↓
Step 6 (verify CropRenderSize) — may be blocking Step 2 if blit fails
    ↓
Step 7 (validate culling) — correctness polish
    ↓
Step 8 (test)
```

**Note:** Step 6 could be a blocker for Step 2. If the subview render doesn't actually produce valid content (because CropRenderSize/viewport handling is broken), then binding scratchImage will just show garbage instead of black. The logging from Step 1 will reveal this.

---

## Risk Assessment

- **CropRenderSize** is the biggest unknown. If the Vulkan backend doesn't handle mid-frame viewport changes and render pass suspend/resume correctly, the subview render may produce nothing. This would require adding an offscreen render target for subviews.
- **Pipeline variants** — Step 3 may require creating new Vulkan pipeline objects with the multiplicative blend state. Check if the existing pipeline creation code supports dynamic blend state.
- **Performance** — Each mirror doubles the scene render cost (it renders the whole scene from the reflected viewpoint). This is inherent to the render-to-texture approach. Ray-traced reflections (Phase 5) would eventually replace this with a single-pass approach for planar mirrors.

## Future: Ray-Traced Reflections (Phase 5)

Once RTAO is working, planar mirrors are a natural candidate for RT reflections:
- Shoot reflection rays from the mirror surface based on surface normal
- No need for the render-to-texture pipeline at all
- Much cheaper for simple planar mirrors, and naturally handles curved/arbitrary reflective surfaces
- This would obsolete Steps 2-3 of this plan, but Steps 4-5 (TLAS filtering) remain relevant
