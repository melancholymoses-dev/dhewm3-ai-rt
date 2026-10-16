/*
Shared gbufAlbedo alpha packing (Stage 2, docs/plans/20261013_reflection_roughness.md).

gbufAlbedo's alpha packs a 2-bit surface class (top bits) and 6-bit roughness
(bottom bits) into one UNORM8 channel: alpha = ((class << 6) | rough6) / 255.
Sampled with texelFetch only — a filtered tap blends two bit patterns into a
third, meaningless value.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef GBUF_ALBEDO_CLASS_GLSL
#define GBUF_ALBEDO_CLASS_GLSL

#define GBUF_CLASS_ORDINARY  0
#define GBUF_CLASS_MVWRONG   1
#define GBUF_CLASS_VIEWLOCAL 2
// 3 is never written by the prepass. gbufAlbedo clears to white (vk_backend.cpp) for
// pixels it skips (sky, translucents), and 1.0 decodes to class 3 / roughness 63 under
// this packing — "never touched this frame" stays distinct from a real class 0 for free.

float GBuf_PackAlbedoAlpha(int classId, float roughness)
{
    int rough6 = int(clamp(roughness, 0.0, 1.0) * 63.0 + 0.5);
    int packed = (classId << 6) | rough6;
    return float(packed) / 255.0;
}

int GBuf_UnpackClass(float alpha)
{
    int packed = int(alpha * 255.0 + 0.5);
    return packed >> 6;
}

float GBuf_UnpackRoughness(float alpha)
{
    int packed = int(alpha * 255.0 + 0.5);
    return float(packed & 0x3F) / 63.0;
}

#endif // GBUF_ALBEDO_CLASS_GLSL
