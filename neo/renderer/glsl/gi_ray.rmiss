/*
===========================================================================

dhewm3-rt Vulkan — gi_ray.rmiss — miss shader for GI bounce rays.

Ray missed all geometry: return a very dim ambient sky colour so open
areas stay dark rather than bright.  The rgen accumulates this as the
GI contribution for that sample.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

#include "gi_payload.glsl"
layout(location = 0) rayPayloadInEXT GIPayload giPayload;

void main()
{
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    float up = dir.y * 0.5 + 0.5; // remap [-1,1] -> [0,1]

    // Very dim ambient: open sky contributes almost nothing to GI.
    // Tweak if outdoor areas need a sky ambient boost.
    giPayload.colour = mix(vec3(0.005), vec3(0.015), max(0.0, up));
}
