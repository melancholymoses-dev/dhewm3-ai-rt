/* rt_light_struct.glsl — the GILightEntry transcription, shared by every consumer
  of the RT light SSBO.

  This file is deliberately pipeline-agnostic: no buffer block, no TLAS, no ray
  payload, no RT built-ins.  Each consumer declares its own `layout(...) buffer`
  at its own set/binding and only borrows the struct and the flag bits from here.
  rt_light_eval.glsl is NOT that shared home — it declares a rayPayloadEXT and
  calls traceRayEXT, so a compute shader cannot include it.

  Before this existed the struct was transcribed four times (rt_light_eval.glsl,
  vol_march.comp, vol_froxel_fill.comp, gi_probe_resolve.comp) and every field
  change had to land in all of them, with no compile error if one was missed.

  This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
  and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
  Code.

  It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
  Code release.
*/

#ifndef RT_LIGHT_STRUCT_GLSL
#define RT_LIGHT_STRUCT_GLSL

#define RT_LIGHT_MAX_LIGHTS 128 // must match VK_GI_MAX_LIGHTS in vk_gi.cpp

// rt_projected_light_cookies.md — this light has a cookie entry at the same index
// in the consumer's cookies[] array.
#define GI_LIGHT_FLAG_HAS_COOKIE 0x1u

// G5b (completed/20260906_froxel_probe_gi.md) — this light is classified as
// flickering: colorIntensity.rgb is its PEAK colour L̂ and fastScale is
// s = current/peak.  Every consumer must multiply the two to get the light as it
// looks right now.
#define GI_LIGHT_FLAG_FAST 0x2u

// ...and this is the one light whose probe transport is cached normalised, in
// fast bucket 0.  At most one light carries it, because one gain is shared by the
// whole bucket — see vk_gi.cpp.  Only the probe path reads this bit; every other
// consumer cares about FAST alone.
#define GI_LIGHT_FLAG_FAST_BUCKET 0x4u

struct RTLight {
    vec4 posRadius;      // xyz = volume centre (parms.origin), w = falloff/pre-cull radius
    vec4 colorIntensity; // rgb = light colour — L̂ when FAST — a = intensity
    vec4 coneDir;        // projected: xyz=dir, w=cos(halfAngle); zeroed for point
    vec4 boxExtents;     // point: xyz=AABB half-extents, w=0; projected: w=max reach, xyz=0
    uint lightType;      // 0 = point, 1 = projected/spot, 2 = player flashlight
    uint flags;          // GI_LIGHT_FLAG_* bitmask
    float fastScale;     // G5b: s = current/peak luminance; 1.0 for a steady light
    uint  fastBucket;    // G5b: which fast bucket caches this light's transport
    // Emitter: globalLightOrigin = parms.origin + axis * lightCenter. See vk_gi.cpp's
    // GILightEntry::emitPos — attenuation stays measured from the volume centre, while
    // direction/N·L and shadow-ray targeting come from here, matching the GL split.
    vec4 emitPos;        // xyz = globalLightOrigin, w unused
};

// G5b — the flicker gain of a light, or 1.0 for a steady one.  By value so it
// works against any consumer's buffer name; rt_light_eval.glsl wraps it by index.
float rt_LightFastScale(RTLight l)
{
    if ((l.flags & GI_LIGHT_FLAG_FAST) == 0u)
        return 1.0;
    return clamp(l.fastScale, 0.0, 1.0);
}

// r_rtGIFalloffMode — point-light attenuation vs the box-normalised L-inf distance
// t (0 = centre, 1 = box face).  Projected lights use a cone ramp and never come
// here.  Scalar in, scalar out: no buffer access, so every consumer can share it
// whatever its block is called.  T4b, rt_optimization_tuning.md.
//
// `reach` is where the light dies, in units of t — a dhewm3-rt extension past Doom's
// own box (t=1) to give lights more throw.  Shape and reach are separate knobs on
// purpose: changing both at once makes an A/B unattributable.
#define RT_FALLOFF_LEGACY 0 // flat to t=0.8, then knee to 0.1, then a linear tail
#define RT_FALLOFF_RASTER 1 // (1-t)^2 — the shape Doom 3's own falloff images give
#define RT_FALLOFF_SMOOTH 2 // (1-t^2)^2 — zero slope at both ends, no centre hotspot

float rt_LightBoxAtten(float t, int mode, float reach)
{
    if (mode == RT_FALLOFF_LEGACY) {
        // Knee is anchored to the box face, the tail to `reach`, so widening the
        // reach stretches only the dim part — the original 1.5 behaviour at default.
        if (t <= 1.0)
            return mix(0.1, 1.0, clamp((1.0 - t) / 0.2, 0.0, 1.0));
        return 0.1 * clamp((reach - t) / max(reach - 1.0, 0.001), 0.0, 1.0);
    }

    // Corrected curves span the whole reach, so a light keeps its throw and trades
    // the flat core for a real gradient.  At the default reach 1.5, mode 1 lands on
    // 0.11 at the box face — near enough the legacy knee's 0.1 that the face value
    // is not what changes between them.
    float u = t / max(reach, 0.001);
    float f = (mode == RT_FALLOFF_SMOOTH) ? max(1.0 - u * u, 0.0) : max(1.0 - u, 0.0);
    return f * f;
}

#endif // RT_LIGHT_STRUCT_GLSL
