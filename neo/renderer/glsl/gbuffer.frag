/*
===========================================================================

dhewm3-rt - G-buffer normal/F0 prepass fragment shader (Stage 3.5), opaque variant.

Writes the world-space bump-mapped shading normal and a normalized F0 to the
gbufNormal attachment so the RT reflection pass can use the correct normal
(rather than the flat geometric normal from rt_ReconstructNormal) and early-out
on non-reflective pixels. See docs/plans/gbuffer_normal_pass.md.

Also writes the material's diffuse/albedo sample to gbufAlbedo (attachment 2,
docs/plans/gi_albedo_target.md) so the GI albedo-modulate compute pass can turn
raw bounced radiance into a receiver-correct contribution — without this, GI
paints incoming light directly onto every surface regardless of how much of it
that surface would actually reflect.

gbufAlbedo's alpha also packs a per-pixel roughness (Stage 2,
docs/plans/20261013_reflection_roughness.md), estimated Toksvig-style from how
much the bump map's normal has shortened — both at this fragment's screen-footprint
mip level and at a second, fixed distance-independent mip, so inherent texture
detail (grates, rivets, rust) still raises roughness up close, not only once
minification starts blending it away. Floored at r_rtReflRoughnessFloor so a
clean/flat bump map doesn't read as a perfect mirror. See gbuf_albedo_class.glsl
for the packing.

Used for opaque (non-alpha-tested) surfaces; see gbuffer_clip.frag for the
MC_PERFORATED variant that additionally alpha-tests the diffuse stage.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 450

layout(location = 0) in vec2 vary_TexCoord_Bump;
layout(location = 1) in vec2 vary_TexCoord_Diffuse;  // unused (no alpha test in the opaque variant)
layout(location = 2) in vec2 vary_TexCoord_Specular;
layout(location = 3) in vec3 vary_TangentWS;
layout(location = 4) in vec3 vary_BiTangentWS;
layout(location = 5) in vec3 vary_NormalWS;
layout(location = 6) in vec4 vary_CurClip;
layout(location = 7) in vec4 vary_PrevClip;

// Shared UBO with gbuffer.vert — binding 0, both stages.
layout(set=0, binding=0) uniform GBufferParams {
    mat4  u_ModelViewProjection;
    mat4  u_ModelMatrix;
    mat4  u_MVPNoJitter;
    mat4  u_PrevMVPNoJitter;
    vec4  u_BumpMatrixS;
    vec4  u_BumpMatrixT;
    vec4  u_DiffuseMatrixS;
    vec4  u_DiffuseMatrixT;
    vec4  u_SpecularMatrixS;
    vec4  u_SpecularMatrixT;
    float u_AlphaTestThreshold;
    float u_SpecF0Scale;
    float u_SpecF0Gamma;
    // U4 transparency-and-composition class (docs/plans/completed/20260918_fsr_upscaling.md §9):
    // 1 = this surface's motion vectors are known wrong (skinned mesh, viewmodel), so FSR 2
    // should lean less on its temporal history here.  Rides gbufAlbedo's alpha alongside the
    // Stage 2 roughness bits (gbuf_albedo_class.glsl) — gi_albedo_mod.comp only reads .rgb,
    // so alpha was free; fsr_tc_mask.comp extracts this class into the R8 mask FSR 2 wants.
    float u_TcClass;
    // 1 = view-local surface (first-person body, view weapon).  Shares the same alpha
    // channel as a third level, read by the RT shadow/GI rgens.
    float u_ViewLocal;
    // r_rtReflRoughnessFloor (Stage 2) — see the Toksvig comment below.
    float u_RoughnessFloor;
};

#include "gbuf_albedo_class.glsl"

// Surface class packed into gbufAlbedo's alpha (see gbuf_albedo_class.glsl).
// View-local wins over the T&C bit because anything non-ordinary is already fully
// reactive to FSR 2, so the flag costs nothing there.
int GBuf_SurfaceClass() {
    if ( u_ViewLocal > 0.5 ) return GBUF_CLASS_VIEWLOCAL;
    return ( u_TcClass > 0.5 ) ? GBUF_CLASS_MVWRONG : GBUF_CLASS_ORDINARY;
}

layout(set=0, binding=1) uniform sampler2D u_BumpMap;
// gi_albedo_target.md: the opaque pipeline now samples binding 2 too, for the
// receiver-albedo target — u_DiffuseMatrixS/T (backend-resolved SL_DIFFUSE stage,
// or identity + whiteImage fallback) drives vary_TexCoord_Diffuse's UV here.
layout(set=0, binding=2) uniform sampler2D u_DiffuseMap;
layout(set=0, binding=3) uniform sampler2D u_SpecularMap;

layout(location = 0) out vec4 fragColor;  // attachment 0 (hdrScene) — write-masked off in the pipeline
layout(location = 1) out vec4 outGbuf;    // attachment 1 (gbufNormal) — rgb = world normal, a = F0
layout(location = 2) out vec4 outAlbedo;  // attachment 2 (gbufAlbedo) — rgb = diffuse sample
layout(location = 3) out vec2 outMotion;  // attachment 3 (motionVectors) — GL NDC, Y up

// Screen-space offset from this frame's position to last frame's, in GL NDC units.
// A non-positive w means the vertex is behind the eye in one of the two frames, where
// the perspective divide is meaningless — report no motion instead of a huge one.
vec2 GBuf_MotionVector(vec4 cur, vec4 prev) {
    if (cur.w <= 0.0 || prev.w <= 0.0) {
        return vec2(0.0);
    }
    return (prev.xy / prev.w) - (cur.xy / cur.w);
}

void main() {
    vec3 T = normalize(vary_TangentWS);
    vec3 B = normalize(vary_BiTangentWS);
    vec3 N = normalize(vary_NormalWS);

    // Raw (un-renormalized) sample, kept around for the Toksvig roughness estimate below.
    vec3  nTSRaw = texture(u_BumpMap, vary_TexCoord_Bump).rgb * 2.0 - 1.0;
    float nTSLen = length(nTSRaw);
    vec3  nTS    = nTSRaw / max(nTSLen, 1e-5);
    vec3  nWS    = normalize(mat3(T, B, N) * nTS);

    // Second Toksvig sample at a fixed, distance-independent mip (Stage 2 refinement):
    // nTSLen above uses the screen-footprint LOD, so a bump map's inherent detail only
    // shows up once minification blends texels together — viewed up close, even a
    // heavily-detailed map still measures as sharp. A fixed textureLod instead measures
    // the TEXTURE's own high-frequency content (grates, rivets, rust) independent of
    // camera distance. A flat/no-bump material (uniform texel colour) decodes to the
    // same length at every mip, so this correctly contributes no extra roughness there.
    const float GBUF_ROUGHNESS_FIXED_MIP = 4.0; // keep in step with gbuffer_clip.frag
    vec3  nTSFixed    = textureLod(u_BumpMap, vary_TexCoord_Bump, GBUF_ROUGHNESS_FIXED_MIP).rgb * 2.0 - 1.0;
    float nTSFixedLen = length(nTSFixed);

    // Specular-luminance → F0 remap (r_rtSpecF0Scale/Gamma); r_rtReflectionDebugMode 3
    // shows the result.
    vec3  specMap = texture(u_SpecularMap, vary_TexCoord_Specular).rgb;
    float specLum = dot(specMap, vec3(0.299, 0.587, 0.114));
    float f0      = clamp(pow(max(specLum, 0.0), u_SpecF0Gamma) * u_SpecF0Scale, 0.0, 1.0);

    // Toksvig/LEAN roughness (Stage 2): trilinear mip filtering shortens a bump map's
    // averaged per-texel normal in proportion to the sub-pixel normal variance inside
    // that footprint. Doom 3 ships no gloss map, so this variance alone sources
    // per-pixel roughness. Variance-to-roughness mapping per Toksvig 2004 / LEAN mapping.
    // Take whichever of the screen-footprint and fixed-mip samples shows MORE variance
    // (the shorter length) — either should raise roughness, and neither should mask it.
    // Floored at u_RoughnessFloor: Toksvig only sees bump-map detail, so a clean/flat
    // bump map would otherwise read as a perfect mirror regardless of the material.
    float toksvigLen      = min(nTSLen, nTSFixedLen);
    float toksvigVariance = (1.0 - toksvigLen) / max(toksvigLen, 1e-3);
    float roughness       = max(clamp(sqrt(toksvigVariance), 0.0, 1.0), u_RoughnessFloor);

    outGbuf   = vec4(nWS * 0.5 + 0.5, f0);
    outAlbedo = vec4(texture(u_DiffuseMap, vary_TexCoord_Diffuse).rgb,
                     GBuf_PackAlbedoAlpha(GBuf_SurfaceClass(), roughness));
    outMotion = GBuf_MotionVector(vary_CurClip, vary_PrevClip);
    fragColor = vec4(0.0);
}
