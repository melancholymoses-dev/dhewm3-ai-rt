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
#include "renderer/Cinematic.h"
#include "idlib/containers/StrList.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_buffer.h"
#include "sys/sys_imgui.h"
#include <SDL.h>

static bool VK_RTShadowsEnabled()
{
#ifdef DHEWM3_RAYTRACING
    return vk.rayTracingSupported && vkRT.isInitialized && r_useRayTracing.GetBool() && r_rtShadows.GetBool();
#else
    return false;
#endif
}

// Forward declarations (defined in vk_pipeline.cpp)
// vkPipelines_t and vkPipes are declared in vk_common.h
void VK_InitPipelines(void);
void VK_ShutdownPipelines(void);

// Forward declarations (defined in vk_swapchain.cpp)
void VK_CreateSwapchain(int width, int height);
void VK_DestroySwapchain(void);
void VK_RecreateSwapchain(int width, int height);

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

static const char *VK_ResultToString(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    default:
        return "VK_ERROR_UNKNOWN";
    }
}

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

static const VkDeviceSize VK_DATA_RING_SIZE = 64 * 1024 * 1024; // 64 MB per frame

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

// ---------------------------------------------------------------------------
// VK_BuildSurfMVP
// Build a model-view-projection matrix for a surface, applying weaponDepthHack
// or modelDepthHack to the projection when the surface requires it.
// Mirrors RB_EnterWeaponDepthHack / RB_EnterModelDepthHack from tr_render.cpp:
//   weaponDepthHack: proj[14] *= 0.25  (compresses near range, weapon stays in front)
//   modelDepthHack:  proj[14] -= depth  (nudges a model's z to avoid z-fighting)
// Both hacks operate on the GL projection matrix (before Vulkan Z remap).
// ---------------------------------------------------------------------------
static void VK_BuildSurfMVP(const viewEntity_t *space, float mvpOut[16])
{
    if (space->weaponDepthHack || space->modelDepthHack != 0.0f)
    {
        // Start from the original GL projection, apply hack, then Vulkan-remap Z.
        float hackProj[16];
        memcpy(hackProj, backEnd.viewDef->projectionMatrix, 64);
        if (space->weaponDepthHack)
            hackProj[14] *= 0.25f;
        else
            hackProj[14] -= space->modelDepthHack;
        // GL→VK Z remap.  weaponDepthHack uses [0, 0.5] to match glDepthRange(0, 0.5);
        // modelDepthHack uses the standard [0, 1] range (it only needs a Z shift).
        const float zS = space->weaponDepthHack ? 0.25f : 0.5f;
        const float zB = space->weaponDepthHack ? 0.25f : 0.5f;
        for (int c = 0; c < 4; c++)
            hackProj[c * 4 + 2] = zS * hackProj[c * 4 + 2] + zB * hackProj[c * 4 + 3];
        VK_MultiplyMatrix4(hackProj, space->modelViewMatrix, mvpOut);
    }
    else
    {
        VK_MultiplyMatrix4(s_projVk, space->modelViewMatrix, mvpOut);
    }
}

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
// VK_RB_DrawInteraction - record draw commands for one light-surface interaction
// Called from VK_RB_DrawInteractions via RB_CreateSingleDrawInteractions callback.
// ---------------------------------------------------------------------------

// We use a file-static to pass the command buffer through the callback
static VkCommandBuffer s_cmd = VK_NULL_HANDLE;
static const char *s_shadowPhaseTag = "unknown";

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

    // MVP matrix (applies weaponDepthHack / modelDepthHack when needed)
    float mvp[16];
    VK_BuildSurfMVP(din->surf->space, mvp);
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
    *useSM = (VK_RTShadowsEnabled() && vkRT.shadowMask[vk.currentFrame].image != VK_NULL_HANDLE) ? 1 : 0;
#else
    int *useSM = (int *)(fsz + 2);
    *useSM = 0;
