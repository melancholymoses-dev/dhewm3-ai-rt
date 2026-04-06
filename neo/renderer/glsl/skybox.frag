/*
===========================================================================


dhewm3-rt Vulkan backend - skybox fragment shader.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 450

layout(location = 0) in vec3 vary_CubeDir;
layout(location = 1) in vec4 vary_Color;

layout(set=0, binding=1) uniform samplerCube u_CubeTexture;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(u_CubeTexture, vary_CubeDir) * vary_Color;
}
