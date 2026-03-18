/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - blend light vertex shader.

Blend lights project a texture onto world surfaces using projective texgen
(S, T, Q homogeneous coordinates) and a separate falloff texture (1D, just S).
Mirrors the GL texgen approach in RB_BlendLight / RB_T_BlendLight.

===========================================================================
*/
#version 450

layout(set=0, binding=0) uniform BlendParams {
    mat4  u_MVP;        // model-view-projection (Vulkan-corrected)
    vec4  u_TexGen0S;   // light projection S row (dot → TC0.x)
    vec4  u_TexGen0T;   // light projection T row (dot → TC0.y)
    vec4  u_TexGen0Q;   // light projection Q row (dot → TC0.w, for perspective divide)
    vec4  u_TexGen1S;   // light falloff S row (dot → TC1.x); TC1.y = 0.5 (fixed)
    vec4  u_Color;      // light color modulate (used by fragment shader)
};

layout(location = 0) in vec3 in_Position;

// TC0 is vec4: (S, T, 0, Q) — passed to textureProj in the frag shader
layout(location = 0) out vec4 out_TC0;
layout(location = 1) out vec2 out_TC1;

void main()
{
    vec4 pos    = vec4(in_Position, 1.0);
    gl_Position = u_MVP * pos;
    out_TC0     = vec4(dot(pos, u_TexGen0S), dot(pos, u_TexGen0T), 0.0, dot(pos, u_TexGen0Q));
    out_TC1     = vec2(dot(pos, u_TexGen1S), 0.5);
}
