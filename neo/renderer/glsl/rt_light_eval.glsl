/* rt_light_eval.glsl — shared direct-lighting evaluation at RT hit points.

  P5 (docs/plans/rt_optimization_tuning.md): the light loops in gi_ray.rchit and
  reflect_ray.rchit had drifted apart (bounceScale, shadow budget, tMax guard).
  This include owns the single loop both use, and is where P3's stochastic light
  selection will land once, later.

  Declares the shared light-list SSBO (set=0, binding=4 — the GI light buffer
  vk_gi.cpp uploads; vk_reflections.cpp binds the same buffer) and the shadow-ray
  payload at location 1.

  Includer contract (before #include of this file):
    - declare:  layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
    - include:  gi_shadow_payload.glsl
    - define:   RT_LIGHT_SHADOW_MISS_INDEX  (index of gi_shadow.rmiss in the
                pipeline's miss-shader region: 1 for the GI pipeline, 2 for the
                reflection pipeline)

  P4: rt_EvalDirectLighting takes minShadowLum — lights whose *unshadowed*
  contribution at the hit point is below it get that contribution added without
  spending a shadow ray, so the per-hit shadow budget flows to lights that
  actually matter there (the light list is sorted for the camera, not the hit).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_LIGHT_EVAL_GLSL
#define RT_LIGHT_EVAL_GLSL

#ifndef RT_LIGHT_SHADOW_MISS_INDEX
#error "define RT_LIGHT_SHADOW_MISS_INDEX before including rt_light_eval.glsl"
#endif

#define RT_LIGHT_MAX_LIGHTS 128  // must match VK_GI_MAX_LIGHTS in vk_gi.cpp

struct RTLight {
    vec4 posRadius;      // xyz = world pos, w = sphere pre-cull radius
    vec4 colorIntensity; // rgb = light colour, a = intensity
    vec4 coneDir;        // projected: xyz=dir, w=cos(halfAngle); zeroed for point
    vec4 boxExtents;     // point: xyz=AABB half-extents, w=0; projected: w=max reach, xyz=0
    uint lightType;      // 0 = point, 1 = projected/spot, 2 = player flashlight
    uint _pad0; uint _pad1; uint _pad2;
};

layout(set = 0, binding = 4, std430) readonly buffer RTLightBuf {
    int     numLights;
    float   bounceScale;   // r_rtGIBounceScale (GI bounce hits only; reflections pass 1.0)
    float   giRadius;      // r_rtGIRadius — max range for light evaluation
    float   emissiveScale; // r_rtGIEmissiveScale — emissive surface multiplier
    RTLight lights[];
} rtLightBuf;

layout(location = 1) rayPayloadEXT GIShadowPayload rtLightShadow;

float rt_LightLuminance(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// ---------------------------------------------------------------------------
// rt_EvalDirectLighting — accumulate direct irradiance at a hit point from the
// shared light list.  Starts at vec3(0); callers add their own ambient floor.
//
//   maxLights    — cap on lights walked (≤ RT_LIGHT_MAX_LIGHTS)
//   shadowBudget — max shadow rays fired at this hit; lights past the budget
//                  contribute unshadowed (graceful degradation, not black)
//   shadowBias   — world-unit normal offset for shadow-ray origin/tMax
//   contribScale — per-light multiplier (GI: bounceScale; reflections: 1.0)
//   minShadowLum — P4: skip the shadow ray (but keep the contribution) when the
//                  unshadowed contribution luminance is below this
// ---------------------------------------------------------------------------
vec3 rt_EvalDirectLighting(vec3 hitPos, vec3 hitNorm, int maxLights, int shadowBudget,
                           float shadowBias, float contribScale, float minShadowLum)
{
    vec3 irradiance  = vec3(0.0);
    int  n           = min(rtLightBuf.numLights, min(maxLights, RT_LIGHT_MAX_LIGHTS));
    int  shadowsUsed = 0;

    for (int i = 0; i < n; i++)
    {
        vec3  lPos   = rtLightBuf.lights[i].posRadius.xyz;
        float lRad   = rtLightBuf.lights[i].posRadius.w;
        vec3  lColor = rtLightBuf.lights[i].colorIntensity.rgb;
        float lInt   = rtLightBuf.lights[i].colorIntensity.a;

        vec3  toL  = lPos - hitPos;
        float dist = length(toL);
        if (dist >= lRad || dist < 0.01)
            continue;

        vec3  lightDir = toL / dist;
        float NdotL    = dot(hitNorm, lightDir);
        if (NdotL <= 0.0)
            continue;

        // Quadratic falloff: zero at the edge of the light radius, hard cutoff beyond.
        float t     = dist / max(lRad, 1.0);
        float atten = 1.0 - t * t;   // dist < lRad above guarantees t < 1 → atten > 0

        vec3 contrib = lColor * (lInt * NdotL * atten * contribScale);

        // Shadow ray. The tMax > 0 guard also covers hit points closer to the
        // light centre than the bias — treated as unoccluded.
        if (shadowsUsed < shadowBudget && rt_LightLuminance(contrib) >= minShadowLum)
        {
            float shadowTMax = dist - shadowBias;
            if (shadowTMax > 0.0)
            {
                rtLightShadow.occluded = true;
                traceRayEXT(
                    tlas,
                    gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                    0xFF,
                    0,                          // sbt hit offset (unused — skip closest hit)
                    0,                          // sbt stride
                    RT_LIGHT_SHADOW_MISS_INDEX, // gi_shadow.rmiss in this pipeline
                    hitPos + hitNorm * shadowBias,
                    0.0,
                    lightDir,
                    shadowTMax,
                    1                           // payload location 1
                );
                shadowsUsed++;
                if (rtLightShadow.occluded)
                    continue;
            }
        }

        irradiance += contrib;
    }
    return irradiance;
}

#endif // RT_LIGHT_EVAL_GLSL
