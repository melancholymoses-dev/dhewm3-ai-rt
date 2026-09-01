/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan — reflect_ray.rchit — closest-hit shader for reflection rays.

Phase 5.4: samples the actual diffuse texture of the hit surface using the
material table (set=1).  UVs are barycentrically interpolated from the hit
triangle's vertex buffer via GL_EXT_buffer_reference.

Phase 5.4b: glass branch.  Translucent surfaces (MC_TRANSLUCENT, flagged
MAT_FLAG_GLASS) get a flat 4 % reflectance (F0 for real glass at normal
incidence).  The remaining 96 % is passed through to the next bounce via
reflPayload.transmittance + nextOrigin/nextDir.  rgen traces the continuation
ray on the next loop iteration.

Lighting: opaque hit surfaces are shaded via the shared rt_light_eval.glsl
loop (P5) over the same GI light list (set=0, binding=4) that gi_ray.rchit
uses for bounce lighting.  Shadow rays are budgeted at REFL_MAX_SHADOW_LIGHTS
per hit (miss index 2 → gi_shadow.rmiss) and skipped for lights whose
contribution at the hit is below REFL_SHADOW_MIN_LUM (P4); lights past the
budget/threshold fall back to NdotL × falloff only so the cap degrades
gracefully.  When no lights are available (numLights == 0) the shader falls
back to raw diffuse albedo (flat shading).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing                              : require
#extension GL_EXT_buffer_reference                         : require
#extension GL_EXT_buffer_reference2                        : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64   : require
#extension GL_EXT_nonuniform_qualifier                     : enable

// TLAS needed to fire shadow rays from this hit shader.
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

#include "rt_material.glsl"
#include "reflect_payload.glsl"
#include "gi_shadow_payload.glsl"

layout(location = 0) rayPayloadInEXT ReflPayload reflPayload;

// Shared light loop (P5) — declares the light SSBO (set=0, binding=4, the same GI
// light buffer vk_reflections.cpp binds) and the location-1 shadow payload.
// Miss index 2 = gi_shadow.rmiss's slot in the reflection pipeline's miss region.
#define RT_LIGHT_SHADOW_MISS_INDEX 2
#include "rt_light_eval.glsl"

// Barycentric coordinates set by the built-in triangle intersection stage.
// baryCoord.x = weight of vertex 1, baryCoord.y = weight of vertex 2.
// Weight of vertex 0 = 1.0 - baryCoord.x - baryCoord.y.
hitAttributeEXT vec2 baryCoord;

// Cap shadow rays per reflection hit for performance. Lights arrive importance-
// ordered for the *camera* (L1, vk_gi.cpp), so this trades quality for cost.
// Increase to 16–32 if shadows look incomplete in heavily-lit scenes.
#define REFL_MAX_SHADOW_LIGHTS 8
#define REFL_SHADOW_BIAS       0.5   // world-unit offset to avoid self-shadowing
// P4: lights contributing less than this (luminance of colour·intensity·NdotL·atten)
// at the hit point don't spend a shadow ray — the list is ordered for the *camera*,
// and reflection hits are often far from it, so without this the 8-ray budget goes
// to lights that barely reach the hit.  They still contribute unshadowed, exactly
// like lights past the budget.
#define REFL_SHADOW_MIN_LUM    0.02

// Small ambient floor so surfaces with no nearby lights aren't fully black.
#define REFL_AMBIENT           0.01

void main()
{
    // matIdx = instanceCustomIndex (== baseGeomIdx of instance) + geometry index.
    // Each BLAS geometry has its own VkMaterialEntry so this gives the correct
    // per-surface texture, flags, etc.
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);

    // Guard: out-of-range custom index → return black with no continuation.
    if (matIdx >= uint(materials.length()))
    {
        reflPayload.colour        = vec3(0.0);
        reflPayload.transmittance = 0.0;
        return;
    }

    MaterialEntry mat = materials[matIdx];
    if ((mat.flags & MAT_FLAG_GLASS) != 0u)
    {
        // Thin-glass approximation: flat F0 = 0.05 (5 % reflectance at all angles).
        // The reflected colour is tinted by the glass diffuse texture.
        // The remaining 95 % continues straight through (no refraction).
        // Glass tint is left unlit — it is a transmission colour, not a surface.
        const float F0      = 0.1;
        const float transmit = 1.0 - F0;

        vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
        reflPayload.colour        = F0 * diffuse.rgb;
        reflPayload.transmittance = transmit;
        // Continuation ray: start just past the glass surface, same direction.
        reflPayload.nextOrigin = gl_WorldRayOriginEXT
                               + gl_WorldRayDirectionEXT * gl_HitTEXT
                               + gl_WorldRayDirectionEXT * 0.01;
        reflPayload.nextDir = gl_WorldRayDirectionEXT;
        return;
    }

    // Emissive surfaces return their emitted colour directly (no lighting evaluation).
    vec3 emissive = rt_EvalEmissiveRadiance(matIdx, gl_PrimitiveID, baryCoord);
    if (dot(emissive, emissive) > 0.001)
    {
        reflPayload.colour        = emissive * rtLightBuf.emissiveScale;
        reflPayload.transmittance = 0.0;
        return;
    }

    // Opaque surface — sample diffuse and apply per-light direct irradiance.
    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);

    if (rtLightBuf.numLights > 0)
    {
        vec3 hitPos  = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
        vec3 hitNorm = rt_InterpolateNormal(matIdx, gl_PrimitiveID, baryCoord);
        // Flip normal if ray hit backface so the hemisphere faces the incoming ray.
        if (dot(hitNorm, -gl_WorldRayDirectionEXT) < 0.0)
            hitNorm = -hitNorm;

        // contribScale 1.0: reflections show first-hit direct light; bounceScale is a
        // GI-bounce-only knob (the pre-P5 drift where GI applied it and reflections
        // didn't is now explicit here).
        vec3 irradiance = vec3(REFL_AMBIENT) +
                          rt_EvalDirectLighting(hitPos, hitNorm, RT_LIGHT_MAX_LIGHTS,
                                                REFL_MAX_SHADOW_LIGHTS, REFL_SHADOW_BIAS,
                                                1.0, REFL_SHADOW_MIN_LUM);
        reflPayload.colour = diffuse.rgb * irradiance;
    }
    else
    {
        // No lights uploaded (GI not active) — fall back to flat albedo.
        reflPayload.colour = diffuse.rgb;
    }

    reflPayload.transmittance = 0.0;
}
