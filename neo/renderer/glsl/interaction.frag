/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
dhewm3 GLSL adaptation.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Interaction fragment shader - replaces interaction.vfp ARB fragment program.
// Computes per-pixel lighting using normal/diffuse/specular maps.

#version 330 core

// Varyings from vertex shader
in vec4 vary_TexCoord_Bump;
in vec4 vary_TexCoord_Diffuse;
in vec4 vary_TexCoord_Specular;
in vec4 vary_LightProjection;   // projective (S, T, 0, Q)
in vec2 vary_LightFalloff;
in vec3 vary_LightDir;          // tangent-space, unnormalized
in vec3 vary_ViewDir;           // tangent-space, unnormalized
in vec4 vary_Color;

// Textures (matching ARB texture unit indices)
uniform sampler2D   u_BumpMap;          // texture 1: per-surface normal map
uniform sampler2D   u_LightFalloff;     // texture 2: 1D falloff (as 2D with height=1)
uniform sampler2D   u_LightProjection;  // texture 3: projected light cookie/spot map
uniform sampler2D   u_DiffuseMap;       // texture 4: diffuse albedo
uniform sampler2D   u_SpecularMap;      // texture 5: specular intensity/color
uniform sampler2D   u_SpecularTable;    // texture 6: NdotH -> specular power lookup

// Fragment program constants
uniform vec4 u_DiffuseColor;    // c[0] diffuseColor from material
uniform vec4 u_SpecularColor;   // c[1] specularColor from material
uniform vec4 u_GammaBrightness; // c[4] PP_GAMMA_BRIGHTNESS: xyz=brightness, w=1/gamma

uniform bool u_ApplyGamma;      // whether to apply gamma in shader

out vec4 fragColor;

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

    // --- Combine ---
    vec3 color = (diffuse + specular) * attenuation;
    color *= vary_Color.rgb;

    vec4 result = vec4(color, vary_Color.a);

    // --- Optional gamma correction (mirrors ARB gamma injection) ---
    if (u_ApplyGamma) {
        // result.rgb = pow(result.rgb * brightness, vec3(1/gamma))
        vec3 brightened = clamp(result.rgb * u_GammaBrightness.rgb, 0.0, 1.0);
        result.rgb = pow(brightened, vec3(u_GammaBrightness.w)); // .w = 1/gamma
    }

    fragColor = result;
}
