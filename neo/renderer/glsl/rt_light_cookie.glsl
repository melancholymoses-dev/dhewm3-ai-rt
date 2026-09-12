/* rt_light_cookie.glsl — light cookie/gobo sampling (rt_projected_light_cookies.md
  Stage 2/3). Shared by rt_light_eval.glsl's rt_LightContribAt (GI bounce +
  reflections) and vol_march.comp's per-step light loop (volumetrics): samples the
  light material's chosen stage image through the world-space S/T/Q projection
  planes vk_gi.cpp derives per light from R_SetLightProject, with the stage's
  texture matrix already folded into the planes CPU-side (project_light_cookie_stage1
  memory) — sampling here is two dot products and a divide, no matrix multiply.

  Includer contract: a set=1 `sampler2D matTextures[4096]` bindless array and
  GL_EXT_nonuniform_qualifier must already be declared/enabled before this include.
  gi_ray.rchit/reflect_ray.rchit/player_reflect.rchit get this via `#include
  "rt_material.glsl"` (set=1, binding=3); vol_march.comp declares just that one
  binding directly instead — rt_material.glsl's other declarations
  (rt_InterpolateNormal) use gl_WorldToObjectEXT, a ray-tracing-*pipeline*-only
  built-in that doesn't exist in vol_march.comp's compute/ray-query shader stage.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_LIGHT_COOKIE_GLSL
#define RT_LIGHT_COOKIE_GLSL

// Mirrors GILightCookie (vk_gi.cpp) — one entry per RTLight slot, same index,
// meaningful only when that light's flags has GI_LIGHT_FLAG_HAS_COOKIE set.
struct RTLightCookie {
    vec4 planeS;
    vec4 planeT;
    vec4 planeQ;
    uint imageIndex; // bindless slot into matTextures[], vk_material_table.cpp
    uint _pad0; uint _pad1; uint _pad2;
};

// rt_SampleLightCookie — only ever called once the caller has confirmed
// GI_LIGHT_FLAG_HAS_COOKIE, so there is no "no cookie" case to special-case
// here. Zeroclamp semantics match real Doom 3's projected-light interaction
// pass: behind the near plane or outside the [0,1] S/T box is black, not a
// passthrough — a hit point can be within a point light's sphere pre-cull
// radius yet outside its (non-uniform) box extents on one axis, and Doom 3
// does not light that point at all in that case.
vec3 rt_SampleLightCookie(RTLightCookie c, vec3 worldPos)
{
    vec4 p = vec4(worldPos, 1.0);
    float s = dot(p, c.planeS);
    float t = dot(p, c.planeT);
    float q = dot(p, c.planeQ);
    if (q <= 0.0)
        return vec3(0.0);
    vec2 uv = vec2(s, t) / q;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return vec3(0.0);
    return texture(matTextures[nonuniformEXT(c.imageIndex)], uv).rgb;
}

float rt_LightLuminance(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}


// rt_ApplyLightCookie — multiplies a cookie sample into a light's contribution,
// Centralised here so gi_ray/reflect_ray (via rt_light_eval.glsl) and
// vol_march.comp render identical debug output and can't drift apart.
vec3 rt_ApplyLightCookie(vec3 preCookieContrib, vec3 cookie)
    return preCookieContrib * cookie;

#endif // RT_LIGHT_COOKIE_GLSL
