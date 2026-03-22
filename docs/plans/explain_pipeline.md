# Explaining The Rendering Pipeline (Stencil + RT)

Audience: physicist / data scientist, new to C++/Vulkan/real-time graphics.

Companion document:
- `docs/plans/explain_pipeline_math.md` (math-first derivations + jargon bridge)
- `docs/plans/explain_pipeline_cheatsheet.md` (one-page quick reference)
- `docs/plans/explain_pipeline_stencil_symbols.md` (stencil symbols mapped to Vulkan/GLSL code)

This document explains:
1. What the current Vulkan rendering pipeline is doing in this codebase.
2. How the legacy stencil shadow pipeline works (the Doom 3 classic method).
3. How the current ray-tracing shadow pipeline works.
4. What rtx_refactor_plan2.md proposes, and the major gaps/risk items.

---

## 1) Mental Model: What Is Being Computed?

At a high level, each frame computes:

- Geometry visibility and depth
- Per-light direct illumination (diffuse + specular)
- Whether each shaded point is visible from each light (shadowing)

In physics language, this is approximating direct radiance transport:

L_o(x, \omega_o) \approx \sum_l V_l(x) * f_r(x, \omega_i, \omega_o) * L_l * G_l

Where:
- x is surface point
- V_l(x) is visibility to light l (1 = unoccluded, 0 = blocked)
- f_r is material response (normal map + specular model in this engine)
- G_l is geometry/attenuation term

Classic Doom 3 computes V_l mostly via stencil volumes.
The RT path computes V_l via ray queries against TLAS/BLAS.

---

## 2) Big Picture Frame Timeline (Current Vulkan Path)

The current Vulkan backend pipeline is roughly:

```text
CPU front-end (visibility, light/surface interaction lists)
    |
    v
GPU command recording (vk_backend.cpp)
    |
    +--> Depth prepass (fills depth/stencil)
    |
    +--> If RT shadows enabled:
    |      - End render pass
    |      - Clear RT shadow mask to lit
    |      - Rebuild TLAS
    |      - Resume render pass
    |
    +--> For each viewLight:
    |      - If RT shadows: end RP -> RT dispatch for this light -> resume RP
    |      - Clear stencil region to 128
    |      - If non-RT: draw shadow volumes (global/local)
    |      - Draw local/global/translucent interactions
    |
    +--> Shader passes / fog / GUI
    |
    +--> Submit + present
```

Key source locations in current code:
- `VK_RB_DrawView` and `VK_RB_SwapBuffers` in `neo/renderer/Vulkan/vk_backend.cpp`
- Per-light loop in `VK_RB_DrawInteractions` in `neo/renderer/Vulkan/vk_backend.cpp`
- RT shadow dispatch in `neo/renderer/Vulkan/vk_shadows.cpp`
- TLAS rebuild in `neo/renderer/Vulkan/vk_accelstruct.cpp`

---

## 3) Stencil Shadow Pipeline (Classic Doom 3 Method)

Even though you are moving to RT, this is worth understanding because the engine architecture is built around it.

## 3.1 Core idea

For each light, build and render a shadow volume from occluder geometry. Then mark pixels in stencil where the view ray to the pixel crosses this volume an odd/effective number of times.

Then the lighting pass for that light only contributes where stencil says "lit" (or not shadowed by that light).

## 3.2 Why Z-fail (Carmack's Reverse)?

Doom 3 commonly uses Z-fail because camera can be inside shadow volumes. Instead of counting intersections from eye to infinity in front of depth, it counts when depth test fails against shadow volume caps/sides.

Conceptual picture:

```text
eye -----> scene depth surface point P
   \      shadow volume side faces extruded from occluder edges
    \____ if depth test fails against volume faces, stencil increments/decrements
```

The front/back face ops are arranged so inside/outside classification survives camera-in-volume cases.

## 3.3 Per-light ordering in this engine

For each light:

1. Clear stencil to reference value (128 here).
2. Draw global shadow volumes.
3. Draw local interactions.
4. Draw local shadow volumes.
5. Draw global interactions.

This ordering is not arbitrary; it is part of Doom 3's local/global light interaction semantics.

## 3.4 Pros / cons

