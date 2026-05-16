/*
===========================================================================

dhewm3-rt Vulkan — vol_composite.frag — Volumetric scatter composite.

Samples the RGBA16F volumetric scatter buffer built by vol_march.comp and
outputs it for additive blending onto the main framebuffer.  This pass runs
once per view (after GI composite, before per-light interaction draws).

The vol_march.comp compute shader already applies density and strength
multipliers when storing, so this shader is a simple passthrough.

Uses the same fullscreen triangle vertex shader as the GI composite pass
(gi_composite.vert).

Phase 7.2 — Volumetric Lighting.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#version 450

layout(set = 0, binding = 0) uniform sampler2D u_VolMap;

layout(location = 0) out vec4 fragColor;

void main()
{
    // Divide by the actual volBuf dimensions — handles any resolution correctly.
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(u_VolMap, 0));
    fragColor = vec4(texture(u_VolMap, uv).rgb, 1.0);
}
