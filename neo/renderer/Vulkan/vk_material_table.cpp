/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan ray tracing — material table (Phase 5.4).

Builds and maintains three persistently-mapped SSBOs and one bindless
combined-image-sampler array that are bound at set=2 in all RT shaders
that need to look up hit-surface material properties.

  set=2, binding=0  — VkMaterialEntry[]   (material SSBO)
  set=2, binding=1  — uint64_t[]          (vertex buffer device addresses)
  set=2, binding=2  — uint64_t[]          (index  buffer device addresses)
  set=2, binding=3  — sampler2D textures[] (bindless, VK_MAT_MAX_TEXTURES slots)

The material SSBO has one entry per TLAS instance, indexed by
gl_InstanceCustomIndexEXT.  It is rebuilt every frame alongside the TLAS:
  - static entries are written only when the static instance set changes
    (mirrors the rewriteStatic flag in VK_RT_RebuildTLAS)
  - dynamic entries are written every frame

The bindless texture array is initialised with the white/flat-normal fallback
images.  When VK_RT_MakeMaterialEntry() encounters a diffuse or normal image
it has not seen before, the slot is assigned and the dirty flag is set.
A rebuild of the bindless descriptors (vkUpdateDescriptorSets) is issued at
the end of VK_RT_UploadMatTableFrame when the dirty flag is set.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Image.h"
#include "renderer/Material.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_image.h"
#include "framework/DeclManager.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// Local constants
// ---------------------------------------------------------------------------

// Matches VK_RT_MAX_TLAS_INSTANCES in vk_accelstruct.cpp
static const uint32_t MAT_MAX_INSTANCES = 4096;

// ---------------------------------------------------------------------------
// Bindless texture tracking
//
// s_bindlessImages[i] holds the idImage* assigned to slot i.
// Index 0 is always the white fallback (diffuse) or flat-normal (normal).
// When a new image is first encountered, it gets the next free slot.
// s_bindlessDirty is set whenever a new slot is assigned.
// ---------------------------------------------------------------------------

static idImage  *s_bindlessImages[VK_MAT_MAX_TEXTURES];
static uint32_t  s_bindlessCount = 0;
static bool      s_bindlessDirty = false;

// Returns the bindless slot index for img, assigning a new one if needed.
// img == NULL returns slot 0 (white/flat-normal fallback, slot 0 is reserved).
static uint32_t GetOrAssignTexIndex(idImage *img)
{
    if (!img)
        return 0;

    for (uint32_t i = 0; i < s_bindlessCount; i++)
    {
        if (s_bindlessImages[i] == img)
            return i;
    }

    if (s_bindlessCount >= VK_MAT_MAX_TEXTURES)
    {
        static int s_overflowWarned = 0;
        if (!s_overflowWarned)
        {
            common->Warning("VK RT MatTable: bindless texture array full (%u slots). "
                            "Extra images will use slot 0 (white fallback).",
                            VK_MAT_MAX_TEXTURES);
            s_overflowWarned = 1;
        }
        return 0;
    }

    uint32_t idx = s_bindlessCount++;
    s_bindlessImages[idx] = img;
    s_bindlessDirty = true;
    return idx;
}

// ---------------------------------------------------------------------------
// RebuildBindlessDescriptors
//
// Writes all assigned bindless slots into vkRT.matDescSet binding=3.
// Uses the image's own VkImageView with vkRT.matSampler.
// Unfilled slots (beyond s_bindlessCount) keep their initial fallback value.
// Called when s_bindlessDirty is set.
// ---------------------------------------------------------------------------

