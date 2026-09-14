/* vol_froxel_common.glsl — shared froxel-grid geometry for the volumetric
  froxel passes (20260906_froxel_probe_gi.md, Part A).

  The grid is camera-frustum shaped: X/Y follow screen position, Z slices are
  PLANAR (constant view-space depth) and exponentially distributed with the same
  spacing vol_march.comp's per-pixel march uses, so an A/B between the two
  compares like with like.

    dist(zc) = exp((zc / Nz) * log(maxDist + 1)) - 1      zc in [0, Nz]
    zc(dist) = log(dist + 1) / log(maxDist + 1) * Nz

  Planar (not radial) slices are what make the resolve cheap: a pixel's slice
  coordinate is a scalar function of its linear depth, with no per-pixel ray
  length involved.

  Includer contract: declares the set=0 binding=2 params UBO below, so every
  froxel pass must place its params block there.  Needs no other bindings — in
  particular NOT depthSampler, which is why this file does not include
  rt_indirect.glsl and carries its own copy of wang_hash/randFloat.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef VOL_FROXEL_COMMON_GLSL
#define VOL_FROXEL_COMMON_GLSL

// std140 — offsets must match VolFroxelParamsUBO in vk_vol_froxel.cpp exactly.
// Deliberately vec4-packed rather than scalar-packed: the scalar layout in
// VolParamsUBO has already cost two offset-mismatch debugging sessions.
layout(set = 0, binding = 2, std140) uniform VolFroxelParams {
    mat4  invViewProj;  //   0  GL-convention clip Z, see vf_RayDirForUV
    vec4  cameraPosW;   //  64  xyz = camera world position
    vec4  camForwardW;  //  80  xyz = viewaxis[0]
    ivec4 gridDim;      //  96  xyz = Nx,Ny,Nz   w = cluster shift (F3)
    vec4  depthParams;  // 112  x=dNear y=dFar z=linNum w=linAdd
    vec4  rangeParams;  // 128  x=logRange=log(dFar/dNear) y=1/logRange z=maxDist w=unused
    vec4  densities;    // 144  x=point y=directed z=flashlight w=whiteNoiseMix
    vec4  strengths;    // 160  x=point y=directed z=flashlight w=temporalAlpha
    vec4  anisos;       // 176  x=point y=directed z=flashlight w=unused
    ivec4 misc;         // 192  x=frameIndex y=maxLights z=debugMode w=debugSlice
    ivec4 screen;       // 208  x=screenW y=screenH z=outW w=outH  (resolve target)
    ivec4 rect;         // 224  resolve dispatch rect, resolve-target space
    mat4  prevViewProj; // 240  F4 reprojection; identity until then
} fp;

// ---------------------------------------------------------------------------
// Wang hash — byte-identical to rt_indirect.glsl's, duplicated because this
// file cannot include that one (it requires a depthSampler binding).
// ---------------------------------------------------------------------------
uint vf_wang_hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed  = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed  = seed ^ (seed >> 15u);
    return seed;
}

float vf_randFloat(uint seed)
{
    return float(vf_wang_hash(seed)) / 4294967296.0;
}

// ---------------------------------------------------------------------------
// Slice <-> planar view distance.  zc is a CONTINUOUS slice coordinate: cell
// centre is float(cell.z) + 0.5, so zc spans [0, Nz] across the whole grid.
//
//   dist(zc) = dNear * (dFar/dNear)^(zc/Nz) = dNear * exp(zc/Nz * logRange)
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
float vf_SliceToDist(float zc)
{
    float nz = float(max(fp.gridDim.z, 1));
    return fp.depthParams.x * exp(clamp(zc / nz, 0.0, 1.0) * fp.rangeParams.x);
}

// Normalised W texture coordinate (0..1) for a planar view distance — what the
// resolve feeds to a trilinear sampler3D fetch.
float vf_DistToTexW(float dist)
{
    float dNear = max(fp.depthParams.x, 1e-4);
    return clamp(log(max(dist, dNear) / dNear) * fp.rangeParams.y, 0.0, 1.0);
}

float vf_DistToSlice(float dist)
{
    return vf_DistToTexW(dist) * float(max(fp.gridDim.z, 1));
}

// ---------------------------------------------------------------------------
// Depth buffer -> planar view distance.  Same idiom as BilateralPC's
// linNum/linAdd in vk_vol.cpp: the projection matrix is GL-convention, so the
// Vulkan [0,1] depth is un-remapped to NDC with 2*d - 1 first.
// ---------------------------------------------------------------------------
float vf_LinearDepth(float depth)
{
    return abs(fp.depthParams.z / (2.0 * depth - 1.0 + fp.depthParams.w));
}

// ---------------------------------------------------------------------------
// World-space ray direction through grid column uv (0..1 across the frustum).
//
// Clip-space convention copied from rt_ReconstructWorldPos: the projection
// matrix is GL-convention (Z in [-1,1]) and the viewport has negative height,
// hence the Y flip.  Do not substitute Vulkan [0,1] Z here.
//
// The direction is near-plane-minus-EYE, never near-minus-far, because Doom 3
// builds an INFINITE far plane: R_SetupProjection leaves proj[10] at -0.999 and
// proj[14] at -2*znear, so ndcZ = -proj[10] + proj[14]/d asymptotes to +0.999 as
// d goes to infinity and never reaches +1.  Unprojecting ndcZ = +1 therefore
// lands at d = proj[14]/(1 + proj[10]) = -6/0.001 = -6000 — six thousand units
// BEHIND the eye.  That made pFar - pNear point backwards, dot(rayDir, forward)
// negative, its clamp pin zFac at 1e-4, and every cell position fly off to
// infinity: overlay mode 3 came back saturated red on first run (2026-09-13).
// ---------------------------------------------------------------------------
vec3 vf_RayDirForUV(vec2 uv)
{
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);

    vec4 nearH = fp.invViewProj * vec4(ndc, -1.0, 1.0);
    vec3 pNear = nearH.xyz / nearH.w;

    return normalize(pNear - fp.cameraPosW.xyz);
}

// ---------------------------------------------------------------------------
// Cell centre in world space.  `jitter` is the sub-cell offset in [0,1)^3;
// pass vec3(0.5) for the exact centre.
//
// dist is a PLANAR view depth, so the ray parameter is dist / cos(angle to the
// view axis) — that division is what makes the slices flat planes rather than
// spherical shells.
// ---------------------------------------------------------------------------
vec3 vf_CellWorldPos(ivec3 cell, vec3 jitter, out vec3 rayDir)
{
    vec2 uv = (vec2(cell.xy) + jitter.xy) / vec2(max(fp.gridDim.xy, ivec2(1)));
    rayDir  = vf_RayDirForUV(uv);

    float zFac = max(dot(rayDir, fp.camForwardW.xyz), 1e-4);
    float dist = vf_SliceToDist(float(cell.z) + jitter.z);

    return fp.cameraPosW.xyz + rayDir * (dist / zFac);
}

// ---------------------------------------------------------------------------
// Per-cell, per-frame sub-cell jitter.
//
// Interleaved Gradient Noise over the cell's XY (spatially coherent — the same
// property that stopped vol_march.comp's beams reading as noise), decorrelated
// per Z slice, rotated per frame by the golden ratio, with a little white noise
// blended in to break the diagonal grid (r_rtVolWhiteNoiseMix, same trade-off
// as the march's jitter).
//
// XY is returned as the exact cell centre for now: jittering XY only pays off
// once the froxel-space EMA (F4) can average the variance away, and without it
// it is pure added noise.
// ---------------------------------------------------------------------------
vec3 vf_CellJitter(ivec3 cell)
{
    vec2 seed = vec2(cell.xy) + vec2(float(cell.z) * 7.0, float(cell.z) * 13.0);
    float ign = fract(52.9829189 * fract(dot(seed, vec2(0.06711056, 0.00583715))));

    uint frame = uint(fp.misc.x);
    float white = vf_randFloat(uint(cell.x) * 1973u ^ vf_wang_hash(uint(cell.y) * 9277u) ^
                               vf_wang_hash(uint(cell.z) * 26699u) ^ vf_wang_hash(frame * 4079u));

    float jz = fract(mix(ign, white, fp.densities.w) + float(frame) * 0.6180339887);

    return vec3(0.5, 0.5, jz);
}

#endif // VOL_FROXEL_COMMON_GLSL