Pros:
- Deterministic hard shadows
- No BVH, no RT hardware requirement

Cons:
- Complex geometry path
- Difficult edge cases (caps, winding, mirrored views)
- Hard to extend to soft shadows

---

## 4) Current RT Shadow Pipeline (What You Have Running)

You now have a hybrid path where rasterization still handles surface shading, but per-light visibility can come from RT.

## 4.1 Data structures

- BLAS: per-entity mesh acceleration structure
- TLAS: per-frame top-level AS over visible entities
- Shadow mask image: single-channel R8 image sampled during interaction shading

## 4.2 Per frame / per light behavior

Current behavior:

1. Build/update TLAS from visible entities.
2. For each light in interaction loop:
   - End render pass
   - Dispatch `vkCmdTraceRaysKHR` (shadow rays for that one light)
   - Resume render pass
   - Draw that light's interaction pass, sampling shadow mask

This now matches the correct per-light sequencing (important).

## 4.3 Shader pipeline roles

- `shadow_ray.rgen`:
  - Reconstruct world position from depth
  - Build ray toward current light
  - Trace shadow rays
  - Write per-pixel shadow factor into mask

- `shadow_ray.rmiss`:
  - If no hit before light: lit (shadowFactor = 1)

- `shadow_ray.rahit`:
  - Any opaque hit: shadowed (shadowFactor = 0)

Then interaction fragment shader multiplies lighting by sampled shadow mask.

## 4.4 Matrix and coordinate details (important)

You identified the key hazard correctly: matrix and clip-space convention mismatches.

There are three interacting conventions:

1. Doom3/OpenGL-style projection matrix expectation
2. Vulkan framebuffer/depth conventions
3. C++ memory layout and GLSL matrix multiplication semantics

Current reconstruction approach is the "Option A" style:

- CPU computes `invViewProj` from GL-style projection * view
- Shader flips Y for Vulkan screen coordinates
- Shader remaps depth z from [0,1] to [-1,1]

So reconstruction is mathematically:

clip = [2u-1, 1-2v, 2d-1, 1]
world = invViewProj * clip

This is the right shape for your current setup, provided the depth samples are valid and the matrix upload order is consistent.

---

## 5) What rtx_refactor_plan2.md Proposes

`rtx_refactor_plan2.md` is a staged migration plan:

1. GLSL modernization
2. Vulkan raster backend parity
3. BLAS/TLAS infrastructure
4. RT shadows replacing stencil path
5. RT AO and reflections

Architecturally, this is a good decomposition because each phase leaves a runnable product and constrains debugging scope.

The long-term intended structure is hybrid:

```text
Rasterization: primary visibility + material shading passes
Ray tracing:    visibility queries and selected secondary effects
```

That is realistic for Doom 3 scale and content.

---

## 6) Major Gaps / Risks (Current State vs Plan)

These are the most important technical gaps right now.

## Gap A: Depth sampling view correctness (high risk)

Current RT depth sampling uses the same depth/stencil image view used as framebuffer attachment in combined depth-stencil formats.

Risk:
- Sampling with combined aspect view is non-conformant/fragile on some drivers.
- Can produce undefined depth reads, visual corruption, and potentially device loss.

What to do:
- Create a separate depth-only image view for sampled depth in RT shaders.

## Gap B: BLAS coverage is incomplete (high impact on correctness)

Current TLAS build path uses only first model surface for BLAS in many cases.

Risk:
- Missing geometry in RT visibility = incorrect shadows (light leaks or missing occlusion).

What to do:
- Build BLAS over all relevant surfaces (or robust merged geometry).
- Ensure dynamic/skinned meshes have correct update policy.

## Gap C: Any-hit material model is still minimal

Current any-hit path treats geometry as opaque baseline.

Risk:
- Alpha-tested materials (fences/grates/foliage-like cutouts) shadow incorrectly.

What to do:
- Add alpha-tested any-hit path with material/texture lookup and threshold logic.

## Gap D: AS rebuild performance

Frequent BLAS rebuilds and full TLAS rebuild can become expensive.

Risk:
- Frame-time spikes in scenes with many dynamic entities.

What to do:
- Distinguish static vs dynamic BLAS.
- Use update path where possible.
- Batch build submissions and reduce sync points.

