/*
===========================================================================

dhewm3-rt Vulkan ray tracing — shared light classifier.

Doom 3 encodes "is this light real" in two independent bits:

  - entity-level parms.noShadows       (per light-entity override, mapper set)
  - material-level lightShader keyword (fogLight / ambientLight / blendLight /
    the "noshadows" material flag — see idMaterial::LightCastsShadows())

Testing only one of them lets ambient fills through and rejects small accent
lights that carry entity noShadows.  This file is the single place that makes the
real/fake call, so GI, volumetrics and the noShadows shadow unlock agree.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

idCVar r_rtLightAccentMaxRadius(
    "r_rtLightAccentMaxRadius", "300", CVAR_RENDERER | CVAR_FLOAT,
    "A noShadows light with radius below this is classed ACCENT (admitted to GI/volumetrics, "
    "eligible for the shadow unlock); at or above it, AMBIENT_FILL (never admitted).");

idCVar r_rtUnlockNoShadows(
    "r_rtUnlockNoShadows", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
    "Give ACCENT-classified noShadows lights (see r_rtLightAccentMaxRadius) a real RT shadow "
    "layer instead of rendering unshadowed. Does not affect GI/volumetric admission.");

// ---------------------------------------------------------------------------
// VK_RT_ClassifyLight
// ---------------------------------------------------------------------------
vkRTLightClass_t VK_RT_ClassifyLight(const renderLight_t &parms, const idMaterial *lightShader)
{
    // fogLight / blendLight have no physical falloff or occlusion volume in
    // the RT sense — they are 2D screen-space effects wearing a light entity.
    if (lightShader != NULL && (lightShader->IsFogLight() || lightShader->IsBlendLight()))
        return RT_LIGHT_FOG_BLEND;

    // Spectrum lights only light surfaces with the same spectrum (Interaction.cpp), so
    // they light neither the medium nor ordinary bounce surfaces.  Teleport FX use one
    // at 2x white / radius 500, which read as a white fog burst.
    if (lightShader != NULL && lightShader->Spectrum() != 0)
        return RT_LIGHT_SPECTRUM;

    // ambientLight materials are non-directional paint-bucket fills (no
    // falloff origin to speak of) — always AMBIENT_FILL regardless of radius
    // or the entity noShadows bit.
    if (lightShader != NULL && lightShader->IsAmbientLight())
        return RT_LIGHT_AMBIENT_FILL;

    const bool noShadows = parms.noShadows ||
                           (lightShader != NULL && !lightShader->LightCastsShadows());

    if (!noShadows)
        return RT_LIGHT_REAL;

    // A small accent (LED, monitor glow; mappers flag these noShadows for stencil cost)
    // or a large fill. Radius is the only signal left to tell them apart.
    float radius = Max(Max(parms.lightRadius.x, parms.lightRadius.y), parms.lightRadius.z);
    return (radius < r_rtLightAccentMaxRadius.GetFloat()) ? RT_LIGHT_ACCENT : RT_LIGHT_AMBIENT_FILL;
}

const char *VK_RT_LightClassName(vkRTLightClass_t cls)
{
    switch (cls)
    {
        case RT_LIGHT_REAL:
            return "REAL";
        case RT_LIGHT_ACCENT:
            return "ACCENT";
        case RT_LIGHT_AMBIENT_FILL:
            return "AMBIENT_FILL";
        case RT_LIGHT_FOG_BLEND:
            return "FOG_BLEND";
        case RT_LIGHT_SPECTRUM:
            return "SPECTRUM";
        default:
            return "?";
    }
}
