/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - main render loop.

Mirrors the structure of tr_backend.cpp / draw_common.cpp for the Vulkan path.
Called from RB_ExecuteBackEndCommands() when r_backend == "vulkan".

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/VertexCache.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_buffer.h"
#include "sys/sys_imgui.h"
#include <SDL.h>

// Forward declarations (defined in vk_pipeline.cpp)
// vkPipelines_t and vkPipes are declared in vk_common.h
void VK_InitPipelines(void);
void VK_ShutdownPipelines(void);

// Forward declarations (defined in vk_swapchain.cpp)
void VK_CreateSwapchain(int width, int height);
void VK_DestroySwapchain(void);
void VK_RecreateSwapchain(int width, int height);

// ---------------------------------------------------------------------------
// Per-frame uniform buffer ring
// Pre-allocated pool of UBO memory for interaction parameters.
// ---------------------------------------------------------------------------

static const uint32_t VK_UBO_RING_SIZE = 4096; // max interactions per frame

// ---------------------------------------------------------------------------
// Per-frame data staging ring
// Host-visible linear allocator for uploading CPU-side vertex/index data.
// Used when geo->indexCache is NULL (r_useIndexBuffers=0, which is the default)
// and for TAG_TEMP vertex data (GUI surfaces etc.).
// ---------------------------------------------------------------------------

static const VkDeviceSize VK_DATA_RING_SIZE = 32 * 1024 * 1024; // 32 MB per frame

struct vkDataRing_t
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize offset; // current linear allocation offset
};

static vkDataRing_t dataRings[VK_MAX_FRAMES_IN_FLIGHT];

static void VK_CreateDataRings(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_CreateBuffer(VK_DATA_RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &dataRings[i].buffer, &dataRings[i].memory);
        VK_CHECK(vkMapMemory(vk.device, dataRings[i].memory, 0, VK_DATA_RING_SIZE, 0, &dataRings[i].mapped));
        dataRings[i].offset = 0;
    }
}

static void VK_DestroyDataRings(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (dataRings[i].memory)
            vkUnmapMemory(vk.device, dataRings[i].memory);
        if (dataRings[i].buffer)
            vkDestroyBuffer(vk.device, dataRings[i].buffer, NULL);
        if (dataRings[i].memory)
            vkFreeMemory(vk.device, dataRings[i].memory, NULL);
    }
    memset(dataRings, 0, sizeof(dataRings));
}

// Allocate 'size' bytes from the current frame's data ring, aligned to 'align'.
// Returns the byte offset into dataRings[vk.currentFrame].buffer, or VK_WHOLE_SIZE on overflow.
static VkDeviceSize VK_AllocDataRing(VkDeviceSize size, VkDeviceSize align)
{
    vkDataRing_t &ring = dataRings[vk.currentFrame];
    // round offset up to alignment
    VkDeviceSize aligned = (ring.offset + align - 1) & ~(align - 1);
    if (aligned + size > VK_DATA_RING_SIZE)
    {
        common->Warning("VK: data ring overflow (need %llu bytes)", (unsigned long long)(aligned + size));
        return VK_WHOLE_SIZE;
    }
    ring.offset = aligned + size;
    return aligned;
}

struct vkUBORing_t
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    uint32_t offset; // current allocation offset in bytes
    uint32_t stride; // aligned stride per UBO entry
};

static vkUBORing_t uboRings[VK_MAX_FRAMES_IN_FLIGHT];

// Vulkan-corrected projection matrix for the current view.
// Computed at the start of each VK_RB_DrawView call.
// Z depth remap: OpenGL NDC z [-1,1] -> Vulkan NDC z [0,1]:
//   new_row2[c] = 0.5 * old_row2[c] + 0.5 * old_row3[c]  (column-major indexing)
// Y flip is handled via negative viewport height, not here.
static float s_projVk[16];

// Interaction UBO size (must match VkInteractionUBO in vk_pipeline.cpp).
// Struct breakdown: 14 vec4s (224) + MVP mat4 (64) + 3 vec4s (48) + applyGamma/pad (16)
// + screenSize vec2 + useShadowMask int + pad = 356 bytes -> round to 384.
static const uint32_t INTERACTION_UBO_SIZE = 384;

static void VK_CreateUBORings(void)
{
    VkPhysicalDeviceProperties devProps;
    vkGetPhysicalDeviceProperties(vk.physicalDevice, &devProps);
    uint32_t align = (uint32_t)devProps.limits.minUniformBufferOffsetAlignment;

    // Round INTERACTION_UBO_SIZE up to required alignment
    uint32_t stride = (INTERACTION_UBO_SIZE + align - 1) & ~(align - 1);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkDeviceSize size = (VkDeviceSize)stride * VK_UBO_RING_SIZE;
        VK_CreateBuffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uboRings[i].buffer,
                        &uboRings[i].memory);
        VK_CHECK(vkMapMemory(vk.device, uboRings[i].memory, 0, size, 0, &uboRings[i].mapped));
        uboRings[i].stride = stride;
        uboRings[i].offset = 0;
    }
}

static void VK_DestroyUBORings(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (uboRings[i].memory)
            vkUnmapMemory(vk.device, uboRings[i].memory);
        if (uboRings[i].buffer)
            vkDestroyBuffer(vk.device, uboRings[i].buffer, NULL);
        if (uboRings[i].memory)
            vkFreeMemory(vk.device, uboRings[i].memory, NULL);
    }
    memset(uboRings, 0, sizeof(uboRings));
}

