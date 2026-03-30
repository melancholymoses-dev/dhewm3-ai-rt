/*
===========================================================================


dhewm3 Vulkan backend - skybox fragment shader.

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