## Gap E: Denoising/temporal stability for future RT effects

Plan includes AO/reflections; both are noisy at practical sample counts.

Risk:
- Without denoising and history management, output will shimmer/noise.

What to do:
- Add temporal accumulation with robust history rejection.
- Add spatial denoising stage for AO/reflections.

## Gap F: Stencil path parity still matters during transition

Even with RT shadows enabled, stencil fallback is still operationally important.

Risk:
- If fallback diverges, debugging mixed regressions gets harder.

What to do:
- Keep fallback path healthy until RT feature parity is validated.

---

## 7) Why Your Log Looks "Alive" But Image Fails

Your log excerpt shows healthy control flow:

- TLAS builds are happening
- Per-light RT dispatches are happening
- SBT addresses are stable
- UBO offsets advance consistently

That proves command sequencing exists, but does not prove sampled depth / geometric coverage / hit semantics are correct.

In other words:

```text
"Pipeline runs" != "Physics of visibility is correct"
```

This is a classic state in GPU debugging.

---

## 8) ASCII Diagrams

## 8.1 Stencil pipeline (per light)

```text
                 (for one light L)

      depth/stencil buffer already populated
                     |
                     v
       clear stencil ref (128 in light scissor)
                     |
                     v
      draw shadow volumes (front/back stencil ops)
                     |
                     v
      draw light interaction pass with stencil test
                     |
                     v
      contribution of light L added to framebuffer
```

## 8.2 RT shadow pipeline (per light)

```text
                 (for one light L)

 [depth image] --> reconstruct world x in rgen
                     |
                     v
            trace ray x -> light(L)
               |                |
               | hit            | miss
               v                v
         shadowFactor=0     shadowFactor=1
                     \      /
                      \    /
                       write shadowMask
                             |
                             v
      interaction.frag samples shadowMask and modulates light L
```

## 8.3 Matrix convention map

```text
CPU:
  proj_gl * view_gl  --> inverse --> invViewProj (uploaded)

Shader:
  uv -> ndc:
    x_ndc = 2u - 1
    y_ndc = 1 - 2v      (Vulkan screen flip)
    z_ndc = 2d - 1      (Vulkan depth [0,1] -> GL [-1,1])

  world = invViewProj * [x_ndc, y_ndc, z_ndc, 1]
```

---

## 9) Vocabulary Bridge (C++/Vulkan to familiar math terms)

- `VkRenderPass`: constrained sequence of framebuffer operations
- `VkPipeline`: compiled fixed+programmable state operator
- `DescriptorSet`: bound parameter bundle (textures, buffers, AS)
- `UBO`: constant parameter vector/matrix block
- `BLAS/TLAS`: hierarchical spatial index for ray intersection
- `SBT`: dispatch table mapping ray stages to shader records
- `Barrier`: explicit memory/order constraint between operators

Think of Vulkan as explicit scheduling over stateful linear operators with strict memory dependencies.

---

## 10) Suggested Reading Order In Code

1. `neo/renderer/Vulkan/vk_backend.cpp`
   - `VK_RB_DrawView`
   - `VK_RB_DrawInteractions`
   - `VK_RB_SwapBuffers`

2. `neo/renderer/Vulkan/vk_shadows.cpp`
   - `VK_RT_InitShadowPipeline`
   - `VK_RT_DispatchShadowRaysForLight`

3. `neo/renderer/glsl/shadow_ray.rgen`
   - `reconstructWorldPos`
   - `traceRayEXT` call and payload logic

4. `neo/renderer/Vulkan/vk_accelstruct.cpp`
   - `VK_RT_BuildBLAS`
   - `VK_RT_RebuildTLAS`

---

## 11) Bottom Line

- The old stencil pipeline computes visibility by rasterized shadow volumes + stencil arithmetic.
- The new RT pipeline computes visibility by ray queries against TLAS/BLAS and feeds a shadow mask into the same interaction lighting pass.
- Your current implementation is already at the right architectural shape for Phase 4.
- The highest-priority correctness gaps are depth sampling view correctness and full geometry coverage in BLAS.
- Once those are solid, the remaining work is mostly material correctness (alpha), performance tuning, and denoised Phase 5 effects.
