/* vol_froxel_sample.glsl — froxel slice mapping, with no bindings of its own.

  The grid math every froxel pass shares, as pure functions taking their
  parameters explicitly.  vol_froxel_common.glsl declares the froxel params UBO
  at set=0 binding=2 and forwards to these; shaders outside the froxel passes
  (the reflection rgen, whose binding 2 is something else entirely) include this
  file alone and supply the values from their own UBO.

  Also holds the world -> texture-coordinate direction, which the froxel passes
  themselves never need: they walk cell -> world (fill) or screen+depth -> cell
  (resolve). Marching an arbitrary ray through the scatter volume is what wants
  the forward projection.

  See docs/plans/20261013_reflection_volumetrics.md.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef VOL_FROXEL_SAMPLE_GLSL
#define VOL_FROXEL_SAMPLE_GLSL

// Z-slice distribution of the froxel grid.  Built from the froxel params UBO by
// vol_froxel_common.glsl, or from its own copy of the fields by anyone else.
struct VolFroxelSlices {
    float dNear;       // depthParams.x — near anchor, world units
    float logRange;    // rangeParams.x — log(dFar / dNear)
    float invLogRange; // rangeParams.y — 1 / logRange
    int   nz;          // gridDim.z — slice count
};

// ---------------------------------------------------------------------------
// Slice <-> planar view distance.  zc is a CONTINUOUS slice coordinate: cell
// centre is float(cell.z) + 0.5, so zc spans [0, nz] across the whole grid.
//
//   dist(zc) = dNear * (dFar/dNear)^(zc/nz) = dNear * exp(zc/nz * logRange)
//
// Spacing is therefore proportional to distance, which is what keeps cells
// roughly CUBICAL: a cell's XY world size already grows linearly with distance
// (perspective), and anisotropic cells are what alias.  That — not any
// inverse-square argument — is why the curve is exponential.  Do not make the
// distribution humped: mid-dense would leave the far cells, where the
// surface-straddle error is worst, exactly as coarse as they are now.
//
// The range is anchored at BOTH ends (F2).  It used to run [0, maxDist], which
// put ~22% of the slices inside r_znear and the outer half of the grid below
// 2% transmittance.  See VK_RT_BuildFroxelParams for how dNear/dFar are derived.
// ---------------------------------------------------------------------------
float vfs_SliceToDist(VolFroxelSlices s, float zc)
{
    float nz = float(max(s.nz, 1));
    return s.dNear * exp(clamp(zc / nz, 0.0, 1.0) * s.logRange);
}

// Normalised W texture coordinate (0..1) for a planar view distance — what a
// trilinear sampler3D fetch wants.  Clamped, so test vfs_InGrid separately when
// "outside the grid" has to mean no data rather than the edge slice.
float vfs_DistToTexW(VolFroxelSlices s, float dist)
{
    float dNear = max(s.dNear, 1e-4);
    return clamp(log(max(dist, dNear) / dNear) * s.invLogRange, 0.0, 1.0);
}

// Does this column/distance pair land inside the grid's own extent?  Tests the
// raw distance, because vfs_DistToTexW has already clamped its result and a
// point past dFar is indistinguishable from one on the last slice.
bool vfs_InGrid(VolFroxelSlices s, vec2 uv, float planarDist)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return false;

    float dFar = s.dNear * exp(s.logRange);
    return planarDist >= s.dNear && planarDist <= dFar;
}

// ---------------------------------------------------------------------------
// World position -> froxel texture coordinate.  The inverse of vf_CellWorldPos:
// XY from the projection, W from the PLANAR view distance (the component along
// the view axis), which is what makes the slices flat planes rather than
// spherical shells.
//
// viewProj must be the GL-convention matrix matching the invViewProj the grid
// was built with (Z in [-1,1]); only xy/w are read, so its Z row is irrelevant.
// The UV flip mirrors vf_RayDirForUV's ndc.y = 1 - 2*uv.y — the viewport has
// negative height.
//
// Returns false when the point is behind the eye or outside the grid, leaving
// texCoord untouched; there is no sensible clamped answer in that case, and
// clamping would smear the edge slice along the whole ray.
// ---------------------------------------------------------------------------
bool vfs_WorldToTexCoord(VolFroxelSlices s, mat4 viewProj, vec3 camPos, vec3 camFwd,
                         vec3 worldPos, out vec3 texCoord)
{
    texCoord = vec3(0.0);

    vec4 clip = viewProj * vec4(worldPos, 1.0);
    if (clip.w <= 1e-6)
        return false; // behind the eye: the perspective divide is meaningless

    vec2 ndc = clip.xy / clip.w;
    vec2 uv  = vec2((ndc.x + 1.0) * 0.5, (1.0 - ndc.y) * 0.5);

    float planarDist = dot(worldPos - camPos, camFwd);
    if (!vfs_InGrid(s, uv, planarDist))
        return false;

    texCoord = vec3(uv, vfs_DistToTexW(s, planarDist));
    return true;
}

#endif // VOL_FROXEL_SAMPLE_GLSL
