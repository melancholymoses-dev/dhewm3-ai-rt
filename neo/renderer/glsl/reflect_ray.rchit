/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan — reflect_ray.rchit — closest-hit shader for reflection rays.

Phase 5.4: samples the actual diffuse texture of the hit surface using the
material table (set=1).  UVs are barycentrically interpolated from the hit
triangle's vertex buffer via GL_EXT_buffer_reference.

Phase 5.4b: glass branch.  Translucent surfaces (MC_TRANSLUCENT, flagged
MAT_FLAG_GLASS) get a flat 4 % reflectance (F0 for real glass at normal
incidence).  The remaining 96 % is passed through to the next bounce via
reflPayload.transmittance + nextOrigin/nextDir.  rgen traces the continuation
ray on the next loop iteration.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing                              : require
#extension GL_EXT_buffer_reference                         : require
#extension GL_EXT_buffer_reference2                        : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64   : require
#extension GL_EXT_nonuniform_qualifier                     : enable

#include "rt_material.glsl"
#include "reflect_payload.glsl"

layout(location = 0) rayPayloadInEXT ReflPayload reflPayload;

// Barycentric coordinates set by the built-in triangle intersection stage.
// baryCoord.x = weight of vertex 1, baryCoord.y = weight of vertex 2.
// Weight of vertex 0 = 1.0 - baryCoord.x - baryCoord.y.
hitAttributeEXT vec2 baryCoord;

void main()
{
    // matIdx = instanceCustomIndex (== baseGeomIdx of instance) + geometry index.
    // Each BLAS geometry has its own VkMaterialEntry so this gives the correct
    // per-surface texture, flags, etc.
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);

    // Guard: out-of-range custom index → return black with no continuation.
    if (matIdx >= uint(materials.length()))
    {
        reflPayload.colour        = vec3(0.0);
        reflPayload.transmittance = 0.0;
        return;
    }

    MaterialEntry mat = materials[matIdx];

    if ((mat.flags & MAT_FLAG_GLASS) != 0u)
    {
        // Thin-glass approximation: flat F0 = 0.04 (4 % reflectance at all angles).
        // The reflected colour is tinted by the glass diffuse texture.
        // The remaining 96 % continues straight through (no refraction).
        const float F0 = 0.3;
        const float transmit = 1.0 - F0;

        vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);

        reflPayload.colour        = F0 * diffuse.rgb;
        reflPayload.transmittance = transmit;
        // Continuation ray: start just past the glass surface, same direction.
        reflPayload.nextOrigin = gl_WorldRayOriginEXT
                               + gl_WorldRayDirectionEXT * gl_HitTEXT
                               + gl_WorldRayDirectionEXT * 0.01;
        reflPayload.nextDir = gl_WorldRayDirectionEXT;
        return;
    }

    // Opaque surface — sample diffuse, stop here.
    vec4 diffuse = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
    reflPayload.colour        = diffuse.rgb;
    reflPayload.transmittance = 0.0;
}
