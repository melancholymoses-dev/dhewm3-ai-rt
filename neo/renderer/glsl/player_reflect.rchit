/*
===========================================================================

dhewm3-rt Vulkan — player_reflect.rchit — closest-hit shader for player body
reflection rays.

Player body instances (noSelfShadow entities) are routed to this hit group via
instanceShaderBindingTableRecordOffset = 2 in the TLAS.  The world hit group
(offset 0) is kept clean and never sees player geometry.

Two cases based on ray travel distance:

  Close range (< PLAYER_REFLECT_SKIP_DIST):
    Ray is most likely fired from a floor or wall surface adjacent to the
    player's own body — e.g. a reflective floor directly under the player's
    feet.  Return transmittance = 1.0 so the rgen bounce loop fires a
    continuation ray straight through the player model, preventing the
    player's dark geometry from appearing as blotches in floor reflections.

  Mirror distance (>= PLAYER_REFLECT_SKIP_DIST):
    Ray arrived from a reflective surface far enough away that the player
    body is a genuine scene element — e.g. a wall mirror.  Sample the
    diffuse texture and return it normally so the player appears in the
    reflection.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing                              : require
#extension GL_EXT_buffer_reference                         : require
#extension GL_EXT_buffer_reference2                        : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64   : require
#extension GL_EXT_nonuniform_qualifier                     : enable

#include "rt_material.glsl"
#include "reflect_payload.glsl"

layout(location = 0) rayPayloadInEXT ReflPayload reflPayload;
hitAttributeEXT vec2 baryCoord;

// ---------------------------------------------------------------------------
// GI light list — same buffer as reflect_ray.rchit uses for irradiance.
// ---------------------------------------------------------------------------
struct ReflGILight {
    vec4 posRadius;      // xyz = world pos, w = bounding radius
    vec4 colorIntensity; // rgb = light colour, a = intensity
};

layout(set = 0, binding = 4, std430) readonly buffer ReflLightBuf {
    int        numLights;
    float      bounceScale;
    float      giRadius;    // r_rtGIRadius — max range for light evaluation
    int        pad1;
    ReflGILight lights[];
} reflLightBuf;

// Reflection rays originating within this distance of the player body are
// treated as pass-through (transmittance = 1.0) to prevent self-occlusion
// blotches in floor / adjacent-surface reflections.
// Value should exceed the player's bounding height (~56 units in Doom3 scale).
const float PLAYER_REFLECT_SKIP_DIST = 10.0;
#define REFL_MAX_LIGHTS 128

void main()
{
    if (gl_HitTEXT < PLAYER_REFLECT_SKIP_DIST)
    {
        // Pass-through: ray continues past the player body.
        reflPayload.colour        = vec3(0.0);
        reflPayload.transmittance = 1.0;
        reflPayload.nextOrigin    = gl_WorldRayOriginEXT
                                  + gl_WorldRayDirectionEXT * (gl_HitTEXT + 0.5);
        reflPayload.nextDir       = gl_WorldRayDirectionEXT;
        return;
    }

    // Mirror distance: look up diffuse texture and apply per-light irradiance.
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);
    if (matIdx >= uint(materials.length()))
    {
        reflPayload.colour        = vec3(0.0);
        reflPayload.transmittance = 0.0;
        return;
    }

    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);

    if (reflLightBuf.numLights > 0)
    {
        vec3 hitPos  = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
        vec3 hitNorm = rt_InterpolateNormal(matIdx, gl_PrimitiveID, baryCoord);
        if (dot(hitNorm, -gl_WorldRayDirectionEXT) < 0.0)
            hitNorm = -hitNorm;

        const float kAmbient = 0.01;
        vec3 irradiance = vec3(kAmbient);
        int n = min(reflLightBuf.numLights, REFL_MAX_LIGHTS);
        for (int i = 0; i < n; i++)
        {
            vec3  lPos   = reflLightBuf.lights[i].posRadius.xyz;
            float lRad   = reflLightBuf.lights[i].posRadius.w;
            vec3  lColor = reflLightBuf.lights[i].colorIntensity.rgb;
            float lInt   = reflLightBuf.lights[i].colorIntensity.a;
            vec3  toL    = lPos - hitPos;
            float dist   = length(toL);
            // Match reflect_ray.rchit: extend eval range beyond lRad so player
            // geometry is still reached by room lights whose origin is far away.
            float evalRadius = max(reflLightBuf.giRadius, lRad * 2.0);
            if (dist >= evalRadius || dist < 0.01) continue;
            float NdotL  = dot(hitNorm, toL / dist);
            if (NdotL <= 0.0) continue;
            // Normalized inverse-square: continuous, no hard cutoff at lRad.
            float t = dist / lRad;
            float atten = 1.0 / (t * t + 1.0);
            if (atten < 0.02) continue;
            irradiance += lColor * lInt * NdotL * atten;
        }
        reflPayload.colour = diffuse.rgb * irradiance;
    }
    else
    {
        reflPayload.colour = diffuse.rgb;
    }

    reflPayload.transmittance = 0.0;
}