#endif

    // lightScale: overBright factor from RB_DetermineLightScale (1.0 when no scaling needed)
    float *lightScalePtr = (float *)(useSM + 1);
    *lightScalePtr = backEnd.overBright;

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

    static const char *s_texSlotNames[6] = {"bump",    "lightFalloff", "lightProjection",
                                            "diffuse", "specular",     "specularTable"};
    VkDescriptorImageInfo imgInfos[6] = {};
    for (int i = 0; i < 6; i++)
    {
        if (!texImages[i] || !VK_Image_GetDescriptorInfo(texImages[i], &imgInfos[i]))
        {
            static idStr lastFallbackInterMat;
            const char *matName = din->surf && din->surf->material ? din->surf->material->GetName() : "<null>";
            if (lastFallbackInterMat != matName)
            {
                lastFallbackInterMat = matName;
                common->Printf("VK Interaction: fallback texture slot=%s mat='%s' img='%s'\n", s_texSlotNames[i],
                               matName, texImages[i] ? texImages[i]->imgName.c_str() : "<null>");
            }
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
// VK_RB_UpdateCinematics
//
// Must be called BEFORE vkCmdBeginRenderPass on the first view of a frame.
// Scans all draw surfaces for cinematic stages; calls ImageForTime on each
// and uploads the decoded RGBA frame into the shared cinematic VkImage via
// cmd (recorded as transfer ops, legal outside a render pass).
//
// Mirrors the GL path in RB_BindVariableStageImage (tr_render.cpp).
// ---------------------------------------------------------------------------

static void VK_RB_UpdateCinematics(VkCommandBuffer cmd)
{
    if (!backEnd.viewDef || r_skipDynamicTextures.GetBool())
        return;

    const int time = (int)(1000 * (backEnd.viewDef->floatTime + backEnd.viewDef->renderView.shaderParms[11]));

    for (int si = 0; si < backEnd.viewDef->numDrawSurfs; si++)
    {
        const drawSurf_t *surf = backEnd.viewDef->drawSurfs[si];
        if (!surf || !surf->material)
            continue;

        const idMaterial *mat = surf->material;
        const float *regs = surf->shaderRegisters;

        for (int stageIdx = 0; stageIdx < mat->GetNumStages(); stageIdx++)
        {
            const shaderStage_t *pStage = mat->GetStage(stageIdx);
            if (!regs[pStage->conditionRegister])
                continue;
            if (!pStage->texture.cinematic)
                continue;

            cinData_t cin = pStage->texture.cinematic->ImageForTime(time);
            if (cin.image)
                VK_Image_UpdateCinematic(cmd, cin.image, cin.imageWidth, cin.imageHeight);
            // Only one cinematic image is supported per frame (matches GL behaviour).
            // If multiple distinct cinematics are ever needed, a per-idCinematic* cache
            // is the natural extension.
            return;
        }
    }
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
    float texMatrixS[4];           // 16 bytes — row 0 of 2D affine UV transform
    float texMatrixT[4];           // 16 bytes — row 1
}; // 128 bytes total

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

        // Fog lights and blend lights have volume geometry in drawSurfs.
        // The GUI pipeline has no depth test, so these volumes would render
        // through floors/walls if not skipped here.  They will be handled
        // by a future VK_RB_FogAllLights pass.
        if (mat->IsFogLight() || mat->IsBlendLight())
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
            if (!img)
            {
                if (pStage->texture.cinematic)
                {
                    // Use the cinematic image uploaded by VK_RB_UpdateCinematics this frame.
                    VK_Image_GetCinematicDescriptorInfo(&imgInfo);
                }
                else
                {
                    // Portal/mirror/other dynamic texture — not yet supported, skip.
                    continue;
                }
            }
            else if (!VK_Image_GetDescriptorInfo(img, &imgInfo))
            {
                // Image not yet on GPU (e.g. cubemap pending upload).
                // Log once per material to avoid per-frame spam, then use white fallback.
                static idStrList s_loggedFallbacks;
                idStr matName(mat->GetName());
                if (s_loggedFallbacks.Find(matName) < 0)
                {
                    s_loggedFallbacks.Append(matName);
                    common->Printf(
                        "VK DrawShaderPasses: fallback texture for mat='%s' stage=%d img='%s' lighting=%d blend=0x%x\n",
                        mat->GetName(), stageIdx, img->imgName.c_str(), (int)pStage->lighting,
                        pStage->drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS));
                }
                VK_Image_GetFallbackDescriptorInfo(&imgInfo);
            }

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
            VK_BuildSurfMVP(surf->space, mvp);

            uint32_t uboOffset = VK_AllocUBO();
            uint8_t *uboPtr = (uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset;
            VkGuiUBO *guiUbo = (VkGuiUBO *)uboPtr;
            memcpy(guiUbo->modelViewProjection, mvp, 64);
            memcpy(guiUbo->colorModulate, colorModulate, 16);
            memcpy(guiUbo->colorAdd, colorAdd, 16);

            // Texture coordinate transform
            if (pStage->texture.hasMatrix)
            {
                const textureStage_t &tex = pStage->texture;
                float s2 = regs[tex.matrix[0][2]];
                if (s2 < -40.f || s2 > 40.f)
                    s2 -= (float)(int)s2;
                float t2 = regs[tex.matrix[1][2]];
                if (t2 < -40.f || t2 > 40.f)
                    t2 -= (float)(int)t2;
                guiUbo->texMatrixS[0] = regs[tex.matrix[0][0]];
                guiUbo->texMatrixS[1] = regs[tex.matrix[0][1]];
                guiUbo->texMatrixS[2] = 0.f;
                guiUbo->texMatrixS[3] = s2;
                guiUbo->texMatrixT[0] = regs[tex.matrix[1][0]];
                guiUbo->texMatrixT[1] = regs[tex.matrix[1][1]];
                guiUbo->texMatrixT[2] = 0.f;
                guiUbo->texMatrixT[3] = t2;
            }
            else
            {
                guiUbo->texMatrixS[0] = 1.f;
                guiUbo->texMatrixS[1] = 0.f;
                guiUbo->texMatrixS[2] = 0.f;
                guiUbo->texMatrixS[3] = 0.f;
                guiUbo->texMatrixT[0] = 0.f;
                guiUbo->texMatrixT[1] = 1.f;
                guiUbo->texMatrixT[2] = 0.f;
                guiUbo->texMatrixT[3] = 0.f;
            }

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

            // Select (or lazily create) a pipeline matching the blend state.
            // 3D world surfaces (GLS_DEPTHFUNC_ALWAYS not set) need depth testing so
            // they respect the depth prepass and don't render through occluders or from
            // behind the camera.  2D GUI surfaces (GLS_DEPTHFUNC_ALWAYS) skip depth.
            extern VkPipeline VK_GetOrCreateGuiBlendPipeline(int drawStateBits, bool depthTest);
            const bool needDepth = !(pStage->drawStateBits & GLS_DEPTHFUNC_ALWAYS);
            VkPipeline pipeline = VK_GetOrCreateGuiBlendPipeline(pStage->drawStateBits, needDepth);
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
    extern bool VK_Image_GetDescriptorInfo(idImage *, VkDescriptorImageInfo *);
    extern void VK_Image_GetFallbackDescriptorInfo(VkDescriptorImageInfo *);

    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

    // Bind the opaque depth pipeline up front; switch to depthClipPipeline per-draw for MC_PERFORATED.
    VkPipeline activePipeline = VK_NULL_HANDLE;

    for (int i = 0; i < backEnd.viewDef->numDrawSurfs; i++)
    {
        const drawSurf_t *surf = backEnd.viewDef->drawSurfs[i];
        if (!surf || !surf->material || !surf->geo)
            continue;

        const idMaterial *mat = surf->material;
        const materialCoverage_t coverage = mat->Coverage();

        // Translucent surfaces never write depth.
        if (coverage == MC_TRANSLUCENT)
            continue;
        // Only opaque and alpha-tested surfaces.
        if (coverage != MC_OPAQUE && coverage != MC_PERFORATED)
            continue;

        const srfTriangles_t *geo = surf->geo;
        if (!geo->indexes || geo->numIndexes <= 0 || geo->numVerts <= 0)
            continue;

        // --- Vertex buffer ---
        VkBuffer vertBuf;
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
                    vertBuf = dataRings[vk.currentFrame].buffer;
                    haveVerts = true;
                }
            }
        }
        if (!haveVerts)
            continue;

        // --- Index buffer ---
        VkDeviceSize idxSize = (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t);
        VkDeviceSize idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
        if (idxOffset == VK_WHOLE_SIZE)
            continue;
        memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, geo->indexes, (size_t)idxSize);

        float mvp[16];
        VK_BuildSurfMVP(surf->space, mvp);

        // Helper lambda to allocate a descriptor set and record one depth draw.
        // imgInfo: texture to bind at binding 1.
        // alphaThreshold: value written to colorAdd[3]; 0 means no clip (opaque pipeline).
        // useClipPipeline: selects depthClipPipeline vs depthPipeline.
        auto drawDepthSurf = [&](VkDescriptorImageInfo imgInfo, float alphaThreshold, bool useClipPipeline) {
            uint32_t uboOffset = VK_AllocUBO();
            VkGuiUBO *ubo = (VkGuiUBO *)((uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset);
            memcpy(ubo->modelViewProjection, mvp, 64);
            ubo->colorModulate[0] = ubo->colorModulate[1] = ubo->colorModulate[2] = ubo->colorModulate[3] = 1.f;
            ubo->colorAdd[0] = ubo->colorAdd[1] = ubo->colorAdd[2] = 0.f;
            ubo->colorAdd[3] = alphaThreshold;
            // Identity texture matrix — depth prepass doesn't need UV animation
            ubo->texMatrixS[0] = 1.f;
            ubo->texMatrixS[1] = 0.f;
            ubo->texMatrixS[2] = 0.f;
            ubo->texMatrixS[3] = 0.f;
            ubo->texMatrixT[0] = 0.f;
            ubo->texMatrixT[1] = 1.f;
            ubo->texMatrixT[2] = 0.f;
            ubo->texMatrixT[3] = 0.f;

            VkDescriptorSetAllocateInfo dsAlloc = {};
            dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAlloc.descriptorPool = vkPipes.descPools[vk.currentFrame];
            dsAlloc.descriptorSetCount = 1;
            dsAlloc.pSetLayouts = &vkPipes.guiDescLayout;

            VkDescriptorSet ds;
            if (vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds) != VK_SUCCESS)
                return;

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

            VkPipeline pipe = useClipPipeline ? vkPipes.depthClipPipeline : vkPipes.depthPipeline;
            if (pipe != activePipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                activePipeline = pipe;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.guiLayout, 0, 1, &ds, 0, NULL);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
            vkCmdBindIndexBuffer(cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);
            vkCmdDrawIndexed(cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0);
        };

        if (coverage == MC_OPAQUE)
        {
            VkDescriptorImageInfo imgInfo = {};
            VK_Image_GetFallbackDescriptorInfo(&imgInfo);
            drawDepthSurf(imgInfo, 0.f, false);
        }
        else // MC_PERFORATED
        {
            const float *regs = surf->shaderRegisters;
            bool didDraw = false;
            for (int si = 0; si < mat->GetNumStages(); si++)
            {
                const shaderStage_t *pStage = mat->GetStage(si);
                if (!pStage->hasAlphaTest)
                    continue;
                if (!regs[pStage->conditionRegister])
                    continue;
                float alphaScale = regs[pStage->color.registers[3]];
                if (alphaScale <= 0.f)
                    continue;

                VkDescriptorImageInfo imgInfo = {};
                if (!pStage->texture.image || !VK_Image_GetDescriptorInfo(pStage->texture.image, &imgInfo))
                    VK_Image_GetFallbackDescriptorInfo(&imgInfo);

                float threshold = regs[pStage->alphaTestRegister];
                drawDepthSurf(imgInfo, threshold, true);
                didDraw = true;
            }
            // If no alpha-test stage was active, fall back to opaque depth write.
            if (!didDraw)
            {
                VkDescriptorImageInfo imgInfo = {};
                VK_Image_GetFallbackDescriptorInfo(&imgInfo);
                drawDepthSurf(imgInfo, 0.f, false);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Shadow UBO layout (matches shadow.vert ShadowParams block)
struct VkShadowUBO
{
    float lightOrigin[4]; // vec4 — local-space light origin (w unused, set 0)
    float mvp[16];        // mat4 — model-view-projection
}; // 80 bytes — fits in the 384-byte UBO ring stride

// Fog UBO layout (matches fog.vert/frag FogParams block, std140)
// Offsets: mvp=0, texGen0S=64, texGen0T=80, texGen1S=96, texGen1T=112, color=128  total=144
struct VkFogUBO
{
    float mvp[16];     // mat4 u_MVP
    float texGen0S[4]; // vec4 u_TexGen0S
    float texGen0T[4]; // vec4 u_TexGen0T
    float texGen1S[4]; // vec4 u_TexGen1S
    float texGen1T[4]; // vec4 u_TexGen1T
    float color[4];    // vec4 u_Color
}; // 144 bytes

// Blend-light UBO layout (matches blendlight.vert/frag BlendParams block, std140)
// Offsets: mvp=0, texGen0S=64, texGen0T=80, texGen0Q=96, texGen1S=112, color=128  total=144
struct VkBlendUBO
{
    float mvp[16];     // mat4 u_MVP
    float texGen0S[4]; // vec4 u_TexGen0S  (light proj S)
    float texGen0T[4]; // vec4 u_TexGen0T  (light proj T)
    float texGen0Q[4]; // vec4 u_TexGen0Q  (light proj Q, for perspective divide)
    float texGen1S[4]; // vec4 u_TexGen1S  (falloff S)
    float color[4];    // vec4 u_Color
}; // 144 bytes

// ---------------------------------------------------------------------------
// VK_RB_DrawShadowSurface
// Records one shadow volume draw.
//
// This mirrors the GL RB_T_Shadow decision tree:
// - choose numIndexes/numShadowIndexesNoFrontCaps/numShadowIndexesNoCaps per surface
// - choose Z-fail (inside) vs Z-pass (external) stencil pipeline
//
// Returns the VkPipeline that was bound (so the caller can rebind if needed).
// lightScissor: the current per-light scissor rect (already set on the command buffer).
// ---------------------------------------------------------------------------

static VkPipeline VK_RB_DrawShadowSurface(VkCommandBuffer cmd, const drawSurf_t *surf, const VkRect2D &lightScissor)
{
    extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);
    struct shadowBranchTrack_t
    {
        const srfTriangles_t *tri;
        const idRenderLightLocal *lightDef;
        const idMaterial *material;
        uint32_t signature;
        int lastSeenFrame;
        int lastLoggedFrame;
        bool valid;
    };
    struct shadowGeomTrack_t
    {
        const srfTriangles_t *tri;
        const idRenderLightLocal *lightDef;
        const idMaterial *material;
        uint32_t signature;
        uint32_t sampledIndexHash;
        int lastSeenFrame;
        int lastLoggedFrame;
        bool valid;
    };
    static const int SHADOW_BRANCH_TRACK_SIZE = 32768;
    static shadowBranchTrack_t s_branchTrack[SHADOW_BRANCH_TRACK_SIZE] = {};
    static shadowGeomTrack_t s_geomTrack[SHADOW_BRANCH_TRACK_SIZE] = {};
    static const viewDef_t *s_loggedView = NULL;
    static const viewLight_t *s_loggedLight = NULL;

    const srfTriangles_t *tri = surf->geo;
    if (!tri || !tri->shadowCache || !tri->indexes || tri->numIndexes <= 0)
        return VK_NULL_HANDLE;

    // Match GL's external-shadow logic so we choose the same index set and
    // stencil method (Z-fail when inside, Z-pass when external).
    int numDrawIndexes = 0;
    bool external = false;

    const char *indexSet = "full";
    const char *decisionPath = "full-default";

    if (r_vkShadowStableMode.GetBool())
    {
        // Stabilization mode: avoid view-dependent external/no-cap branch switching.
        numDrawIndexes = tri->numIndexes;
        indexSet = "full (stable)";
        decisionPath = "stable-force-full";
        external = false;
    }
    else if (!r_useExternalShadows.GetInteger())
    {
        numDrawIndexes = tri->numIndexes;
        indexSet = "full";
        decisionPath = "external-shadows-disabled";
    }
    else if (r_useExternalShadows.GetInteger() == 2)
    {
        // Debug/testing path from GL: force no-caps index set.
        numDrawIndexes = tri->numShadowIndexesNoCaps;
        indexSet = "no-caps (forced)";
        decisionPath = "external-shadows-force-no-caps";
    }
    else if (!(surf->dsFlags & DSF_VIEW_INSIDE_SHADOW))
    {
        // Viewer outside the shadow projection: caps are not needed.
        numDrawIndexes = tri->numShadowIndexesNoCaps;
        indexSet = "no-caps";
        decisionPath = "outside-shadow";
        external = true;
    }
    else if (!backEnd.vLight->viewInsideLight && !(tri->shadowCapPlaneBits & SHADOW_CAP_INFINITE))
    {
        // Inside shadow projection, outside light, finite shadow volume.
        // May omit front caps (or all caps) depending on visible cap planes.
        if (backEnd.vLight->viewSeesShadowPlaneBits & tri->shadowCapPlaneBits)
        {
            numDrawIndexes = tri->numShadowIndexesNoFrontCaps;
            indexSet = "no-front-caps";
            decisionPath = "inside-shadow-outside-light-visible-cap-planes";
        }
        else
        {
            numDrawIndexes = tri->numShadowIndexesNoCaps;
            indexSet = "no-caps";
            decisionPath = "inside-shadow-outside-light-hidden-cap-planes";
        }

        external = true;
    }
    else
    {
        numDrawIndexes = tri->numIndexes;
        indexSet = "full";
        decisionPath = "full-default";
    }

    if (numDrawIndexes <= 0)
        return VK_NULL_HANDLE;

    const bool mirrorView = backEnd.viewDef && backEnd.viewDef->isMirror;
    const bool flipShadowOps = r_vkShadowFlipOps.GetBool();
    const bool effectiveMirrorOps = mirrorView ^ flipShadowOps;
    VkPipeline shadowPipe;
    if (external)
        shadowPipe = effectiveMirrorOps ? vkPipes.shadowPipelineZPassMirror : vkPipes.shadowPipelineZPass;
    else
        shadowPipe = effectiveMirrorOps ? vkPipes.shadowPipelineZFailMirror : vkPipes.shadowPipelineZFail;
    const char *pipeTag =
        external ? (effectiveMirrorOps ? "zpass-mirror" : "zpass") : (effectiveMirrorOps ? "zfail-mirror" : "zfail");
    if (!shadowPipe)
        return VK_NULL_HANDLE;

    const float shadowBiasConst = -r_shadowPolygonOffset.GetFloat();
    const float shadowBiasSlope = r_shadowPolygonFactor.GetFloat();

    const int indexSetId = (indexSet[0] == 'f') ? 0 : (indexSet[3] == 'f' ? 1 : 2); // full / no-front-caps / no-caps
    uint32_t branchSig = 0;
    branchSig |= external ? (1u << 0) : 0u;
    branchSig |= mirrorView ? (1u << 1) : 0u;
    branchSig |= effectiveMirrorOps ? (1u << 2) : 0u;
    branchSig |= flipShadowOps ? (1u << 3) : 0u;
    branchSig |= (surf->dsFlags & DSF_VIEW_INSIDE_SHADOW) ? (1u << 4) : 0u;
    branchSig |= backEnd.vLight->viewInsideLight ? (1u << 5) : 0u;
    branchSig |= (uint32_t)(indexSetId & 0x3) << 6;
    branchSig |= (uint32_t)(numDrawIndexes & 0xFFFF) << 8;

    const int logMode = r_vkLogShadowBranch.GetInteger();
    bool branchChangedThisDraw = false;
    if (logMode > 0)
    {
        if (s_loggedView != backEnd.viewDef)
        {
            s_loggedView = backEnd.viewDef;
            s_loggedLight = NULL;
        }

        const bool shouldLog = (logMode == 2) || ((logMode == 1) && (s_loggedLight != backEnd.vLight));
        if (shouldLog)
        {
            common->Printf("VK SHADOW BRANCH: mode=%s mirror=%d effectiveMirror=%d flipOps=%d indexSet=%s draw=%d "
                           "full=%d noFront=%d noCaps=%d ext=%d insideFlag=%d insideLight=%d capBits=0x%X "
                           "biasConst=%.3f biasSlope=%.3f\n",
                           external ? "zpass" : "zfail", mirrorView ? 1 : 0, effectiveMirrorOps ? 1 : 0,
                           flipShadowOps ? 1 : 0, indexSet, numDrawIndexes, tri->numIndexes,
                           tri->numShadowIndexesNoFrontCaps, tri->numShadowIndexesNoCaps, external ? 1 : 0,
                           (surf->dsFlags & DSF_VIEW_INSIDE_SHADOW) ? 1 : 0, backEnd.vLight->viewInsideLight ? 1 : 0,
                           tri->shadowCapPlaneBits, shadowBiasConst, shadowBiasSlope);
            s_loggedLight = backEnd.vLight;
        }

        // mode 3: only log when this light+surface branch/signature changes between frames.
        if (logMode >= 3)
        {

            const idRenderLightLocal *lightDef = backEnd.vLight ? backEnd.vLight->lightDef : NULL;
            const idMaterial *material = surf->material;
            const uintptr_t hTri = (uintptr_t)tri >> 4;
            const uintptr_t hLightDef = (uintptr_t)lightDef >> 4;
            const uintptr_t hMat = (uintptr_t)material >> 4;
            uint32_t slot = (uint32_t)((hTri ^ (hLightDef * 33u) ^ (hMat * 131u)) & (SHADOW_BRANCH_TRACK_SIZE - 1));
            int found = -1;
            int empty = -1;

            for (int probe = 0; probe < SHADOW_BRANCH_TRACK_SIZE; probe++)
            {
                const int i = (int)((slot + (uint32_t)probe) & (SHADOW_BRANCH_TRACK_SIZE - 1));
                if (!s_branchTrack[i].valid)
                {
                    if (empty < 0)
                        empty = i;
                    continue;
                }
                if (s_branchTrack[i].tri == tri && s_branchTrack[i].lightDef == lightDef &&
                    s_branchTrack[i].material == material)
                {
                    found = i;
                    break;
                }
            }

            const int trackIdx = (found >= 0) ? found : ((empty >= 0) ? empty : (int)slot);
            shadowBranchTrack_t &track = s_branchTrack[trackIdx];
            const bool firstSeen = !track.valid;
            const bool seenLastFrame = !firstSeen && (track.lastSeenFrame == tr.frameCount - 1);
            const bool changed = seenLastFrame && (track.signature != branchSig);

            if (changed && track.lastLoggedFrame != tr.frameCount)
            {
                const uint32_t delta = track.signature ^ branchSig;
                const int changedMode = (delta & (1u << 0)) ? 1 : 0;
                const int changedMirror = (delta & (1u << 1)) ? 1 : 0;
                const int changedEffMirror = (delta & (1u << 2)) ? 1 : 0;
                const int changedFlipOps = (delta & (1u << 3)) ? 1 : 0;
                const int changedInsideFlag = (delta & (1u << 4)) ? 1 : 0;
                const int changedInsideLight = (delta & (1u << 5)) ? 1 : 0;
                const int changedIndexSet = (delta & (0x3u << 6)) ? 1 : 0;
                const int changedDrawCount = (delta & (0xFFFFu << 8)) ? 1 : 0;
                const bool structuralChange = (changedMode | changedMirror | changedEffMirror | changedFlipOps |
                                               changedInsideFlag | changedInsideLight) != 0;

                // At normal debug levels, suppress branch changes that are only
                // index-set/draw-count churn to keep logs readable during camera sweeps.
                if (!structuralChange && logMode < 5)
                {
                    track.lastLoggedFrame = tr.frameCount;
                }
                else
                {
                    uint32_t preHash = 2166136261u;
                    if (numDrawIndexes > 0)
                    {
                        const int sampleCount = 64;
                        const int step = (numDrawIndexes / sampleCount > 1) ? (numDrawIndexes / sampleCount) : 1;
                        for (int ii = 0; ii < numDrawIndexes; ii += step)
                        {
                            preHash ^= (uint32_t)tri->indexes[ii];
                            preHash *= 16777619u;
                        }
                        preHash ^= (uint32_t)tri->indexes[numDrawIndexes - 1];
                        preHash *= 16777619u;
                    }
                    common->Printf(
                        "VK SHADOW BRANCH CHANGE: tri=%p lightDef=%p frame=%d prevSig=0x%08X newSig=0x%08X "
                        "d(mode=%d mirror=%d effMirror=%d flipOps=%d inside=%d insideLight=%d indexSet=%d draw=%d) "
                        "mode=%s indexSet=%s draw=%d hash=0x%08X full=%d noFront=%d noCaps=%d insideFlag=%d "
                        "insideLight=%d "
                        "mirror=%d effectiveMirror=%d phase=%s scissor=(%d,%d %u,%u)\n",
                        tri, lightDef, tr.frameCount, track.signature, branchSig, changedMode, changedMirror,
                        changedEffMirror, changedFlipOps, changedInsideFlag, changedInsideLight, changedIndexSet,
                        changedDrawCount, external ? "zpass" : "zfail", indexSet, numDrawIndexes, preHash,
                        tri->numIndexes, tri->numShadowIndexesNoFrontCaps, tri->numShadowIndexesNoCaps,
                        (surf->dsFlags & DSF_VIEW_INSIDE_SHADOW) ? 1 : 0, backEnd.vLight->viewInsideLight ? 1 : 0,
                        mirrorView ? 1 : 0, effectiveMirrorOps ? 1 : 0, s_shadowPhaseTag, lightScissor.offset.x,
                        lightScissor.offset.y, (unsigned int)lightScissor.extent.width,
                        (unsigned int)lightScissor.extent.height);

                    track.lastLoggedFrame = tr.frameCount;
                    branchChangedThisDraw = true;
                }

                if (changedInsideFlag && logMode >= 4)
                {
                    uint32_t insideHash = 2166136261u;
                    if (numDrawIndexes > 0)
                    {
                        const int sampleCount = 64;
                        const int step = (numDrawIndexes / sampleCount > 1) ? (numDrawIndexes / sampleCount) : 1;
                        for (int ii = 0; ii < numDrawIndexes; ii += step)
                        {
                            insideHash ^= (uint32_t)tri->indexes[ii];
                            insideHash *= 16777619u;
                        }
                        insideHash ^= (uint32_t)tri->indexes[numDrawIndexes - 1];
                        insideHash *= 16777619u;
                    }
                    const int prevInsideFlag = (track.signature & (1u << 4)) ? 1 : 0;
                    const int newInsideFlag = (branchSig & (1u << 4)) ? 1 : 0;
                    common->Printf(
                        "VK SHADOW INSIDE FLAG CHANGE: tri=%p lightDef=%p frame=%d prevInside=%d newInside=%d "
                        "dsFlags=0x%X viewInsideLight=%d seesBits=0x%X capBits=0x%X capInfinite=%d "
                        "path=%s indexSet=%s pipe=%s mirror=%d flipOps=%d effectiveMirror=%d phase=%s "
                        "draw=%d hash=0x%08X scissor=(%d,%d %u,%u)\n",
                        tri, lightDef, tr.frameCount, prevInsideFlag, newInsideFlag, (unsigned int)surf->dsFlags,
                        backEnd.vLight->viewInsideLight ? 1 : 0, backEnd.vLight->viewSeesShadowPlaneBits,
                        tri->shadowCapPlaneBits, (tri->shadowCapPlaneBits & SHADOW_CAP_INFINITE) ? 1 : 0, decisionPath,
                        indexSet, pipeTag, mirrorView ? 1 : 0, flipShadowOps ? 1 : 0, effectiveMirrorOps ? 1 : 0,
                        s_shadowPhaseTag, numDrawIndexes, insideHash, lightScissor.offset.x, lightScissor.offset.y,
                        (unsigned int)lightScissor.extent.width, (unsigned int)lightScissor.extent.height);
                }

                if (logMode >= 4 && (changedInsideFlag || changedInsideLight))
                {
                    const int prevInsideFlag = (track.signature & (1u << 4)) ? 1 : 0;
                    const int prevInsideLight = (track.signature & (1u << 5)) ? 1 : 0;
                    const int newInsideFlag = (branchSig & (1u << 4)) ? 1 : 0;
                    const int newInsideLight = (branchSig & (1u << 5)) ? 1 : 0;
                    common->Printf(
                        "VK SHADOW FLICKER CANDIDATE: tri=%p lightDef=%p frame=%d dInside=%d dInsideLight=%d "
                        "inside=%d->%d insideLight=%d->%d pipe=%s mode=%s path=%s indexSet=%s "
                        "mirror=%d flipOps=%d effectiveMirror=%d seesBits=0x%X capBits=0x%X phase=%s "
                        "scissor=(%d,%d %u,%u)\n",
                        tri, lightDef, tr.frameCount, changedInsideFlag, changedInsideLight, prevInsideFlag,
                        newInsideFlag, prevInsideLight, newInsideLight, pipeTag, external ? "zpass" : "zfail",
                        decisionPath, indexSet, mirrorView ? 1 : 0, flipShadowOps ? 1 : 0, effectiveMirrorOps ? 1 : 0,
                        backEnd.vLight->viewSeesShadowPlaneBits, tri->shadowCapPlaneBits, s_shadowPhaseTag,
                        lightScissor.offset.x, lightScissor.offset.y, (unsigned int)lightScissor.extent.width,
                        (unsigned int)lightScissor.extent.height);
                }
            }

            track.tri = tri;
            track.lightDef = lightDef;
            track.material = material;
            track.signature = branchSig;
            track.lastSeenFrame = tr.frameCount;
            track.valid = true;
        }
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipe);
    // GL parity: qglPolygonOffset(r_shadowPolygonFactor, -r_shadowPolygonOffset)
    // Vulkan mapping: slopeFactor = factor, constantFactor = units.
    vkCmdSetDepthBias(cmd, shadowBiasConst, 0.0f, shadowBiasSlope);

    const bool forceCpuShadowGeometry = r_vkShadowStableMode.GetBool();
    const int geomLogMode = r_vkLogShadowGeom.GetInteger();
    bool usingCpuVerts = false;
    bool usingCpuIdx = false;

    int maxShadowVertForDraw = -1;
    if (geomLogMode > 0 || branchChangedThisDraw)
    {
        for (int i = 0; i < numDrawIndexes; i++)
        {
            const int idx = tri->indexes[i];
            if (idx > maxShadowVertForDraw)
                maxShadowVertForDraw = idx;
        }
    }

    // Shadow vertices: shadowCache_t = idVec4 (16 bytes each)
    VkBuffer vertBuf;
    VkDeviceSize vertOffset;
    bool haveVerts = false;
    // Stable mode should only force CPU geometry when an explicit CPU shadow array
    // is present (dynamic/private shadows). For static cached shadows, prefer the
    // cache buffers to avoid stale CPU index/vertex divergence.
    const bool forceCpuShadowUpload = forceCpuShadowGeometry && (tri->shadowVertexes != NULL);

    if (!forceCpuShadowUpload && VK_VertexCache_GetBuffer(tri->shadowCache, &vertBuf, &vertOffset))
    {
        haveVerts = true;
    }
    else
    {
        // Prefer stable CPU shadow vertices when present. Dynamic/private shadow volumes
        // keep authoritative data in tri->shadowVertexes and can be volatile in cache memory.
        const shadowCache_t *cpuShadowVerts = tri->shadowVertexes;
        if (!cpuShadowVerts)
            cpuShadowVerts = (const shadowCache_t *)vertexCache.Position(tri->shadowCache);

        const void *cpuVerts = cpuShadowVerts;
        if (cpuVerts)
        {
            // Size shadow vertex upload from the actual index range used by this draw.
            // Heuristic sizing (e.g. numVerts*2) can under/over-copy and produce malformed shadow wedges.
            int maxShadowVert = -1;
            for (int i = 0; i < numDrawIndexes; i++)
            {
                const int idx = tri->indexes[i];
                if (idx > maxShadowVert)
                    maxShadowVert = idx;
            }

            int numShadowVerts = maxShadowVert + 1;

            // Private shadow arrays are explicitly sized by tri->numVerts.
            if (tri->shadowVertexes)
            {
                if (numShadowVerts > tri->numVerts)
                {
                    static bool s_warnedShadowIndexRange = false;
                    if (!s_warnedShadowIndexRange)
                    {
                        common->Warning("VK: shadow index range exceeds shadowVertexes size (%d > %d)", numShadowVerts,
                                        tri->numVerts);
                        s_warnedShadowIndexRange = true;
                    }
                    return VK_NULL_HANDLE;
                }
            }

            if (numShadowVerts <= 0)
                return VK_NULL_HANDLE;

            VkDeviceSize sz = (VkDeviceSize)numShadowVerts * sizeof(shadowCache_t);
            vertOffset = VK_AllocDataRing(sz, sizeof(float));
            if (vertOffset != VK_WHOLE_SIZE)
            {
                memcpy((byte *)dataRings[vk.currentFrame].mapped + vertOffset, cpuVerts, (size_t)sz);
                vertBuf = dataRings[vk.currentFrame].buffer;
                haveVerts = true;
                usingCpuVerts = true;
            }
        }
    }
    if (!haveVerts)
        return VK_NULL_HANDLE;

    // Index data: if we forced CPU shadow vertices, keep indices on the same source
    // (CPU) for consistency. Otherwise prefer resident index cache.
    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    VkBuffer idxBuf = VK_NULL_HANDLE;
    VkDeviceSize idxOffset = 0;
    if (!forceCpuShadowUpload && VK_VertexCache_GetBuffer(tri->indexCache, &idxBuf, &idxOffset))
    {
        // Use resident cache buffer.
    }
    else
    {
        VkDeviceSize idxSize = (VkDeviceSize)numDrawIndexes * sizeof(glIndex_t);
        idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
        if (idxOffset == VK_WHOLE_SIZE)
            return VK_NULL_HANDLE;
        memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, tri->indexes, (size_t)idxSize);
        idxBuf = dataRings[vk.currentFrame].buffer;
        usingCpuIdx = true;
    }

    // Sampled index-content hash: catches index mutation even when draw counts stay constant.
    uint32_t sampledIndexHash = 2166136261u;
    if (numDrawIndexes > 0)
    {
        const int sampleCount = 64;
        const int step = (numDrawIndexes / sampleCount > 1) ? (numDrawIndexes / sampleCount) : 1;
        for (int i = 0; i < numDrawIndexes; i += step)
        {
            sampledIndexHash ^= (uint32_t)tri->indexes[i];
            sampledIndexHash *= 16777619u;
        }
        sampledIndexHash ^= (uint32_t)tri->indexes[numDrawIndexes - 1];
        sampledIndexHash *= 16777619u;
    }

    if (geomLogMode > 0)
    {
        const idRenderLightLocal *lightDef = backEnd.vLight ? backEnd.vLight->lightDef : NULL;
        const idMaterial *material = surf->material;
        const uintptr_t hTri = (uintptr_t)tri >> 4;
        const uintptr_t hLightDef = (uintptr_t)lightDef >> 4;
        const uintptr_t hMat = (uintptr_t)material >> 4;
        const uint32_t slot = (uint32_t)((hTri ^ (hLightDef * 33u) ^ (hMat * 131u)) & (SHADOW_BRANCH_TRACK_SIZE - 1));

        uint32_t geomSig = 0;
        geomSig |= usingCpuVerts ? (1u << 0) : 0u;
        geomSig |= usingCpuIdx ? (1u << 1) : 0u;
        geomSig |= (tri->shadowVertexes != NULL) ? (1u << 2) : 0u;
        geomSig |= (uint32_t)(numDrawIndexes & 0xFFFF) << 8;
        const uint32_t clampedShadowVerts = (uint32_t)((maxShadowVertForDraw + 1) < 0 ? 0 : (maxShadowVertForDraw + 1));
        geomSig |= (clampedShadowVerts & 0xFFu) << 24;

        int found = -1;
        int empty = -1;
        for (int probe = 0; probe < SHADOW_BRANCH_TRACK_SIZE; probe++)
        {
            const int i = (int)((slot + (uint32_t)probe) & (SHADOW_BRANCH_TRACK_SIZE - 1));
            if (!s_geomTrack[i].valid)
            {
                if (empty < 0)
                    empty = i;
                continue;
            }
            if (s_geomTrack[i].tri == tri && s_geomTrack[i].lightDef == lightDef && s_geomTrack[i].material == material)
            {
                found = i;
                break;
            }
        }

        const int trackIdx = (found >= 0) ? found : ((empty >= 0) ? empty : (int)slot);
        shadowGeomTrack_t &track = s_geomTrack[trackIdx];
        const bool firstSeen = !track.valid;
        const bool seenLastFrame = !firstSeen && (track.lastSeenFrame == tr.frameCount - 1);
        const bool changed =
            seenLastFrame && ((track.signature != geomSig) || (track.sampledIndexHash != sampledIndexHash));
        const bool shouldLogGeom = (geomLogMode >= 2) || changed;

        if (shouldLogGeom && track.lastLoggedFrame != tr.frameCount)
        {
            const uint32_t delta = track.signature ^ geomSig;
            const int dCpuVerts = (delta & (1u << 0)) ? 1 : 0;
            const int dCpuIdx = (delta & (1u << 1)) ? 1 : 0;
            const int dHasShadowVerts = (delta & (1u << 2)) ? 1 : 0;
            const int dDraw = (delta & (0xFFFFu << 8)) ? 1 : 0;
            const int dVertRange = (delta & (0xFFu << 24)) ? 1 : 0;
            const int dHash = (track.sampledIndexHash != sampledIndexHash) ? 1 : 0;
            common->Printf("VK SHADOW GEOM: tri=%p lightDef=%p frame=%d changed=%d src(v=%s i=%s cpuShadow=%d) "
                           "draw=%d maxVert=%d hash=0x%08X full=%d noFront=%d noCaps=%d d(vsrc=%d isrc=%d cpuShadow=%d "
                           "draw=%d maxVert=%d hash=%d)\n",
                           tri, lightDef, tr.frameCount, changed ? 1 : 0, usingCpuVerts ? "cpu" : "cache",
                           usingCpuIdx ? "cpu" : "cache", tri->shadowVertexes ? 1 : 0, numDrawIndexes,
                           maxShadowVertForDraw + 1, sampledIndexHash, tri->numIndexes,
                           tri->numShadowIndexesNoFrontCaps, tri->numShadowIndexesNoCaps, dCpuVerts, dCpuIdx,
                           dHasShadowVerts, dDraw, dVertRange, dHash);
            track.lastLoggedFrame = tr.frameCount;
        }

        track.tri = tri;
        track.lightDef = lightDef;
        track.material = material;
        track.signature = geomSig;
        track.sampledIndexHash = sampledIndexHash;
        track.lastSeenFrame = tr.frameCount;
        track.valid = true;
    }
    else if (branchChangedThisDraw)
    {
        const idRenderLightLocal *lightDef = backEnd.vLight ? backEnd.vLight->lightDef : NULL;
        common->Printf("VK SHADOW GEOM SNAP: tri=%p lightDef=%p frame=%d src(v=%s i=%s cpuShadow=%d) "
                       "draw=%d maxVert=%d hash=0x%08X full=%d noFront=%d noCaps=%d\n",
                       tri, lightDef, tr.frameCount, usingCpuVerts ? "cpu" : "cache", usingCpuIdx ? "cpu" : "cache",
                       tri->shadowVertexes ? 1 : 0, numDrawIndexes, maxShadowVertForDraw + 1, sampledIndexHash,
                       tri->numIndexes, tri->numShadowIndexesNoFrontCaps, tri->numShadowIndexesNoCaps);
    }

    // Light origin in model local space
    idVec3 localLight;
    R_GlobalPointToLocal(surf->space->modelMatrix, backEnd.vLight->globalLightOrigin, localLight);

    // MVP matrix
    float mvp[16];
    VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp);

    // Shadow UBO
    uint32_t uboOffset = VK_AllocUBO();
    VkShadowUBO *ubo = (VkShadowUBO *)((uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset);
    ubo->lightOrigin[0] = localLight.x;
    ubo->lightOrigin[1] = localLight.y;
    ubo->lightOrigin[2] = localLight.z;
    ubo->lightOrigin[3] = 0.f;
    memcpy(ubo->mvp, mvp, 64);

    // Descriptor set (shadowDescLayout: binding 0 = UBO only)
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkPipes.descPools[vk.currentFrame];
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &vkPipes.shadowDescLayout;

    VkDescriptorSet ds;
    if (vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = uboRings[vk.currentFrame].buffer;
    bufInfo.offset = uboOffset;
    bufInfo.range = sizeof(VkShadowUBO);

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ds;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.shadowLayout, 0, 1, &ds, 0, NULL);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
    vkCmdBindIndexBuffer(cmd, idxBuf, idxOffset, idxType);

    vkCmdDrawIndexed(cmd, (uint32_t)numDrawIndexes, 1, 0, 0, 0);

    return shadowPipe;
}

