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

layout(push_constant) uniform PC {
    vec2 invScreenSize;   // 1 / swapchain extent
} pc;

layout(location = 0) out vec4 fragColor;

void main()
{
    // UV must come from the SCREEN size, not textureSize(u_VolMap): since P8 the
    // sampled image may be the half-res march buffer (r_rtVolHalfRes 1 with
    // r_rtVolBilateral 0), in which case dividing by its own dimensions would put
    // gl_FragCoord into [0,2] and sample edge-clamped garbage over most of the
    // screen.  The vol image always covers the full view, just at its own
    // resolution, so the sampler's LINEAR filter does the upsample.
    vec2 uv = gl_FragCoord.xy * pc.invScreenSize;
    fragColor = vec4(texture(u_VolMap, uv).rgb, 1.0);
}