// Allocate space in the UBO ring and return the byte offset
static uint32_t VK_AllocUBO(void)
{
    vkUBORing_t &ring = uboRings[vk.currentFrame];
    uint32_t off = ring.offset;
    ring.offset += ring.stride;
    if (ring.offset >= ring.stride * VK_UBO_RING_SIZE)
    {
        common->Warning("VK: UBO ring overflowed (>%u interactions in one frame)", VK_UBO_RING_SIZE);
        ring.offset = 0;
    }
    return off;
}

// ---------------------------------------------------------------------------
// Matrix helpers
// ---------------------------------------------------------------------------

static void VK_MultiplyMatrix4(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            float sum = 0.f;
            for (int k = 0; k < 4; k++)
            {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
}

// ---------------------------------------------------------------------------
// VK_RB_DrawInteraction - record draw commands for one light-surface interaction
// Called from VK_RB_DrawInteractions via RB_CreateSingleDrawInteractions callback.
// ---------------------------------------------------------------------------

// We use a file-static to pass the command buffer through the callback
static VkCommandBuffer s_cmd = VK_NULL_HANDLE;

static void VK_RB_DrawInteraction(const drawInteraction_t *din)
{
    vkUBORing_t &ring = uboRings[vk.currentFrame];

    // Allocate UBO slot
    uint32_t uboOffset = VK_AllocUBO();
    uint8_t *uboPtr = (uint8_t *)ring.mapped + uboOffset;

    // Fill UBO (layout matches VkInteractionUBO in vk_pipeline.cpp)
    float *f = (float *)uboPtr;
    memcpy(f, din->localLightOrigin.ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->localViewOrigin.ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->lightProjection[0].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->lightProjection[1].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->lightProjection[2].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->lightProjection[3].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->bumpMatrix[0].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->bumpMatrix[1].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->diffuseMatrix[0].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->diffuseMatrix[1].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->specularMatrix[0].ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->specularMatrix[1].ToFloatPtr(), 16);
    f += 4;

    // colorModulate / colorAdd
    static const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1}, neg[4] = {-1, -1, -1, -1};
    const float *cm, *ca;
    switch (din->vertexColor)
    {
    case SVC_IGNORE:
        cm = zero;
        ca = one;
        break;
    case SVC_MODULATE:
        cm = one;
        ca = zero;
        break;
    case SVC_INVERSE_MODULATE:
        cm = neg;
        ca = one;
        break;
    default:
        cm = zero;
        ca = one;
        break;
    }
    memcpy(f, cm, 16);
    f += 4;
    memcpy(f, ca, 16);
    f += 4;

    // MVP matrix
    float mvp[16];
    VK_MultiplyMatrix4(s_projVk, din->surf->space->modelViewMatrix, mvp);
    memcpy(f, mvp, 64);
    f += 16;

    // diffuseColor, specularColor
    memcpy(f, din->diffuseColor.ToFloatPtr(), 16);
    f += 4;
    memcpy(f, din->specularColor.ToFloatPtr(), 16);
    f += 4;

    // gammaBrightness
    float gb[4] = {r_brightness.GetFloat(), r_brightness.GetFloat(), r_brightness.GetFloat(),
                   1.0f / r_gamma.GetFloat()};
    memcpy(f, gb, 16);
    f += 4;

    // applyGamma
    int *ip = (int *)f;
    *ip = r_gammaInShader.GetBool() ? 1 : 0;

    // screenSize (used by shader to compute shadow mask UV from gl_FragCoord)
    float *fsz = (float *)(ip + 1);
    fsz[0] = (float)vk.swapchainExtent.width;
    fsz[1] = (float)vk.swapchainExtent.height;

    // useShadowMask: 1 when RT shadow mask is valid this frame
#ifdef DHEWM3_RAYTRACING
    int *useSM = (int *)(fsz + 2);
    *useSM = (vk.rayTracingSupported && vkRT.isInitialized && r_rtShadows.GetBool() &&
              vkRT.shadowMask[vk.currentFrame].image != VK_NULL_HANDLE)
                 ? 1
                 : 0;
#else
    int *useSM = (int *)(fsz + 2);
    *useSM = 0;
#endif

    // Allocate descriptor set from the current frame's pool (pool is reset each frame)
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkPipes.descPools[vk.currentFrame];
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &vkPipes.interactionDescLayout;

    VkDescriptorSet ds;
    VkResult dsResult = vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds);
    if (dsResult != VK_SUCCESS)
    {
        common->Warning("VK: descriptor set allocation failed");
        return;
    }

    // Write UBO descriptor
    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = ring.buffer;
    bufInfo.offset = uboOffset;
    bufInfo.range = INTERACTION_UBO_SIZE;

    // Collect the 6 texture images in binding order (matches interaction.frag samplers):
    //  1=bump, 2=lightFalloff, 3=lightProjection, 4=diffuse, 5=specular, 6=specularTable
    idImage *texImages[6] = {
        din->bumpImage,    din->lightFalloffImage,
        din->lightImage, // light projection
        din->diffuseImage, din->specularImage,     globalImages->specularTableImage,
    };

    extern bool VK_Image_GetDescriptorInfo(idImage *, VkDescriptorImageInfo *);
    extern void VK_Image_GetFallbackDescriptorInfo(VkDescriptorImageInfo *);

    VkDescriptorImageInfo imgInfos[6] = {};
    for (int i = 0; i < 6; i++)
    {
        if (!texImages[i] || !VK_Image_GetDescriptorInfo(texImages[i], &imgInfos[i]))
        {
            VK_Image_GetFallbackDescriptorInfo(&imgInfos[i]);
        }
    }

    // Binding 7: shadow mask