// ---------------------------------------------------------------------------
// VK_RB_DrawInteractions - per-light interaction loop
// Mirrors RB_ARB2_DrawInteractions / RB_GLSL_DrawInteractions
// ---------------------------------------------------------------------------

static void VK_RB_DrawInteractions(VkCommandBuffer cmd)
{
    s_cmd = cmd;

    struct lightPassTrack_t
    {
        const idRenderLightLocal *lightDef;
        uint32_t signature;
        int lastSeenFrame;
        int lastLoggedFrame;
        bool valid;
    };
    static const int LIGHT_PASS_TRACK_SIZE = 8192;
    static lightPassTrack_t s_lightPassTrack[LIGHT_PASS_TRACK_SIZE] = {};

    auto CountAndHashSurfList = [](const drawSurf_t *head, int &outCount, uint32_t &outHash) {
        outCount = 0;
        outHash = 2166136261u;
        for (const drawSurf_t *s = head; s; s = s->nextOnLight)
        {
            outCount++;
            const srfTriangles_t *tri = s->geo;
            const uintptr_t hSurf = (uintptr_t)s >> 4;
            const uintptr_t hTri = (uintptr_t)tri >> 4;
            const uintptr_t hMat = (uintptr_t)s->material >> 4;
            outHash ^= (uint32_t)(hSurf ^ (hTri * 33u) ^ (hMat * 131u));
            outHash *= 16777619u;
            if (tri)
            {
                outHash ^= (uint32_t)tri->numIndexes;
                outHash *= 16777619u;
            }
        }
    };

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

        // Set scissor for this light.
        // vLight->scissorRect uses OpenGL Y-up window coordinates (y=0 at bottom).
        // Vulkan scissor rects use Y-down coordinates (y=0 at top).
        // Convert: vulkan_top = framebuffer_height - 1 - opengl_top_edge
        // Keep a copy of the rect so we can use it in vkCmdClearAttachments below.
        VkRect2D lightScissor = {{0, 0}, vk.swapchainExtent}; // default: full framebuffer

        if (r_useScissor.GetBool() && !r_vkLightFullScissor.GetBool())
        {
            int h = (int)vk.swapchainExtent.height;
            int absX1 = backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1;
            int absY1 = backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1; // OpenGL bottom edge
            int absY2 = backEnd.viewDef->viewport.y1 + vLight->scissorRect.y2; // OpenGL top edge

            lightScissor.offset.x = absX1;
            lightScissor.offset.y = h - 1 - absY2; // flip Y: OpenGL top edge -> Vulkan top edge
            lightScissor.extent.width = vLight->scissorRect.x2 - vLight->scissorRect.x1 + 1;
            lightScissor.extent.height = absY2 - absY1 + 1;
            vkCmdSetScissor(cmd, 0, 1, &lightScissor);
        }

        if (r_vkLogShadowBranch.GetInteger() >= 5)
        {
            int countGS = 0, countLS = 0, countGI = 0, countLI = 0, countTI = 0;
            uint32_t hashGS = 0, hashLS = 0, hashGI = 0, hashLI = 0, hashTI = 0;
            CountAndHashSurfList(vLight->globalShadows, countGS, hashGS);
            CountAndHashSurfList(vLight->localShadows, countLS, hashLS);
            CountAndHashSurfList(vLight->globalInteractions, countGI, hashGI);
            CountAndHashSurfList(vLight->localInteractions, countLI, hashLI);
            CountAndHashSurfList(vLight->translucentInteractions, countTI, hashTI);

            const idRenderLightLocal *lightDef = vLight->lightDef;
            const uintptr_t hLightDef = (uintptr_t)lightDef >> 4;
            const uint32_t slot = (uint32_t)(hLightDef & (LIGHT_PASS_TRACK_SIZE - 1));
            int found = -1;
            int empty = -1;
            for (int probe = 0; probe < LIGHT_PASS_TRACK_SIZE; probe++)
            {
                const int i = (int)((slot + (uint32_t)probe) & (LIGHT_PASS_TRACK_SIZE - 1));
                if (!s_lightPassTrack[i].valid)
                {
                    if (empty < 0)
                        empty = i;
                    continue;
                }
                if (s_lightPassTrack[i].lightDef == lightDef)
                {
                    found = i;
                    break;
                }
            }

            const int trackIdx = (found >= 0) ? found : ((empty >= 0) ? empty : (int)slot);
            lightPassTrack_t &track = s_lightPassTrack[trackIdx];
            const bool firstSeen = !track.valid;

            uint32_t passSig = 2166136261u;
            passSig ^= (uint32_t)lightScissor.offset.x;
            passSig *= 16777619u;
            passSig ^= (uint32_t)lightScissor.offset.y;
            passSig *= 16777619u;
            passSig ^= (uint32_t)lightScissor.extent.width;
            passSig *= 16777619u;
            passSig ^= (uint32_t)lightScissor.extent.height;
            passSig *= 16777619u;
            passSig ^= (uint32_t)countGS;
            passSig *= 16777619u;
            passSig ^= (uint32_t)countLS;
            passSig *= 16777619u;
            passSig ^= (uint32_t)countGI;
            passSig *= 16777619u;
            passSig ^= (uint32_t)countLI;
            passSig *= 16777619u;
            passSig ^= (uint32_t)countTI;
            passSig *= 16777619u;
            passSig ^= hashGS;
            passSig *= 16777619u;
            passSig ^= hashLS;
            passSig *= 16777619u;
            passSig ^= hashGI;
            passSig *= 16777619u;
            passSig ^= hashLI;
            passSig *= 16777619u;
            passSig ^= hashTI;
            passSig *= 16777619u;
            passSig ^= vLight->viewInsideLight ? 0x9E3779B9u : 0u;
            passSig *= 16777619u;
            passSig ^= (uint32_t)vLight->viewSeesShadowPlaneBits;
            passSig *= 16777619u;

            const bool changed = !firstSeen && (track.signature != passSig);
            if (changed && track.lastLoggedFrame != tr.frameCount)
            {
                common->Printf(
                    "VK LIGHT PASS CHANGE: lightDef=%p frame=%d prevSig=0x%08X newSig=0x%08X "
                    "insideLight=%d seesBits=0x%X scissor=(%d,%d %u,%u) "
                    "counts(gs=%d ls=%d gi=%d li=%d ti=%d) hashes(gs=0x%08X ls=0x%08X gi=0x%08X li=0x%08X ti=0x%08X)\n",
                    lightDef, tr.frameCount, track.signature, passSig, vLight->viewInsideLight ? 1 : 0,
                    vLight->viewSeesShadowPlaneBits, lightScissor.offset.x, lightScissor.offset.y,
                    (unsigned int)lightScissor.extent.width, (unsigned int)lightScissor.extent.height, countGS, countLS,
                    countGI, countLI, countTI, hashGS, hashLS, hashGI, hashLI, hashTI);
                track.lastLoggedFrame = tr.frameCount;
            }

            track.lightDef = lightDef;
            track.signature = passSig;
            track.lastSeenFrame = tr.frameCount;
            track.valid = true;
        }

        // Clear stencil to 128 within this light's scissor rect before processing it.
        // Mirrors the GL path: qglClear(GL_STENCIL_BUFFER_BIT) before each light with shadows,
        // or qglStencilFunc(GL_ALWAYS,...) for lights without shadows.
        // Our interaction pipeline uses EQUAL 128, so clearing to 128 = "always pass" for
        // unlit surfaces, while shadow volumes will re-mark their areas correctly.
        // Without this, shadow volumes from light N bleed into light N+1's stencil test,
        // causing black trailing regions behind moving lights.
        {
            VkClearAttachment clearAtt = {};
            clearAtt.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            clearAtt.clearValue.depthStencil.stencil = 128;

            VkClearRect clearRect = {};
            clearRect.rect = lightScissor;
            clearRect.baseArrayLayer = 0;
            clearRect.layerCount = 1;

            vkCmdClearAttachments(cmd, 1, &clearAtt, 1, &clearRect);
        }

        // Stencil shadow volumes — only when RT shadows are unavailable or disabled.
        // When RT is active, shadows are applied per-pixel in the interaction shader
        // via shadowMaskSampler. Running both would produce double-shadowing.
#ifdef DHEWM3_RAYTRACING
        const bool useStencilShadows = !VK_RTShadowsEnabled();
#else
        const bool useStencilShadows = true;
#endif
        const bool useFullShadowScissor = r_vkShadowFullScissor.GetBool();
        const VkRect2D shadowScissor = useFullShadowScissor ? VkRect2D{{0, 0}, vk.swapchainExtent} : lightScissor;
        // Interaction/shadow ordering mirrors RB_ARB2_DrawInteractions (draw_interaction.cpp):
        //   1. global shadow volumes (affect all surfaces including local)
        //   2. local interactions (unshadowed by global volumes — local means near-light)
        //   3. local shadow volumes (only affect global interactions)
        //   4. global interactions (shadowed by both global and local volumes)
        // This ordering is required for correctness: local interactions must be drawn
        // BEFORE local shadows so they are not incorrectly darkened by them.

        // Step 1: global shadow volumes
        // VK_RB_DrawShadowSurface binds the correct pipeline (Z-pass or Z-fail) per surface.
        if (useStencilShadows && vLight->globalShadows && !r_skipShadows.GetBool())
        {
            vkCmdSetScissor(cmd, 0, 1, &shadowScissor);
            s_shadowPhaseTag = "global";
            for (const drawSurf_t *s = vLight->globalShadows; s; s = s->nextOnLight)
                VK_RB_DrawShadowSurface(cmd, s, shadowScissor);
        }

        // Step 2: local interactions — rebind interaction pipeline after shadow draws.
        if (!r_skipInteractions.GetBool())
        {
            vkCmdSetScissor(cmd, 0, 1, &lightScissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);
            for (const drawSurf_t *surf = vLight->localInteractions; surf; surf = surf->nextOnLight)
                RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
        }

        // Step 3: local shadow volumes
        if (useStencilShadows && vLight->localShadows && !r_skipShadows.GetBool())
        {
            vkCmdSetScissor(cmd, 0, 1, &shadowScissor);
            s_shadowPhaseTag = "local";
            for (const drawSurf_t *s = vLight->localShadows; s; s = s->nextOnLight)
                VK_RB_DrawShadowSurface(cmd, s, shadowScissor);
        }

        // Step 4: global interactions — rebind interaction pipeline after shadow draws.
        if (!r_skipInteractions.GetBool())
        {
            vkCmdSetScissor(cmd, 0, 1, &lightScissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);
            for (const drawSurf_t *surf = vLight->globalInteractions; surf; surf = surf->nextOnLight)
                RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
        }

        if (!r_skipTranslucent.GetBool() && vLight->translucentInteractions)
        {
            vkCmdSetScissor(cmd, 0, 1, &lightScissor);
            // Translucent surfaces didn't write depth during the prepass, so they may not
            // be at the same depth as opaque geometry.  Use a pipeline with stencil disabled
            // so shadow volumes don't incorrectly cull them (mirrors GL path: performStencilTest=false).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipelineNoStencil);
            for (const drawSurf_t *surf = vLight->translucentInteractions; surf; surf = surf->nextOnLight)
                RB_CreateSingleDrawInteractions(surf, VK_RB_DrawInteraction);
            // Restore opaque interaction pipeline for next light
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline);
        }
    }

    s_cmd = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_RB_FogAllLights
