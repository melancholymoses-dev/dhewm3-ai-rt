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

Lighting: opaque hit surfaces are shaded using the same GI light list
(set=0, binding=4) that gi_ray.rchit uses for bounce lighting.  Shadow rays
are fired for the first REFL_MAX_SHADOW_LIGHTS candidates per hit (miss
index 2 → gi_shadow.rmiss); remaining lights fall back to NdotL × falloff
only so the cap degrades gracefully.  When no lights are available
(numLights == 0) the shader falls back to raw diffuse albedo (flat shading).

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

layout(location = 0) rayPayloadInEXT ReflPayload         reflPayload;
layout(location = 1) rayPayloadEXT   GIShadowPayload     reflShadow;

// ---------------------------------------------------------------------------
// GI light list — same buffer as gi_ray.rchit uses for bounce evaluation.
// Provided by vk_reflections.cpp at set=0, binding=4 (GI light SSBO).
// ---------------------------------------------------------------------------
struct ReflGILight {
    vec4 posRadius;      // xyz = world pos, w = bounding radius
    vec4 colorIntensity; // rgb = light colour, a = intensity
};

layout(set = 0, binding = 4, std430) readonly buffer ReflLightBuf {
    int        numLights;
    float      bounceScale;   // unused here; kept for layout compatibility
    float      giRadius;      // r_rtGIRadius — max range for light evaluation
    float      emissiveScale; // r_rtGIEmissiveScale — emissive surface multiplier
    ReflGILight lights[];
} reflLightBuf;

// Barycentric coordinates set by the built-in triangle intersection stage.
// baryCoord.x = weight of vertex 1, baryCoord.y = weight of vertex 2.
// Weight of vertex 0 = 1.0 - baryCoord.x - baryCoord.y.
hitAttributeEXT vec2 baryCoord;

#define REFL_MAX_LIGHTS      128
// Cap shadow rays per reflection hit for performance. Lights are iterated
// in buffer order (closest not guaranteed), so this trades quality for cost.
// Increase to 16–32 if shadows look incomplete in heavily-lit scenes.
#define REFL_MAX_SHADOW_LIGHTS 8
#define REFL_SHADOW_BIAS       0.5   // world-unit offset to avoid self-shadowing

// ---------------------------------------------------------------------------
// rt_ReflEvalLighting — evaluate direct irradiance at a surface point using
// the scene light list.  Shadow rays are fired for the first REFL_MAX_SHADOW_LIGHTS
// candidates; remaining lights use NdotL × falloff only (no shadow test) so the
// cap degrades gracefully rather than going black.
// Returns vec3(kAmbient) as a floor when no lights reach the surface.
// ---------------------------------------------------------------------------
vec3 rt_ReflEvalLighting(vec3 hitPos, vec3 hitNorm)
{
    // Small ambient floor so surfaces with no nearby lights aren't fully black.
    const float kAmbient = 0.01;
    vec3 irradiance = vec3(kAmbient);

    int n = min(reflLightBuf.numLights, REFL_MAX_LIGHTS);
    int shadowsUsed = 0;
    for (int i = 0; i < n; i++)
    {
        vec3  lPos   = reflLightBuf.lights[i].posRadius.xyz;
        float lRad   = reflLightBuf.lights[i].posRadius.w;
        vec3  lColor = reflLightBuf.lights[i].colorIntensity.rgb;
        float lInt   = reflLightBuf.lights[i].colorIntensity.a;

        vec3  toL  = lPos - hitPos;
        float dist = length(toL);
        if (dist >= lRad || dist < 0.01)
            continue;

        vec3  lightDir = toL / dist;
        float NdotL = dot(hitNorm, lightDir);
        if (NdotL <= 0.0)
            continue;

        // Quadratic falloff matching GI: zero at the edge of light radius, hard cutoff beyond.
        float t     = dist / max(lRad, 1.0);
        float atten = t < 1.0 ? 1.0 - t * t : 0.0;
        if (atten <= 0.0)
            continue;

        // Shadow ray — fire for the first REFL_MAX_SHADOW_LIGHTS candidates.
        // miss index 2 → gi_shadow.rmiss (sets occluded=false when unblocked).
        if (shadowsUsed < REFL_MAX_SHADOW_LIGHTS)
        {
            reflShadow.occluded = true;
            traceRayEXT(
                tlas,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF,
                0,              // sbt hit offset (unused — skip closest hit)
                0,              // sbt stride
                2,              // miss index 2 → gi_shadow.rmiss
                hitPos + hitNorm * REFL_SHADOW_BIAS,
                0.0,
                lightDir,
                dist - REFL_SHADOW_BIAS,
                1               // payload location 1
            );
            shadowsUsed++;
            if (reflShadow.occluded)
                continue;
        }

        irradiance += lColor * lInt * NdotL * atten;
    }
    return irradiance;
}

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
        const float F0      = 0.05;
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
        reflPayload.colour        = emissive * reflLightBuf.emissiveScale;
        reflPayload.transmittance = 0.0;
        return;
    }

    // Opaque surface — sample diffuse and apply per-light direct irradiance.
    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);

    if (reflLightBuf.numLights > 0)
    {
        vec3 hitPos  = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
        vec3 hitNorm = rt_InterpolateNormal(matIdx, gl_PrimitiveID, baryCoord);
        // Flip normal if ray hit backface so the hemisphere faces the incoming ray.
        if (dot(hitNorm, -gl_WorldRayDirectionEXT) < 0.0)
            hitNorm = -hitNorm;

        reflPayload.colour = diffuse.rgb * rt_ReflEvalLighting(hitPos, hitNorm);
    }
    else
    {
        // No lights uploaded (GI not active) — fall back to flat albedo.
        reflPayload.colour = diffuse.rgb;
    }

    reflPayload.transmittance = 0.0;
}