static void RebuildBindlessDescriptors(void)
{
    if (!vkRT.matTableInitialized || s_bindlessCount == 0)
        return;

    // Build one VkDescriptorImageInfo per assigned slot.
    // Stack-allocate for the expected common case; heap for overflow.
    static VkDescriptorImageInfo infos[VK_MAT_MAX_TEXTURES];

    // White fallback info — used for any slot whose image has no Vulkan data yet.
    VkDescriptorImageInfo fallbackInfo = {};
    VK_Image_GetDescriptorInfo(globalImages->whiteImage, &fallbackInfo);
    fallbackInfo.sampler     = vkRT.matSampler;
    fallbackInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    for (uint32_t i = 0; i < s_bindlessCount; i++)
    {
        VkDescriptorImageInfo info = fallbackInfo;
        if (s_bindlessImages[i])
        {
            VkDescriptorImageInfo tmp = {};
            if (VK_Image_GetDescriptorInfo(s_bindlessImages[i], &tmp))
            {
                info.imageView   = tmp.imageView;
                info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        info.sampler = vkRT.matSampler; // always use our bilinear sampler
        infos[i] = info;
    }

    VkWriteDescriptorSet write = {};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = vkRT.matDescSet;
    write.dstBinding      = 3;
    write.dstArrayElement = 0;
    write.descriptorCount = s_bindlessCount;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = infos;

    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    if (r_vkLogRT.GetInteger() >= 2)
    {
        common->Printf("VK RT MatTable: rebuilt bindless descriptors — %u textures\n",
                       s_bindlessCount);
        fflush(NULL);
    }

    s_bindlessDirty = false;
}

// ---------------------------------------------------------------------------
// VK_RT_InitMaterialTable
// ---------------------------------------------------------------------------

void VK_RT_InitMaterialTable(void)
{
    if (vkRT.matTableInitialized)
        return;

    // --- Persistent SSBOs ---

    const VkDeviceSize matSSBOSize  = (VkDeviceSize)MAT_MAX_INSTANCES * sizeof(VkMaterialEntry);
    const VkDeviceSize addrSSBOSize = (VkDeviceSize)MAT_MAX_INSTANCES * sizeof(uint64_t);

    const VkBufferUsageFlags ssboUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkMemoryPropertyFlags hostFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VK_CreateBuffer(matSSBOSize,  ssboUsage, hostFlags,
                    &vkRT.matTableSSBO, &vkRT.matTableSSBOMemory);
    VK_CHECK(vkMapMemory(vk.device, vkRT.matTableSSBOMemory, 0,
                         matSSBOSize, 0, &vkRT.matTableMapped));

    VK_CreateBuffer(addrSSBOSize, ssboUsage, hostFlags,
                    &vkRT.vtxAddrSSBO, &vkRT.vtxAddrSSBOMemory);
    VK_CHECK(vkMapMemory(vk.device, vkRT.vtxAddrSSBOMemory, 0,
                         addrSSBOSize, 0, &vkRT.vtxAddrMapped));

    VK_CreateBuffer(addrSSBOSize, ssboUsage, hostFlags,
                    &vkRT.idxAddrSSBO, &vkRT.idxAddrSSBOMemory);
    VK_CHECK(vkMapMemory(vk.device, vkRT.idxAddrSSBOMemory, 0,
                         addrSSBOSize, 0, &vkRT.idxAddrMapped));

    // Zero-initialise so GPU reads zeros for uninitialised instances.
    memset(vkRT.matTableMapped, 0, (size_t)matSSBOSize);
    memset(vkRT.vtxAddrMapped,  0, (size_t)addrSSBOSize);
    memset(vkRT.idxAddrMapped,  0, (size_t)addrSSBOSize);

    // --- Bilinear sampler for material textures ---

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter        = VK_FILTER_LINEAR;
    sampInfo.minFilter        = VK_FILTER_LINEAR;
    sampInfo.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.minLod           = 0.0f;
    sampInfo.maxLod           = VK_LOD_CLAMP_NONE;
    sampInfo.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(vk.device, &sampInfo, NULL, &vkRT.matSampler));

    // --- Descriptor set layout: set=2 ---
    // binding=0  STORAGE_BUFFER   — MaterialTable
    // binding=1  STORAGE_BUFFER   — VtxAddrTable
    // binding=2  STORAGE_BUFFER   — IdxAddrTable
    // binding=3  COMBINED_IMAGE_SAMPLER [VK_MAT_MAX_TEXTURES] — bindless textures

    VkDescriptorSetLayoutBinding bindings[4] = {};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                   VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                   VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = bindings[0].stageFlags;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = bindings[0].stageFlags;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = VK_MAT_MAX_TEXTURES;
    bindings[3].stageFlags      = bindings[0].stageFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL,
                                         &vkRT.matDescLayout));

    // --- Descriptor pool ---

    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          3 }, // 3 SSBOs
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  VK_MAT_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes    = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL,
                                    &vkRT.matDescPool));

    // --- Allocate descriptor set ---

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = vkRT.matDescPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &vkRT.matDescLayout;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, &vkRT.matDescSet));

    // --- Write SSBO descriptors (static — buffers never change, only content) ---

    VkDescriptorBufferInfo matBufInfo = {};
    matBufInfo.buffer = vkRT.matTableSSBO;
    matBufInfo.offset = 0;
    matBufInfo.range  = matSSBOSize;

    VkDescriptorBufferInfo vtxBufInfo = {};
    vtxBufInfo.buffer = vkRT.vtxAddrSSBO;
    vtxBufInfo.offset = 0;
    vtxBufInfo.range  = addrSSBOSize;

    VkDescriptorBufferInfo idxBufInfo = {};
    idxBufInfo.buffer = vkRT.idxAddrSSBO;
    idxBufInfo.offset = 0;
    idxBufInfo.range  = addrSSBOSize;

    VkWriteDescriptorSet ssboWrites[3] = {};
    ssboWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssboWrites[0].dstSet          = vkRT.matDescSet;
    ssboWrites[0].dstBinding      = 0;
    ssboWrites[0].descriptorCount = 1;
    ssboWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssboWrites[0].pBufferInfo     = &matBufInfo;

    ssboWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssboWrites[1].dstSet          = vkRT.matDescSet;
    ssboWrites[1].dstBinding      = 1;
    ssboWrites[1].descriptorCount = 1;
    ssboWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssboWrites[1].pBufferInfo     = &vtxBufInfo;

    ssboWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ssboWrites[2].dstSet          = vkRT.matDescSet;
    ssboWrites[2].dstBinding      = 2;
    ssboWrites[2].descriptorCount = 1;
    ssboWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssboWrites[2].pBufferInfo     = &idxBufInfo;

    vkUpdateDescriptorSets(vk.device, 3, ssboWrites, 0, NULL);

    // --- Seed bindless slot 0 with fallback images ---
    // Slot 0 = white (diffuse fallback), slot 1 = flat normal fallback.

    s_bindlessCount = 0;
    s_bindlessDirty = false;
    memset(s_bindlessImages, 0, sizeof(s_bindlessImages));

    GetOrAssignTexIndex(globalImages->whiteImage);    // slot 0 — diffuse fallback
    GetOrAssignTexIndex(globalImages->flatNormalMap); // slot 1 — normal fallback
    // s_bindlessDirty is now true; will be flushed on first UploadMatTableFrame.

    vkRT.matTableInitialized = true;

    common->Printf("VK RT MatTable: initialised — matSSBO=%u KB, addrSSBO=%u KB, "
                   "bindlessSlots=%u\n",
                   (unsigned)(matSSBOSize / 1024),
                   (unsigned)(addrSSBOSize / 1024),
                   VK_MAT_MAX_TEXTURES);
}

