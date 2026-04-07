/*
===========================================================================

dhewm3-rt Vulkan — glass_probe.rchit — closest-hit shader for glass probe ray.

The probe is fired from the camera with gl_RayFlagsCullOpaqueEXT so only
non-opaque (translucent) geometry is tested.  This shader checks whether
the hit is actually a glass surface (MAT_FLAG_GLASS).

  MAT_FLAG_GLASS set:   record world-space hit position and vertex-
                        interpolated normal in glassProbe payload (hitT > 0).
  Not glass:            signal miss (hitT = 0) so the rgen falls through to
                        the depth-reconstructed surface normal.

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
#include "glass_probe_payload.glsl"

layout(location = 1) rayPayloadInEXT GlassProbePayload glassProbe;
hitAttributeEXT vec2 baryCoord;

// Normal offset within the 15-float idDrawVert stride.
#define RT_VTX_NORMAL_OFF 5u

void main()
{
    uint matIdx = uint(gl_InstanceCustomIndexEXT) + uint(gl_GeometryIndexEXT);

    // Not a valid or not a glass surface — signal miss and let the rgen
    // fall back to the depth-reconstructed surface.
    if (matIdx >= uint(materials.length()) ||
        (materials[matIdx].flags & MAT_FLAG_GLASS) == 0u)
    {
        glassProbe.hitT = 0.0;
        return;
    }

    // --- Glass surface found ---
    glassProbe.hitT   = gl_HitTEXT;
    glassProbe.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

    // Barycentrically interpolate vertex normals from the hit triangle.
    MaterialEntry mat = materials[matIdx];
    // geomSlot == matIdx because each entry's baseGeomIdx equals its own index.
    uint geomSlot     = mat.baseGeomIdx;
    uint64_t idxAddr  = idxAddrs[geomSlot];
    uint64_t vtxAddr  = vtxAddrs[geomSlot];

    vec3 objNormal = vec3(0.0, 1.0, 0.0); // safe fallback
    if (idxAddr != 0ul && vtxAddr != 0ul)
    {
        IdxBuf iBuf = IdxBuf(idxAddr);
        VtxBuf vBuf = VtxBuf(vtxAddr);

        uint base = uint(gl_PrimitiveID) * 3u;
        uint i0 = iBuf.idx[base + 0u];
        uint i1 = iBuf.idx[base + 1u];
        uint i2 = iBuf.idx[base + 2u];

        vec3 n0 = vec3(vBuf.data[i0 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF     ],
                       vBuf.data[i0 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF + 1u],
                       vBuf.data[i0 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF + 2u]);
        vec3 n1 = vec3(vBuf.data[i1 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF     ],
                       vBuf.data[i1 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF + 1u],
                       vBuf.data[i1 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF + 2u]);
        vec3 n2 = vec3(vBuf.data[i2 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF     ],
                       vBuf.data[i2 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF + 1u],
                       vBuf.data[i2 * RT_VTX_STRIDE + RT_VTX_NORMAL_OFF + 2u]);

        float w0 = 1.0 - baryCoord.x - baryCoord.y;
        objNormal = normalize(w0 * n0 + baryCoord.x * n1 + baryCoord.y * n2);
    }

    // Transform object-space normal to world space.
    // mat3(gl_ObjectToWorldEXT) is correct for uniform-scale transforms
    // (Doom 3 static world + standard dynamic entity placements).
    vec3 worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * objNormal);

    // Orient toward the incoming ray origin (camera-side of the glass).
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    glassProbe.hitNormal = worldNormal;
}
