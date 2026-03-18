/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - fog light vertex shader.

Computes two texture coordinates from world-position dot-products with the
fog planes, mirroring the GL texgen (GL_OBJECT_PLANE) approach in RB_FogPass.

===========================================================================
*/
#version 450

layout(set=0, binding=0) uniform FogParams {
    mat4  u_MVP;        // model-view-projection (Vulkan-corrected)
    vec4  u_TexGen0S;   // fog depth plane S (dot product → TC0.x)
    vec4  u_TexGen0T;   // fog depth plane T (dot product → TC0.y), usually (0,0,0,0.5)
    vec4  u_TexGen1S;   // fog enter plane S (dot product → TC1.x, view-distance based)
    vec4  u_TexGen1T;   // fog enter plane T (dot product → TC1.y, fog-top-plane based)
    vec4  u_Color;      // fog color (used by fragment shader)
};

layout(location = 0) in vec3 in_Position;

layout(location = 0) out vec2 out_TC0;
layout(location = 1) out vec2 out_TC1;

void main()
{
    vec4 pos    = vec4(in_Position, 1.0);
    gl_Position = u_MVP * pos;
    out_TC0     = vec2(dot(pos, u_TexGen0S), dot(pos, u_TexGen0T));
    out_TC1     = vec2(dot(pos, u_TexGen1S), dot(pos, u_TexGen1T));
}
