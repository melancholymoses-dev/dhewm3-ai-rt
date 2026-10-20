/*
===========================================================================

dhewm3-rt Vulkan — vol_composite.frag — Volumetric scatter composite.

Samples the RGBA16F volumetric scatter buffer built by vol_froxel_resolve.comp
and outputs it for additive blending onto the main framebuffer.  This pass runs
once per view (after GI composite, before per-light interaction draws).

The resolve already applies density and strength multipliers when storing, so
this shader is a passthrough in normal mode.  debugMode 2 is selected when a
r_rtVolFroxelDebug overlay is active: the composite binds a replace-blend
pipeline so the visualization isn't muddied by adding onto the already-lit scene.

Uses the same fullscreen triangle vertex shader as the GI composite pass
(gi_composite.vert).

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
    // UV comes from the SCREEN size, not textureSize(u_VolMap): the vol image is
    // display-sized and written identity-mapped into the render sub-rect, which is
    // not necessarily the whole image.
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

    // .a is path transmittance, not coverage/opacity.
    // With r_rtVolAttenuateBackground the composite binds a pipeline whose
    // dstColorBlendFactor is SRC_ALPHA, giving dst = airlight + T*dst — the
    // attenuating half of the transport equation, with the same sigma_t the
    // airlight above was integrated against. The additive pipeline ignores alpha.
    fragColor = vec4(vol.rgb, vol.a);
}
