/*
===========================================================================

dhewm3 Vulkan ray tracing - temporal EMA resolve for ambient occlusion.

Temporal accumulation blends the raw per-frame AO image into a per-slot
history image:   history = mix(history, current, alpha)

The interaction shader samples the history image instead of the raw AO
image, so a low alpha (e.g. 0.1) accumulates 8–16 frames worth of AO
rays and dramatically reduces perceived noise without motion vectors.

Safe in-flight design
---------------------
Each per-slot history image is only ever touched by commands in the
command buffer recorded for that slot.  The per-slot in-flight fence
guarantees that the previous commands for slot N are complete before any
new commands for slot N are recorded, so history images never have a
read/write conflict across the double-buffer boundary.

Camera-cut detection
--------------------
On the first use of a slot, or whenever the per-slot cached invViewProj
matrix differs from the current frame's matrix by more than
r_rtTemporalCutThreshold, alpha is forced to 1.0 so history is replaced
entirely with the current frame.  This prevents stale history from
ghosting through level transitions and teleport cuts.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtTemporal("r_rtTemporal", "1", CVAR_RENDERER | CVAR_BOOL,
                    "Enable temporal EMA accumulation for AO (requires r_rtAO 1)");

idCVar r_rtTemporalAlpha("r_rtTemporalAlpha", "0.3", CVAR_RENDERER | CVAR_FLOAT,
                         "EMA blend factor: 0=use only history, 1=use only current frame (reset). "
                         "Lower values are smoother but ghost more during movement.");

idCVar r_rtTemporalCutThreshold("r_rtTemporalCutThreshold", "0.5", CVAR_RENDERER | CVAR_FLOAT,
                                "Max L-inf distance between consecutive invViewProj matrices before "
                                "history is discarded (camera cut / teleport detection).");

idCVar r_rtAtrousIterations("r_rtAtrousIterations", "4", CVAR_RENDERER | CVAR_INTEGER,
                            "Atrous spatial AO filter passes after EMA (0=off, 2 or 4 recommended). "
                            "Requires r_rtAO 1 and r_rtTemporal 1.");

idCVar r_rtAtrousSigmaDepth("r_rtAtrousSigmaDepth", "0.01", CVAR_RENDERER | CVAR_FLOAT,
                            "Atrous depth edge-stop sigma (NDC). Smaller = sharper geometry edges.");

idCVar r_rtAtrousSigmaLuminance("r_rtAtrousSigmaLuminance", "0.1", CVAR_RENDERER | CVAR_FLOAT,
                                "Atrous AO luminance edge-stop sigma. Smaller = more AO contrast preserved.");

// ---------------------------------------------------------------------------
// Forward declarations (provided by other compilation units)
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern idCVar r_vkLogRT;
extern idCVar r_useRayTracing;
extern idCVar r_rtAO;

static VkRect2D s_temporalDispatchRect[VK_MAX_FRAMES_IN_FLIGHT] = {};

static VkRect2D VK_RT_ComputeViewDispatchRect(const viewDef_t *viewDef)
{
    const int w = (int)vk.swapchainExtent.width;
    const int h = (int)vk.swapchainExtent.height;
    const idScreenRect &s = viewDef->scissor;

    VkRect2D r;
    r.offset.x = idMath::ClampInt(0, w - 1, s.x1);
    r.offset.y = idMath::ClampInt(0, h - 1, h - 1 - s.y2);

    const int rw = s.x2 - s.x1 + 1;
    const int rh = s.y2 - s.y1 + 1;
    if (rw <= 0 || rh <= 0)
        return VkRect2D{{0, 0}, {0, 0}};

    r.extent.width = (uint32_t)idMath::ClampInt(1, w - r.offset.x, rw);
    r.extent.height = (uint32_t)idMath::ClampInt(1, h - r.offset.y, rh);
    return r;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Allocate and transition one history image to VK_IMAGE_LAYOUT_GENERAL.
// Uses a one-time submit + vkQueueWaitIdle — this is only called during
// init/resize (outside the hot path) so the stall is acceptable.
static bool VK_RT_AllocHistoryImage(vkAOMask_t &img, uint32_t width, uint32_t height)
{
    img.width = width;
    img.height = height;

    VkImageCreateInfo imgCI = {};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_R8_UNORM;
    imgCI.extent = {width, height, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    // STORAGE: imageLoad/imageStore in compute
    // SAMPLED: sampler2D in the interaction fragment shader
    imgCI.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgCI, NULL, &img.image));

    // Allocate device-local memory
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vk.device, img.image, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice, &memProps);
    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t m = 0; m < memProps.memoryTypeCount; m++)
    {
        if ((memReq.memoryTypeBits & (1u << m)) &&
            (memProps.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            memTypeIdx = m;
            break;
        }
    }
    if (memTypeIdx == UINT32_MAX)
    {
        common->Warning("VK RT Temporal: no device-local memory type for history image");
        vkDestroyImage(vk.device, img.image, NULL);
        img.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocI = {};
    allocI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocI.allocationSize = memReq.size;
    allocI.memoryTypeIndex = memTypeIdx;
    VK_CHECK(vkAllocateMemory(vk.device, &allocI, NULL, &img.memory));
    VK_CHECK(vkBindImageMemory(vk.device, img.image, img.memory, 0));

    // Image view
    VkImageViewCreateInfo viewCI = {};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = img.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_R8_UNORM;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewCI, NULL, &img.view));

    // Transition UNDEFINED → GENERAL (required for imageLoad/imageStore).
    // One-time submit; acceptable cost at init/resize time.
    {
        VkCommandBufferAllocateInfo cbAlloc = {};
        cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool = vk.commandPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

        VkCommandBufferBeginInfo beginI = {};
        beginI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmpCmd, &beginI);

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = img.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             NULL, 0, NULL, 1, &barrier);

        vkEndCommandBuffer(tmpCmd);

        VkSubmitInfo submitI = {};
        submitI.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitI.commandBufferCount = 1;
        submitI.pCommandBuffers = &tmpCmd;
        vkQueueSubmit(vk.graphicsQueue, 1, &submitI, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.graphicsQueue);
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
    }

    return true;
}

// Free one history image (view → image → memory).  Caller owns the idle guarantee.
static void VK_RT_FreeHistoryImage(vkAOMask_t &img)
{
    if (img.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(vk.device, img.view, NULL);
        img.view = VK_NULL_HANDLE;
    }
    if (img.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(vk.device, img.image, NULL);
        img.image = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, img.memory, NULL);
        img.memory = VK_NULL_HANDLE;
    }
    img.width = 0;
    img.height = 0;
}

// ---------------------------------------------------------------------------
// VK_RT_CreateHistoryImages / VK_RT_DestroyHistoryImages
// ---------------------------------------------------------------------------

static void VK_RT_CreateHistoryImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocHistoryImage(vkRT.aoHistory[i], width, height))
        {
            common->Warning("VK RT Temporal: failed to allocate history image slot %d", i);
        }
        // History is not valid yet — first dispatch will fill it with alpha=1.0
        vkRT.aoHistoryValid[i] = false;
        memset(vkRT.aoPrevInvViewProj[i], 0, sizeof(vkRT.aoPrevInvViewProj[i]));
        // Default: backend samples aoHistory directly until Atrous updates aoReadView
        vkRT.aoReadView[i] = vkRT.aoHistory[i].view;
    }
}

static void VK_RT_DestroyHistoryImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeHistoryImage(vkRT.aoHistory[i]);
        vkRT.aoHistoryValid[i] = false;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitTemporalPipeline
// Create the compute pipeline, push-constant layout, descriptor pool + sets.
// ---------------------------------------------------------------------------

static void VK_RT_InitTemporalPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: currentImage (storage image, r8, GENERAL — read-only)
    // binding 1: historyImage (storage image, r8, GENERAL — read+write)
    VkDescriptorSetLayoutBinding bindings[2] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.temporalDescLayout));

    // --- Push constant: alpha + dispatch rect = 32 bytes ---
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 32;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.temporalDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.temporalPipelineLayout));

    // --- Compute shader ---
    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/temporal_resolve.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Temporal: failed to load temporal_resolve.comp.spv — temporal disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stageCI = {};
    stageCI.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCI.module = compModule;
    stageCI.pName = "main";

    VkComputePipelineCreateInfo pipeCI = {};
    pipeCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.stage = stageCI;
    pipeCI.layout = vkRT.temporalPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.temporalPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    // --- Descriptor pool and sets ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * VK_MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.temporalDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.temporalDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.temporalDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.temporalDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.temporalDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Temporal: EMA resolve pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void VK_RT_InitTemporal(void)
{
    VK_RT_InitTemporalPipeline();
    VK_RT_CreateHistoryImages(vk.swapchainExtent.width, vk.swapchainExtent.height);
    VK_RT_InitAtrous();
}

void VK_RT_ShutdownTemporal(void)
{
    // Device must be idle — the caller (VK_RT_Shutdown) guarantees this.
    VK_RT_ShutdownAtrous(); // must run before history images are destroyed
    VK_RT_DestroyHistoryImages();

    if (vkRT.temporalPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.temporalPipeline, NULL);
        vkRT.temporalPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.temporalPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.temporalPipelineLayout, NULL);
        vkRT.temporalPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.temporalDescPool != VK_NULL_HANDLE)
    {
        // Freeing the pool implicitly frees all descriptor sets allocated from it.
        vkDestroyDescriptorPool(vk.device, vkRT.temporalDescPool, NULL);
        vkRT.temporalDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.temporalDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.temporalDescLayout, NULL);
        vkRT.temporalDescLayout = VK_NULL_HANDLE;
    }
}

void VK_RT_ResizeTemporal(uint32_t width, uint32_t height)
{
    // Wait for all in-flight work to complete before touching any images that
    // may be referenced by currently executing command buffers.
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyHistoryImages();
    VK_RT_CreateHistoryImages(width, height);

    // Force descriptor refresh — old image views are dead.
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRT.temporalDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.aoHistoryValid[i] = false;
    }
    VK_RT_ResizeAtrous(width, height);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchTemporalResolveAO
// ---------------------------------------------------------------------------

void VK_RT_DispatchTemporalResolveAO(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtAO.GetBool() || !r_rtTemporal.GetBool())
        return;
    if (vkRT.temporalPipeline == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Temporal: pipeline is NULL, skipping dispatch");
        return;
    }

    const int frameIdx = vk.currentFrame;
    const VkRect2D dispatchRect = VK_RT_ComputeViewDispatchRect(viewDef);
    s_temporalDispatchRect[frameIdx] = dispatchRect;

    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    vkAOMask_t &current = vkRT.aoMask[frameIdx];
    vkAOMask_t &history = vkRT.aoHistory[frameIdx];

    if (current.image == VK_NULL_HANDLE || history.image == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Temporal: skip — images not ready (slot %d)\n", frameIdx);
        return;
    }

    // --- Camera-cut detection ---
    // Build the current invViewProj (same convention as AO dispatch)
    float invVP[16];
    {
        const float *proj = viewDef->projectionMatrix;
        const float *mv = viewDef->worldSpace.modelViewMatrix;
        float vp[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
            {
                vp[c * 4 + r] = 0.0f;
                for (int k = 0; k < 4; k++)
                    vp[c * 4 + r] += proj[k * 4 + r] * mv[c * 4 + k];
            }
        idMat4 vpMat(idVec4(vp[0], vp[1], vp[2], vp[3]), idVec4(vp[4], vp[5], vp[6], vp[7]),
                     idVec4(vp[8], vp[9], vp[10], vp[11]), idVec4(vp[12], vp[13], vp[14], vp[15]));
        idMat4 inv = vpMat.Inverse();
        memcpy(invVP, inv.ToFloatPtr(), 16 * sizeof(float));
    }

    // First-frame or cut: use alpha=1.0 to avoid NaN/stale history.
    float effectiveAlpha = 1.0f;
    if (vkRT.aoHistoryValid[frameIdx])
    {
        // L-inf distance between the previous and current invViewProj matrices.
        float maxDiff = 0.0f;
        for (int i = 0; i < 16; i++)
        {
            float d = fabsf(invVP[i] - vkRT.aoPrevInvViewProj[frameIdx][i]);
            if (d > maxDiff)
                maxDiff = d;
        }
        float cutThresh = Max(0.0f, r_rtTemporalCutThreshold.GetFloat());
        if (maxDiff <= cutThresh)
        {
            effectiveAlpha = idMath::ClampFloat(0.0f, 1.0f, r_rtTemporalAlpha.GetFloat());
        }
        else if (r_vkLogRT.GetInteger() >= 1)
        {
            common->Printf("VK RT Temporal: camera cut detected slot=%d maxDiff=%.4f — resetting history\n", frameIdx,
                           maxDiff);
        }
    }

    // Store current matrix for next time this slot is used.
    memcpy(vkRT.aoPrevInvViewProj[frameIdx], invVP, sizeof(invVP));
    vkRT.aoHistoryValid[frameIdx] = true;

    // --- Update descriptor set (once per frame slot) ---
    // Important: descriptors bind specific VkImageViews. Re-check every frame
    // in case the AO mask or history images were recreated (resize).
    bool refreshSet = (vkRT.temporalDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount);
    if (refreshSet)
    {
        VkDescriptorImageInfo currInfo = {};
        currInfo.imageView = current.view;
        currInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo histInfo = {};
        histInfo.imageView = history.view;
        histInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = vkRT.temporalDescSets[frameIdx];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &currInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = vkRT.temporalDescSets[frameIdx];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &histInfo;

        vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
        vkRT.temporalDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // --- Push constants ---
    struct
    {
        float alpha;
        float pad[3];
        int32_t scissorOffsetX;
        int32_t scissorOffsetY;
        int32_t scissorExtentX;
        int32_t scissorExtentY;
    } pc;
    pc.alpha = effectiveAlpha;
    pc.pad[0] = pc.pad[1] = pc.pad[2] = 0.0f;
    pc.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    pc.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    pc.scissorExtentX = (int32_t)dispatchRect.extent.width;
    pc.scissorExtentY = (int32_t)dispatchRect.extent.height;

    // --- Dispatch ---
    uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.temporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.temporalPipelineLayout, 0, 1,
                            &vkRT.temporalDescSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.temporalPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &pc);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Temporal: dispatch slot=%d alpha=%.3f rect=(%d,%d %u,%u)\n", frameIdx,
                   effectiveAlpha, dispatchRect.offset.x, dispatchRect.offset.y,
                   (unsigned int)dispatchRect.extent.width, (unsigned int)dispatchRect.extent.height);

    // --- Barrier: compute write to aoHistory → fragment and/or next compute read ---
    // Include COMPUTE_SHADER in dst so the Atrous pass (if enabled) can read
    // aoHistory without an additional barrier.  If Atrous is disabled the
    // FRAGMENT_SHADER stage alone would suffice, but the extra stage is harmless.
    {
        VkMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &barrier, 0, NULL, 0, NULL);
    }
}

// ===========================================================================
// Atrous spatial filter (Step 5.2b)
// ===========================================================================

// Creates/destroys per-slot scratch images (same allocation path as history images).
static void VK_RT_CreateScratchImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocHistoryImage(vkRT.aoScratch[i], width, height))
            common->Warning("VK RT Atrous: failed to allocate scratch image slot %d", i);
        // aoReadView points at aoHistory by default; DispatchAtrousAO updates it each frame
        vkRT.aoReadView[i] = vkRT.aoHistory[i].view;
    }
}

static void VK_RT_DestroyScratchImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeHistoryImage(vkRT.aoScratch[i]);
        vkRT.aoReadView[i] = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitAtrousPipeline
// Descriptor layout, push constants, compute pipeline, pool + sets.
// ---------------------------------------------------------------------------

static void VK_RT_InitAtrousPipeline(void)
{
    // Binding 0: input image (storage, r8, GENERAL, readonly)
    // Binding 1: output image (storage, r8, GENERAL, writeonly)
    // Binding 2: depth sampler (combined image sampler)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.atrousDescLayout));

    // Push constant: base params + dispatch rect = 32 bytes
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 32;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.atrousDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.atrousPipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/atrous_filter.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Atrous: failed to load atrous_filter.comp.spv — Atrous disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stageCI = {};
    stageCI.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCI.module = compModule;
    stageCI.pName = "main";

    VkComputePipelineCreateInfo pipeCI = {};
    pipeCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.stage = stageCI;
    pipeCI.layout = vkRT.atrousPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.atrousPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    // Pool: 2 ping-pong sets per slot × VK_MAX_FRAMES_IN_FLIGHT
    const int totalSets = VK_MAX_FRAMES_IN_FLIGHT * 2;
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 2 * totalSets; // 2 storage images per set
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 1 * totalSets; // 1 depth sampler per set

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = totalSets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.atrousDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT * 2];
    for (int i = 0; i < totalSets; i++)
        layouts[i] = vkRT.atrousDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.atrousDescPool;
    dsAlloc.descriptorSetCount = totalSets;
    dsAlloc.pSetLayouts = layouts;
    // Allocate as flat array: [0][0], [0][1], [1][0], [1][1]
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, &vkRT.atrousDescSets[0][0]));

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.atrousDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Atrous: spatial filter pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void VK_RT_InitAtrous(void)
{
    VK_RT_InitAtrousPipeline();
    VK_RT_CreateScratchImages(vk.swapchainExtent.width, vk.swapchainExtent.height);
}

void VK_RT_ShutdownAtrous(void)
{
    // Device must be idle — caller guarantees this.
    VK_RT_DestroyScratchImages();

    if (vkRT.atrousPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.atrousPipeline, NULL);
        vkRT.atrousPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.atrousPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.atrousPipelineLayout, NULL);
        vkRT.atrousPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.atrousDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.atrousDescPool, NULL);
        vkRT.atrousDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.atrousDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.atrousDescLayout, NULL);
        vkRT.atrousDescLayout = VK_NULL_HANDLE;
    }
}

void VK_RT_ResizeAtrous(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyScratchImages();
    VK_RT_CreateScratchImages(width, height); // also resets aoReadView → aoHistory

    // Descriptors must be refreshed: image views and depth view have changed
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.atrousDescSetLastUpdatedFrameCount[i] = -1;
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchAtrousAO
// ---------------------------------------------------------------------------

void VK_RT_DispatchAtrousAO(VkCommandBuffer cmd)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtAO.GetBool() || !r_rtTemporal.GetBool())
        return;

    const int frameIdx = vk.currentFrame;
    const VkRect2D dispatchRect = s_temporalDispatchRect[frameIdx];

    int iters = idMath::ClampInt(0, 8, r_rtAtrousIterations.GetInteger());
    if (iters == 0)
    {
        // No Atrous: ensure backend samples the up-to-date EMA history directly
        if (vkRT.aoHistory[frameIdx].view != VK_NULL_HANDLE)
            vkRT.aoReadView[frameIdx] = vkRT.aoHistory[frameIdx].view;
        return;
    }

    if (vkRT.atrousPipeline == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Atrous: pipeline is NULL, skipping dispatch");
        return;
    }

    vkAOMask_t &history = vkRT.aoHistory[frameIdx];
    vkAOMask_t &scratch = vkRT.aoScratch[frameIdx];

    if (history.image == VK_NULL_HANDLE || scratch.image == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Atrous: skip — images not ready (slot %d)\n", frameIdx);
        return;
    }

    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    // --- Update descriptor sets (once per tr.frameCount per slot) ---
    // Set [frameIdx][0]: binding0=aoHistory(in), binding1=aoScratch(out), binding2=depth
    // Set [frameIdx][1]: binding0=aoScratch(in), binding1=aoHistory(out), binding2=depth
    if (vkRT.atrousDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount)
    {
        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        // imgViews[setIdx][bindingIdx] — 0=input, 1=output
        VkDescriptorImageInfo imgViews[2][2] = {};
        imgViews[0][0] = {VK_NULL_HANDLE, history.view, VK_IMAGE_LAYOUT_GENERAL}; // set0 in = history
        imgViews[0][1] = {VK_NULL_HANDLE, scratch.view, VK_IMAGE_LAYOUT_GENERAL}; // set0 out = scratch
        imgViews[1][0] = {VK_NULL_HANDLE, scratch.view, VK_IMAGE_LAYOUT_GENERAL}; // set1 in = scratch
        imgViews[1][1] = {VK_NULL_HANDLE, history.view, VK_IMAGE_LAYOUT_GENERAL}; // set1 out = history

        VkWriteDescriptorSet writes[6] = {};
        for (int s = 0; s < 2; s++)
        {
            VkDescriptorSet ds = vkRT.atrousDescSets[frameIdx][s];

            writes[s * 3 + 0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[s * 3 + 0].dstSet = ds;
            writes[s * 3 + 0].dstBinding = 0;
            writes[s * 3 + 0].descriptorCount = 1;
            writes[s * 3 + 0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[s * 3 + 0].pImageInfo = &imgViews[s][0];

            writes[s * 3 + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[s * 3 + 1].dstSet = ds;
            writes[s * 3 + 1].dstBinding = 1;
            writes[s * 3 + 1].descriptorCount = 1;
            writes[s * 3 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[s * 3 + 1].pImageInfo = &imgViews[s][1];

            writes[s * 3 + 2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[s * 3 + 2].dstSet = ds;
            writes[s * 3 + 2].dstBinding = 2;
            writes[s * 3 + 2].descriptorCount = 1;
            writes[s * 3 + 2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[s * 3 + 2].pImageInfo = &depthInfo;
        }
        vkUpdateDescriptorSets(vk.device, 6, writes, 0, NULL);
        vkRT.atrousDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    const float sigmaDepth = r_rtAtrousSigmaDepth.GetFloat();
    const float sigmaLum = r_rtAtrousSigmaLuminance.GetFloat();
    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.atrousPipeline);

    for (int pass = 0; pass < iters; pass++)
    {
        // Alternating descriptor set: [0]=history→scratch, [1]=scratch→history
        const int descIdx = pass % 2;

        struct AtrousPC
        {
            int32_t stepWidth;
            float sigmaDepth;
            float sigmaLuminance;
            float pad;
            int32_t scissorOffsetX;
            int32_t scissorOffsetY;
            int32_t scissorExtentX;
            int32_t scissorExtentY;
        } pc;
        pc.stepWidth = 1 << pass; // 1, 2, 4, 8, 16 ...
        pc.sigmaDepth = sigmaDepth;
        pc.sigmaLuminance = sigmaLum;
        pc.pad = 0.0f;
        pc.scissorOffsetX = (int32_t)dispatchRect.offset.x;
        pc.scissorOffsetY = (int32_t)dispatchRect.offset.y;
        pc.scissorExtentX = (int32_t)dispatchRect.extent.width;
        pc.scissorExtentY = (int32_t)dispatchRect.extent.height;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.atrousPipelineLayout, 0, 1,
                                &vkRT.atrousDescSets[frameIdx][descIdx], 0, NULL);
        vkCmdPushConstants(cmd, vkRT.atrousPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &pc);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        // Barrier after each pass: output becomes input for the next pass (COMPUTE→COMPUTE),
        // or transitions to FRAGMENT_READ after the final pass.
        const bool isLastPass = (pass == iters - 1);
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             isLastPass ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }

    // Update aoReadView to the image that holds the final output.
    // Even pass count (0, 2, 4 ...): set[0] ran last (→scratch) then set[1] ran last (→history);
    // for iters passes: descIdx of last pass = (iters-1) % 2.
    // set[0] writes scratch, set[1] writes history.
    const bool finalInHistory = ((iters - 1) % 2 == 1); // last pass used set[1] → history
    vkRT.aoReadView[frameIdx] = finalInHistory ? history.view : scratch.view;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Atrous: %d passes slot=%d final=%s step_max=%d\n", iters, frameIdx,
                       finalInHistory ? "history" : "scratch", 1 << (iters - 1));
}
