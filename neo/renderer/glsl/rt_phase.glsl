/* rt_phase.glsl — Henyey-Greenstein scattering phase, shared by every volumetric
  consumer (20261013_reflection_volumetrics.md Stage 5).

  Previously vol_march.comp and vol_froxel_fill.comp each carried their own copy
  with a comment asking that they stay identical.  Stage 5 adds a third consumer
  (vol_froxel_integrate.comp) and a fourth (reflect_ray.rgen), so the function
  moved here instead.

  Stage 5's actual change is rt_PhaseFromMeanDir.  The froxel fill used to bake
  phase(dot(camToCell, cellToLight)) into each cell, which made froxelScatter a
  CAMERA-path quantity dressed up as a ray-agnostic one: a reflected ray reading
  it inherited the primary view's lobe.  That is catastrophic for the flashlight
  specifically, because the light sits on the camera — cos(theta) pins to ~-1
  across the whole beam (the dimmest end of the lobe, and near-constant, so the
  beam carries no angular structure at all), while a ray reflected back toward
  the eye wants ~+1, the brightest.  Measured 21x at g=0.7, isoMix=0.45.

  So the grid now stores the in-scatter unphased plus a mean scattering
  direction, and each consumer applies the phase for ITS OWN ray direction.

  Direction convention: the stored vector points from the cell TOWARD the light,
  matching the `lightDir` the fill loop already had, and rayDir is the ray's
  direction of travel.  Keeping the old convention means no sign flips anywhere.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_PHASE_GLSL
#define RT_PHASE_GLSL

#define RT_PHASE_INV_4PI 0.0795774715

float rt_HenyeyGreenstein(float cosTheta, float g)
{
    float g2    = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265359 * pow(max(denom, 1e-2), 1.5));
}

// Forward HG lobe mixed with an isotropic one.  isoMix (r_rtVolIsotropicMix)
// raises side-on shaft visibility without flattening the lobe itself; it is also
// the only part of the phase that transported correctly before Stage 5, being
// direction-independent.
float rt_PhaseFunction(float cosTheta, float g, float isoMix)
{
    return mix(rt_HenyeyGreenstein(cosTheta, g), RT_PHASE_INV_4PI, clamp(isoMix, 0.0, 1.0));
}

// Stage 5 — the phase for `rayDir` from a cell's stored mean scattering
// direction.  |meanDir| IS the effective mean cosine g: the fill accumulates
// sum(w * g_i * lightDir_i) / sum(w), so a cell lit from one direction keeps its
// light's own g while a cell lit from several gets a shorter vector and reads
// more isotropic — which is the physically sensible degradation, and why g is
// encoded as the length rather than stored in a fourth channel.
//
// Trilinear filtering of the direction grid shortens the vector between cells
// with disagreeing directions, so interpolation degrades the same graceful way.
float rt_PhaseFromMeanDir(vec3 rayDir, vec3 meanDir, float isoMix)
{
    float g = length(meanDir);
    if (g < 1e-4)
        return RT_PHASE_INV_4PI; // no dominant direction: isotropic, no divide by zero
    return rt_PhaseFunction(dot(rayDir, meanDir / g), g, isoMix);
}

#endif // RT_PHASE_GLSL
