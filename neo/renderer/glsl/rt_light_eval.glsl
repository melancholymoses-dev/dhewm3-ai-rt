/* rt_light_eval.glsl — shared direct-lighting evaluation at RT hit points.

  P5 (docs/plans/rt_optimization_tuning.md): the light loops in gi_ray.rchit and
  reflect_ray.rchit had drifted apart (bounceScale, shadow budget, tMax guard).
  This include owns the single loop both use.
  P3 (Wave 3): rt_EvalDirectLightingStochastic adds importance-sampled light
  selection — numDraws shadow rays per hit instead of one per light. GI bounce
  hits use it (r_rtGIStochasticLights); reflections stay on the deterministic
  loop until their UBO plumbs a toggle (they have no temporal denoiser yet).

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

// Self-contained Wang hash/rand — named to avoid colliding with rt_indirect.glsl's
// wang_hash if a future shader includes both files.
uint rt_LightHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed  = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed  = seed ^ (seed >> 15u);
    return seed;
}

float rt_LightRand(uint seed)
{
    return float(rt_LightHash(seed)) / 4294967296.0;
}

// World-anchored selection seed (mirrors rt_indirect.glsl's rt_WorldAnchoredSeed).
// Anchoring the CDF draw to the hit position keeps light selection stable across
// frames for static geometry, so temporal accumulation converges instead of
// chasing a per-frame reshuffle; frameIndex still decorrelates over time.
uint rt_LightSelectionSeed(vec3 hitPos, uint frameIndex)
{
    ivec3 cell = ivec3(floor(hitPos * 8.0));
    uint  s    = rt_LightHash(uint(cell.x) * 2053u
               ^ rt_LightHash(uint(cell.y) * 4099u
               ^ rt_LightHash(uint(cell.z) * 8191u)));
    return rt_LightHash(s + frameIndex * 7919u);
}

// ---------------------------------------------------------------------------
// rt_LightContribution — unshadowed contribution of light i at (hitPos, hitNorm),
// before any contribScale. Returns vec3(0) for out-of-range/backfacing lights;
// lightDir/dist are only meaningful when the return is non-zero.
// Single source of truth for the falloff model — the deterministic loop, the
// stochastic CDF (P3), and its selection weights all call this.
// ---------------------------------------------------------------------------
vec3 rt_LightContribution(int i, vec3 hitPos, vec3 hitNorm, out vec3 lightDir, out float dist)
{
    vec3  lPos = rtLightBuf.lights[i].posRadius.xyz;
    float lRad = rtLightBuf.lights[i].posRadius.w;

    vec3 toL = lPos - hitPos;
    dist     = length(toL);
    lightDir = vec3(0.0, 0.0, 1.0);
    if (dist >= lRad || dist < 0.01)
        return vec3(0.0);

    lightDir    = toL / dist;
    float NdotL = dot(hitNorm, lightDir);
    if (NdotL <= 0.0)
        return vec3(0.0);

    // Quadratic falloff: zero at the edge of the light radius, hard cutoff beyond.
    // dist < lRad above guarantees t < 1 → atten > 0.
    float t     = dist / max(lRad, 1.0);
    float atten = 1.0 - t * t;

    return rtLightBuf.lights[i].colorIntensity.rgb * (rtLightBuf.lights[i].colorIntensity.a * NdotL * atten);
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
        vec3  lightDir;
        float dist;
        vec3  contrib = rt_LightContribution(i, hitPos, hitNorm, lightDir, dist) * contribScale;
        if (dot(contrib, contrib) <= 0.0)
            continue;

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

// ---------------------------------------------------------------------------
// rt_EvalDirectLightingStochastic — P3: importance-sampled direct lighting.
//
// Instead of a shadow ray per in-range light (up to 128 rays/hit), draw
// numDraws lights with probability proportional to their unshadowed
// contribution luminance and fire shadow rays only for the winners. Each
// winner's contribution is divided by its selection probability (standard
// importance sampling — unbiased); the temporal accumulation + à-trous chain
// downstream is the denoiser for the added variance.
//
// Two passes over the light list (total weight, then a CDF walk per draw)
// recompute the cheap per-light math instead of storing a 128-float array —
// ALU is far cheaper than the scratch/occupancy cost in a hit shader.
//
//   maxLights    — candidate cap (pass RT_LIGHT_MAX_LIGHTS: with rays no longer
//                  proportional to candidates, the CDF can span every uploaded
//                  light — this is what lets L1's far-tier lights actually win
//                  draws at hit points near them)
//   numDraws     — shadow-ray budget per hit (1-2 typical; the 8×-cut math in
//                  the plan assumes 2)
//   seed         — from rt_LightSelectionSeed(hitPos, frameIndex)
// ---------------------------------------------------------------------------
vec3 rt_EvalDirectLightingStochastic(vec3 hitPos, vec3 hitNorm, int maxLights, int numDraws,
                                     float shadowBias, float contribScale, uint seed)
{
    int n = min(rtLightBuf.numLights, min(maxLights, RT_LIGHT_MAX_LIGHTS));

    // Pass 1: total selection weight.
    float totalW = 0.0;
    for (int i = 0; i < n; i++)
    {
        vec3  lightDir;
        float dist;
        totalW += rt_LightLuminance(rt_LightContribution(i, hitPos, hitNorm, lightDir, dist));
    }
    if (totalW <= 1e-6)
        return vec3(0.0);

    vec3 irradiance   = vec3(0.0);
    int  prevSel      = -1;
    bool prevOccluded = false;

    for (int k = 0; k < numDraws; k++)
    {
        float target = rt_LightRand(seed + uint(k) * 9781u) * totalW;

        // Pass 2: walk the CDF to the winner.
        float cum        = 0.0;
        int   sel        = -1;
        float selW       = 0.0;
        vec3  selContrib = vec3(0.0);
        vec3  selDir     = vec3(0.0);
        float selDist    = 0.0;
        for (int i = 0; i < n; i++)
        {
            vec3  lightDir;
            float dist;
            vec3  contrib = rt_LightContribution(i, hitPos, hitNorm, lightDir, dist);
            float w       = rt_LightLuminance(contrib);
            cum += w;
            if (w > 0.0 && cum >= target)
            {
                sel        = i;
                selW       = w;
                selContrib = contrib;
                selDir     = lightDir;
                selDist    = dist;
                break;
            }
        }
        if (sel < 0)
            continue; // fp edge: target landed a hair past the recomputed total

        // Shadow ray for the winner; a repeat winner reuses the previous result.
        bool occluded = false;
        if (sel == prevSel)
        {
            occluded = prevOccluded;
        }
        else
        {
            float shadowTMax = selDist - shadowBias;
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
                    selDir,
                    shadowTMax,
                    1                           // payload location 1
                );
                occluded = rtLightShadow.occluded;
            }
        }
        prevSel      = sel;
        prevOccluded = occluded;

        // contrib / (pdf · numDraws), pdf = selW / totalW.
        if (!occluded)
            irradiance += selContrib * (contribScale * totalW / (selW * float(numDraws)));
    }
    return irradiance;
}

#endif // RT_LIGHT_EVAL_GLSL
