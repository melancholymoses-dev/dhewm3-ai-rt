/*
===========================================================================

dhewm3-rt - G-buffer normal/F0 prepass vertex shader (Stage 3.5).

Runs as part of the depth prepass (VK_RB_FillDepthBuffer) when ray tracing and
vk.gbufferSupported are both active. Transforms the tangent/bitangent/normal
basis into world space so gbuffer.frag can build a world-space shading normal
for the RT reflection pass. See docs/plans/gbuffer_normal_pass.md.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 450

// Vertex attributes (matching idDrawVert ARB attrib layout — see VK_GetInteractionVertexInput)
layout(location = 0)  in vec3 in_Position;
layout(location = 8)  in vec2 in_TexCoord;
layout(location = 9)  in vec3 in_Tangent;    // ARB attrib 9 = tangents[0]
layout(location = 10) in vec3 in_BiTangent;  // ARB attrib 10 = tangents[1]
layout(location = 11) in vec3 in_Normal;     // ARB attrib 11 = normal

// Shared UBO with gbuffer.frag / gbuffer_clip.frag — binding 0, both stages.
layout(set=0, binding=0) uniform GBufferParams {
    mat4  u_ModelViewProjection;
    mat4  u_ModelMatrix;        // surf->space->modelMatrix — rigid transform (no shear/non-uniform scale)
    vec4  u_BumpMatrixS;
    vec4  u_BumpMatrixT;
    vec4  u_DiffuseMatrixS;     // alpha-test UV (gbuffer_clip.frag only)
    vec4  u_DiffuseMatrixT;
    vec4  u_SpecularMatrixS;
    vec4  u_SpecularMatrixT;
    float u_AlphaTestThreshold; // gbuffer_clip.frag only
    float u_SpecF0Scale;        // r_rtSpecF0Scale
    float u_SpecF0Gamma;        // r_rtSpecF0Gamma
    float _pad0;
};

layout(location = 0) out vec2 vary_TexCoord_Bump;
layout(location = 1) out vec2 vary_TexCoord_Diffuse;
layout(location = 2) out vec2 vary_TexCoord_Specular;
layout(location = 3) out vec3 vary_TangentWS;
layout(location = 4) out vec3 vary_BiTangentWS;
layout(location = 5) out vec3 vary_NormalWS;

void main() {
    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);
    vary_TexCoord_Bump.x     = dot(tc, u_BumpMatrixS);
    vary_TexCoord_Bump.y     = dot(tc, u_BumpMatrixT);
    vary_TexCoord_Diffuse.x  = dot(tc, u_DiffuseMatrixS);
    vary_TexCoord_Diffuse.y  = dot(tc, u_DiffuseMatrixT);
    vary_TexCoord_Specular.x = dot(tc, u_SpecularMatrixS);
    vary_TexCoord_Specular.y = dot(tc, u_SpecularMatrixT);

    // Doom 3 entity transforms are rigid (uniform scale, no shear), so a plain
    // mat3 multiply — no inverse-transpose — is safe for normals/tangents here.
    mat3 modelRot = mat3(u_ModelMatrix);
    vary_TangentWS   = modelRot * in_Tangent;
    vary_BiTangentWS = modelRot * in_BiTangent;
    vary_NormalWS    = modelRot * in_Normal;

    gl_Position = u_ModelViewProjection * vec4(in_Position, 1.0);
}
