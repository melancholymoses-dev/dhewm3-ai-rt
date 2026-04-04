/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan — reflect_ray.rchit — closest-hit shader for reflection rays.

Phase 5.4: samples the actual diffuse texture of the hit surface using the
material table (set=1).  UVs are barycentrically interpolated from the hit
triangle's vertex buffer via GL_EXT_buffer_reference.

This replaces the Phase 5.3 direction-tint approximation.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing                              : require
#extension GL_EXT_buffer_reference                         : require
#extension GL_EXT_buffer_reference2                        : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64   : require
#extension GL_EXT_nonuniform_qualifier                     : enable

#include "rt_material.glsl"

layout(location = 0) rayPayloadInEXT vec4 reflPayload;

// Barycentric coordinates set by the built-in triangle intersection stage.
// baryCoord.x = weight of vertex 1, baryCoord.y = weight of vertex 2.
// Weight of vertex 0 = 1.0 - baryCoord.x - baryCoord.y.
hitAttributeEXT vec2 baryCoord;

void main()
{
    uint matIdx = uint(gl_InstanceCustomIndexEXT);
    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
    reflPayload = vec4(diffuse.rgb, 1.0);
}
