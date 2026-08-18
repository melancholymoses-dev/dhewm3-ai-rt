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

// Shared UBO with gbuffer.vert — binding 0, both stages.
layout(set=0, binding=0) uniform GBufferParams {
    mat4  u_ModelViewProjection;
    mat4  u_ModelMatrix;
    vec4  u_BumpMatrixS;
    vec4  u_BumpMatrixT;
    vec4  u_DiffuseMatrixS;
    vec4  u_DiffuseMatrixT;
    vec4  u_SpecularMatrixS;
    vec4  u_SpecularMatrixT;
    float u_AlphaTestThreshold;
    float u_SpecF0Scale;
    float u_SpecF0Gamma;
    float _pad0;
};

layout(set=0, binding=1) uniform sampler2D u_BumpMap;
// gi_albedo_target.md: the opaque pipeline now samples binding 2 too, for the
// receiver-albedo target — u_DiffuseMatrixS/T (backend-resolved SL_DIFFUSE stage,
// or identity + whiteImage fallback) drives vary_TexCoord_Diffuse's UV here.
layout(set=0, binding=2) uniform sampler2D u_DiffuseMap;
layout(set=0, binding=3) uniform sampler2D u_SpecularMap;

layout(location = 0) out vec4 fragColor;  // attachment 0 (hdrScene) — write-masked off in the pipeline
layout(location = 1) out vec4 outGbuf;    // attachment 1 (gbufNormal) — rgb = world normal, a = F0
layout(location = 2) out vec4 outAlbedo;  // attachment 2 (gbufAlbedo) — rgb = diffuse sample

void main() {
    vec3 T = normalize(vary_TangentWS);
    vec3 B = normalize(vary_BiTangentWS);
    vec3 N = normalize(vary_NormalWS);

    vec3 nTS = normalize(texture(u_BumpMap, vary_TexCoord_Bump).rgb * 2.0 - 1.0);
    vec3 nWS = normalize(mat3(T, B, N) * nTS);

    // Same specular-luminance → F0 remap as interaction.frag's Fresnel block, so
    // r_rtReflectionDebugMode 1 (interaction.frag) and mode 3 (this G-buffer's F0,
    // see the plan's Step 9) agree on the same surfaces.
    vec3  specMap = texture(u_SpecularMap, vary_TexCoord_Specular).rgb;
    float specLum = dot(specMap, vec3(0.299, 0.587, 0.114));
    float f0      = clamp(pow(max(specLum, 0.0), u_SpecF0Gamma) * u_SpecF0Scale, 0.0, 1.0);

    outGbuf   = vec4(nWS * 0.5 + 0.5, f0);
    outAlbedo = vec4(texture(u_DiffuseMap, vary_TexCoord_Diffuse).rgb, 1.0);
    fragColor = vec4(0.0);
}
