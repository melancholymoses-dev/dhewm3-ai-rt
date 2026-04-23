/*
===========================================================================

dhewm3-rt Vulkan — gi_ray.rchit — closest-hit shader for GI bounce rays.

Phase 6.1 Option B — Single light evaluation at secondary hit:
  At the secondary hit point, evaluates direct lighting from each scene
  light to produce physically motivated bounce radiance:

    gi_colour = albedo * sum_lights(lightColour * NdotL * attenuation * shadow)

  The light list is provided by vk_gi.cpp via a per-frame SSBO (binding 4),
  populated from tr.primaryWorld->lightDefs (all world lights, not just
  frustum-visible ones) within r_rtGIRadius of the camera each frame.

  For each in-range light:
    1. Compute Lambert NdotL using the vertex-interpolated surface normal.
    2. Fire a shadow ray (missIndex=1 → gi_shadow.rmiss) to test visibility.
    3. Accumulate irradiance contribution.

  Fallback (numLights == 0, e.g. r_rtGILightBounce 0):
    Returns raw albedo (Option A behaviour) so the rgen's giStrength scale
    provides a uniform ambient lift.

Shadow ray flags:
  gl_RayFlagsTerminateOnFirstHitEXT — stop on first blocker.
  gl_RayFlagsSkipClosestHitShaderEXT — do not invoke rchit for shadow rays
    (payload default occluded=true remains unless gi_shadow.rmiss fires).

Recursion depth: 2 (primary GI ray in rgen + inline shadow ray here).

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

// ---------------------------------------------------------------------------
// set=0 bindings (per-frame GI resources)
// ---------------------------------------------------------------------------
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 3) uniform GIParams {
    mat4  invViewProj;
    float giRadius;
    int   numSamples;
    uint  frameIndex;
    float giStrength;
    ivec2 screenSize;
    ivec2 scissorOffset;
    ivec2 scissorExtent;
    int   checker;
    int   maxBounceLights; // 0 = Option A fallback; otherwise caps light loop
} params;

struct GILight {
    vec4 posRadius;       // xyz = world pos, w = bounding radius
    vec4 colorIntensity;  // rgb = light colour, a = intensity
};

layout(set = 0, binding = 4, std430) readonly buffer GILightBuffer {
    int     numLights;
    float   bounceScale;   // r_rtGIBounceScale from host
    float   giRadius;      // r_rtGIRadius — max range for light evaluation
    float   emissiveScale; // r_rtGIEmissiveScale from host
    GILight lights[];
} giLightBuf;

// ---------------------------------------------------------------------------
// Material table (set=1) and payload
// ---------------------------------------------------------------------------
#include "rt_material.glsl"
#include "gi_payload.glsl"
#include "gi_shadow_payload.glsl"

layout(location = 0) rayPayloadInEXT GIPayload    giPayload;
layout(location = 1) rayPayloadEXT   GIShadowPayload giShadow;

hitAttributeEXT vec2 baryCoord;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define GI_SHADOW_BIAS  0.5   // world-unit offset to avoid self-shadowing
#define GI_MAX_LIGHTS   128   // must match VK_GI_MAX_LIGHTS in vk_gi.cpp

void main()
{
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);

    if (matIdx >= uint(materials.length()))
    {
        giPayload.colour = vec3(0.0);
        return;
    }

    // Sample diffuse albedo at secondary hit.
    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
    vec3 albedo  = diffuse.rgb;

    // Emissive surfaces contribute their own colour directly as bounce radiance.
    vec3 emissive = rt_SampleEmissive(matIdx, gl_PrimitiveID, baryCoord);
    if (dot(emissive, emissive) > 0.001)
    {
        giPayload.colour = emissive * giLightBuf.emissiveScale;
        return;
    }

    // --- Option B: evaluate each in-range light at the secondary hit ---
    vec3 hitPos  = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
    vec3 hitNorm = rt_InterpolateNormal(matIdx, gl_PrimitiveID, baryCoord);

    // If the ray hit the back face (e.g. two-sided geometry), flip the normal
    // so the hemisphere face toward the incoming ray direction.
    if (dot(hitNorm, -gl_WorldRayDirectionEXT) < 0.0)
        hitNorm = -hitNorm;

    vec3 irradiance = vec3(0.0);
    // Cap at both the hard shader limit and the per-frame bounce budget from the UBO.
    // params.maxBounceLights == 0 signals Option A fallback (bounce disabled).
    int  n          = min(giLightBuf.numLights, min(params.maxBounceLights, GI_MAX_LIGHTS));

    for (int i = 0; i < n; i++)
    {
        vec3  lightPos    = giLightBuf.lights[i].posRadius.xyz;
        float lightRadius = giLightBuf.lights[i].posRadius.w; // Doom3 half-extent
        vec3  lightColor  = giLightBuf.lights[i].colorIntensity.rgb;
        float intensity   = giLightBuf.lights[i].colorIntensity.a;

        vec3  toLight = lightPos - hitPos;
        float dist    = length(toLight);

        // Skip lights beyond the light radius.  
        if (dist >= lightRadius || dist < 0.01)
            continue;

        vec3 lightDir = toLight / dist;
        float NdotL   = dot(hitNorm, lightDir);
        if (NdotL <= 0.0)
            continue;

        // Shadow visibility test.
        // missIndex=1 → gi_shadow.rmiss (clears occluded); default=true.
        giShadow.occluded = true;
        traceRayEXT(
            tlas,
            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
            0xFF,           // cull mask
            0,              // sbt hit group offset (unused — skip closest hit)
            0,              // sbt stride
            1,              // miss index 1 → gi_shadow.rmiss
            hitPos + hitNorm * GI_SHADOW_BIAS,
            0.0,            // tmin
            lightDir,
            dist - GI_SHADOW_BIAS, // tmax: stop just before the light centre
            1               // payload location 1
        );

        if (giShadow.occluded)
            continue;

        // Drop off light to zero at edge.
        float t     = (dist / max(lightRadius, 1.0));
        float atten = t < 1 ? 1.0 - t * t  : 0;

        irradiance += lightColor * intensity * NdotL * atten * giLightBuf.bounceScale;
    }

    // Final bounce colour: albedo × gathered irradiance.
    // giStrength in the rgen provides an additional global scale.
    // Option A fallback: if no light contributed (numLights == 0 or all failed
    // radius/NdotL checks), return raw albedo so the rgen's giStrength still
    // provides a uniform ambient lift rather than returning black.
    if (n == 0 || giLightBuf.bounceScale == 0.0)
        giPayload.colour = albedo;
    else
        giPayload.colour = albedo * irradiance;
}
