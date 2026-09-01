/*
===========================================================================

dhewm3-rt Vulkan — player_reflect.rchit — closest-hit shader for player body
reflection rays.

Player body instances (noSelfShadow entities) are routed to this hit group via
instanceShaderBindingTableRecordOffset = 2 in the TLAS.  The world hit group
(offset 0) is kept clean and never sees player geometry.

Lighting now uses the same shadowed rt_light_eval.glsl loop as reflect_ray.rchit
(shares this pipeline's SBT, so RT_LIGHT_SHADOW_MISS_INDEX is 2 here too) instead
of a hand-rolled unshadowed distance/NdotL sum — the old loop had no occlusion
test at all, so any light in range lit the player reflection at full strength
even through walls, reading as a flat, too-bright reflection regardless of the
room's actual lighting.

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

// TLAS needed to fire shadow rays from this hit shader.
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

#include "rt_material.glsl"
#include "reflect_payload.glsl"
#include "gi_shadow_payload.glsl"

layout(location = 0) rayPayloadInEXT ReflPayload reflPayload;

// Shared light loop (P5) — declares the light SSBO (set=0, binding=4) and the
// location-1 shadow payload. Miss index 2 = gi_shadow.rmiss's slot in this
// pipeline (this hit group shares reflect_ray.rchit's pipeline/SBT).
#define RT_LIGHT_SHADOW_MISS_INDEX 2
#include "rt_light_eval.glsl"

hitAttributeEXT vec2 baryCoord;

// Same budget/bias constants as reflect_ray.rchit — see that file for rationale.
#define REFL_MAX_SHADOW_LIGHTS 8
#define REFL_SHADOW_BIAS       0.5
#define REFL_SHADOW_MIN_LUM    0.02
#define REFL_AMBIENT           0.01

void main()
{
    // Mirror distance: look up diffuse texture and apply per-light shadowed irradiance.
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);
    if (matIdx >= uint(materials.length()))
    {
        reflPayload.colour        = vec3(0.0);
        reflPayload.transmittance = 0.0;
        return;
    }

    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);

    if (rtLightBuf.numLights > 0)
    {
        vec3 hitPos  = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
        vec3 hitNorm = rt_InterpolateNormal(matIdx, gl_PrimitiveID, baryCoord);
        if (dot(hitNorm, -gl_WorldRayDirectionEXT) < 0.0)
            hitNorm = -hitNorm;

        vec3 irradiance = vec3(REFL_AMBIENT) +
                          rt_EvalDirectLighting(hitPos, hitNorm, RT_LIGHT_MAX_LIGHTS,
                                                REFL_MAX_SHADOW_LIGHTS, REFL_SHADOW_BIAS,
                                                1.0, REFL_SHADOW_MIN_LUM);
        reflPayload.colour = diffuse.rgb * irradiance;
    }
    else
    {
        reflPayload.colour = diffuse.rgb;
    }

    reflPayload.transmittance = 0.0;
}
