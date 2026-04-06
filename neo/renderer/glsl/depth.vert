/*
===========================================================================

Depth Vertex Shader
dhewm3 GLSL adaptation.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

// Depth pre-pass vertex shader.
// Used by RB_T_FillDepthBuffer to populate the depth buffer before lighting passes.

#version 450

layout(location = 0) in vec3 in_Position;
layout(location = 8) in vec2 in_TexCoord;   // for alpha-test

// Shared UBO with depth.frag — binding 0, both stages.
layout(set=0, binding=0) uniform DepthParams {
    mat4  u_ModelViewProjection;
    vec4  u_TextureMatrixS;       // diffuse matrix S (for alpha-tested surfaces)
    vec4  u_TextureMatrixT;       // diffuse matrix T
    int   u_AlphaTest;
    float u_AlphaTestThreshold;
    vec2  _pad;
};

layout(location = 0) out vec2 vary_TexCoord;

void main() {
    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);
    vary_TexCoord.x = dot(tc, u_TextureMatrixS);
    vary_TexCoord.y = dot(tc, u_TextureMatrixT);

    gl_Position = u_ModelViewProjection * vec4(in_Position, 1.0);
}
