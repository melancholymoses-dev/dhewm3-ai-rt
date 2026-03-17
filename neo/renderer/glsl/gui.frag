/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - GUI / shader-pass fragment shader.

Samples a single texture and modulates by the interpolated vertex color.

===========================================================================
*/

#version 450

layout(location = 0) in vec2 vary_TexCoord;
layout(location = 1) in vec4 vary_Color;

layout(set=0, binding=1) uniform sampler2D u_Texture;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(u_Texture, vary_TexCoord) * vary_Color;
}