// ---------------------------------------------------------------------------
// VK_RT_ShutdownMaterialTable
// ---------------------------------------------------------------------------

void VK_RT_ShutdownMaterialTable(void)
{
    if (!vkRT.matTableInitialized)
        return;

    if (vkRT.matTableMapped)
    {
        vkUnmapMemory(vk.device, vkRT.matTableSSBOMemory);
        vkRT.matTableMapped = NULL;
    }
    if (vkRT.vtxAddrMapped)
    {
        vkUnmapMemory(vk.device, vkRT.vtxAddrSSBOMemory);
        vkRT.vtxAddrMapped = NULL;
    }
    if (vkRT.idxAddrMapped)
    {
        vkUnmapMemory(vk.device, vkRT.idxAddrSSBOMemory);
        vkRT.idxAddrMapped = NULL;
    }

    if (vkRT.matTableSSBO != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.matTableSSBO, NULL);
        vkRT.matTableSSBO = VK_NULL_HANDLE;
    }
    if (vkRT.matTableSSBOMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.matTableSSBOMemory, NULL);
        vkRT.matTableSSBOMemory = VK_NULL_HANDLE;
    }

    if (vkRT.vtxAddrSSBO != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.vtxAddrSSBO, NULL);
        vkRT.vtxAddrSSBO = VK_NULL_HANDLE;
    }
    if (vkRT.vtxAddrSSBOMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.vtxAddrSSBOMemory, NULL);
        vkRT.vtxAddrSSBOMemory = VK_NULL_HANDLE;
    }

    if (vkRT.idxAddrSSBO != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.idxAddrSSBO, NULL);
        vkRT.idxAddrSSBO = VK_NULL_HANDLE;
    }
    if (vkRT.idxAddrSSBOMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.idxAddrSSBOMemory, NULL);
        vkRT.idxAddrSSBOMemory = VK_NULL_HANDLE;
    }

    if (vkRT.matSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.matSampler, NULL);
        vkRT.matSampler = VK_NULL_HANDLE;
    }

    if (vkRT.matDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.matDescPool, NULL);
        vkRT.matDescPool    = VK_NULL_HANDLE;
        vkRT.matDescSet     = VK_NULL_HANDLE;
    }

    if (vkRT.matDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.matDescLayout, NULL);
        vkRT.matDescLayout = VK_NULL_HANDLE;
    }

    s_bindlessCount = 0;
    s_bindlessDirty = false;
    memset(s_bindlessImages, 0, sizeof(s_bindlessImages));

    vkRT.matTableInitialized = false;
}

