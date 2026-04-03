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

// Interaction vertex shader - replaces interaction.vfp ARB program
// Computes per-vertex light/view vectors in tangent space and texture coordinates.

#version 450

// Vertex attributes (matching ARB attrib indices for compatibility)
layout(location = 0) in vec3 in_Position;
layout(location = 3) in vec4 in_Color;        // GL_COLOR_ARRAY, normalized [0,1]
layout(location = 8) in vec2 in_TexCoord;     // ARB attrib 8 = st
layout(location = 9) in vec3 in_Tangent;      // ARB attrib 9 = tangents[0]
layout(location = 10) in vec3 in_BiTangent;   // ARB attrib 10 = tangents[1]
layout(location = 11) in vec3 in_Normal;      // ARB attrib 11 = normal

// Shared UBO — binding 0, both vertex and fragment stages.
// Field order matches VkInteractionUBO in vk_pipeline.cpp (std140).
layout(set=0, binding=0) uniform InteractionParams {
    // vertex stage parameters
    vec4  u_LightOrigin;        // c[4]  PP_LIGHT_ORIGIN
    vec4  u_ViewOrigin;         // c[5]  PP_VIEW_ORIGIN
    vec4  u_LightProjectionS;   // c[6]  PP_LIGHT_PROJECT_S
    vec4  u_LightProjectionT;   // c[7]  PP_LIGHT_PROJECT_T
    vec4  u_LightProjectionQ;   // c[8]  PP_LIGHT_PROJECT_Q
    vec4  u_LightFalloffS;      // c[9]  PP_LIGHT_FALLOFF_S
    vec4  u_BumpMatrixS;        // c[10] PP_BUMP_MATRIX_S
    vec4  u_BumpMatrixT;        // c[11] PP_BUMP_MATRIX_T
    vec4  u_DiffuseMatrixS;     // c[12] PP_DIFFUSE_MATRIX_S
    vec4  u_DiffuseMatrixT;     // c[13] PP_DIFFUSE_MATRIX_T
    vec4  u_SpecularMatrixS;    // c[14] PP_SPECULAR_MATRIX_S
    vec4  u_SpecularMatrixT;    // c[15] PP_SPECULAR_MATRIX_T
    vec4  u_ColorModulate;      // c[16] PP_COLOR_MODULATE
    vec4  u_ColorAdd;           // c[17] PP_COLOR_ADD
    mat4  u_ModelViewProjection;
    // fragment stage parameters (unused in this stage, kept for shared layout)
    vec4  u_DiffuseColor;
    vec4  u_SpecularColor;
    vec4  u_GammaBrightness;
    int   u_ApplyGamma;
    float u_ScreenWidth;
    float u_ScreenHeight;
    int   u_UseShadowMask;
    int   u_UseAO;
    float u_LightScale;
};

// Varyings to fragment shader
layout(location = 0) out vec4 vary_TexCoord_Bump;        // xy = bump texcoord
layout(location = 1) out vec4 vary_TexCoord_Diffuse;     // xy = diffuse texcoord
layout(location = 2) out vec4 vary_TexCoord_Specular;    // xy = specular texcoord
layout(location = 3) out vec4 vary_LightProjection;      // xyzw = projective light tex coords (S,T,_,Q)
layout(location = 4) out vec2 vary_LightFalloff;         // x = falloff S
layout(location = 5) out vec3 vary_LightDir;             // tangent-space light direction (unnormalized)
layout(location = 6) out vec3 vary_ViewDir;              // tangent-space view direction (unnormalized)
layout(location = 7) out vec4 vary_Color;                // vertex color after modulate/add

void main() {
    vec4 pos = vec4(in_Position, 1.0);

    // --- Texture coordinate transforms ---
    // Bump map texcoords: dot(texcoord, bumpMatrix row)
    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);
    vary_TexCoord_Bump.x     = dot(tc, u_BumpMatrixS);
    vary_TexCoord_Bump.y     = dot(tc, u_BumpMatrixT);

    vary_TexCoord_Diffuse.x  = dot(tc, u_DiffuseMatrixS);
    vary_TexCoord_Diffuse.y  = dot(tc, u_DiffuseMatrixT);

    vary_TexCoord_Specular.x = dot(tc, u_SpecularMatrixS);
    vary_TexCoord_Specular.y = dot(tc, u_SpecularMatrixT);

    // --- Projective light texture coords ---
    vary_LightProjection.x = dot(pos, u_LightProjectionS);
    vary_LightProjection.y = dot(pos, u_LightProjectionT);
    vary_LightProjection.z = 0.0;
    vary_LightProjection.w = dot(pos, u_LightProjectionQ);

    // --- Light falloff (1D texture) ---
    vary_LightFalloff.x = dot(pos, u_LightFalloffS);
    vary_LightFalloff.y = 0.5; // center of 1D texture

    // --- Tangent-space light and view directions ---
    vec3 lightVec = u_LightOrigin.xyz - in_Position;
    vary_LightDir.x = dot(lightVec, in_Tangent);
    vary_LightDir.y = dot(lightVec, in_BiTangent);
    vary_LightDir.z = dot(lightVec, in_Normal);

    vec3 viewVec = u_ViewOrigin.xyz - in_Position;
    vary_ViewDir.x = dot(viewVec, in_Tangent);
    vary_ViewDir.y = dot(viewVec, in_BiTangent);
    vary_ViewDir.z = dot(viewVec, in_Normal);

    // --- Vertex color ---
    vary_Color = in_Color * u_ColorModulate + u_ColorAdd;

    gl_Position = u_ModelViewProjection * pos;
}
