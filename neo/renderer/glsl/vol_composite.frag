/*
===========================================================================

dhewm3-rt Vulkan — vol_composite.frag — Volumetric scatter composite.

Samples the RGBA16F volumetric scatter buffer built by vol_march.comp and
outputs it for additive blending onto the main framebuffer.  This pass runs
once per view (after GI composite, before per-light interaction draws).

The vol_march.comp compute shader already applies density and strength
multipliers when storing, so this shader is a simple passthrough in normal
mode. r_rtVolDebugMode (0/1/2) switches to a replace-blend debug pipeline
(vkRT.volCompositeDebugPipeline, see vk_vol.cpp) so these visualizations
aren't muddied by additively blending onto the already-lit scene — mirrors
r_rtReflectionDebugMode's pattern in refl_composite.frag.

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
                          // 1 = path transmittance, greyscale (replace pipeline)
                          // 2 = raw scatter colour, unscaled (replace pipeline)
    float debugGain;      // mode 2 only — see r_rtVolDebugGain in vk_vol.cpp for why
} pc;

layout(location = 0) out vec4 fragColor;

// No exponent on the transmittance any more.  An earlier version raised it to a
// T^k artistic power because sigma_t was conflated with the per-class scattering
// strength and was ~30x too thick to use as a real extinction coefficient; the two
// are separate cvars now (20260917_vol_transport_coefficients.md), so .a is the
// honest path transmittance and the blend is the plain transport equation.

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

    if (pc.debugMode == 1)
    {
        // Path transmittance (vol.a): 1.0 = clear air between camera and surface,
        // 0.0 = the medium fully extinguished the ray before it got there.  This is
        // exactly what the composite multiplies the background by, so it is the
        // direct check on r_rtVolExtinction: it should read near-white across a room
        // and visibly grey down a long corridor.  Uniformly near-black a few units
        // out means sigma_t is in smoke territory, not haze.
        fragColor = vec4(vec3(vol.a), 1.0);
        return;
    }

    if (pc.debugMode == 2)
    {
        // Raw scatter colour, same rgb the normal composite adds — just
        // replace-blended so you can see it in isolation against black instead of
        // on top of the scene. This is a genuinely tiny linear-HDR value (fog is
        // meant to be subtle), and unlike mode 1's already-normalised [0,1]
        // transmittance, it still passes through the end-of-frame tonemap's toe
        // curve, which crushes small values toward black by design. debugGain
        // exists ONLY to survive that crush for viewing — it has no effect on
        // mode 0's actual composite.
        fragColor = vec4(vol.rgb * pc.debugGain, 1.0);
        return;
    }

    // .a is path transmittance (see vol_march.comp), not coverage/opacity.
    // With r_rtVolAttenuateBackground the composite binds a pipeline whose
    // dstColorBlendFactor is SRC_ALPHA, giving dst = airlight + T*dst — the
    // attenuating half of the transport equation, with the same sigma_t the
    // airlight above was integrated against. The additive pipeline ignores alpha.
    fragColor = vec4(vol.rgb, vol.a);
}
