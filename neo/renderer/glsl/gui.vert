/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan backend - GUI / shader-pass vertex shader.

Draws 2D GUI surfaces, unlit world surfaces (menus, HUD, console),
and TG_SCREEN projective-texture surfaces (mirrors, screen captures).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 450

// Vertex attributes (matching idDrawVert ARB attrib layout)
layout(location = 0) in vec3 in_Position;
layout(location = 3) in vec4 in_Color;       // per-vertex RGBA, normalized [0,1]
layout(location = 8) in vec2 in_TexCoord;    // ARB attrib 8 = st

// Per-draw UBO: binding 0
layout(set=0, binding=0) uniform GuiParams {
    mat4 u_ModelViewProjection;
    vec4 u_ColorModulate;   // multiply vertex color
    vec4 u_ColorAdd;        // add to color
    vec4 u_TexMatrixS;      // row 0 of 2D affine UV transform; z = texgen flag
    vec4 u_TexMatrixT;      // row 1 of 2D affine UV transform
    vec4 u_TexGenS;         // TG_SCREEN S plane (0.5*MVP_row0 + 0.5*MVP_row3)
    vec4 u_TexGenT;         // TG_SCREEN T plane (-0.5*MVP_row1 + 0.5*MVP_row3)
    vec4 u_TexGenQ;         // TG_SCREEN Q plane (MVP row 3)
};

layout(location = 0) out vec4 vary_TexCoord;  // xyw used by textureProj in frag
layout(location = 1) out vec4 vary_Color;

void main() {
    gl_Position = u_ModelViewProjection * vec4(in_Position, 1.0);

    if (u_TexMatrixS.z > 0.5) {
        // TG_SCREEN: projective texcoords from vertex position + texgen planes
        vec4 pos = vec4(in_Position, 1.0);
        float s = dot(pos, u_TexGenS);
        float t = dot(pos, u_TexGenT);
        float q = dot(pos, u_TexGenQ);
        vary_TexCoord = vec4(s, t, 0.0, q);
    } else {
        // Normal UV path: apply 2D texture matrix
        vec4 tc = vec4(in_TexCoord, 0.0, 1.0);
        float u = dot(tc, u_TexMatrixS);
        float v = dot(tc, u_TexMatrixT);
        vary_TexCoord = vec4(u, v, 0.0, 1.0);
    }

    vary_Color = clamp(in_Color * u_ColorModulate + u_ColorAdd, 0.0, 1.0);
}
