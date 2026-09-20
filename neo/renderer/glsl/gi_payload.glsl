/* gi_payload.glsl — shared ray payload for the GI ray pipeline.

  Included by gi_ray.rgen, gi_ray.rchit, gi_ray.rahit, gi_ray.rmiss and
  gi_probe_trace.rgen before the rayPayloadEXT/rayPayloadInEXT declaration.

  Layout (must match across all GI shader stages):

    colour   — RGB bounce radiance at the secondary hit, or ambient sky colour
               on miss.  In probe mode this carries the STABLE bucket only.
    hitDist  — gl_HitTEXT at a hit, gl_RayTmaxEXT on a miss, NEGATED for a
               back-face hit.  Probe GI needs the magnitude for the visibility
               (Chebyshev) moments and the sign for probe classification (G4);
               the per-pixel rgen ignores both.  The sign carries the back-face
               flag because gi_probe_trace.rgen's scratch image already encodes
               it that way, so a separate float was storing it twice.
    fast     — G5b, and the field is read on the way IN and written on the way
               OUT:
                 IN   bit 0 = GI_PAYLOAD_PROBE_MODE.  Only gi_probe_trace.rgen
                      sets it.  gi_ray.rchit needs to know which caller it is
                      serving because the two want different things from a
                      flickering light: the probe wants its transport cached at
                      s = 1 in a separate bucket, the per-pixel path wants the
                      light exactly as it looks this frame, summed as before.
                 OUT  the fast bucket's radiance, packed.  Zero in per-pixel
                      mode, which is what keeps r_rtGIProbes 0 unchanged.

  20 bytes.  20260906_froxel_probe_gi.md B.2 and G5b.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef GI_PAYLOAD_GLSL
#define GI_PAYLOAD_GLSL

struct GIPayload {
    vec3  colour;   // stable-bucket bounce radiance (or ambient on miss)
    float hitDist;  // |t| at hit / tMax on miss; NEGATIVE means a back face
    uint  fast;     // in: mode bits.  out: packed fast-bucket radiance
};

// Set by gi_probe_trace.rgen only.  gi_ray.rgen leaves `fast` at 0, so the
// per-pixel path takes the "sum both buckets into colour" branch by default —
// the safe direction if a future rgen forgets to initialise the field.
#define GI_PAYLOAD_PROBE_MODE 0x1u

// Fast-bucket radiance packing.  Hand-rolled rather than packF2x11_1x10, which
// is unsigned-normalised-in-[0,1] and would clip HDR bounce radiance; this is a
// shared exponent over the three channels, i.e. RGB9E5 by hand.  Range and
// precision comfortably exceed what the r11f_g11f_b10f scratch image that
// receives it can hold, so the pack is never the limiting step.
uint gip_PackRadiance(vec3 c)
{
    c = max(c, vec3(0.0));
    float m = max(max(c.r, c.g), c.b);
    if (m <= 1e-9)
        return 0u;

    // Exponent biased by 16, clamped to 5 bits: covers 2^-16 .. 2^15, far wider
    // than any radiance that survives the tonemap.
    int e = clamp(int(ceil(log2(m))) + 16, 0, 31);
    float scale = exp2(float(e - 16 - 9)); // 9 mantissa bits per channel
    uvec3 q = uvec3(clamp(c / scale, vec3(0.0), vec3(511.0)));
    return q.r | (q.g << 9) | (q.b << 18) | (uint(e) << 27);
}

vec3 gip_UnpackRadiance(uint p)
{
    if (p == 0u)
        return vec3(0.0);
    float scale = exp2(float(int(p >> 27) - 16 - 9));
    return vec3(float(p & 511u), float((p >> 9) & 511u), float((p >> 18) & 511u)) * scale;
}

#endif // GI_PAYLOAD_GLSL
