/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - fog light fragment shader.

Multiplies the two fog textures together and modulates by the fog color.
samp0 = globalImages->fogImage      (the 1D fog gradient, mapped via TC0)
samp1 = globalImages->fogEnterImage (the entry-fade correction, mapped via TC1)

===========================================================================
*/
#version 450

layout(set=0, binding=0) uniform FogParams {
    mat4  u_MVP;
    vec4  u_TexGen0S;
    vec4  u_TexGen0T;
    vec4  u_TexGen1S;
    vec4  u_TexGen1T;
    vec4  u_Color;
};

layout(set=0, binding=1) uniform sampler2D samp0;
layout(set=0, binding=2) uniform sampler2D samp1;

layout(location = 0) in vec2 in_TC0;
layout(location = 1) in vec2 in_TC1;

layout(location = 0) out vec4 out_Color;

void main()
{
    out_Color = texture(samp0, in_TC0) * texture(samp1, in_TC1) * u_Color;
}