// Post-interaction pass: renders fog volumes and blend lights.
// Mirrors RB_STD_FogAllLights / RB_FogPass / RB_BlendLight from draw_common.cpp.
//
// Fog lights:
//   For each visible surface inside the fog volume, sample two textures whose
//   coordinates are derived from camera-space depth (fogImage) and the fog entry
//   plane (fogEnterImage).  Uses SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend, depth EQUAL.
//   A second pass with the frustum back-caps (depth LESS, front-cull) fills in areas
//   where the fog extends beyond visible geometry.
//
// Blend lights:
//   Project a coloured light image onto surfaces via projective texgen, attenuated
//   by a 1D falloff texture.  Blend mode comes from stage->drawStateBits.
// ---------------------------------------------------------------------------

static void VK_RB_DrawFogSurface(VkCommandBuffer cmd, const drawSurf_t *surf, const void *uboData, idImage *samp0img,
                                 idImage *samp1img)
{
    extern bool VK_Image_GetDescriptorInfo(idImage *, VkDescriptorImageInfo *);
    extern void VK_Image_GetFallbackDescriptorInfo(VkDescriptorImageInfo *);
    extern bool VK_VertexCache_GetBuffer(vertCache_t *, VkBuffer *, VkDeviceSize *);

    const srfTriangles_t *geo = surf->geo;
    if (!geo || geo->numIndexes <= 0 || geo->numVerts <= 0)
        return;

    // Vertex buffer
    VkBuffer vertBuf;
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
                vertBuf = dataRings[vk.currentFrame].buffer;
                haveVerts = true;
            }
        }
    }
    if (!haveVerts)
        return;

    // Index buffer
    VkDeviceSize idxSize = (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t);
    VkDeviceSize idxOffset = VK_AllocDataRing(idxSize, sizeof(glIndex_t));
    if (idxOffset == VK_WHOLE_SIZE)
        return;
    memcpy((byte *)dataRings[vk.currentFrame].mapped + idxOffset, geo->indexes, (size_t)idxSize);

    // UBO
    uint32_t uboOffset = VK_AllocUBO();
    memcpy((uint8_t *)uboRings[vk.currentFrame].mapped + uboOffset, uboData, sizeof(VkFogUBO));

    // Descriptor set
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkPipes.descPools[vk.currentFrame];
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &vkPipes.fogDescLayout;

    VkDescriptorSet ds;
    if (vkAllocateDescriptorSets(vk.device, &dsAlloc, &ds) != VK_SUCCESS)
        return;

    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = uboRings[vk.currentFrame].buffer;
    bufInfo.offset = uboOffset;
    bufInfo.range = sizeof(VkFogUBO);

    VkDescriptorImageInfo imgInfo0, imgInfo1;
    if (!VK_Image_GetDescriptorInfo(samp0img, &imgInfo0))
        VK_Image_GetFallbackDescriptorInfo(&imgInfo0);
    if (!VK_Image_GetDescriptorInfo(samp1img, &imgInfo1))
        VK_Image_GetFallbackDescriptorInfo(&imgInfo1);

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &bufInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imgInfo0;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = ds;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &imgInfo1;

    vkUpdateDescriptorSets(vk.device, 3, writes, 0, NULL);
    // fog/blendlight UBO is a regular uniform buffer; offset is set in VkDescriptorBufferInfo.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.fogLayout, 0, 1, &ds, 0, NULL);

    const VkIndexType idxType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &vertOffset);
    vkCmdBindIndexBuffer(cmd, dataRings[vk.currentFrame].buffer, idxOffset, idxType);
    vkCmdDrawIndexed(cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0);
}

