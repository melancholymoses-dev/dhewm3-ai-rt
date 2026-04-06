/*

dhewm3-rt - Ambient Occlusion

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

*/

#version 460
#extension GL_EXT_ray_tracing : require

// ao_ray.rmiss — AO miss shader
// No geometry was hit within aoRadius: the direction is unoccluded.
layout(location = 0) rayPayloadInEXT float aoPayload;

void main()
{
    aoPayload = 1.0; // unoccluded
}
