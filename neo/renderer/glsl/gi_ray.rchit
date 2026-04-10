/*
===========================================================================

dhewm3-rt Vulkan — gi_ray.rchit — closest-hit shader for GI bounce rays.

Phase 6.1 (Option A — Ambient-only):
  Samples the real diffuse albedo of the hit surface using the material
  table (set=1).  UV coordinates are barycentrically interpolated from
  the hit triangle's vertex buffer via GL_EXT_buffer_reference.

  Returns the raw albedo to the rgen for scaling by giStrength.
  No shadow ray is fired (Option A).  Colour bleeding and contact
  brightening come from the albedo of directly-hit surfaces.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing                              : require
#extension GL_EXT_buffer_reference                         : require
#extension GL_EXT_buffer_reference2                        : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64   : require
#extension GL_EXT_nonuniform_qualifier                     : enable

#include "rt_material.glsl"
#include "gi_payload.glsl"

layout(location = 0) rayPayloadInEXT GIPayload giPayload;
hitAttributeEXT vec2 baryCoord;

void main()
{
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);

    if (matIdx >= uint(materials.length()))
    {
        giPayload.colour = vec3(0.0);
        return;
    }

    // Sample the diffuse albedo of the hit surface.
    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
    giPayload.colour = diffuse.rgb;
}
