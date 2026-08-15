/*
===========================================================================

Interaction Fragment Shader

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
===========================================================================
*/

// Interaction fragment shader - replaces interaction.vfp ARB fragment program.
// Computes per-pixel lighting using normal/diffuse/specular maps.

#version 450

// Varyings from vertex shader
layout(location = 0) in vec4 vary_TexCoord_Bump;
layout(location = 1) in vec4 vary_TexCoord_Diffuse;
layout(location = 2) in vec4 vary_TexCoord_Specular;
layout(location = 3) in vec4 vary_LightProjection;   // projective (S, T, 0, Q)
layout(location = 4) in vec2 vary_LightFalloff;
layout(location = 5) in vec3 vary_LightDir;          // tangent-space, unnormalized
layout(location = 6) in vec3 vary_ViewDir;           // tangent-space, unnormalized
layout(location = 7) in vec4 vary_Color;

// Samplers with explicit Vulkan bindings (set=0, binding=1..7)
layout(set=0, binding=1) uniform sampler2D u_BumpMap;         // per-surface normal map
layout(set=0, binding=2) uniform sampler2D u_LightFalloff;    // 1D radial falloff
layout(set=0, binding=3) uniform sampler2D u_LightProjection; // projected light cookie
layout(set=0, binding=4) uniform sampler2D u_DiffuseMap;      // diffuse albedo
layout(set=0, binding=5) uniform sampler2D u_SpecularMap;     // specular intensity/color
layout(set=0, binding=6) uniform sampler2D u_SpecularTable;   // NdotH -> specular power
layout(set=0, binding=7) uniform sampler2DArray u_ShadowMask; // RT shadow mask array, one layer per
                                                              // shadow-casting light (1=lit, 0=shadowed)
layout(set=0, binding=8) uniform sampler2D u_AOMap;           // RT AO mask (1=unoccluded, 0=occluded)
// binding=9 (RT reflection buffer) removed: reflections are now composited by the
// dedicated refl_composite.frag fullscreen pass (Stage 3.5, Step 8 — see
// docs/plans/gbuffer_normal_pass.md), not sampled per-light here.

// Shared UBO — binding 0, both vertex and fragment stages.
// Field order matches VkInteractionUBO in vk_pipeline.cpp (std140).
layout(set=0, binding=0) uniform InteractionParams {
    // vertex stage parameters (unused in this stage, kept for shared layout)
    vec4  u_LightOrigin;
    vec4  u_ViewOrigin;
    vec4  u_LightProjectionS;
    vec4  u_LightProjectionT;
    vec4  u_LightProjectionQ;
    vec4  u_LightFalloffS;
    vec4  u_BumpMatrixS;
    vec4  u_BumpMatrixT;
    vec4  u_DiffuseMatrixS;
    vec4  u_DiffuseMatrixT;
    vec4  u_SpecularMatrixS;
    vec4  u_SpecularMatrixT;
    vec4  u_ColorModulate;
    vec4  u_ColorAdd;
    mat4  u_ModelViewProjection;
    // fragment stage parameters
    vec4  u_DiffuseColor;
    vec4  u_SpecularColor;
    vec4  u_GammaBrightness;    // xyz=brightness, w=1/gamma
    int   u_ApplyGamma;
    float u_ScreenWidth;
    float u_ScreenHeight;
    int   u_UseShadowMask;
    int   u_UseAO;       // 1 when RT AO mask is valid this frame
    float u_LightScale;  // backEnd.overBright — multiply final color before gamma
    int   u_UseReflections;      // unused (Stage 3.5, Step 8) — kept so this block's
                                  // layout still matches VkInteractionUBO in vk_pipeline.cpp
    float u_SpecF0Scale;         // r_rtSpecF0Scale — multiplier for specular→F0 remap
    float u_SpecF0Gamma;         // r_rtSpecF0Gamma — power exponent for specular→F0 remap
    int   u_ReflectionDebugMode; // r_rtReflectionDebugMode — 0=off, 1=Fresnel greyscale
    int   u_ShadowMaskLayer;     // P1b — array layer holding this light's shadow mask
};

layout(location = 0) out vec4 fragColor;

