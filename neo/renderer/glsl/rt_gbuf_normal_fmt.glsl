/* rt_gbuf_normal_fmt.glsl — the gbufNormal (RGBA8) pixel format, and the
   octahedral unit-vector codec it is built on.

  Stage 2a (docs/plans/20261017_reflection_pipeline_refactor.md): the normal moved
  from rgb (3 channels for 2 degrees of freedom) to an octahedral pair in RG, which
  frees B for a coarse F0.  Worst-case angular error is ~1deg, comparable to the
  RGB8 encoding it replaces.

  | Channel | Contents                                  |
  |---------|-------------------------------------------|
  | RG      | octahedral normal, [-1,1] mapped to [0,1] |
  | B       | coarse F0 (Stage 2b); always <= 254/255   |
  | A       | per-pixel F0                              |

  B == 1.0 is the "prepass never wrote this pixel" sentinel (sky, translucent,
  null fallback image).  It has to live outside RG because every RG pair is a
  valid octahedral code — there is no spare normal value to reserve.

  No includer contract: this file is pure math, safe to include anywhere.
  gi_probe_common.glsl's gip_Oct* probe-atlas helpers also come from here.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_GBUF_NORMAL_FMT_GLSL
#define RT_GBUF_NORMAL_FMT_GLSL

// ---------------------------------------------------------------------------
// Octahedral mapping (Cigolle et al. / DDGI)
// ---------------------------------------------------------------------------

vec2 rt_OctSignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Unit vector -> [-1,1]^2
vec2 rt_OctEncode(vec3 n)
{
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    n /= max(l1, 1e-8);
    return (n.z < 0.0) ? ((1.0 - abs(n.yx)) * rt_OctSignNotZero(n.xy)) : n.xy;
}

// [-1,1]^2 -> unit vector
vec3 rt_OctDecode(vec2 e)
{
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * rt_OctSignNotZero(v.xy);
    return normalize(v);
}

// ---------------------------------------------------------------------------
// gbufNormal RG <-> world normal
// ---------------------------------------------------------------------------

// Highest B a writer may emit; 1.0 is reserved for the sentinel.
const float GBUF_NORMAL_B_MAX = 254.0 / 255.0;

vec2 GBuf_EncodeNormal(vec3 n)
{
    return rt_OctEncode(normalize(n)) * 0.5 + 0.5;
}

vec3 GBuf_DecodeNormal(vec2 rg)
{
    return rt_OctDecode(rg * 2.0 - 1.0);
}

// False when the prepass never wrote this pixel.  Half an 8-bit step of slack so
// a writer that clamps to 254/255 is never mistaken for the sentinel.
bool GBuf_NormalValid(vec4 gbuf)
{
    return gbuf.b < (1.0 - 0.5 / 255.0);
}

// Coarse (fixed-mip) F0, the reflection trace mask (Stage 2b).  0 at an unwritten
// pixel — B there is the 1.0 sentinel, which would otherwise read as a mirror.
float GBuf_CoarseF0(vec4 gbuf)
{
    return GBuf_NormalValid(gbuf) ? gbuf.b : 0.0;
}

#endif // RT_GBUF_NORMAL_FMT_GLSL
