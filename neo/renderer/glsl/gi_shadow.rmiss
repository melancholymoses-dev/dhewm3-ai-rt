/*
===========================================================================

dhewm3-rt Vulkan — gi_shadow.rmiss — shadow miss shader for GI Option B.

Shadow rays are fired from secondary hit points toward each scene light.
If the ray misses all geometry, this shader fires and clears occluded to
false, meaning the light is visible from that point.

If the ray hits any opaque geometry before reaching the light,
gl_RayFlagsTerminateOnFirstHitEXT stops traversal without invoking any
closest-hit shader, leaving occluded = true (the payload default).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

#include "gi_shadow_payload.glsl"
layout(location = 1) rayPayloadInEXT GIShadowPayload giShadow;

void main()
{
    // Ray reached the light without hitting anything — not occluded.
    giShadow.occluded = false;
}