void main() {
    // --- Normal from bump map (tangent space) ---
    vec3 N = texture(u_BumpMap, vary_TexCoord_Bump.xy).rgb;
    N = N * 2.0 - 1.0;     // expand [0,1] -> [-1,1]
    N = normalize(N);

    // --- Light and view directions in tangent space ---
    vec3 L = normalize(vary_LightDir);
    vec3 V = normalize(vary_ViewDir);
    vec3 H = normalize(L + V);  // half-angle vector

    // --- Light attenuation ---
    // Projective light map: perform perspective divide
    vec2 lightProjTC = vary_LightProjection.xy / vary_LightProjection.w;
    vec3 lightColor  = texture(u_LightProjection, lightProjTC).rgb;

    // Falloff texture encodes radial attenuation along S axis
    float falloff = texture(u_LightFalloff, vary_LightFalloff).r;

    // Combined light attenuation
    vec3 attenuation = lightColor * falloff;

    // --- Diffuse ---
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse  = texture(u_DiffuseMap,   vary_TexCoord_Diffuse.xy).rgb;
    diffuse *= u_DiffuseColor.rgb * NdotL;

    // --- Specular (Blinn-Phong via lookup table) ---
    // texture 6 encodes specular power/falloff based on NdotH
    float NdotH   = clamp(dot(N, H), 0.0, 1.0);
    float NdotV   = clamp(dot(N, V), 0.0, 1.0);
    vec3 specLookup = texture(u_SpecularTable, vec2(NdotH, 0.5)).rgb;
    vec3 specMap    = texture(u_SpecularMap, vary_TexCoord_Specular.xy).rgb;
    vec3 specular   = specMap * u_SpecularColor.rgb * specLookup;

    // --- Fresnel / F0 remap (Stage 2) ---
    // Doom 3 specular maps were authored for Blinn-Phong, not PBR, and run bright
    // almost everywhere. A power-curve remap gates non-trivial F0 to only the
    // brightest texels (actual metal / wet surfaces), preventing the hall-of-mirrors
    // look that occurs when the reflection buffer is enabled on ordinary surfaces.
    // Only computed for the debug visualization below — since Stage 3.5 (Step 8)
    // moved the real reflection blend to refl_composite.frag, this is its only consumer.
    float fresnel = 0.0;
    if (u_ReflectionDebugMode == 1) {
        float specLum = dot(specMap, vec3(0.299, 0.587, 0.114));
        float normF0  = clamp(pow(max(specLum, 0.0), u_SpecF0Gamma) * u_SpecF0Scale, 0.0, 1.0);
        fresnel = normF0 + (1.0 - normF0) * pow(1.0 - NdotV, 5.0);
    }

    // --- RT shadow mask ---
    float shadow = 1.0;
    if (u_UseShadowMask != 0) {
        vec2 shadowUV = gl_FragCoord.xy / vec2(u_ScreenWidth, u_ScreenHeight);
        shadow = texture(u_ShadowMask, vec3(shadowUV, float(u_ShadowMaskLayer))).r;
    }

    // --- RT ambient occlusion ---
    // Applied to diffuse light: contact darkening in corners and crevices.
    // Not applied to specular; AO modulates surface-level indirect light, not
    // direct specular reflections.
    float ao = 1.0;
    if (u_UseAO != 0) {
        vec2 aoUV = gl_FragCoord.xy / vec2(u_ScreenWidth, u_ScreenHeight);
        ao = texture(u_AOMap, aoUV).r;
    }

    // --- RT reflections ---
    // Composited by the dedicated refl_composite.frag fullscreen pass (Stage 3.5,
    // Step 8 — see docs/plans/gbuffer_normal_pass.md), once per view instead of
    // once per light here. That pass also fixes the flat-polygon reflection-direction
    // bug the old per-light block had: it uses the bump-mapped G-buffer normal from
    // reflect_ray.rgen instead of rt_ReconstructNormal's depth-gradient geometric normal.

    // --- RT global illumination (Phase 6.1) ---
    // GI is now applied by the dedicated VK_RT_CompositeGI fullscreen pass (gi_composite.frag)

    // --- Combine ---
    vec3 color = (diffuse * ao + specular) * attenuation * shadow;
    color *= vary_Color.rgb;

    color *= u_LightScale;

    vec4 result = vec4(color, vary_Color.a);

    // --- Debug: Fresnel visualisation ---
    // Mode 1: output the Fresnel term as greyscale so you can walk the level and
    // confirm which surfaces will receive reflections. This is the pre-remap
    // specular→F0 curve only (no grazing-angle clamp); see modes 2-4 (reflect_ray.rgen)
    // for the actual G-buffer data the reflection pass consumes.
    // Concrete/cloth/skin should read near-black; metal/wet/glass bright at grazing.
    if (u_ReflectionDebugMode == 1) {
        fragColor = vec4(vec3(fresnel), 1.0);
        return;
    }

    // --- Optional gamma correction (mirrors ARB gamma injection) ---
    if (u_ApplyGamma != 0) {
        // result.rgb = pow(result.rgb * brightness, vec3(1/gamma))
        vec3 brightened = clamp(result.rgb * u_GammaBrightness.rgb, 0.0, 1.0);
        result.rgb = pow(brightened, vec3(u_GammaBrightness.w)); // .w = 1/gamma
    }

    fragColor = result;
}