// Build the VkFogUBO for one surface, computing local fog planes from the global ones.
static void VK_BuildFogSurfaceUBO(const drawSurf_t *surf, const idPlane fogPlanes[4], const float color[4],
                                  VkFogUBO *out)
{
    // MVP
    float mvp[16];
    VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp);
    memcpy(out->mvp, mvp, sizeof(out->mvp));

    // Transform global fog planes into local (model) space
    idPlane local0, local1, local2, local3;
    R_GlobalPlaneToLocal(surf->space->modelMatrix, fogPlanes[0], local0);
    R_GlobalPlaneToLocal(surf->space->modelMatrix, fogPlanes[1], local1);
    R_GlobalPlaneToLocal(surf->space->modelMatrix, fogPlanes[2], local2);
    R_GlobalPlaneToLocal(surf->space->modelMatrix, fogPlanes[3], local3);

    // Add the +0.5 bias to the depth S plane after the transform (matches GL RB_T_BasicFog).
    local0[3] += 0.5f;

    memcpy(out->texGen0S, local0.ToFloatPtr(), 16);
    memcpy(out->texGen0T, local1.ToFloatPtr(), 16);
    memcpy(out->texGen1S, local3.ToFloatPtr(), 16); // S-1 = fogPlanes[3] (view-origin entry S)
    memcpy(out->texGen1T, local2.ToFloatPtr(), 16); // T-1 = fogPlanes[2] (fog top-plane entry T)
    memcpy(out->color, color, 16);
}

