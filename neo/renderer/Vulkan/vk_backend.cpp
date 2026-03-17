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
    VK_MultiplyMatrix4(backEnd.viewDef->projectionMatrix, din->surf->space->modelViewMatrix, mvp);
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
        common->Printf("VK Shader surface %d\n", si);
        fflush(NULL);

        const drawSurf_t *surf = backEnd.viewDef->drawSurfs[si];
        common->Printf("VK surf ptr=%p\n", (void*)surf); fflush(NULL);
        if (!surf || !surf->material || !surf->geo)
            continue;

        common->Printf("VK surf mat=%p geo=%p space=%p\n", (void*)surf->material, (void*)surf->geo, (void*)surf->space); fflush(NULL);
        const idMaterial *mat = surf->material;
        if (mat->SuppressInSubview())
            continue;

        common->Printf("VK surf passed SuppressInSubview\n"); fflush(NULL);
        const srfTriangles_t *geo = surf->geo;
        common->Printf("VK geo indexes=%p numIdx=%d numVerts=%d ambientCache=%p\n", (void*)geo->indexes, geo->numIndexes, geo->numVerts, (void*)geo->ambientCache); fflush(NULL);
        if (!geo->indexes || geo->numIndexes <= 0 || geo->numVerts <= 0)
            continue;

        // Get vertex data (from cache or raw verts)
        VkBuffer vertBuf;
        VkDeviceSize vertOffset;
        bool haveVerts = false;

        extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);

        common->Printf("VK calling VK_VertexCache_GetBuffer\n"); fflush(NULL);
        if (geo->ambientCache && VK_VertexCache_GetBuffer(geo->ambientCache, &vertBuf, &vertOffset))
        {
            haveVerts = true;
        }
        else
        {
            common->Printf("VK Shader no verts.  Assigning\n");
            fflush(NULL);

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
            common->Printf("VK Shader stage %d\n", stageIdx);
            fflush(NULL);

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

            // Compute MVP
            common->Printf("VK Shader Compute MVP\n");
            fflush(NULL);

            float mvp[16];
            VK_MultiplyMatrix4(backEnd.viewDef->projectionMatrix, surf->space->modelViewMatrix, mvp);

            // Upload GUI UBO
            common->Printf("VK Shader Upload GUI UBO\n");
            fflush(NULL);

            uint32_t uboOffset = VK_AllocUBO();
            uint8_t *uboPtr = (uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset;
            VkGuiUBO *guiUbo = (VkGuiUBO *)uboPtr;
            memcpy(guiUbo->modelViewProjection, mvp, 64);
            memcpy(guiUbo->colorModulate, colorModulate, 16);
            memcpy(guiUbo->colorAdd, colorAdd, 16);

            // Allocate GUI descriptor set
            common->Printf("VK Shader Allocate Descriptor\n");
            fflush(NULL);

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

            // Select pipeline based on blend mode
            common->Printf("VK Shader Select Pipeline\n");
            fflush(NULL);

            int blendBits = pStage->drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS);
            VkPipeline pipeline = (blendBits != 0) ? vkPipes.guiAlphaPipeline : vkPipes.guiOpaquePipeline;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.guiLayout, 0, 1, &ds, 0, NULL);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
            vkCmdBindIndexBuffer(cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);
            vkCmdDrawIndexed(cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0);
        }
    }
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

    int lightCount = 0;
    for (viewLight_t *l = backEnd.viewDef->viewLights; l; l = l->next)
        lightCount++;
    common->Printf("VK DrawInteractions: %d lights\n", lightCount);
    fflush(NULL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);

    int lightIdx = 0;
    for (viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next, lightIdx++)
    {
        backEnd.vLight = vLight;

        common->Printf("VK light %d: lightShader=%p localI=%p globalI=%p transI=%p\n", lightIdx,
                       (void *)vLight->lightShader, (void *)vLight->localInteractions,
                       (void *)vLight->globalInteractions, (void *)vLight->translucentInteractions);
        fflush(NULL);

        if (!vLight->lightShader)
        {
            common->Warning("VK: NULL lightShader on viewLight %d, skipping", lightIdx);
            continue;
        }

        if (vLight->lightShader->IsFogLight())
        {
            common->Printf("VK light %d: skipped (fog)\n", lightIdx);
            fflush(NULL);
            continue;
        }
        if (vLight->lightShader->IsBlendLight())
        {
            common->Printf("VK light %d: skipped (blend)\n", lightIdx);
            fflush(NULL);
            continue;
        }
        if (!vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions)
        {
            common->Printf("VK light %d: skipped (no interactions)\n", lightIdx);
            fflush(NULL);
            continue;
        }

        common->Printf("VK light %d: entering draw body\n", lightIdx);
        fflush(NULL);

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

        // Stencil shadow pass (still using geometry-based stencil volumes)
        // Switch to shadow pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.shadowPipeline);
        // TODO: record shadow volume draw calls here from vLight->globalShadows / localShadows
        // This is connected to vk_accelstruct.cpp when RT shadows replace this path.

        // Switch back to interaction pipeline for lit surfaces
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);

        // Draw lit interactions
        for (const drawSurf_t *surf = vLight->localInteractions; surf; surf = surf->nextOnLight)
        {
            common->Printf("VK localI surf=%p material=%p geo=%p space=%p\n", (void *)surf,
                           surf ? (void *)surf->material : nullptr, surf ? (void *)surf->geo : nullptr,
                           surf ? (void *)surf->space : nullptr);
            fflush(NULL);
            RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
        }
        for (const drawSurf_t *surf = vLight->globalInteractions; surf; surf = surf->nextOnLight)
        {
            common->Printf("VK globalI surf=%p material=%p geo=%p space=%p\n", (void *)surf,
                           surf ? (void *)surf->material : nullptr, surf ? (void *)surf->geo : nullptr,
                           surf ? (void *)surf->space : nullptr);
            fflush(NULL);
            RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
        }

        if (!r_skipTranslucent.GetBool())
        {
            for (const drawSurf_t *surf = vLight->translucentInteractions; surf; surf = surf->nextOnLight)
            {
                common->Printf("VK transI surf=%p material=%p geo=%p space=%p\n", (void *)surf,
                               surf ? (void *)surf->material : nullptr, surf ? (void *)surf->geo : nullptr,
                               surf ? (void *)surf->space : nullptr);
                fflush(NULL);
                RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
            }
        }
    }

    s_cmd = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_RB_DrawView - main per-frame view rendering
