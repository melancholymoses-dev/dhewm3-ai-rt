/* rt_light_eval.glsl — shared direct-lighting evaluation at RT hit points.

  P5 (docs/plans/rt_optimization_tuning.md): the light loops in gi_ray.rchit and
  reflect_ray.rchit had drifted apart (bounceScale, shadow budget, tMax guard).
  This include owns the single loop both use.

  P3 (Wave 3) landed the stochastic variant here:
  rt_EvalDirectLightingStochastic() walks the same lights but fires shadow rays
  for only 1-2 importance-sampled winners, dividing by the selection probability
  (resampled importance sampling — unbiased).  GI's worst case drops from
  numSamples x maxBounceLights rays/pixel to numSamples x picks.

  Declares the shared light-list SSBO (set=0, binding=4 — the GI light buffer
  vk_gi.cpp uploads; vk_reflections.cpp binds the same buffer) and the shadow-ray
  payload at location 1.

  Includer contract (before #include of this file):
    - declare:  layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
    - include:  gi_shadow_payload.glsl
    - define:   RT_LIGHT_SHADOW_MISS_INDEX  (index of gi_shadow.rmiss in the
                pipeline's miss-shader region: 1 for the GI pipeline, 2 for the
                reflection pipeline)

  This file deliberately references NO uniform block.  Binding 3 (the params UBO)
  is declared VK_SHADER_STAGE_RAYGEN_BIT_KHR in the reflection pipeline, so a hit
  shader reading it is undefined behaviour; per-pipeline knobs must arrive as
  function arguments from the includer instead.

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
    vec4 posRadius;      // xyz = volume centre (parms.origin), w = falloff/pre-cull radius
    vec4 colorIntensity; // rgb = light colour, a = intensity
    vec4 coneDir;        // projected: xyz=dir, w=cos(halfAngle); zeroed for point
    vec4 boxExtents;     // point: xyz=AABB half-extents, w=0; projected: w=max reach, xyz=0
    uint lightType;      // 0 = point, 1 = projected/spot, 2 = player flashlight
    uint _pad0; uint _pad1; uint _pad2;
    // Emitter: globalLightOrigin = parms.origin + axis * lightCenter. See vk_gi.cpp's
    // GILightEntry::emitPos — attenuation stays measured from the volume centre, while
    // direction/N·L and shadow-ray targeting come from here, matching the GL split.
    vec4 emitPos;        // xyz = globalLightOrigin, w unused
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

// Local Wang hash — rt_indirect.glsl has the same primitive but pulls in
// depthSampler, which hit shaders don't bind.  Prefixed to avoid a clash if a
// future includer ends up with both.
uint rtle_hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed  = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed  = seed ^ (seed >> 15u);
    return seed;
}

float rtle_rand(uint seed)
{
    return float(rtle_hash(seed)) / 4294967296.0;
}

// ---------------------------------------------------------------------------
// rt_LightContribAt — unshadowed contribution of light i at a hit point.
// Single source of the falloff/NdotL math for both the deterministic and the
// stochastic loop below.  Returns false when the light doesn't reach the point.
// ---------------------------------------------------------------------------
bool rt_LightContribAt(int i, vec3 hitPos, vec3 hitNorm, float contribScale,
                       out vec3 contrib, out vec3 lightDir, out float dist)
{
    contrib  = vec3(0.0);
    lightDir = vec3(0.0, 0.0, 1.0);
    dist     = 0.0;

    vec3  lPos   = rtLightBuf.lights[i].posRadius.xyz;   // volume centre
    vec3  lEmit  = rtLightBuf.lights[i].emitPos.xyz;     // emitter (lightCenter applied)
    float lRad   = rtLightBuf.lights[i].posRadius.w;
    vec3  lColor = rtLightBuf.lights[i].colorIntensity.rgb;
    float lInt   = rtLightBuf.lights[i].colorIntensity.a;

    // Range test and falloff are properties of the light's VOLUME, which does not move
    // when a mapper offsets lightCenter — so both measure from the volume centre. This
    // mirrors GL, where R_SetLightProject builds the falloff planes around parms.origin.
    float volDist = length(lPos - hitPos);
    if (volDist >= lRad)
        return false;

    // Direction and shadow-ray distance are properties of where the light actually IS.
    vec3  toL = lEmit - hitPos;
    dist      = length(toL);
    if (dist < 0.01)
        return false;

    lightDir     = toL / dist;
    float NdotL  = dot(hitNorm, lightDir);
    if (NdotL <= 0.0)
        return false;

    // Quadratic falloff: zero at the edge of the light radius, hard cutoff beyond.
    float t     = volDist / max(lRad, 1.0);
    float atten = 1.0 - t * t;   // volDist < lRad above guarantees t < 1 → atten > 0

    contrib = lColor * (lInt * NdotL * atten * contribScale);
    return true;
}

// ---------------------------------------------------------------------------
// rt_TraceLightShadow — true if the light is occluded from the hit point.
// The tMax > 0 guard covers hit points closer to the light centre than the
// bias — those are treated as unoccluded rather than tracing a negative range.
// ---------------------------------------------------------------------------
bool rt_TraceLightShadow(vec3 hitPos, vec3 hitNorm, vec3 lightDir, float dist, float shadowBias)
{
    float shadowTMax = dist - shadowBias;
    if (shadowTMax <= 0.0)
        return false;

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
    return rtLightShadow.occluded;
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
        vec3  contrib, lightDir;
        float dist;
        if (!rt_LightContribAt(i, hitPos, hitNorm, contribScale, contrib, lightDir, dist))
            continue;

        if (shadowsUsed < shadowBudget && rt_LightLuminance(contrib) >= minShadowLum)
        {
            if (dist - shadowBias > 0.0)
            {
                shadowsUsed++;
                if (rt_TraceLightShadow(hitPos, hitNorm, lightDir, dist, shadowBias))
                    continue;
            }
        }

        irradiance += contrib;
    }
    return irradiance;
}

// ---------------------------------------------------------------------------
// rt_EvalDirectLightingStochastic — P3.
//
// Same lights, same math, but only `picks` (1-2) shadow rays are fired.  Each
// pick is drawn with probability proportional to the light's *unshadowed*
// contribution luminance via streaming weighted-reservoir sampling (no CDF
// array, so the shader stays register-resident), then divided by that
// probability.  The estimator is unbiased: E[result] equals the fully-shadowed
// sum, and its luminance is bounded by the total unshadowed luminance, so the
// weighting itself cannot produce fireflies — only the binary shadow term is
// noisy, which is exactly what the temporal + à-trous chain already denoises.
//
// `seed` must vary per pixel, per GI sample, and per frame, or the same light
// wins every frame at a given point and the noise bakes into a permanent blotch.
//
// When n <= picks every light would be sampled anyway, so it defers to the
// deterministic loop (zero variance for the same ray count) — the common case
// in Doom 3 rooms with 1-3 lights in range.
// ---------------------------------------------------------------------------
vec3 rt_EvalDirectLightingStochastic(vec3 hitPos, vec3 hitNorm, int maxLights, int picks,
                                     float shadowBias, float contribScale, uint seed)
{
    int n = min(rtLightBuf.numLights, min(maxLights, RT_LIGHT_MAX_LIGHTS));
    if (n <= 0)
        return vec3(0.0);

    int k = clamp(picks, 1, 2);
    if (n <= k)
        return rt_EvalDirectLighting(hitPos, hitNorm, maxLights, RT_LIGHT_MAX_LIGHTS,
                                     shadowBias, contribScale, 0.0);

    // Two independent single-sample reservoirs, held in scalars/vectors.
    float wSum = 0.0;
    vec3  c0 = vec3(0.0), d0 = vec3(0.0); float t0 = 0.0, w0 = 0.0; int i0 = -1;
    vec3  c1 = vec3(0.0), d1 = vec3(0.0); float t1 = 0.0, w1 = 0.0; int i1 = -1;

    uint streamA = seed;
    uint streamB = seed ^ 0x9e3779b9u;

    for (int i = 0; i < n; i++)
    {
        vec3  contrib, lightDir;
        float dist;
        if (!rt_LightContribAt(i, hitPos, hitNorm, contribScale, contrib, lightDir, dist))
            continue;

        float w = rt_LightLuminance(contrib);
        if (w <= 0.0)
            continue;

        wSum += w;

        // WRS update: accept light i with probability w / wSum.
        if (rtle_rand(streamA + uint(i) * 747796405u) * wSum < w)
        {
            c0 = contrib; d0 = lightDir; t0 = dist; w0 = w; i0 = i;
        }
        if (k > 1 && rtle_rand(streamB + uint(i) * 2891336453u) * wSum < w)
        {
            c1 = contrib; d1 = lightDir; t1 = dist; w1 = w; i1 = i;
        }
    }

    if (wSum <= 0.0 || i0 < 0)
        return vec3(0.0);

    vec3 result   = vec3(0.0);
    bool occluded = rt_TraceLightShadow(hitPos, hitNorm, d0, t0, shadowBias);
    if (!occluded)
        result += c0 * (wSum / w0);

    if (k > 1 && i1 >= 0)
    {
        // Both reservoirs can land on the same light; reuse the trace instead of
        // spending a second identical ray.
        bool occluded1 = (i1 == i0) ? occluded
                                    : rt_TraceLightShadow(hitPos, hitNorm, d1, t1, shadowBias);
        if (!occluded1)
            result += c1 * (wSum / w1);
        result *= 0.5;
    }

    return result;
}

#endif // RT_LIGHT_EVAL_GLSL
