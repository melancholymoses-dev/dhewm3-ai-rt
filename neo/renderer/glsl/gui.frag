/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - GUI / shader-pass fragment shader.

Samples a single texture and modulates by the interpolated vertex color.
Uses textureProj for projective sampling — when vary_TexCoord.w == 1.0
(normal UV path), textureProj(tex, vec3(u, v, 1)) == texture(tex, vec2(u, v)).
When w != 1 (TG_SCREEN texgen), it performs perspective-correct projective divide.

===========================================================================
*/

#version 450

layout(location = 0) in vec4 vary_TexCoord;
layout(location = 1) in vec4 vary_Color;

layout(set=0, binding=1) uniform sampler2D u_Texture;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = textureProj(u_Texture, vary_TexCoord.xyw) * vary_Color;
}
