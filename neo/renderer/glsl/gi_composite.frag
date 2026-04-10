/*
===========================================================================

dhewm3-rt Vulkan — gi_composite.frag — GI buffer composite into framebuffer.

Samples the RGBA16F GI buffer built by gi_ray.rgen and outputs it for
additive blending onto the main framebuffer.  This pass runs once per
view (before the per-light interaction draws) so the GI contribution is
applied exactly once per pixel regardless of how many lights touch it.

Compatible with future G-buffer and Option B upgrades: the rgen/rchit
upstream can be changed freely without touching this shader.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#version 450

layout(set = 0, binding = 0) uniform sampler2D u_GIMap;

layout(location = 0) out vec4 fragColor;

void main()
{
    // Divide by the actual GI image dimensions — avoids a push constant for
    // screen size and correctly handles any resolution.
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(u_GIMap, 0));
    fragColor = vec4(texture(u_GIMap, uv).rgb, 1.0);
}
