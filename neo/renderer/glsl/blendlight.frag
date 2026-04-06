/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan backend - blend light fragment shader.

Projects the light image (samp0) onto the surface via projective divide and
multiplies by the 1D falloff (samp1), then by the light color.
samp0 = stage texture (projected light image)
samp1 = vLight->falloffImage (1D attenuation)

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.


===========================================================================
*/
#version 450

layout(set=0, binding=0) uniform BlendParams {
    mat4  u_MVP;
    vec4  u_TexGen0S;
    vec4  u_TexGen0T;
    vec4  u_TexGen0Q;
    vec4  u_TexGen1S;
    vec4  u_Color;
};

layout(set=0, binding=1) uniform sampler2D samp0;
layout(set=0, binding=2) uniform sampler2D samp1;

// TC0.xyw used for projective sample (S/Q, T/Q via textureProj vec3)
layout(location = 0) in vec4 in_TC0;
layout(location = 1) in vec2 in_TC1;

layout(location = 0) out vec4 out_Color;

void main()
{
    // textureProj(sampler2D, vec3) divides xy by z to get final coords
    out_Color = textureProj(samp0, in_TC0.xyw) * texture(samp1, in_TC1) * u_Color;
}