#ifdef DHEWM3_RAYTRACING
    VkDescriptorImageInfo shadowMaskInfo = {};
    if (vk.rayTracingSupported && vkRT.isInitialized && vkRT.shadowMask[vk.currentFrame].image != VK_NULL_HANDLE)
    {
        shadowMaskInfo.sampler = vkRT.shadowMaskSampler;
        shadowMaskInfo.imageView = vkRT.shadowMask[vk.currentFrame].view;
        shadowMaskInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    else
    {
        VK_Image_GetFallbackDescriptorInfo(&shadowMaskInfo);
    }
#else
    VkDescriptorImageInfo shadowMaskInfo = {};
    VK_Image_GetFallbackDescriptorInfo(&shadowMaskInfo);
#endif

    VkWriteDescriptorSet writes[8] = {};

    // Binding 0: UBO
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;

    // Bindings 1-6: combined image samplers
    for (int i = 0; i < 6; i++)
    {
        writes[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1 + i].dstSet = ds;
        writes[1 + i].dstBinding = (uint32_t)(1 + i);
        writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1 + i].descriptorCount = 1;
        writes[1 + i].pImageInfo = &imgInfos[i];
    }

    // Binding 7: shadow mask
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = ds;
    writes[7].dstBinding = 7;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].descriptorCount = 1;
    writes[7].pImageInfo = &shadowMaskInfo;

    vkUpdateDescriptorSets(vk.device, 8, writes, 0, NULL);

    // Bind descriptor set
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionLayout, 0, 1, &ds, 0, NULL);

    // Bind vertex and index buffers
    extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);
    const srfTriangles_t *geo = din->surf->geo;
    if (!geo)
    {
        common->Warning("VK: NULL geo in DrawInteraction, skipping");
        return;
    }
    if (!geo->indexes || geo->numIndexes <= 0 || geo->numVerts <= 0)
    {
        return;
    }

    // --- Vertex buffer ---
    VkBuffer vertBuf;
    VkDeviceSize vertOffset;
    if (!VK_VertexCache_GetBuffer(geo->ambientCache, &vertBuf, &vertOffset))
    {
        // TAG_TEMP or no VkBuffer allocated — upload from CPU vertex cache
        const void *cpuVerts = vertexCache.Position(geo->ambientCache);
        if (!cpuVerts)
            return;
        VkDeviceSize vertSize = (VkDeviceSize)geo->numVerts * sizeof(idDrawVert);
        vertOffset = VK_AllocDataRing(vertSize, sizeof(float));
        if (vertOffset == VK_WHOLE_SIZE)
            return;
        memcpy((byte *)dataRings[vk.currentFrame].mapped + vertOffset, cpuVerts, (size_t)vertSize);
        vertBuf = dataRings[vk.currentFrame].buffer;
    }
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &vertBuf, &vertOffset);

    // --- Index buffer ---
    // geo->indexCache is always NULL (r_useIndexBuffers defaults to 0).
    // Upload geo->indexes from the CPU array every time.
    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    VkDeviceSize idxSize = (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t);
    VkDeviceSize idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
    if (idxOffset == VK_WHOLE_SIZE)
        return;
    memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, geo->indexes, (size_t)idxSize);
    vkCmdBindIndexBuffer(s_cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);

    vkCmdDrawIndexed(s_cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// VK_RB_DrawShaderPasses - non-light-dependent surface rendering
// Mirrors RB_STD_DrawShaderPasses for the Vulkan path.
// Draws GUI surfaces, console, menus, and other unlit/2D content.
// ---------------------------------------------------------------------------

// GUI UBO layout (must match gui.vert uniform block, std140)
struct VkGuiUBO
{
    float modelViewProjection[16]; // 64 bytes
    float colorModulate[4];        // 16 bytes
    float colorAdd[4];             // 16 bytes
}; // 96 bytes total

static void VK_RB_DrawShaderPasses(VkCommandBuffer cmd)
{
    if (!backEnd.viewDef || backEnd.viewDef->numDrawSurfs == 0)
        return;

    extern bool VK_Image_GetDescriptorInfo(idImage *, VkDescriptorImageInfo *);
    extern void VK_Image_GetFallbackDescriptorInfo(VkDescriptorImageInfo *);

    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

    for (int si = 0; si < backEnd.viewDef->numDrawSurfs; si++)
    {
        const drawSurf_t *surf = backEnd.viewDef->drawSurfs[si];
        if (!surf || !surf->material || !surf->geo)
            continue;

        const idMaterial *mat = surf->material;
        if (mat->SuppressInSubview())
            continue;

        const srfTriangles_t *geo = surf->geo;
        if (!geo->indexes || geo->numIndexes <= 0 || geo->numVerts <= 0)
            continue;

        // Get vertex data (from cache or raw verts)
        VkBuffer vertBuf;
        VkDeviceSize vertOffset;
        bool haveVerts = false;

        extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);

        if (geo->ambientCache && VK_VertexCache_GetBuffer(geo->ambientCache, &vertBuf, &vertOffset))
        {
            haveVerts = true;
        }
        else
        {
            const void *cpuVerts = NULL;
            if (geo->ambientCache)
                cpuVerts = vertexCache.Position(geo->ambientCache);
            else if (geo->verts)
                cpuVerts = geo->verts;

            if (cpuVerts)
            {
                VkDeviceSize sz = (VkDeviceSize)geo->numVerts * sizeof(idDrawVert);
                vertOffset = VK_AllocDataRing(sz, sizeof(float));
                if (vertOffset != VK_WHOLE_SIZE)
                {
                    memcpy((byte *)dataRings[vk.currentFrame].mapped + vertOffset, cpuVerts, (size_t)sz);
                    vertBuf = dataRings[vk.currentFrame].buffer;
                    haveVerts = true;
                }
            }
        }
        if (!haveVerts)
            continue;

        // Upload index data
        VkDeviceSize idxSize = (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t);
        VkDeviceSize idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
        if (idxOffset == VK_WHOLE_SIZE)
            continue;
        memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, geo->indexes, (size_t)idxSize);

        const float *regs = surf->shaderRegisters;

        for (int stageIdx = 0; stageIdx < mat->GetNumStages(); stageIdx++)
        {
            const shaderStage_t *pStage = mat->GetStage(stageIdx);

            // Skip disabled stages
            if (!regs[pStage->conditionRegister])
                continue;

            // Skip lighting-specific stages (handled by interaction pass)
            if (pStage->lighting != SL_AMBIENT)
                continue;

            idImage *img = pStage->texture.image;
            VkDescriptorImageInfo imgInfo = {};
            if (!img || !VK_Image_GetDescriptorInfo(img, &imgInfo))
                VK_Image_GetFallbackDescriptorInfo(&imgInfo);

            // Build color modulate/add from stage vertex color mode
            float color[4] = {
                regs[pStage->color.registers[0]],
                regs[pStage->color.registers[1]],
                regs[pStage->color.registers[2]],
                regs[pStage->color.registers[3]],
            };

            float colorModulate[4] = {1, 1, 1, 1};
            float colorAdd[4] = {0, 0, 0, 0};

            if (pStage->vertexColor == SVC_IGNORE)
            {
                // Constant color only: colorModulate = (0,0,0,0), colorAdd = color
                colorModulate[0] = colorModulate[1] = colorModulate[2] = colorModulate[3] = 0.0f;
                colorAdd[0] = color[0];
                colorAdd[1] = color[1];
                colorAdd[2] = color[2];
                colorAdd[3] = color[3];
            }
            else if (pStage->vertexColor == SVC_MODULATE)
            {
                // vertex * color: colorModulate = color, colorAdd = (0,0,0,0)
                colorModulate[0] = color[0];
                colorModulate[1] = color[1];
                colorModulate[2] = color[2];
                colorModulate[3] = color[3];
            }
            else if (pStage->vertexColor == SVC_INVERSE_MODULATE)
            {
                colorModulate[0] = -color[0];
                colorModulate[1] = -color[1];
                colorModulate[2] = -color[2];
                colorModulate[3] = -color[3];
                colorAdd[0] = color[0];
                colorAdd[1] = color[1];
                colorAdd[2] = color[2];
                colorAdd[3] = color[3];
            }

            float mvp[16];
            VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp);

            uint32_t uboOffset = VK_AllocUBO();
            uint8_t *uboPtr = (uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset;
            VkGuiUBO *guiUbo = (VkGuiUBO *)uboPtr;
            memcpy(guiUbo->modelViewProjection, mvp, 64);
            memcpy(guiUbo->colorModulate, colorModulate, 16);
            memcpy(guiUbo->colorAdd, colorAdd, 16);

            // Allocate GUI descriptor set
            VkDescriptorSetAllocateInfo dsAlloc = {};
            dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAlloc.descriptorPool = vkPipes.descPools[vk.currentFrame];
            dsAlloc.descriptorSetCount = 1;
            dsAlloc.pSetLayouts = &vkPipes.guiDescLayout;

            VkDescriptorSet ds;
            if (vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds) != VK_SUCCESS)
                continue;

            VkDescriptorBufferInfo bufInfo = {};
            bufInfo.buffer = uboRings[vk.currentFrame].buffer;
            bufInfo.offset = uboOffset;
            bufInfo.range = sizeof(VkGuiUBO);

            VkWriteDescriptorSet writes[2] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = ds;
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &bufInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = ds;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &imgInfo;

            vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);

            // Select (or lazily create) a pipeline matching the exact GLS blend state.
            extern VkPipeline VK_GetOrCreateGuiBlendPipeline(int drawStateBits);
            VkPipeline pipeline = VK_GetOrCreateGuiBlendPipeline(pStage->drawStateBits);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.guiLayout, 0, 1, &ds, 0, NULL);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
            vkCmdBindIndexBuffer(cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);
            vkCmdDrawIndexed(cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// VK_RB_FillDepthBuffer - depth prepass
// Draws all opaque (MC_OPAQUE) surfaces to the depth buffer only, so the
// interaction pass can depth-test against a fully populated depth image.
// Uses the depth pipeline (gui.vert.spv + guiLayout, colorWriteMask=0, depthWrite=LESS).
// ---------------------------------------------------------------------------

static void VK_RB_FillDepthBuffer(VkCommandBuffer cmd)
{
    if (!backEnd.viewDef || !vkPipes.depthPipeline)
        return;

    extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);
    extern void VK_Image_GetFallbackDescriptorInfo(VkDescriptorImageInfo *);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.depthPipeline);

    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

    for (int i = 0; i < backEnd.viewDef->numDrawSurfs; i++)
    {
        const drawSurf_t *surf = backEnd.viewDef->drawSurfs[i];
        if (!surf || !surf->material || !surf->geo)
            continue;

        // Only opaque surfaces contribute to the depth prepass.
        // MC_PERFORATED (alpha-tested) and MC_TRANSLUCENT are skipped.
        if (surf->material->Coverage() != MC_OPAQUE)
            continue;

        const srfTriangles_t *geo = surf->geo;
        if (!geo->indexes || geo->numIndexes <= 0 || geo->numVerts <= 0)
            continue;

        // --- Vertex buffer ---
        VkBuffer     vertBuf;
        VkDeviceSize vertOffset;
        bool haveVerts = false;

        if (geo->ambientCache && VK_VertexCache_GetBuffer(geo->ambientCache, &vertBuf, &vertOffset))
        {
            haveVerts = true;
        }
        else
        {
            const void *cpuVerts = geo->ambientCache ? vertexCache.Position(geo->ambientCache) : geo->verts;
            if (cpuVerts)
            {
                VkDeviceSize sz = (VkDeviceSize)geo->numVerts * sizeof(idDrawVert);
                vertOffset = VK_AllocDataRing(sz, sizeof(float));
                if (vertOffset != VK_WHOLE_SIZE)
                {
                    memcpy((byte *)dataRings[vk.currentFrame].mapped + vertOffset, cpuVerts, (size_t)sz);
                    vertBuf   = dataRings[vk.currentFrame].buffer;
                    haveVerts = true;
                }
            }
        }
        if (!haveVerts)
            continue;

        // --- Index buffer ---
        VkDeviceSize idxSize   = (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t);
        VkDeviceSize idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
        if (idxOffset == VK_WHOLE_SIZE)
            continue;
        memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, geo->indexes, (size_t)idxSize);

        // --- UBO (GuiUBO layout: MVP + colorModulate + colorAdd) ---
        float mvp[16];
        VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp);

        uint32_t  uboOffset = VK_AllocUBO();
        VkGuiUBO *ubo       = (VkGuiUBO *)((uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset);
        memcpy(ubo->modelViewProjection, mvp, 64);
        ubo->colorModulate[0] = ubo->colorModulate[1] = ubo->colorModulate[2] = ubo->colorModulate[3] = 1.f;
        ubo->colorAdd[0]      = ubo->colorAdd[1]      = ubo->colorAdd[2]      = ubo->colorAdd[3]      = 0.f;

        // --- Descriptor set (guiDescLayout: binding0=UBO, binding1=sampler dummy) ---
        VkDescriptorSetAllocateInfo dsAlloc = {};
        dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool     = vkPipes.descPools[vk.currentFrame];
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts        = &vkPipes.guiDescLayout;

        VkDescriptorSet ds;
        if (vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds) != VK_SUCCESS)
            continue;

        VkDescriptorBufferInfo bufInfo = {};
        bufInfo.buffer = uboRings[vk.currentFrame].buffer;
        bufInfo.offset = uboOffset;
        bufInfo.range  = sizeof(VkGuiUBO);

        VkDescriptorImageInfo imgInfo = {};
        VK_Image_GetFallbackDescriptorInfo(&imgInfo); // dummy sampler — colour writes disabled

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = ds;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bufInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = ds;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &imgInfo;

        vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vkPipes.guiLayout, 0, 1, &ds, 0, NULL);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
        vkCmdBindIndexBuffer(cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);
        vkCmdDrawIndexed(cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Shadow UBO layout (matches shadow.vert ShadowParams block)
struct VkShadowUBO
{
    float lightOrigin[4]; // vec4 — local-space light origin (w unused, set 0)
    float mvp[16];        // mat4 — model-view-projection
}; // 80 bytes — fits in the 384-byte UBO ring stride

// ---------------------------------------------------------------------------
// VK_RB_DrawShadowSurface
// Records one shadow volume draw (Carmack's Reverse, depth-fail stencil).
// Uploads shadow vertices from tri->shadowCache (vec4 idVec4 positions),
// writes the shadow UBO (light origin + MVP), and issues vkCmdDrawIndexed.
// ---------------------------------------------------------------------------

static void VK_RB_DrawShadowSurface(VkCommandBuffer cmd, const drawSurf_t *surf)
{
    extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);

    const srfTriangles_t *tri = surf->geo;
    if (!tri || !tri->shadowCache || tri->numIndexes <= 0)
        return;

    // Shadow vertices: shadowCache_t = idVec4 (16 bytes each)
    VkBuffer     vertBuf;
    VkDeviceSize vertOffset;
    bool haveVerts = false;

    if (VK_VertexCache_GetBuffer(tri->shadowCache, &vertBuf, &vertOffset))
    {
        haveVerts = true;
    }
    else
    {
        const void *cpuVerts = vertexCache.Position(tri->shadowCache);
        if (cpuVerts)
        {
            // Count shadow verts: indexes reference vertices up to numVerts*2 (mirrored)
            // Conservative: use numVerts from the ambient surface if available, else
            // scan indexes to find the max referenced vertex.
            const srfTriangles_t *amb = tri->ambientSurface ? tri->ambientSurface : tri;
            int numShadowVerts = amb->numVerts * 2; // shadow verts are doubled (front+back caps)
            if (numShadowVerts <= 0) numShadowVerts = 256; // safe fallback

            VkDeviceSize sz = (VkDeviceSize)numShadowVerts * sizeof(shadowCache_t);
            vertOffset = VK_AllocDataRing(sz, sizeof(float));
            if (vertOffset != VK_WHOLE_SIZE)
            {
                memcpy((byte *)dataRings[vk.currentFrame].mapped + vertOffset, cpuVerts, (size_t)sz);
                vertBuf   = dataRings[vk.currentFrame].buffer;
                haveVerts = true;
            }
        }
    }
    if (!haveVerts)
        return;

    // Index data (tri->indexes, same field as regular geometry)
    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    VkDeviceSize idxSize   = (VkDeviceSize)tri->numIndexes * sizeof(glIndex_t);
    VkDeviceSize idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
    if (idxOffset == VK_WHOLE_SIZE)
        return;
    memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, tri->indexes, (size_t)idxSize);

    // Light origin in model local space
    idVec3 localLight;
    R_GlobalPointToLocal(surf->space->modelMatrix, backEnd.vLight->globalLightOrigin, localLight);

    // MVP matrix
    float mvp[16];
    VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp);

    // Shadow UBO
    uint32_t    uboOffset = VK_AllocUBO();
    VkShadowUBO *ubo      = (VkShadowUBO *)((uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset);
    ubo->lightOrigin[0] = localLight.x;
    ubo->lightOrigin[1] = localLight.y;
    ubo->lightOrigin[2] = localLight.z;
    ubo->lightOrigin[3] = 0.f;
    memcpy(ubo->mvp, mvp, 64);

    // Descriptor set (shadowDescLayout: binding 0 = UBO only)
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = vkPipes.descPools[vk.currentFrame];
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &vkPipes.shadowDescLayout;

    VkDescriptorSet ds;
    if (vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds) != VK_SUCCESS)
        return;

    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = uboRings[vk.currentFrame].buffer;
    bufInfo.offset = uboOffset;
    bufInfo.range  = sizeof(VkShadowUBO);

    VkWriteDescriptorSet write = {};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo     = &bufInfo;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vkPipes.shadowLayout, 0, 1, &ds, 0, NULL);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
    vkCmdBindIndexBuffer(cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);
    vkCmdDrawIndexed(cmd, (uint32_t)tri->numIndexes, 1, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// VK_RB_DrawInteractions - per-light interaction loop
// Mirrors RB_ARB2_DrawInteractions / RB_GLSL_DrawInteractions
// ---------------------------------------------------------------------------

static void VK_RB_DrawInteractions(VkCommandBuffer cmd)
{
    s_cmd = cmd;

    // Must be called before RB_CreateSingleDrawInteractions so backEnd.lightScale is valid.
    // Without this, all lightColor[] values are 0 and RB_SubmittInteraction never fires.
    RB_DetermineLightScale();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);

    int lightIdx = 0;
    for (viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next, lightIdx++)
    {
        backEnd.vLight = vLight;

        if (!vLight->lightShader)
            continue;

        if (vLight->lightShader->IsFogLight())
            continue;
        if (vLight->lightShader->IsBlendLight())
            continue;
        if (!vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions)
            continue;

        // Set scissor for this light
        if (r_useScissor.GetBool())
        {
            VkRect2D scissor = {};
            scissor.offset.x = backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1;
            scissor.offset.y = backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1;
            scissor.extent.width = vLight->scissorRect.x2 - vLight->scissorRect.x1 + 1;
            scissor.extent.height = vLight->scissorRect.y2 - vLight->scissorRect.y1 + 1;
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }

        // Stencil shadow volumes — only when RT shadows are unavailable or disabled.
        // When RT is active, shadows are applied per-pixel in the interaction shader
        // via shadowMaskSampler. Running both would produce double-shadowing.
#ifdef DHEWM3_RAYTRACING
        const bool useStencilShadows = !(vk.rayTracingSupported && r_rtShadows.GetBool());
#else
        const bool useStencilShadows = true;
#endif
        if (useStencilShadows && vkPipes.shadowPipeline &&
            (vLight->globalShadows || vLight->localShadows))
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.shadowPipeline);
            for (const drawSurf_t *s = vLight->globalShadows; s; s = s->nextOnLight)
                VK_RB_DrawShadowSurface(cmd, s);
            for (const drawSurf_t *s = vLight->localShadows; s; s = s->nextOnLight)
                VK_RB_DrawShadowSurface(cmd, s);
        }

        // Switch back to interaction pipeline for lit surfaces
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);

        // Draw lit interactions
        for (const drawSurf_t *surf = vLight->localInteractions; surf; surf = surf->nextOnLight)
            RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
        for (const drawSurf_t *surf = vLight->globalInteractions; surf; surf = surf->nextOnLight)
            RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);

        if (!r_skipTranslucent.GetBool())
        {
            for (const drawSurf_t *surf = vLight->translucentInteractions; surf; surf = surf->nextOnLight)
                RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
        }
    }

    s_cmd = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_RB_DrawView / VK_RB_SwapBuffers - main per-frame view rendering