static void VK_RB_FogPass(VkCommandBuffer cmd, const viewLight_t *vLight)
{
    if (r_skipFogLights.GetBool())
        return;

    const idMaterial *lightShader = vLight->lightShader;
    const float *regs = vLight->shaderRegisters;
    const shaderStage_t *stage = lightShader->GetStage(0); // fog shaders have one stage

    float fogColor[4];
    fogColor[0] = regs[stage->color.registers[0]];
    fogColor[1] = regs[stage->color.registers[1]];
    fogColor[2] = regs[stage->color.registers[2]];
    fogColor[3] = regs[stage->color.registers[3]];

    // Fog density factor: if alpha <= 1 use default distance 500, else distance = alpha
    float a = (fogColor[3] <= 1.0f) ? (-0.5f / DEFAULT_FOG_DISTANCE) : (-0.5f / fogColor[3]);

    // Override alpha so the fog tint colour is always fully opaque (the fog textures carry density)
    fogColor[3] = 1.0f;

    // Compute the four fog plane vectors (mirror of RB_FogPass in draw_common.cpp).
    // fogPlanes[0]: depth S (world-space camera forward)
    // fogPlanes[1]: depth T (constant 0.5)
    // fogPlanes[2]: enter T (fog top-plane, for entry fade)
    // fogPlanes[3]: enter S (view origin distance into fog)
    const float FOG_SCALE = 0.001f;
    const float s = vLight->fogPlane.Distance(backEnd.viewDef->renderView.vieworg);

    idPlane fogPlanes[4];
    // fogPlanes[0]: fog depth S — camera forward direction scaled by fog density 'a'.
    // The +0.5 bias is intentionally NOT included here; it is added after R_GlobalPlaneToLocal
    // in VK_BuildFogSurfaceUBO, matching the GL path's RB_T_BasicFog which does local[3] += 0.5.
    fogPlanes[0][0] = a * backEnd.viewDef->worldSpace.modelViewMatrix[2];
    fogPlanes[0][1] = a * backEnd.viewDef->worldSpace.modelViewMatrix[6];
    fogPlanes[0][2] = a * backEnd.viewDef->worldSpace.modelViewMatrix[10];
    fogPlanes[0][3] = a * backEnd.viewDef->worldSpace.modelViewMatrix[14];

    fogPlanes[1][0] = 0.0f;
    fogPlanes[1][1] = 0.0f;
    fogPlanes[1][2] = 0.0f;
    fogPlanes[1][3] = 0.5f;

    fogPlanes[2][0] = FOG_SCALE * vLight->fogPlane[0];
    fogPlanes[2][1] = FOG_SCALE * vLight->fogPlane[1];
    fogPlanes[2][2] = FOG_SCALE * vLight->fogPlane[2];
    fogPlanes[2][3] = FOG_SCALE * vLight->fogPlane[3] + FOG_ENTER;

    fogPlanes[3][0] = 0.0f;
    fogPlanes[3][1] = 0.0f;
    fogPlanes[3][2] = 0.0f;
    fogPlanes[3][3] = FOG_SCALE * s + FOG_ENTER;

    // Draw world/model surfaces with depth EQUAL (surface pass)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.fogPipeline);

    auto drawFogChain = [&](const drawSurf_t *surf) {
        for (; surf; surf = surf->nextOnLight)
        {
            VkFogUBO ubo;
            VK_BuildFogSurfaceUBO(surf, fogPlanes, fogColor, &ubo);
            VK_RB_DrawFogSurface(cmd, surf, &ubo, globalImages->fogImage, globalImages->fogEnterImage);
        }
    };
    drawFogChain(vLight->globalInteractions);
    drawFogChain(vLight->localInteractions);

    // Draw frustum back-caps with depth LESS (fog cap pass) — fills fog where no geometry exists
    // Uses worldSpace for the frustumTris since they are in global space
    if (vLight->frustumTris && vLight->frustumTris->ambientCache)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.fogFrustumPipeline);

        // frustumTris is in global (world) space — use world modelViewMatrix directly
        static drawSurf_t capSurf;
        memset(&capSurf, 0, sizeof(capSurf));
        capSurf.space = &backEnd.viewDef->worldSpace;
        capSurf.geo = vLight->frustumTris;

        VkFogUBO capUbo;
        VK_BuildFogSurfaceUBO(&capSurf, fogPlanes, fogColor, &capUbo);
        VK_RB_DrawFogSurface(cmd, &capSurf, &capUbo, globalImages->fogImage, globalImages->fogEnterImage);
    }
}

