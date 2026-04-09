/* rt_material.glsl — shared material table include for dhewm3 RT shaders.
 
  Provides:
    - MaterialEntry struct (mirrors VkMaterialEntry in vk_raytracing.h)
    - set=1 SSBO bindings: MatTable, VtxAddrTable, IdxAddrTable
    - set=1 bindless sampler array: matTextures[4096]
    - rt_InterpolateUV(matIdx, primId, bary)  — barycentrically-interpolated UV
    - rt_SampleDiffuse(matIdx, primId, bary)  — diffuse texel at hit surface
 
  Required extensions (include before this file):
    #extension GL_EXT_buffer_reference                        : require
    #extension GL_EXT_buffer_reference2                       : require
    #extension GL_EXT_shader_explicit_arithmetic_types_int64  : require
    #extension GL_EXT_nonuniform_qualifier                    : enable

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/


// ---------------------------------------------------------------------------
// Per-instance material entry — must match VkMaterialEntry (vk_raytracing.h)
// std430: all fields 4 bytes, total 32 bytes.
// ---------------------------------------------------------------------------
struct MaterialEntry {
    uint  diffuseTexIndex;  // index into matTextures[] (0 = white fallback)
    uint  normalTexIndex;   // index into matTextures[] (1 = flat normal fallback)
    float roughness;        // GGX roughness [0,1]
    uint  flags;            // MAT_FLAG_*
    uint  baseGeomIdx;      // offset into per-geometry VtxAddrTable/IdxAddrTable
    float alphaThreshold;   // alpha discard threshold (MAT_FLAG_ALPHA_TESTED)
    uint  pad0;
    uint  pad1;
};

#define MAT_FLAG_ALPHA_TESTED  0x01u
#define MAT_FLAG_TWO_SIDED     0x02u
#define MAT_FLAG_GLASS         0x04u  // MC_TRANSLUCENT — thin glass, F0=0.04
#define MAT_FLAG_PLAYER_BODY   0x08u  // noSelfShadow entity — player/weapon model

// ---------------------------------------------------------------------------
// set=1 bindings
// ---------------------------------------------------------------------------
layout(set = 1, binding = 0, std430) readonly buffer MatTable {
    MaterialEntry materials[];
};

layout(set = 1, binding = 1, std430) readonly buffer VtxAddrTable {
    uint64_t vtxAddrs[];
};

layout(set = 1, binding = 2, std430) readonly buffer IdxAddrTable {
    uint64_t idxAddrs[];
};

// 4096-slot bindless sampler array — slot 0 = white, slot 1 = flat normal.
layout(set = 1, binding = 3) uniform sampler2D matTextures[4096];

// ---------------------------------------------------------------------------
// Buffer-reference types for raw vertex / index access
// ---------------------------------------------------------------------------
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxBuf {
    uint idx[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VtxBuf {
    float data[];
};

// ---------------------------------------------------------------------------
// idDrawVert memory layout (float view, all floats are 4 bytes):
//
//   floats [0-2]  : xyz          (idVec3, 12 bytes)
//   floats [3-4]  : st           (idVec2,  8 bytes)  ← UV here
//   floats [5-7]  : normal       (idVec3, 12 bytes)
//   floats [8-10] : tangents[0]  (idVec3, 12 bytes)
//   floats [11-13]: tangents[1]  (idVec3, 12 bytes)
//   float  [14]   : color[4]     (byte[4] = 4 bytes, viewed as 1 float)
//
//   stride = 15 floats = 60 bytes
// ---------------------------------------------------------------------------
#define RT_VTX_STRIDE 15u
#define RT_VTX_UV_OFF  3u   // float offset of st.x within one vertex

// ---------------------------------------------------------------------------
// rt_InterpolateUV — barycentrically interpolate texture coordinates for the
// hit triangle.  bary = (u, v) from hitAttributeEXT / GL_HIT_BARYCENTRICS.
// ---------------------------------------------------------------------------
vec2 rt_InterpolateUV(uint matIdx, int primId, vec2 bary)
{
    MaterialEntry mat = materials[matIdx];

    // Index into the flat per-geometry address tables using this entry's baseGeomIdx.
    // Since matIdx = gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT and each entry's
    // baseGeomIdx == matIdx (one entry per geometry), this is equivalent to matIdx.
    uint geomSlot    = mat.baseGeomIdx;
    if (geomSlot >= uint(idxAddrs.length()) || geomSlot >= uint(vtxAddrs.length()))
        return vec2(0.0);

    uint64_t idxAddr = idxAddrs[geomSlot];
    uint64_t vtxAddr = vtxAddrs[geomSlot];
    if (idxAddr == 0ul || vtxAddr == 0ul)
        return vec2(0.0);

    IdxBuf iBuf = IdxBuf(idxAddr);
    VtxBuf vBuf = VtxBuf(vtxAddr);

    uint base = uint(primId) * 3u;
    uint i0 = iBuf.idx[base + 0u];
    uint i1 = iBuf.idx[base + 1u];
    uint i2 = iBuf.idx[base + 2u];

    vec2 uv0 = vec2(vBuf.data[i0 * RT_VTX_STRIDE + RT_VTX_UV_OFF],
                    vBuf.data[i0 * RT_VTX_STRIDE + RT_VTX_UV_OFF + 1u]);
    vec2 uv1 = vec2(vBuf.data[i1 * RT_VTX_STRIDE + RT_VTX_UV_OFF],
                    vBuf.data[i1 * RT_VTX_STRIDE + RT_VTX_UV_OFF + 1u]);
    vec2 uv2 = vec2(vBuf.data[i2 * RT_VTX_STRIDE + RT_VTX_UV_OFF],
                    vBuf.data[i2 * RT_VTX_STRIDE + RT_VTX_UV_OFF + 1u]);

    float w0 = 1.0 - bary.x - bary.y;
    return w0 * uv0 + bary.x * uv1 + bary.y * uv2;
}

// ---------------------------------------------------------------------------
// rt_SampleDiffuse — sample the diffuse texture at the hit surface.
// Returns rgba; rgb used for colour, a used for alpha testing.
// ---------------------------------------------------------------------------
vec4 rt_SampleDiffuse(uint matIdx, int primId, vec2 bary)
{
    vec2 uv = rt_InterpolateUV(matIdx, primId, bary);
    uint texIdx = materials[matIdx].diffuseTexIndex;
    if (texIdx >= 4096u)
        texIdx = 0u;
    return texture(matTextures[nonuniformEXT(texIdx)], uv);
}
