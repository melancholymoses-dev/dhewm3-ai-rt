/*
===========================================================================

Roughness-aware Schlick Fresnel, shared by the reflection pipeline.

Shared because Stage 2b demodulates: reflect_ray.rgen stores incident light with
this weight in alpha, and refl_upsample.comp recomputes the SAME weight at full
resolution from the full-res G-buffer (Stage 4c). Two copies that drifted apart
would make the half-res path differ from the full-res one for no visible reason.

The grazing term uses the split-sum IBL form rather than a hard 1.0: a rough
surface's lobe is wide enough that the mirror-direction Fresnel overstates the
edge, which is what turns rough concrete into a chrome rim at grazing angles.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#ifndef REFL_FRESNEL_GLSL
#define REFL_FRESNEL_GLSL

// f0: per-pixel reflectance at normal incidence (gbufNormal.a).
// perceptualRoughness: Stage 2's packed roughness AFTER r_rtReflRoughnessScale.
// NdotV: against the macro (shading) normal, not the GGX half-vector — keeps the
// pre-trace cull exact without needing the lobe sampled first.
float ReflFresnel(float f0, float perceptualRoughness, float NdotV)
{
    float fGrazing = max(1.0 - perceptualRoughness, f0);
    return f0 + (fGrazing - f0) * pow(1.0 - NdotV, 5.0);
}

#endif // REFL_FRESNEL_GLSL
