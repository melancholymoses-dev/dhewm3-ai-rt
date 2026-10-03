/*

dhewm3-rt Ambient Occlusion — closest-hit

Returns the nearest opaque hit distance as a fraction of the AO radius, so the raygen can
weight occlusion by distance instead of treating every hit inside the radius as full
occlusion.  See docs/plans/completed/20260930_AO_GI_refine.md.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

*/

#version 460
#extension GL_EXT_ray_tracing : require

// In: the AO radius (set by ao_ray.rgen before the trace).
// Out: hit distance / radius, kept below 1.0 so the raygen can tell a hit from a miss.
layout(location = 0) rayPayloadInEXT float aoPayload;

void main()
{
    aoPayload = min(gl_HitTEXT / max(aoPayload, 1e-3), 0.999);
}
