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
layout(set = 0, binding = 1) uniform sampler2D u_AOMap; // RT AO (1=open) or 1x1 white fallback

// docs/plans/20260930_AO_GI_refine.md: AO darkens indirect light here instead of
// (or as well as, per r_rtAODirectStrength) the direct diffuse in interaction.frag.
layout(push_constant) uniform CompositePC {
    int   useAO;      // 0 when AO was not written this frame
    float aoStrength; // r_rtAOIndirectStrength
    int   debugMode;  // r_rtAODebug; non-zero only on the replace-blend debug pipeline
    float debugGain;  // r_rtAODebugGain
} pc;

layout(location = 0) out vec4 fragColor;

void main()
{
    // Divide by the actual image dimensions — avoids a push constant for
    // screen size and correctly handles any resolution.
    vec2 uv   = gl_FragCoord.xy / vec2(textureSize(u_GIMap, 0));
    vec3 gi   = texture(u_GIMap, uv).rgb;
    float ao  = (pc.useAO != 0) ? texture(u_AOMap, gl_FragCoord.xy / vec2(textureSize(u_AOMap, 0))).r : 1.0;
    float aoTerm = mix(1.0, ao, pc.aoStrength);

    if (pc.debugMode == 1)
    {
        // Raw mask; red tint flags "AO not valid this frame" rather than reading as all-open.
        fragColor = (pc.useAO != 0) ? vec4(vec3(ao), 1.0) : vec4(1.0, 0.3, 0.3, 1.0);
        return;
    }
    if (pc.debugMode == 2)
    {
        fragColor = vec4(gi * aoTerm * pc.debugGain, 1.0);
        return;
    }
    if (pc.debugMode == 3)
    {
        fragColor = vec4(gi * pc.debugGain, 1.0);
        return;
    }

    fragColor = vec4(gi * aoTerm, 1.0);
}
