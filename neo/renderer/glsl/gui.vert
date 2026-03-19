/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - GUI / shader-pass vertex shader.

Draws 2D GUI surfaces and unlit world surfaces (menus, HUD, console).

===========================================================================
*/

#version 450

// Vertex attributes (matching idDrawVert ARB attrib layout)
layout(location = 0) in vec3 in_Position;
layout(location = 3) in vec4 in_Color;       // per-vertex RGBA, normalized [0,1]
layout(location = 8) in vec2 in_TexCoord;    // ARB attrib 8 = st

// Per-draw UBO: binding 0, vertex stage only
layout(set=0, binding=0) uniform GuiParams {
    mat4 u_ModelViewProjection;
    vec4 u_ColorModulate;   // multiply vertex color (SVC_MODULATE → (1,1,1,1); SVC_IGNORE → (0,0,0,0))
    vec4 u_ColorAdd;        // add to color (SVC_IGNORE → stage color; SVC_MODULATE → (0,0,0,0))
    vec4 u_TexMatrixS;      // row 0 of 2D affine UV transform: new_u = dot(vec4(uv, 0, 1), u_TexMatrixS)
    vec4 u_TexMatrixT;      // row 1 of 2D affine UV transform: new_v = dot(vec4(uv, 0, 1), u_TexMatrixT)
};

layout(location = 0) out vec2 vary_TexCoord;
layout(location = 1) out vec4 vary_Color;

void main() {
    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);
    vary_TexCoord = vec2(dot(tc, u_TexMatrixS), dot(tc, u_TexMatrixT));
    vary_Color = clamp(in_Color * u_ColorModulate + u_ColorAdd, 0.0, 1.0);
    gl_Position = u_ModelViewProjection * vec4(in_Position, 1.0);
}