// ---------------------------------------------------------------------------
// VK_RT_MakeMaterialEntry
//
// Converts an idMaterial + vkBLAS_t into a VkMaterialEntry ready for upload
// to the material SSBO.  Also writes the surface-0 geometry device addresses
// to *outVtxAddr / *outIdxAddr for the VtxAddrTable / IdxAddrTable SSBOs.
//
// shader may be NULL — all fields get safe defaults.
// blas   may be NULL — vtx/idx addresses are set to 0.
// ---------------------------------------------------------------------------

VkMaterialEntry VK_RT_MakeMaterialEntry(const idMaterial *shader, const vkBLAS_t *blas,
                                         uint32_t instanceIndex,
                                         uint64_t *outVtxAddr, uint64_t *outIdxAddr)
{
    VkMaterialEntry entry = {};
    entry.diffuseTexIndex = 0; // white fallback
    entry.normalTexIndex  = 1; // flat normal fallback
    entry.roughness       = 1.0f;
    entry.flags           = 0;
    entry.vtxBufInstance  = instanceIndex;
    entry.idxBufInstance  = instanceIndex;
    entry.alphaThreshold  = 0.5f;
    entry.pad             = 0;

    // --- Geometry addresses ---

    if (blas && blas->geomCount > 0 && blas->geomVertAddrs && blas->geomIdxAddrs)
    {
        *outVtxAddr = (uint64_t)blas->geomVertAddrs[0];
        *outIdxAddr = (uint64_t)blas->geomIdxAddrs[0];
    }
    else
    {
        *outVtxAddr = 0;
        *outIdxAddr = 0;
    }

    if (!shader)
        return entry;

    // --- Coverage flags ---

    if (shader->Coverage() == MC_PERFORATED)
        entry.flags |= VK_MAT_FLAG_ALPHA_TESTED;

    if (shader->GetCullType() == CT_TWO_SIDED)
        entry.flags |= VK_MAT_FLAG_TWO_SIDED;

    // --- Walk stages for diffuse and normal images ---

    for (int i = 0; i < shader->GetNumStages(); i++)
    {
        const shaderStage_t *stage = shader->GetStage(i);
        if (!stage)
            continue;

        // Skip stages with no image (e.g. vertex-program-only stages).
        idImage *img = stage->texture.image;
        if (!img)
            continue;

        if (stage->lighting == SL_DIFFUSE)
            entry.diffuseTexIndex = GetOrAssignTexIndex(img);
        else if (stage->lighting == SL_BUMP)
            entry.normalTexIndex  = GetOrAssignTexIndex(img);
        // SL_SPECULAR: no scalar exponent available in Doom3 material API.
        // Roughness stays at 1.0 (fully diffuse) — revisit in Phase 6.
    }

    // Alpha threshold: default 0.5 for all MC_PERFORATED materials.
    // Doom3 materials don't expose the evaluated register value at this level.
    if (entry.flags & VK_MAT_FLAG_ALPHA_TESTED)
        entry.alphaThreshold = 0.5f;

    return entry;
}

