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
layout(set=0, binding=7) uniform sampler2D u_ShadowMask;      // RT shadow mask (1=lit, 0=shadowed)
layout(set=0, binding=8) uniform sampler2D u_AOMap;           // RT AO mask (1=unoccluded, 0=occluded)
layout(set=0, binding=9) uniform sampler2D u_ReflectionMap;   // RT reflection buffer (RGBA16F)

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
    int   u_UseReflections; // 1 when RT reflection buffer is valid this frame
    int   _pad;             // reserved (was u_UseGI — GI now handled by gi_composite pass)
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
    float NdotH = clamp(dot(N, H), 0.0, 1.0);
    vec3 specLookup = texture(u_SpecularTable, vec2(NdotH, 0.5)).rgb;
    vec3 specular = texture(u_SpecularMap, vary_TexCoord_Specular.xy).rgb;
    specular *= u_SpecularColor.rgb * specLookup;

    // --- RT shadow mask ---
    float shadow = 1.0;
    if (u_UseShadowMask != 0) {
        vec2 shadowUV = gl_FragCoord.xy / vec2(u_ScreenWidth, u_ScreenHeight);
        shadow = texture(u_ShadowMask, shadowUV).r;
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

    // --- RT reflections (lighting-weighted, Step 5.3) ---
    // NOTE: this shader is called once per light, so reflColor accumulates across
    // all N lights that illuminate a pixel.  To prevent over-brightening we weight
    // the contribution by how much THIS light actually illuminates the surface:
    //   litness = lightLum * NdotL * shadow
    // Effect:
    //   - Shadow (RT mask = 0)  → no reflection from this light's pass.
    //   - Surface facing away   → NdotL = 0, no contribution.
    //   - Dim/distant light     → attenuation luma → 0, minimal contribution.
    //   - Well-lit surface      → full weighted reflection.
    // Global brightness is controlled by r_rtReflectionBlend (baked into the
    // reflection buffer at dispatch time).  kReflLightScale is a per-light
    // dampener to keep multi-light accumulation visually reasonable.
    vec3 reflColor = vec3(0.0);
    if (u_UseReflections != 0) {
        vec2 reflUV     = gl_FragCoord.xy / vec2(u_ScreenWidth, u_ScreenHeight);
        vec3 reflSample = texture(u_ReflectionMap, reflUV).rgb;

        // Specular map masks which surfaces are reflective (matte → 0, shiny → 1).
        float specBase = dot(texture(u_SpecularMap, vary_TexCoord_Specular.xy).rgb,
                             vec3(0.299, 0.587, 0.114));

        // How much does this light illuminate this pixel?
        float lightLum = dot(attenuation, vec3(0.299, 0.587, 0.114));
        float litness  = lightLum * NdotL * shadow;

        // Per-light scale keeps accumulation across multiple lights from over-brightening.
        // Tune r_rtReflectionBlend for global brightness; this constant controls the
        // per-light weighting strength.
        const float kReflLightScale = 0.15;
        reflColor = reflSample * specBase * litness * kReflLightScale;
    }

    // --- RT global illumination (Phase 6.1) ---
    // GI is now applied by the dedicated VK_RT_CompositeGI fullscreen pass (gi_composite.frag)

    // --- Combine ---
    // reflColor already encodes per-light weighting (attenuation × NdotL × shadow)
    // so it is zero on shadowed / unlit surfaces.
    vec3 color = (diffuse * ao + specular) * attenuation * shadow + reflColor;
    color *= vary_Color.rgb;

    color *= u_LightScale;

    vec4 result = vec4(color, vary_Color.a);

    // --- Optional gamma correction (mirrors ARB gamma injection) ---
    if (u_ApplyGamma != 0) {
        // result.rgb = pow(result.rgb * brightness, vec3(1/gamma))
        vec3 brightened = clamp(result.rgb * u_GammaBrightness.rgb, 0.0, 1.0);
        result.rgb = pow(brightened, vec3(u_GammaBrightness.w)); // .w = 1/gamma
    }

    fragColor = result;
}