static void VK_RB_BlendLightPass(VkCommandBuffer cmd, const viewLight_t *vLight)
{
    if (!vLight->globalInteractions && !vLight->localInteractions)
        return;
    if (r_skipBlendLights.GetBool())
        return;

    extern VkPipeline VK_GetOrCreateBlendlightPipeline(int drawStateBits);

    const idMaterial *lightShader = vLight->lightShader;
    const float *regs = vLight->shaderRegisters;
    idImage *falloffImage = vLight->falloffImage;

    for (int i = 0; i < lightShader->GetNumStages(); i++)
    {
        const shaderStage_t *stage = lightShader->GetStage(i);
        if (!regs[stage->conditionRegister])
            continue;

        // Pick or create a pipeline matching this stage's blend mode (depth EQUAL)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_GetOrCreateBlendlightPipeline(stage->drawStateBits));

        float lightColor[4];
        lightColor[0] = regs[stage->color.registers[0]];
        lightColor[1] = regs[stage->color.registers[1]];
        lightColor[2] = regs[stage->color.registers[2]];
        lightColor[3] = regs[stage->color.registers[3]];

        idImage *projImage = stage->texture.image;
        if (!projImage)
            continue;

        // GL parity: blend lights can have a stage texture matrix. In GL this is
        // applied in texture space after texgen; here we fold it into S/T texgen planes.
        float stageTexMat[16];
        bool hasStageTexMat = stage->texture.hasMatrix;
        if (hasStageTexMat)
            RB_GetShaderTextureMatrix(regs, &stage->texture, stageTexMat);

        auto drawBlendChain = [&](const drawSurf_t *surf) {
            for (; surf; surf = surf->nextOnLight)
            {
                // Per-surface: transform light projection planes to local space
                idPlane localProj[4];
                for (int p = 0; p < 4; p++)
                    R_GlobalPlaneToLocal(surf->space->modelMatrix, vLight->lightProject[p], localProj[p]);

                if (hasStageTexMat)
                {
                    const idPlane sPlane = localProj[0];
                    const idPlane tPlane = localProj[1];
                    const idPlane qPlane = localProj[2];

                    // Matrix rows from RB_GetShaderTextureMatrix:
                    // S' = m00*S + m01*T + m03*Q
                    // T' = m10*S + m11*T + m13*Q
                    for (int c = 0; c < 4; c++)
                    {
                        localProj[0][c] =
                            stageTexMat[0] * sPlane[c] + stageTexMat[4] * tPlane[c] + stageTexMat[12] * qPlane[c];
                        localProj[1][c] =
                            stageTexMat[1] * sPlane[c] + stageTexMat[5] * tPlane[c] + stageTexMat[13] * qPlane[c];
                    }
                }

                float mvp[16];
                VK_MultiplyMatrix4(s_projVk, surf->space->modelViewMatrix, mvp);

                VkBlendUBO ubo;
                memcpy(ubo.mvp, mvp, sizeof(ubo.mvp));
                memcpy(ubo.texGen0S, localProj[0].ToFloatPtr(), 16);
                memcpy(ubo.texGen0T, localProj[1].ToFloatPtr(), 16);
                memcpy(ubo.texGen0Q, localProj[2].ToFloatPtr(), 16);
                memcpy(ubo.texGen1S, localProj[3].ToFloatPtr(), 16);
                memcpy(ubo.color, lightColor, 16);

                VK_RB_DrawFogSurface(cmd, surf, &ubo, projImage, falloffImage);
            }
        };
        drawBlendChain(vLight->globalInteractions);
        drawBlendChain(vLight->localInteractions);
    }
}

