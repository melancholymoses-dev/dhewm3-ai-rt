/*
===========================================================================


dhewm3 GLSL adaptation.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

// Depth pre-pass fragment shader.
// Outputs nothing (depth only), with optional alpha test for masked surfaces.

#version 450

layout(location = 0) in vec2 vary_TexCoord;

// Shared UBO with depth.vert — binding 0, both stages.
layout(set=0, binding=0) uniform DepthParams {
    mat4  u_ModelViewProjection;
    vec4  u_TextureMatrixS;
    vec4  u_TextureMatrixT;
    int   u_AlphaTest;
    float u_AlphaTestThreshold;
    vec2  _pad;
};
layout(set=0, binding=1) uniform sampler2D u_DiffuseMap;  // alpha-test surfaces only

layout(location = 0) out vec4 fragColor;

void main() {
    if (u_AlphaTest != 0) {
        float alpha = texture(u_DiffuseMap, vary_TexCoord).a;
        if (alpha < u_AlphaTestThreshold) {
            discard;
        }
    }
    // No color output needed — depth is written automatically.
    fragColor = vec4(0.0);
}
