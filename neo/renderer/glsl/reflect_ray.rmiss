/*
===========================================================================
reflect_ray.rmiss — miss shader for reflection rays.

Ray missed all geometry: the reflected direction points toward open space
(sky, void, or far distance).  Return a sky gradient colour and set
transmittance = 0 so the rgen bounce loop stops here.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

#include "reflect_payload.glsl"
layout(location = 0) rayPayloadInEXT ReflPayload reflPayload;

void main()
{
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    float up = dir.y * 0.5 + 0.5;   // remap [-1,1] → [0,1]

    // Return a very dark ambient colour for missed rays.
    // The TLAS only contains geometry visible in the current frustum+PVS, so
    // rays that escape the scene (behind the player, outside the loaded area)
    // hit nothing and reach here.  Using the Martian sky colour caused vivid
    // orange halos on indoor surfaces reflected in glass.  A near-black answer
    // is visually correct: distant void == dark.  The 'up' term adds a tiny
    // directional gradient so pure-downward misses aren't identical to pure-up.
    vec3 color = mix(vec3(0.01), vec3(0.03), max(0.0, up));

    reflPayload.colour        = color;
    reflPayload.transmittance = 0.0;  // stop — sky has no continuation
}
