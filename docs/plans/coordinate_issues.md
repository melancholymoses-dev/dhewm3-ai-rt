# Coordinate System Issues: Screen → World Projection

Reference for anyone writing shaders or CPU code that reconstructs world positions
from the depth buffer (ray generation shaders, SSAO, deferred lighting, etc.).

---

## 1. Doom 3 World-Space Axis Convention

Doom 3 uses a **right-handed, Z-up** world coordinate system:

| Axis | Meaning |
|------|---------|
| X    | East / right |
| Y    | North / forward |
| Z    | Up |

This differs from OpenGL's typical **Y-up** camera convention.
The `modelViewMatrix` (including `viewDef->worldSpace.modelViewMatrix`) already
applies the axis remap so that the camera space it produces IS the standard
OpenGL camera space (Y-up, looking down -Z).  Do not manually remap axes
when working with matrices from `viewDef`.

**Light origins, entity origins, and TLAS geometry are all in Doom 3 world space (Z-up).**
`R_AxisToModelMatrix` builds model-to-world transforms; the BLAS vertex data is in
local model space, and the TLAS instance transform converts it to world space.

---

## 2. The Vulkan Y-Flip

### The Problem

Vulkan framebuffer coordinates have **y=0 at the top**, increasing downward.
OpenGL (and Doom 3's projection matrices) treat **y=0 as the bottom**, with NDC
y=+1 at the top.

dhewm3_rtx resolves this in rendering by setting a **negative-height viewport**:

```cpp
// vk_backend.cpp
VkViewport vp = { 0, (float)height, (float)width, -(float)height, 0.f, 1.f };
```

This maps:
- NDC y = +1 → framebuffer row 0 (top of screen)
- NDC y = -1 → framebuffer bottom row (`height - 1`)

So geometry rendered with the original GL projection matrix appears right-side-up.

### The Trap for Screen-Space Shaders

Any shader that reads from a screen-space texture (depth buffer, shadow mask, G-buffer)
and needs to convert pixel coordinates → NDC → world space **must account for this flip**.

**Wrong** (NDC y inverted — screen top maps to NDC bottom):
```glsl
vec2 uv = (vec2(coord) + 0.5) / vec2(size);
vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);  // uv.y=0 → NDC y=-1 (wrong)
```

**Correct** (screen top = NDC y=+1, matching viewport flip):
```glsl
vec2 uv = (vec2(coord) + 0.5) / vec2(size);
vec4 clipPos = vec4( uv.x * 2.0 - 1.0,
                     1.0 - 2.0 * uv.y,   // flip Y
                     ...,
                     1.0 );
```

Getting this wrong causes shadow rays / world-position reconstructions to be
**vertically mirrored** — rays trace from positions on the opposite side of the
scene, almost always hitting geometry → everything appears black/shadowed.

---

## 3. Z Range: OpenGL [-1, 1] vs Vulkan [0, 1]

### The Problem

OpenGL NDC Z runs from **-1** (near) to **+1** (far).
Vulkan NDC Z runs from **0** (near) to **+1** (far).

`viewDef->projectionMatrix` is the original GL projection matrix.
To make geometry render correctly in Vulkan, `vk_backend.cpp` derives a corrected
matrix `s_projVk` that remaps Z:

```cpp
// vk_backend.cpp — applied each frame
for (int c = 0; c < 4; c++)
    s_projVk[c*4 + 2] = 0.5f * src[c*4 + 2] + 0.5f * src[c*4 + 3];
```

This is the matrix actually used for rendering, so the **depth buffer contains
Z in [0, 1]** (Vulkan range).

### The Trap for Screen-Space Shaders

If you compute `invViewProj` from `viewDef->projectionMatrix` (the GL matrix)
but feed it a depth value from the Vulkan depth buffer, the Z component is
**half-range off**:

| depth buffer value | If used directly as GL NDC Z (wrong) | Correct Vulkan NDC Z meaning |
|--------------------|--------------------------------------|------------------------------|
| 0.0 | NDC z = 0.0 (mid-range, not near) | near plane |
| 0.5 | NDC z = 0.5 (toward far) | midpoint |
| 1.0 | NDC z = 1.0 (far) | far plane |

So the wrong path compresses away the near-half of GL clip space: values that should map to
GL NDC [-1, 0] are never produced unless you remap depth with `2.0 * depth - 1.0`.

**Wrong** (raw Vulkan depth fed into GL-based invViewProj):
```glsl
vec4 clipPos = vec4(ndcXY, depth, 1.0);  // depth ∈ [0,1]
vec4 worldPos = params.invViewProj * clipPos;  // invViewProj from GL proj
```

**Correct — option A** (remap depth to GL range, keep GL invViewProj):
```glsl
vec4 clipPos = vec4(ndcXY, 2.0 * depth - 1.0, 1.0);  // → [-1,1]
vec4 worldPos = params.invViewProj * clipPos;
```

**Correct — option B** (compute invViewProj from s_projVk, keep raw depth):
```glsl
// CPU: expose s_projVk and compute invViewProj from it instead of
//      viewDef->projectionMatrix
vec4 clipPos = vec4(ndcXY, depth, 1.0);  // depth ∈ [0,1] matches s_projVk
vec4 worldPos = params.invViewProj * clipPos;
```

We currently use **Option A** (`shadow_ray.rgen`): remap depth in the shader and
keep `invViewProj` computed from the GL projection matrix on the CPU side.

---

## 4. Combined Correction (shadow_ray.rgen)

Both fixes applied together, starting from a Vulkan pixel coordinate:

```glsl
vec3 reconstructWorldPos(ivec2 coord, ivec2 size) {
    vec2 uv    = (vec2(coord) + 0.5) / vec2(size);
    float depth = texelFetch(depthSampler, coord, 0).r;  // Vulkan Z ∈ [0,1]

    vec4 clipPos = vec4(
        uv.x * 2.0 - 1.0,     // X: standard
        1.0 - 2.0 * uv.y,     // Y: flipped (screen top = NDC +1)
        2.0 * depth - 1.0,    // Z: Vulkan [0,1] → GL [-1,1]
        1.0
    );

    vec4 worldPos = params.invViewProj * clipPos;  // invViewProj from GL proj * view
    return worldPos.xyz / worldPos.w;
}
```

The `invViewProj` is `(projectionMatrix * worldSpace.modelViewMatrix)^-1`, computed
on the CPU using `viewDef->projectionMatrix` (not `s_projVk`).

---

## 5. Stencil Shadow Volumes

Stencil shadows are **rasterized geometry** — they do not read the depth buffer as a
texture and do not reconstruct world positions.  The Y-flip and Z-range issues above
therefore affect them differently.

### Y-Flip: Face Winding

The Y-flip viewport inverts the winding order of every triangle in screen space.
A triangle that was CCW (front-facing in OpenGL) becomes CW in Vulkan window space.

**Fix already applied in all shadow pipelines** (`vk_pipeline.cpp`):
```cpp
rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
rasterizer.cullMode  = VK_CULL_MODE_NONE;        // both faces needed for shadow volumes
```

Setting `frontFace = CLOCKWISE` makes Vulkan's internal front/back face classification
match OpenGL's, so the separate `front` and `back` stencil ops are applied to the
correct faces.  **This is the only Y-flip correction needed for stencil shadows.**

### Z Range: Depth Test Consistency

Stencil shadow geometry is rendered with `s_projVk` (the Z-remapped matrix), so its
clip-space Z is in [0, 1], exactly matching the depth buffer that was written by the
same matrix in the depth prepass and ambient pass.  The hardware depth test (LESS or
LEQUAL) compares two values in the same space, so no correction is needed.

Rule: **any draw that uses `s_projVk` will compare correctly against the depth buffer.**
Only shaders that *sample* the depth buffer as a texture and then unproject through a
CPU-side matrix need the Z correction described in section 3.

### Stencil Op Convention (Carmack's Reverse / Z-Fail)

The Z-fail (Carmack's Reverse) stencil ops in our Vulkan pipeline match the GL
`glStencilOpSeparate` path in `draw_common.cpp RB_T_Shadow`:

| Face (post-Y-flip correction) | depthFailOp | GL reference |
|-------------------------------|-------------|--------------|
| front (facing camera)         | DECREMENT   | stencilDecr on firstFace=GL_BACK for non-mirror¹ |
| back  (facing away)           | INCREMENT   | stencilIncr on secondFace=GL_FRONT |
| mirror view: swap the ops     |             | firstFace=GL_FRONT |

¹ GL's `stencilOpSeparate` path uses `firstFace = isMirror ? GL_FRONT : GL_BACK`.
For the non-mirror case, `GL_BACK` gets DECR and `GL_FRONT` gets INCR, which maps to
Vulkan `back.depthFailOp = INCR, front.depthFailOp = DECR`.  This matches our code.

The GL non-`StencilOpSeparate` path (two separate single-sided draw calls) has the
INCR/DECR assignment reversed; this is a historical inconsistency in the GL code and
does not affect correctness because both conventions satisfy `stencil ≠ 128` for
shadowed pixels when starting from 128.

### Z-Pass

Z-pass stencil ops (`passOp` instead of `depthFailOp`) follow the same convention:
front=DECREMENT, back=INCREMENT (matching GL's `stencilOpSeparate` path).
Z-pass is used for "external" shadows where the camera is guaranteed to be outside
the shadow volume.

### What Does NOT Affect Stencil Shadows

| Issue | Stencil shadows affected? |
|-------|--------------------------|
| Screen-space Y flip (UV reconstruction) | No — shadows are geometry, not texture reads |
| Depth buffer Z range [0,1] for invViewProj | No — shadows don't sample the depth buffer |
| Doom 3 world-space Z-up convention | No — shadow volume vertices are in local space, transformed by MVP |

---

## 6. Ray Tracing Specific Concerns

### TLAS Coordinate Space

- **BLAS geometry**: vertices are in **local/model space** (from `tri->verts`)
- **TLAS instance transform**: `ent->modelMatrix` (model-to-world, built by
  `R_AxisToModelMatrix` from the entity's world-space `origin` and `axis`)
- **Result**: rays and light origins in world space (Doom 3 Z-up) match TLAS geometry

### Shadow Ray `tMin` Bias

Shadow rays use a fixed `tMin = 0.5` Doom-unit offset (≈1.3 cm) along the ray
direction to avoid self-intersection.  This may be insufficient for certain geometry
but is the current baseline.  Without a normal buffer, offsetting along the surface
normal is not available.

### Depth Image View for Sampling

`vk.depthView` includes both `VK_IMAGE_ASPECT_DEPTH_BIT` and
`VK_IMAGE_ASPECT_STENCIL_BIT` (needed for the framebuffer attachment).  Using a
combined depth+stencil image view as a sampler is technically non-conformant in
Vulkan; a separate depth-only view should be created for the RT shadow depth sampler
(deferred fix).

---

## 8. Quick Reference

| Question | Answer |
|----------|--------|
| Doom 3 world up axis? | Z |
| Does `viewDef->worldSpace.modelViewMatrix` include axis remap? | Yes — output is GL camera space (Y-up) |
| Depth buffer Z range in our Vulkan renderer? | [0, 1] (Vulkan) |
| `viewDef->projectionMatrix` Z range? | [-1, 1] (GL) — remap depth before using |
| Screen y=0 direction? | Top of screen |
| NDC y=+1 direction (with viewport flip)? | Top of screen |
| Correct screen→NDC Y formula? | `ndcY = 1.0 - 2.0 * (screenY / height)` |
| `s_projVk` vs `viewDef->projectionMatrix`? | `s_projVk` has Z remapped to [0,1]; only used for the rasterization pipeline draw calls — not stored in `viewDef` |