// Replaces RB_DrawView() for the Vulkan path.
// ---------------------------------------------------------------------------

void VK_RB_DrawView(const void *data)
{
    if (!vk.isInitialized || !vkPipes.isValid)
    {
        common->Printf("VK_RB_DrawView: skipped (isInitialized=%d, pipesValid=%d)\n", (int)vk.isInitialized,
                       (int)vkPipes.isValid);
        fflush(NULL);
        return;
    }

    static int s_frameCount = 0;
    const int thisFrame = s_frameCount++;
    if (thisFrame < 10 || (thisFrame % 60) == 0)
    {
        common->Printf("VK frame %d: begin (currentFrame slot=%d)\n", thisFrame, vk.currentFrame);
        fflush(NULL);
    }
    const drawSurfsCommand_t *cmd = (const drawSurfsCommand_t *)data;
    backEnd.viewDef = cmd->viewDef;
    common->Printf("VK frame %d: viewDef=%p viewLights=%p viewEntitys=%p\n", thisFrame, (void *)backEnd.viewDef,
                   backEnd.viewDef ? (void *)backEnd.viewDef->viewLights : nullptr,
                   backEnd.viewDef ? (void *)backEnd.viewDef->viewEntitys : nullptr);
    fflush(NULL);

    // --- Wait for previous frame's fence ---
    VkResult fenceResult = vkWaitForFences(vk.device, 1, &vk.inFlightFences[vk.currentFrame], VK_TRUE, UINT64_MAX);
    if (fenceResult != VK_SUCCESS)
    {
        common->Printf("VK frame %d: vkWaitForFences returned %d (DEVICE_LOST=%d)\n", thisFrame, (int)fenceResult,
                       (int)VK_ERROR_DEVICE_LOST);
        fflush(NULL);
        return;
    }
    common->Printf("VK frame %d: fence wait OK\n", thisFrame);
    fflush(NULL);
    // --- Acquire swapchain image ---
    uint32_t imageIndex;
    VkResult acquireResult = vkAcquireNextImageKHR(
        vk.device, vk.swapchain, UINT64_MAX, vk.imageAvailableSemaphores[vk.currentFrame], VK_NULL_HANDLE, &imageIndex);
    common->Printf("VK frame %d: acquired imageIndex=%u\n", thisFrame, imageIndex);
    fflush(NULL);
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        common->Printf("VK frame %d: vkAcquireNextImageKHR returned %d (OUT_OF_DATE=%d, DEVICE_LOST=%d)\n", thisFrame,
                       (int)acquireResult, (int)VK_ERROR_OUT_OF_DATE_KHR, (int)VK_ERROR_DEVICE_LOST);
        fflush(NULL);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
            return;
        return;
    }

    vk.currentImageIdx = imageIndex;
    vkResetFences(vk.device, 1, &vk.inFlightFences[vk.currentFrame]);

    // Reset UBO/data rings and descriptor pool for this frame
    uboRings[vk.currentFrame].offset = 0;
    dataRings[vk.currentFrame].offset = 0;
    vkResetDescriptorPool(vk.device, vkPipes.descPools[vk.currentFrame], 0);
    common->Printf("VK frame %d: fences reset, UBO ring reset\n", thisFrame);
    fflush(NULL);
    // --- Record command buffer ---
    VkCommandBuffer cmdBuf = vk.commandBuffers[vk.currentFrame];
    VkResult resetResult = vkResetCommandBuffer(cmdBuf, 0);
    common->Printf("VK frame %d: vkResetCommandBuffer=%d\n", thisFrame, (int)resetResult);
    fflush(NULL);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VkResult beginCBResult = vkBeginCommandBuffer(cmdBuf, &beginInfo);
    common->Printf("VK frame %d: vkBeginCommandBuffer=%d\n", thisFrame, (int)beginCBResult);
    fflush(NULL);
    if (beginCBResult != VK_SUCCESS)
    {
        common->Printf("VK frame %d: vkBeginCommandBuffer FAILED, aborting frame\n", thisFrame);
        fflush(NULL);
        return;
    }

    // Begin render pass
    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[1].depthStencil = {1.0f, 128}; // depth=1.0, stencil=128

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = vk.renderPass;
    rpBegin.framebuffer = vk.swapchainFramebuffers[imageIndex];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = vk.swapchainExtent;
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;

    vkCmdBeginRenderPass(cmdBuf, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    common->Printf("VK frame %d: render pass begun, fb=%p imageIndex=%u\n", thisFrame,
                   (void *)vk.swapchainFramebuffers[imageIndex], imageIndex);
    fflush(NULL);

    // Set viewport
    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
    VkRect2D fullScissor = {{0, 0}, vk.swapchainExtent};
    vkCmdSetScissor(cmdBuf, 0, 1, &fullScissor);
    common->Printf("VK set viewpoirt and scissor for frame %d, fb=%p imageIndex=%u\n", thisFrame,
                   (void *)vk.swapchainFramebuffers[imageIndex], imageIndex);
    fflush(NULL);

    // If RT is available and r_rtShadows is on, dispatch shadow rays before interaction pass
#ifdef DHEWM3_RAYTRACING
    if (vk.rayTracingSupported && vkRT.isInitialized && r_rtShadows.GetBool())
    {
        vkCmdEndRenderPass(cmdBuf); // RT dispatch happens outside render pass
        VK_RT_RebuildTLAS(cmdBuf, backEnd.viewDef);
        VK_RT_DispatchShadowRays(cmdBuf, backEnd.viewDef);
        // Re-open render pass for rasterization
        vkCmdBeginRenderPass(cmdBuf, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
        vkCmdSetScissor(cmdBuf, 0, 1, &fullScissor);
    }
#endif

    // Draw unlit/2D shader passes (GUI, menus, HUD)
    common->Printf("VK frame %d:Starting Shaders\n", thisFrame);
    fflush(NULL);

    VK_RB_DrawShaderPasses(cmdBuf);
    common->Printf("VK frame %d:Shaders Done\n", thisFrame);
    fflush(NULL);

    // Draw all light interactions
    common->Printf("VK frame %d: calling DrawInteractions\n", thisFrame);
    fflush(NULL);
    VK_RB_DrawInteractions(cmdBuf);
    common->Printf("VK frame %d: DrawInteractions done\n", thisFrame);
    fflush(NULL);

    // Render ImGui overlay (must be inside render pass)
    common->Printf("VK frame %d: calling ImGui RenderVulkan\n", thisFrame);
    fflush(NULL);
    D3::ImGuiHooks::RenderVulkan(cmdBuf);
    common->Printf("VK frame %d: ImGui done\n", thisFrame);
    fflush(NULL);

    vkCmdEndRenderPass(cmdBuf);
    common->Printf("VK frame %d: ending command buffer\n", thisFrame);
    fflush(NULL);
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
        common->Printf("VK frame %d: vkQueueSubmit FAILED %d (DEVICE_LOST=%d)\n", thisFrame, (int)submitResult,
                       (int)VK_ERROR_DEVICE_LOST);
        fflush(NULL);
        common->FatalError("Vulkan error %d in vkQueueSubmit", (int)submitResult);
    }
    if (thisFrame < 10 || (thisFrame % 60) == 0)
    {
        common->Printf("VK frame %d: submit OK, imageIndex=%u\n", thisFrame, imageIndex);
        fflush(NULL);
    }

    // --- Present ---
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &vk.renderFinishedSemaphores[vk.currentFrame];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vk.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    vkQueuePresentKHR(vk.presentQueue, &presentInfo);

    vk.currentFrame = (vk.currentFrame + 1) % VK_MAX_FRAMES_IN_FLIGHT;
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
