/* rt_emitter.glsl — projectile glow emitters for reflection rays.

  Mirrors vkRTEmitter_t (vk_rt_emitters.h). Each emitter is a disc that always faces
  the ray, so a billboard seen in a reflection is oriented for the reflected view.
  Pipeline-agnostic: no payload, TLAS or hit built-ins. The includer declares the
  SSBO and the matTextures array.
  See docs/plans/20261002_reflection_emitters.md.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#define RT_MAX_EMITTERS  64   // must match VK_RT_MAX_EMITTERS
#define RT_MAX_GLOW_TRIS 128  // must match VK_RT_MAX_GLOW_TRIS

struct RtEmitter {
    vec3  pos;
    float radius;
    vec3  rgb;
    uint  texIndex;   // matTextures slot; 0 = procedural falloff
};

// World-space triangle of an additive translucent surface the BLAS excludes
// (noshadows, e.g. the Mars City bio-scanner beam). Mirrors vkRTGlowTri_t, 80 bytes.
struct RtGlowTri {
    vec4  v0;         // xyz, w = s
    vec4  v1;
    vec4  v2;
    vec4  t;          // xyz = t per vertex
    vec3  rgb;
    uint  texIndex;
};

// Two-sided Moller-Trumbore. On a hit, t is the ray distance and uv the interpolated texcoord.
bool rt_GlowTriHit(RtGlowTri g, vec3 o, vec3 d, out float t, out vec2 uv)
{
    t  = 0.0;
    uv = vec2(0.0);
    vec3  e1  = g.v1.xyz - g.v0.xyz;
    vec3  e2  = g.v2.xyz - g.v0.xyz;
    vec3  p   = cross(d, e2);
    float det = dot(e1, p);
    if (abs(det) < 1e-8)
        return false;
    float inv = 1.0 / det;
    vec3  s   = o - g.v0.xyz;
    float b1  = dot(s, p) * inv;
    if (b1 < 0.0 || b1 > 1.0)
        return false;
    vec3  q   = cross(s, e1);
    float b2  = dot(d, q) * inv;
    if (b2 < 0.0 || b1 + b2 > 1.0)
        return false;
    t = dot(e2, q) * inv;
    if (t <= 0.0)
        return false;
    float b0 = 1.0 - b1 - b2;
    uv = b0 * vec2(g.v0.w, g.t.x) + b1 * vec2(g.v1.w, g.t.y) + b2 * vec2(g.v2.w, g.t.z);
    return true;
}

// Ray (o, unit d) against a ray-facing disc. On a hit, t is the distance along the
// ray to the disc centre's plane, uv is in [0,1]² over the disc's bounding square
// (Doom 3 Z-up keeps the texture upright), rho2 is squared radial distance in [0,1).
bool rt_EmitterDisc(RtEmitter e, vec3 o, vec3 d, out float t, out vec2 uv, out float rho2)
{
    t    = dot(e.pos - o, d);
    uv   = vec2(0.5);
    rho2 = 1.0;
    if (t <= 0.0 || e.radius <= 0.0)
        return false;

    vec3 offs = o + t * d - e.pos;   // lies in the plane perpendicular to d
    rho2 = dot(offs, offs) / (e.radius * e.radius);
    if (rho2 >= 1.0)
        return false;

    vec3 up = abs(d.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 u  = normalize(cross(up, d));
    vec3 v  = cross(d, u);
    uv = vec2(dot(offs, u), dot(offs, v)) / e.radius * 0.5 + 0.5;
    return true;
}
