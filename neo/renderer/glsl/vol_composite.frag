/*
===========================================================================

dhewm3-rt Vulkan — vol_composite.frag — Volumetric scatter composite.

Samples the RGBA16F volumetric scatter buffer built by vol_march.comp and
outputs it for additive blending onto the main framebuffer.  This pass runs
once per view (after GI composite, before per-light interaction draws).

The vol_march.comp compute shader already applies density and strength
multipliers when storing, so this shader is a simple passthrough in normal
mode. r_rtVolDebugMode (0/2) switches to a replace-blend debug pipeline
(vkRT.volCompositeDebugPipeline, see vk_vol.cpp) so these visualizations
aren't muddied by additively blending onto the already-lit scene.

Uses the same fullscreen triangle vertex shader as the GI composite pass
(gi_composite.vert).

Phase 7.2 — Volumetric Lighting. Debug modes added 2026-08-23 alongside the
self-attenuation fix (see vol_march.comp) to make the per-pixel path
transmittance inspectable directly instead of only visible as a change in
the final composited brightness.

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
    vec2  invScreenSize;  // 1 / swapchain extent
    int   debugMode;      // 0 = normal composite (additive pipeline)
                          // 2 = raw scatter colour, unscaled (replace pipeline)
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
    vec2 uv  = gl_FragCoord.xy * pc.invScreenSize;
    vec4 vol = texture(u_VolMap, uv);

    if (pc.debugMode == 2)
    {
        // Raw scatter colour, same rgb the normal composite adds — just
        // replace-blended so you can see it in isolation against black instead of
        // on top of the scene. 
        fragColor = vec4(vol.rgb , 1.0);
        return;
    }

    // .a is path transmittance (see vol_march.comp), not coverage/opacity.
    // With r_rtVolAttenuateBackground the composite binds a pipeline whose
    // dstColorBlendFactor is SRC_ALPHA, giving dst = airlight + T*dst — the
    // attenuating half of the transport equation, with the same sigma_t the
    // airlight above was integrated against. The additive pipeline ignores alpha.
    fragColor = vec4(vol.rgb, vol.a);
}