//
// Doom 3 submits multiple RC_DRAW_VIEW commands per EndFrame (e.g. the 3D
// world view followed by the 2D GUI/menu overlay).  Each RC_DRAW_VIEW call
// lands here via VKBackend::DrawView.  All views in one EndFrame must share
// a single swapchain image and command buffer; only RC_SWAP_BUFFERS should
// submit and present.
//
// VK_RB_DrawView   – records draw calls into the shared per-frame cmd buffer.
//                    On the first call per EndFrame it acquires the image and
//                    begins the command buffer + render pass.  Subsequent calls
//                    just reset viewport/scissor and keep recording.
// VK_RB_SwapBuffers – called from RC_SWAP_BUFFERS; finishes the render pass,
//                    ends the command buffer, submits, and presents.
// ---------------------------------------------------------------------------

// Per-EndFrame state shared across multiple RC_DRAW_VIEW calls.
static bool s_frameActive = false;
static VkCommandBuffer s_frameCmdBuf = VK_NULL_HANDLE;
static uint32_t s_frameImageIndex = 0;

void VK_RB_DrawView(const void *data)
{
    if (!vk.isInitialized || !vkPipes.isValid)
        return;

    const drawSurfsCommand_t *cmd = (const drawSurfsCommand_t *)data;
    backEnd.viewDef = cmd->viewDef;

    // Build Vulkan-corrected projection matrix for this view.
    // Remap Z from OpenGL NDC [-1,1] to Vulkan NDC [0,1]:
    //   new_row2 = 0.5 * old_row2 + 0.5 * old_row3  (column-major: row2 @ index c*4+2)
    // Y flip is handled by the viewport (negative height), not here.
    {
        const float *src = backEnd.viewDef->projectionMatrix;
        memcpy(s_projVk, src, 64);
        for (int c = 0; c < 4; c++)
        {
            s_projVk[c * 4 + 2] = 0.5f * src[c * 4 + 2] + 0.5f * src[c * 4 + 3];
        }
    }

    if (!s_frameActive)
    {
        // === First RC_DRAW_VIEW this EndFrame: acquire image and open command buffer ===

        // --- Wait for previous frame's fence ---
        VkResult fenceResult = vkWaitForFences(vk.device, 1, &vk.inFlightFences[vk.currentFrame], VK_TRUE, UINT64_MAX);
        if (fenceResult != VK_SUCCESS)
        {
            common->Warning("VK: vkWaitForFences failed: %d", (int)fenceResult);
            return;
        }

        // --- Acquire swapchain image ---
        uint32_t imageIndex;
        VkResult acquireResult =
            vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.imageAvailableSemaphores[vk.currentFrame],
                                  VK_NULL_HANDLE, &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
        {
            VK_RecreateSwapchain(glConfig.vidWidth, glConfig.vidHeight);
            return; // s_frameActive stays false; VK_RB_SwapBuffers will no-op
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        {
            common->Warning("VK: vkAcquireNextImageKHR failed: %d", (int)acquireResult);
            return;
        }

        vk.currentImageIdx = imageIndex;
        s_frameImageIndex  = imageIndex;
        vkResetFences(vk.device, 1, &vk.inFlightFences[vk.currentFrame]);

        // Reset per-frame allocators (shared across all views in this EndFrame)
        uboRings[vk.currentFrame].offset = 0;
        dataRings[vk.currentFrame].offset = 0;
        vkResetDescriptorPool(vk.device, vkPipes.descPools[vk.currentFrame], 0);

        // Begin command buffer
        VkCommandBuffer cmdBuf = vk.commandBuffers[vk.currentFrame];
        vkResetCommandBuffer(cmdBuf, 0);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS)
        {
            common->Warning("VK: vkBeginCommandBuffer failed, aborting frame");
            return;
        }

        // Begin render pass (one pass for the entire EndFrame; all views composite into it)
        VkClearValue clearValues[2] = {};
        clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 128};

        VkRenderPassBeginInfo rpBegin = {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = vk.renderPass;
        rpBegin.framebuffer = vk.swapchainFramebuffers[imageIndex];
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = vk.swapchainExtent;
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clearValues;
        vkCmdBeginRenderPass(cmdBuf, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        // Negative height flips Y to match OpenGL NDC convention (Y-up).
        // NOTE: the negative height inverts the effective winding order, so our pipelines
        // use VK_FRONT_FACE_CLOCKWISE (OpenGL CCW front faces become CW after Y-flip).
        VkViewport viewport = {0,
                               (float)vk.swapchainExtent.height,
                               (float)vk.swapchainExtent.width,
                               -(float)vk.swapchainExtent.height,
                               0.0f,
                               1.0f};
        vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
        VkRect2D fullScissor = {{0, 0}, vk.swapchainExtent};
        vkCmdSetScissor(cmdBuf, 0, 1, &fullScissor);

        s_frameCmdBuf = cmdBuf;
        s_frameActive = true;

        // RT shadow dispatch happens outside the render pass, before interactions.
        // Only valid on the first (3D world) view.
#ifdef DHEWM3_RAYTRACING
        if (vk.rayTracingSupported && vkRT.isInitialized && r_rtShadows.GetBool())
        {
            vkCmdEndRenderPass(cmdBuf);
            VK_RT_RebuildTLAS(cmdBuf, backEnd.viewDef);
            VK_RT_DispatchShadowRays(cmdBuf, backEnd.viewDef);
            // Reopen render pass; TODO: use a LOAD render pass variant to preserve prior draws
            vkCmdBeginRenderPass(cmdBuf, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
            vkCmdSetScissor(cmdBuf, 0, 1, &fullScissor);
        }
#endif
    }
    else
    {
        // === Subsequent RC_DRAW_VIEW: render pass already open; reset viewport/scissor ===
        VkViewport viewport = {0,
                               (float)vk.swapchainExtent.height,
                               (float)vk.swapchainExtent.width,
                               -(float)vk.swapchainExtent.height,
                               0.0f,
                               1.0f};
        vkCmdSetViewport(s_frameCmdBuf, 0, 1, &viewport);
        VkRect2D fullScissor = {{0, 0}, vk.swapchainExtent};
        vkCmdSetScissor(s_frameCmdBuf, 0, 1, &fullScissor);
    }

    VkCommandBuffer cmdBuf = s_frameCmdBuf;

    // Rendering order: depth prepass → interactions → ambient/unlit shader passes.
    // Matches vkDOOM3 reference: depth fills first, then lit surfaces, then 2D overlays.
    VK_RB_FillDepthBuffer(cmdBuf);

    VK_RB_DrawInteractions(cmdBuf);

    VK_RB_DrawShaderPasses(cmdBuf);

    // Submit/present deferred to VK_RB_SwapBuffers (called from RC_SWAP_BUFFERS)
}

// ---------------------------------------------------------------------------
// VK_RB_SwapBuffers - end the frame and present.
// Called from RC_SWAP_BUFFERS (tr_backend.cpp) for the Vulkan path.
// ---------------------------------------------------------------------------

void VK_RB_SwapBuffers()
{
    if (!s_frameActive)
        return; // acquire failed or no RC_DRAW_VIEW this EndFrame

    VkCommandBuffer cmdBuf = s_frameCmdBuf;

    // Render ImGui overlay (must be inside render pass)
    D3::ImGuiHooks::RenderVulkan(cmdBuf);

    vkCmdEndRenderPass(cmdBuf);
    VK_CHECK(vkEndCommandBuffer(cmdBuf));

    // --- Submit ---
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &vk.imageAvailableSemaphores[vk.currentFrame];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &vk.renderFinishedSemaphores[vk.currentFrame];

    VkResult submitResult = vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, vk.inFlightFences[vk.currentFrame]);
    if (submitResult != VK_SUCCESS)
    {
        s_frameActive = false;
        common->FatalError("Vulkan error %d in vkQueueSubmit", (int)submitResult);
        return;
    }

    // --- Present ---
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &vk.renderFinishedSemaphores[vk.currentFrame];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vk.swapchain;
    presentInfo.pImageIndices = &s_frameImageIndex;

    VkResult presentResult = vkQueuePresentKHR(vk.presentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
        VK_RecreateSwapchain(glConfig.vidWidth, glConfig.vidHeight);

    vk.currentFrame = (vk.currentFrame + 1) % VK_MAX_FRAMES_IN_FLIGHT;
    s_frameActive = false;
    s_frameCmdBuf = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_Init / VK_Shutdown - called from glimp.cpp
// ---------------------------------------------------------------------------

// Forward declarations (vk_instance.cpp)
void VKimp_Init(SDL_Window *window);
void VKimp_Shutdown(void);

// Forward declarations (vk_image.cpp)
extern void VK_Image_Init(void);
extern void VK_Image_Shutdown(void);

void VKimp_PostInit(int width, int height)
{
    common->Printf("VK: creating swapchain\n");
    VK_CreateSwapchain(width, height);
    common->Printf("VK: creating pipelines\n");
    VK_InitPipelines();
    common->Printf("VK: creating UBO rings\n");
    VK_CreateUBORings();
    common->Printf("VK: creating data rings\n");
    VK_CreateDataRings();
    common->Printf("VK: initializing images\n");
    VK_Image_Init();

#ifdef DHEWM3_RAYTRACING
    if (vk.rayTracingSupported)
    {
        common->Printf("VK: initializing RT\n");
        VK_RT_Init();
        common->Printf("VK: initializing RT shadows\n");
        VK_RT_InitShadows();
    }
#endif

    common->Printf("VK: Backend ready\n");
}

void VKimp_PreShutdown(void)
{
    if (!vk.isInitialized)
        return;
    vkDeviceWaitIdle(vk.device);

#ifdef DHEWM3_RAYTRACING
    if (vk.rayTracingSupported && vkRT.isInitialized)
    {
        VK_RT_Shutdown();
    }
#endif

    VK_Image_Shutdown();
    VK_DestroyDataRings();
    VK_DestroyUBORings();
    VK_ShutdownPipelines();
    VK_DestroySwapchain();
}
