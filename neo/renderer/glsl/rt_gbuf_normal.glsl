/* rt_gbuf_normal.glsl — decode the depth-prepass G-buffer shading normal.

  P9 (docs/plans/rt_optimization_tuning.md): AO and GI used to rebuild a normal
  with rt_ReconstructNormal — 5 depth fetches + 4 invViewProj multiplies per
  pixel, and only ever a *geometric* normal from depth gradients. The depth
  prepass already writes the bump-mapped shading normal (Stage 3.5), so both
  passes read it instead: fewer fetches, and AO/GI now follow bump detail.

  Includer contract (before #include of this file):
    - declare: layout(set = 0, binding = N) uniform sampler2D gbufNormalSampler;
               (AO binding 4, GI binding 5 — reflections read theirs inline)

  IMPORTANT — the "no data" test is on RGB, not alpha.  reflect_ray.rgen tests
  `gbuf.a > 0.0` because a pixel with F0 == 0 is uninteresting to *reflections*
  either way. AO/GI are the opposite case: most of a Doom 3 scene is matte with a
  black or absent specular map, so F0 == 0 is the norm, and testing alpha here
  would fall back to reconstruction almost everywhere and make P9 a no-op. The
  honest test for "the prepass never wrote this pixel" is the rgb clear sentinel
  (0.5, 0.5, 0.5) — the encoding of a zero vector, which no unit normal produces.
  This is the same test reflect_ray.rgen's debug mode 4 uses.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_GBUF_NORMAL_GLSL
#define RT_GBUF_NORMAL_GLSL

// ---------------------------------------------------------------------------
// rt_GbufNormalAt — decode the shading normal at a pixel.
//
//   enabled — params.useGbufNormal (r_rtGbufNormals, forced 0 when the engine
//             has no G-buffer image; the binding is then a 1x1 null image)
//   orient  — direction the normal must face (toward the camera for AO/GI).
//             A back-facing result is rejected rather than flipped: flipping a
//             bump-mapped normal points it into the surface, whereas
//             rt_ReconstructNormal guaranteed a camera-facing normal and the
//             hemisphere sampling downstream depends on that invariant.
//
// Returns false when the caller should fall back to rt_ReconstructNormal.
// ---------------------------------------------------------------------------
bool rt_GbufNormalAt(ivec2 coord, int enabled, vec3 orient, out vec3 normal)
{
    normal = orient;

    if (enabled == 0)
        return false;

    // Clamped fetch: the bound image is 1x1 when the real G-buffer doesn't exist,
    // and texelFetch out of bounds is undefined without robustImageAccess.
    ivec2 gbufSize = textureSize(gbufNormalSampler, 0);
    vec4  gbuf     = texelFetch(gbufNormalSampler, clamp(coord, ivec2(0), gbufSize - 1), 0);

    // Clear sentinel → prepass never wrote here (sky, translucent, null image).
    if (all(lessThanEqual(abs(gbuf.rgb - vec3(0.5)), vec3(1.0 / 255.0))))
        return false;

    vec3  n   = gbuf.rgb * 2.0 - 1.0;
    float len = length(n);
    if (len < 0.5)   // quantization noise, not a unit normal
        return false;

    n = n / len;
    if (dot(n, orient) < 0.01)
        return false;

    normal = n;
    return true;
}

#endif // RT_GBUF_NORMAL_GLSL