static void VK_RB_FogAllLights(VkCommandBuffer cmd)
{
    if (r_skipFogLights.GetBool())
        return;
    if (!backEnd.viewDef || !vkPipes.fogPipeline)
        return;

    for (const viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next)
    {
        if (!vLight->lightShader)
            continue;
        backEnd.vLight = const_cast<viewLight_t *>(vLight);

        if (vLight->lightShader->IsFogLight())
            VK_RB_FogPass(cmd, vLight);
        else if (vLight->lightShader->IsBlendLight())
            VK_RB_BlendLightPass(cmd, vLight);
    }
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

// Window minimized state.
// Set by VK_SetWindowMinimized() (called from SDL event handlers) and also
// auto-detected in VK_RecreateSwapchain when the surface extent is 0x0.
// When true, VK_RB_DrawView skips the frame entirely to avoid blocking
// indefinitely inside vkAcquireNextImageKHR.
static bool s_windowMinimized = false;

void VK_SetWindowMinimized(bool minimized)
{
    s_windowMinimized = minimized;
}

// Swapchain needs recreation flag.
// Set by VK_NotifyWindowModeChanged() when GLimp_SetScreenParms succeeds
// (e.g., Alt+Enter fullscreen toggle).  On some drivers the SDL fullscreen
// toggle invalidates the Vulkan surface so the already-acquired swapchain
// image becomes unusable, causing VK_ERROR_DEVICE_LOST on vkQueueSubmit.
// VK_RB_SwapBuffers checks this flag before submitting and, if set, aborts
// the current frame and recreates the swapchain so the next frame starts
// clean without ever touching the invalidated image.
static bool s_swapchainNeedsRecreate = false;

void VK_NotifyWindowModeChanged()
{
    s_swapchainNeedsRecreate = true;
}

// Screenshot readback state.
// VK_RequestReadback() is called just before rendering a screenshot frame.
// VK_RB_SwapBuffers() appends a copy-to-buffer command, waits for the fence,
// and sets s_readbackDone so VK_ReadPixels() can retrieve the data.
static VkBuffer s_readbackBuf = VK_NULL_HANDLE;
static VkDeviceMemory s_readbackMem = VK_NULL_HANDLE;
static void *s_readbackMapped = nullptr;
static VkDeviceSize s_readbackSize = 0;  // allocated size of s_readbackBuf
static bool s_readbackPending = false;   // set by VK_RequestReadback
static bool s_readbackSubmitted = false; // set inside SwapBuffers when copy was added
static bool s_readbackDone = false;      // set after fence wait; read by VK_ReadPixels

void VK_RB_DrawView(const void *data)
{
    if (!vk.isInitialized || !vkPipes.isValid)
        return;

    // When the window is minimized the presentation engine holds all swapchain
    // images.  vkAcquireNextImageKHR with UINT64_MAX would block forever.
    // Skip the frame; SDL_WINDOWEVENT_RESTORED will clear this flag.
    if (s_windowMinimized)
    {
        SDL_Delay(10); // yield so we don't spin at 100% CPU while iconified
        return;
    }

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
            common->Printf("VK Out of Date\n");
            fflush(NULL);
            VK_RecreateSwapchain(glConfig.vidWidth, glConfig.vidHeight);
            return; // s_frameActive stays false; VK_RB_SwapBuffers will no-op
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        {
            common->Warning("VK: vkAcquireNextImageKHR failed: %d", (int)acquireResult);
            return;
        }

        vk.currentImageIdx = imageIndex;
        s_frameImageIndex = imageIndex;
        vkResetFences(vk.device, 1, &vk.inFlightFences[vk.currentFrame]);
        common->DPrintf("VK: frame slot %u, image %u\n", vk.currentFrame, imageIndex);

        // Drain deferred image and buffer deletions queued during the previous use of this frame slot.
        extern void VK_Image_DrainGarbage(uint32_t frameIdx);
        VK_Image_DrainGarbage(vk.currentFrame);
        extern void VK_Buffer_DrainGarbage(uint32_t frameIdx);
        VK_Buffer_DrainGarbage(vk.currentFrame);

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

        // Upload any cinematic frames before the render pass opens
        // (transfer ops are illegal inside a render pass).
        VK_RB_UpdateCinematics(cmdBuf);

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
        if (VK_RTShadowsEnabled())
        {
            vkCmdEndRenderPass(cmdBuf);
            VK_RT_RebuildTLAS(cmdBuf, backEnd.viewDef);
            VK_RT_DispatchShadowRays(cmdBuf, backEnd.viewDef);
            // Reopen with the LOAD render pass so prior colour/depth is preserved.
            VkRenderPassBeginInfo rpResume = rpBegin;
            rpResume.renderPass = vk.renderPassResume;
            rpResume.clearValueCount = 0;
            rpResume.pClearValues = NULL;
            vkCmdBeginRenderPass(cmdBuf, &rpResume, VK_SUBPASS_CONTENTS_INLINE);
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

    // Rendering order matches vkDOOM3 reference and GL path (draw_common.cpp RB_STD_DrawView):
    //   1. Depth prepass — populate depth buffer for early-Z rejection
    //   2. Interactions — per-light Phong shading with stencil shadow volumes
    //   3. Shader passes — unlit/2D surfaces (decals, sky, GUI overlays)
    //   4. FogAllLights — fog volumes and blend lights (post-lighting atmospheric pass)
    if (!r_skipDepthPrepass.GetBool())
        VK_RB_FillDepthBuffer(cmdBuf);

    if (!r_skipInteractions.GetBool())
        VK_RB_DrawInteractions(cmdBuf);

    if (!r_skipAmbient.GetBool() && !r_skipShaderPasses.GetBool())
        VK_RB_DrawShaderPasses(cmdBuf);

    if (!r_skipFogLights.GetBool())
        VK_RB_FogAllLights(cmdBuf);

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

    // If a screenshot readback was requested, copy the swapchain image to the
    // staging buffer before presenting.  The image is in PRESENT_SRC_KHR after
    // vkCmdEndRenderPass (that is the renderpass finalLayout).
    s_readbackSubmitted = false;
    if (s_readbackPending && s_readbackBuf != VK_NULL_HANDLE)
    {
        VK_TransitionImageLayout(cmdBuf, vk.swapchainImages[s_frameImageIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {vk.swapchainExtent.width, vk.swapchainExtent.height, 1};
        vkCmdCopyImageToBuffer(cmdBuf, vk.swapchainImages[s_frameImageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_readbackBuf, 1, &region);

        // Transition back so the image can be presented normally.
        VK_TransitionImageLayout(cmdBuf, vk.swapchainImages[s_frameImageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        s_readbackPending = false;
        s_readbackSubmitted = true;
    }

    VK_CHECK(vkEndCommandBuffer(cmdBuf));

    // If the window mode changed this frame (Alt+Enter fullscreen toggle), the
    // SDL surface may have been invalidated by the driver before we submit.
    // Skip the submit and recreate the swapchain so the next frame starts clean.
    if (s_swapchainNeedsRecreate)
    {
        common->Printf("VK: window changed mid-frame, dropping frame and recreating swapchain\n");
        s_swapchainNeedsRecreate = false;
        vkResetCommandBuffer(vk.commandBuffers[vk.currentFrame], 0);
        VK_RecreateSwapchain(glConfig.vidWidth, glConfig.vidHeight);
        s_frameActive = false;
        s_frameCmdBuf = VK_NULL_HANDLE;
        return;
    }

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

    uint32_t submittedFrame = vk.currentFrame; // capture before increment
    const int submitLogMode = r_vkLogSubmitInfo.GetInteger();
    if (submitLogMode > 0)
    {
        int rtActive = 0;
#ifdef DHEWM3_RAYTRACING
        rtActive = VK_RTShadowsEnabled() ? 1 : 0;
#endif
        common->Printf("VK SUBMIT: frame=%u image=%u rt=%d useRT=%d rtShadows=%d readbackPending=%d "
                       "readbackSubmitted=%d swapRecreate=%d\n",
                       submittedFrame, (unsigned int)s_frameImageIndex, rtActive, r_useRayTracing.GetBool() ? 1 : 0,
                       r_rtShadows.GetBool() ? 1 : 0, s_readbackPending ? 1 : 0, s_readbackSubmitted ? 1 : 0,
                       s_swapchainNeedsRecreate ? 1 : 0);

        if (submitLogMode >= 2)
        {
            VkResult fenceStatus = vkGetFenceStatus(vk.device, vk.inFlightFences[submittedFrame]);
            common->Printf("VK SUBMIT: fenceStatus(before)=%d (%s)\n", (int)fenceStatus,
                           VK_ResultToString(fenceStatus));
        }
    }

    // fflush before submit: ensures any preceding swapchain recreation log is written, and
    // incidentally gives the presentation engine time to finish processing OUT_OF_DATE.
    fflush(NULL);
    VkResult submitResult = vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, vk.inFlightFences[submittedFrame]);
    if (submitResult != VK_SUCCESS)
    {
        s_frameActive = false;
        common->Printf("VK: vkQueueSubmit failed with error %d (%s)\n", (int)submitResult,
                       VK_ResultToString(submitResult));
        fflush(NULL);
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
    {
        common->Printf("VK: present returned %s, recreating swapchain\n",
                       presentResult == VK_ERROR_OUT_OF_DATE_KHR ? "OUT_OF_DATE" : "SUBOPTIMAL");
        VK_RecreateSwapchain(glConfig.vidWidth, glConfig.vidHeight);
    }
    else if (presentResult != VK_SUCCESS)
    {
        common->Warning("VK: vkQueuePresentKHR failed: %d (%s)", (int)presentResult, VK_ResultToString(presentResult));
    }

    // If we appended a readback copy this frame, wait for it to complete so
    // R_ReadTiledPixels can immediately access the mapped buffer.
    if (s_readbackSubmitted)
    {
        VkResult rbFence = vkWaitForFences(vk.device, 1, &vk.inFlightFences[submittedFrame], VK_TRUE, UINT64_MAX);
        common->Printf("VK: readback fence wait result=%d (frame=%u)\n", (int)rbFence, submittedFrame);
        fflush(NULL);
        s_readbackDone = true;
        s_readbackSubmitted = false;
    }

    vk.currentFrame = (vk.currentFrame + 1) % VK_MAX_FRAMES_IN_FLIGHT;
    s_frameActive = false;
    s_frameCmdBuf = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Screenshot readback helpers
// ---------------------------------------------------------------------------

// Call this just before the screenshot render frame (before session->UpdateScreen).
// Allocates the staging buffer on first use.
void VK_RequestReadback()
{
    if (!vk.isInitialized)
        return;

    VkDeviceSize size = (VkDeviceSize)vk.swapchainExtent.width * vk.swapchainExtent.height * 4;

    // Reallocate if the swapchain grew since the buffer was first created.
    if (s_readbackBuf != VK_NULL_HANDLE && size > s_readbackSize)
    {
        vkUnmapMemory(vk.device, s_readbackMem);
        vkDestroyBuffer(vk.device, s_readbackBuf, NULL);
        vkFreeMemory(vk.device, s_readbackMem, NULL);
        s_readbackBuf = VK_NULL_HANDLE;
        s_readbackMem = VK_NULL_HANDLE;
        s_readbackMapped = nullptr;
        s_readbackSize = 0;
    }

    if (s_readbackBuf == VK_NULL_HANDLE)
    {
        VK_CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s_readbackBuf,
                        &s_readbackMem);
        VK_CHECK(vkMapMemory(vk.device, s_readbackMem, 0, size, 0, &s_readbackMapped));
        s_readbackSize = size;
    }

    s_readbackPending = true;
    s_readbackDone = false;
}

void VK_CleanupReadback()
{
    if (s_readbackBuf != VK_NULL_HANDLE)
    {
        vkUnmapMemory(vk.device, s_readbackMem);
        vkDestroyBuffer(vk.device, s_readbackBuf, NULL);
        vkFreeMemory(vk.device, s_readbackMem, NULL);
        s_readbackBuf = VK_NULL_HANDLE;
        s_readbackMem = VK_NULL_HANDLE;
        s_readbackMapped = nullptr;
        s_readbackSize = 0;
    }
}

// Call this after the screenshot render frame returns.
// Copies (x,y,w,h) pixels from the last captured swapchain image into out_rgb (packed RGB).
// Does nothing if the readback did not complete.
void VK_ReadPixels(int x, int y, int w, int h, byte *out_rgb)
{
    if (!s_readbackDone || !s_readbackMapped)
        return;

    uint32_t sw = vk.swapchainExtent.width;
    const byte *src = (const byte *)s_readbackMapped;
    bool isBGR = (vk.swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM || vk.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB);

    for (int row = 0; row < h; row++)
    {
        for (int col = 0; col < w; col++)
        {
            const byte *p = src + ((y + row) * sw + (x + col)) * 4;
            byte *d = out_rgb + (row * w + col) * 3;
            if (isBGR)
            {
                d[0] = p[2];
                d[1] = p[1];
                d[2] = p[0]; // BGRA → RGB
            }
            else
            {
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2]; // RGBA → RGB
            }
        }
    }

    s_readbackDone = false;
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

    // Switch ImGui from the OpenGL backend (used during GLimp_Init before
    // glConfig.isVulkan was set) to the Vulkan backend now that the device,
    // swapchain, and render pass are all up.
    D3::ImGuiHooks::InitVulkan();
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
