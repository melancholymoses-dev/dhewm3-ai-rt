/*
===========================================================================


dhewm3-rt Vulkan backend - skybox vertex shader.

Matches Doom3 GL skybox texgen behavior: sample direction is local vertex
position minus local view origin.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 450

layout(location = 0) in vec3 in_Position;
layout(location = 3) in vec4 in_Color;

layout(set=0, binding=0) uniform SkyboxParams {
    mat4 u_ModelViewProjection;
    vec4 u_LocalViewOrigin;
    vec4 u_ColorModulate;
    vec4 u_ColorAdd;
};

layout(location = 0) out vec3 vary_CubeDir;
layout(location = 1) out vec4 vary_Color;

void main() {
    vec4 position = vec4(in_Position, 1.0);
    gl_Position = u_ModelViewProjection * position;

    vary_CubeDir = in_Position - u_LocalViewOrigin.xyz;
    vary_Color = clamp(in_Color * u_ColorModulate + u_ColorAdd, 0.0, 1.0);
}
