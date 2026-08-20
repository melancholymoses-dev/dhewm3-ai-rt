/*
===========================================================================

dhewm3-rt Vulkan ray tracing — shared light classifier (auto_relight.md §0).

Doom 3 encodes "is this light real" in two independent bits that the RT path
used to test in different combinations depending on which pass was asking:

  - entity-level parms.noShadows       (per light-entity override, mapper set)
  - material-level lightShader keyword (fogLight / ambientLight / blendLight /
    the "noshadows" material flag — see idMaterial::LightCastsShadows())

The GI/volumetric light collector (vk_gi.cpp) historically tested only the
entity bit.  That let material-level ambient-fill lights (paint-bucket washes
with no falloff origin) through GI/vol while rejecting small mapper-placed
colored accents that happen to carry entity noShadows — backwards from what
either pass wants.  This file is the single place that makes the real/fake
call, so GI, volumetrics, and the (future) noShadows shadow unlock can't
drift out of sync on the answer.

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
    "auto_relight.md §0/§6: a noShadows light with radius below this is classed ACCENT "
    "(admitted to GI/volumetrics, eligible for the noShadows shadow unlock); at or above "
    "it, classed AMBIENT_FILL (semantic mapper fill — never admitted, never unlocked)");

// ---------------------------------------------------------------------------
// VK_RT_ClassifyLight
// ---------------------------------------------------------------------------
vkRTLightClass_t VK_RT_ClassifyLight(const renderLight_t &parms, const idMaterial *lightShader)
{
    // fogLight / blendLight have no physical falloff or occlusion volume in
    // the RT sense — they are 2D screen-space effects wearing a light entity.
    if (lightShader != NULL && (lightShader->IsFogLight() || lightShader->IsBlendLight()))
        return RT_LIGHT_FOG_BLEND;

    // ambientLight materials are non-directional paint-bucket fills (no
    // falloff origin to speak of) — always AMBIENT_FILL regardless of radius
    // or the entity noShadows bit.
    if (lightShader != NULL && lightShader->IsAmbientLight())
        return RT_LIGHT_AMBIENT_FILL;

    const bool noShadows = parms.noShadows ||
                           (lightShader != NULL && !lightShader->LightCastsShadows());

    if (!noShadows)
        return RT_LIGHT_REAL;

    // noShadows and not material-ambient: either a small placed accent (LED,
    // monitor glow, alarm strobe — usually entity-flagged noShadows by the
    // mapper for stencil-shadow cost, not because it's fake) or a large
    // noShadows fill used the same way ambientLight is used semantically.
    // Radius is the only signal left to tell them apart.
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
        default:
            return "?";
    }
}
