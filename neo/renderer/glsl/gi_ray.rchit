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

Wave 3 / P3 — stochastic light selection:
  With r_rtGIStochasticLights > 0 the light loop instead importance-samples
  1-2 lights per hit and fires shadow rays only for those, dividing by the
  selection probability (rt_EvalDirectLightingStochastic in rt_light_eval.glsl).
  Worst case falls from numSamples x maxBounceLights rays/pixel (4 x 16 = 64) to
  numSamples x picks (4 x 2 = 8), which is what buys the headroom to raise
  r_rtGISamples.  Set r_rtGIStochasticLights 0 for the legacy all-lights path.

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
    int   maxBounceLights;   // 0 = Option A fallback; otherwise caps light loop
    float giContrast;        // rgen only — present so the block matches GIParamsUBO
    int   stochasticLights;  // P3: shadow rays per bounce hit (0 = every light)
} params;

// ---------------------------------------------------------------------------
// Material table (set=1), payloads, and the shared light loop (P5) — declares
// the light SSBO (set=0, binding=4) and the location-1 shadow payload.
// Miss index 1 = gi_shadow.rmiss's slot in the GI pipeline's miss region.
// ---------------------------------------------------------------------------
#include "rt_material.glsl"
#include "gi_payload.glsl"
#include "gi_shadow_payload.glsl"

layout(location = 0) rayPayloadInEXT GIPayload giPayload;

#define RT_LIGHT_SHADOW_MISS_INDEX 1
#include "rt_light_eval.glsl"

hitAttributeEXT vec2 baryCoord;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define GI_SHADOW_BIAS  0.5   // world-unit offset to avoid self-shadowing

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
    vec3 emissive = rt_EvalEmissiveRadiance(matIdx, gl_PrimitiveID, baryCoord);
    if (dot(emissive, emissive) > 0.001)
    {
        giPayload.colour = emissive * rtLightBuf.emissiveScale;
        return;
    }

    // --- Option B: evaluate each in-range light at the secondary hit ---
    vec3 hitPos  = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
    vec3 hitNorm = rt_InterpolateNormal(matIdx, gl_PrimitiveID, baryCoord);

    // If the ray hit the back face (e.g. two-sided geometry), flip the normal
    // so the hemisphere face toward the incoming ray direction.
    if (dot(hitNorm, -gl_WorldRayDirectionEXT) < 0.0)
        hitNorm = -hitNorm;

    // Shared light loop (rt_light_eval.glsl). bounceScale rides in as contribScale.
    // params.maxBounceLights == 0 signals Option A fallback (bounce disabled).
    int n = min(rtLightBuf.numLights, min(params.maxBounceLights, RT_LIGHT_MAX_LIGHTS));

    vec3 irradiance;
    if (params.stochasticLights > 0)
    {
        // P3: importance-sample 1-2 lights and fire only those shadow rays.
        // The seed must differ per pixel, per GI sample and per frame — gl_HitTEXT
        // separates the rgen's numSamples hemisphere rays (same launch ID, different
        // hit distance), frameIndex lets temporal accumulation average the picks.
        uint seed = rtle_hash(uint(gl_LaunchIDEXT.x) * 73856093u
                            ^ uint(gl_LaunchIDEXT.y) * 19349663u
                            ^ params.frameIndex     * 83492791u
                            ^ floatBitsToUint(gl_HitTEXT));

        irradiance = rt_EvalDirectLightingStochastic(hitPos, hitNorm, params.maxBounceLights,
                                                     params.stochasticLights, GI_SHADOW_BIAS,
                                                     rtLightBuf.bounceScale, seed);
    }
    else
    {
        // Legacy path: every in-range light gets a shadow ray (budget = max,
        // threshold 0). Kept for A/B against the stochastic estimator.
        irradiance = rt_EvalDirectLighting(hitPos, hitNorm, params.maxBounceLights,
                                           RT_LIGHT_MAX_LIGHTS, GI_SHADOW_BIAS,
                                           rtLightBuf.bounceScale, 0.0);
    }

    // Final bounce colour: albedo × gathered irradiance.
    // giStrength in the rgen provides an additional global scale.
    // Option A fallback: if no light contributed (numLights == 0 or all failed
    // radius/NdotL checks), return raw albedo so the rgen's giStrength still
    // provides a uniform ambient lift rather than returning black.
    if (n == 0 || rtLightBuf.bounceScale == 0.0)
        giPayload.colour = albedo;
    else
        giPayload.colour = albedo * irradiance;
}