// ---------------------------------------------------------------------------
// VK_RT_UploadMatTableFrame
//
// Merges static+dynamic material entries into the SSBO (same split pattern
// as the TLAS instance buffer in VK_RT_RebuildTLAS).  Also uploads the
// parallel VtxAddrTable and IdxAddrTable arrays.
//
// If the bindless texture set changed (s_bindlessDirty), rebuilds the
// bindless descriptor writes via vkUpdateDescriptorSets before returning.
// ---------------------------------------------------------------------------

void VK_RT_UploadMatTableFrame(
    const VkMaterialEntry *staticEntries,  uint32_t staticCount,  bool rewriteStatic,
    const VkMaterialEntry *dynamicEntries, uint32_t dynamicCount,
    const uint64_t *staticVtx,  const uint64_t *staticIdx,
    const uint64_t *dynamicVtx, const uint64_t *dynamicIdx)
{
    if (!vkRT.matTableInitialized)
        return;

    const size_t matEntrySize = sizeof(VkMaterialEntry);
    const size_t addrSize     = sizeof(uint64_t);

    uint8_t *matDst = (uint8_t *)vkRT.matTableMapped;
    uint8_t *vtxDst = (uint8_t *)vkRT.vtxAddrMapped;
    uint8_t *idxDst = (uint8_t *)vkRT.idxAddrMapped;

    // Static block
    if (rewriteStatic && staticCount > 0)
    {
        memcpy(matDst, staticEntries, staticCount * matEntrySize);
        memcpy(vtxDst, staticVtx,     staticCount * addrSize);
        memcpy(idxDst, staticIdx,      staticCount * addrSize);
    }

    // Dynamic block (always written, immediately after the static block)
    if (dynamicCount > 0)
    {
        size_t dynamicOffset = staticCount * matEntrySize;
        memcpy(matDst + dynamicOffset, dynamicEntries, dynamicCount * matEntrySize);
        dynamicOffset = staticCount * addrSize;
        memcpy(vtxDst + dynamicOffset, dynamicVtx, dynamicCount * addrSize);
        memcpy(idxDst + dynamicOffset, dynamicIdx, dynamicCount * addrSize);
    }

    // Rebuild bindless descriptor array if new images were encountered.
    if (s_bindlessDirty)
        RebuildBindlessDescriptors();

    if (r_vkLogRT.GetInteger() >= 2)
    {
        common->Printf("VK RT MatTable: uploaded — static=%u%s dynamic=%u bindless=%u\n",
                       staticCount, rewriteStatic ? "(rewritten)" : "(cached)",
                       dynamicCount, s_bindlessCount);
        fflush(NULL);
    }
}
