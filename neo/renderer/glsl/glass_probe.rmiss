/*
===========================================================================

dhewm3-rt Vulkan — glass_probe.rmiss — miss shader for glass probe ray.

No glass surface was found between the camera and the depth-buffer
surface.  Signal "no glass" by leaving hitT at 0.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

#include "glass_probe_payload.glsl"
layout(location = 1) rayPayloadInEXT GlassProbePayload glassProbe;

void main()
{
    glassProbe.hitT    = 0.0;
    glassProbe.hitPos  = vec3(0.0);
    glassProbe.hitNormal = vec3(0.0);
}
