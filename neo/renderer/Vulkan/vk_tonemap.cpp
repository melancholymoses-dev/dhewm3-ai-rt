/*
===========================================================================

dhewm3-rt Vulkan — vk_tonemap.cpp — HDR scene buffer and Uchimura tonemap resolve.

Phase 8.1: allocates a per-frame RGBA16F HDR accumulation image (hdrScene) that
replaces the swapchain as the colour attachment for scene and composite passes.
A final compute dispatch (tonemap.comp) reads hdrScene, applies the Uchimura
filmic S-curve to the Rec. 709 scalar luminance, and writes the tonemapped
result to a per-frame RGBA8 resolve image (tonemapResolve).  That resolve image
is then blitted to the acquired swapchain image (BGRA8_UNORM) before presentation.

The blit handles RGBA8 → BGRA8 format conversion: the Vulkan spec maps each named
component (R, G, B, A) to its counterpart in the destination format, so colours
are preserved correctly.

Dispatch order in the RT frame:
  [scene render pass  → hdrScene]
  [GI composite       → hdrScene]
  [vol composite      → hdrScene]
  VK_RT_DispatchTonemap   ← this file (tonemap.comp + blit → swapchain)
  [resume render pass → swapchain (HUD / 2D)]

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtTonemap("r_rtTonemap", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
                   "Enable HDR resolve with Uchimura tonemapping (Phase 8.1).");
idCVar r_rtTonemapExposure("r_rtTonemapExposure", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                           "Exposure scale applied before tonemapping.");
idCVar r_rtTonemapToe("r_rtTonemapToe", "2.2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                      "Toe strength (c). 1=linear, 2=Doom-dark, 3+=crushed blacks.");
idCVar r_rtTonemapLinStart("r_rtTonemapLinStart", "0.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                           "Luminance at which the linear section begins.");
idCVar r_rtTonemapLinLen("r_rtTonemapLinLen", "0.40", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                         "Length of the linear section.");

// ---------------------------------------------------------------------------
// Externs from other translation units
// ---------------------------------------------------------------------------

extern VkShaderModule VK_LoadSPIRV(const char *path);
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// HDR images (RGBA16F — colour attachment for scene passes)
// ---------------------------------------------------------------------------

static void VK_RT_CreateHDRImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &hdr = vkRT.hdrScene[i];
        hdr.width = width;
        hdr.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // COLOR_ATTACHMENT: render passes write here instead of the swapchain.
        // STORAGE: tonemap.comp reads it as a storage image.
        // SAMPLED: fallback blit path if storage swapchain writes aren't supported.
        // TRANSFER_DST: initial clear on creation.
        // TRANSFER_SRC: bypass blit when tonemapping is disabled (hdrScene → swapchain directly).
        imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &hdr.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, hdr.image, &memReq);

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
            common->Error("VK RT Tonemap: no device-local memory type for HDR scene buffer");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &hdr.memory));
        VK_CHECK(vkBindImageMemory(vk.device, hdr.image, hdr.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hdr.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &hdr.view));

        // Transition UNDEFINED → COLOR_ATTACHMENT_OPTIMAL and clear to black.
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo cbAlloc = {};
            cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAlloc.commandPool = vk.commandPool;
            cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAlloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmpCmd, &beginInfo);

            VkImageSubresourceRange subRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            // UNDEFINED → GENERAL for the initial clear.
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = hdr.image;
            barrier.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, hdr.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

            // GENERAL → COLOR_ATTACHMENT_OPTIMAL: ready for the first render pass.
            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier2.image = hdr.image;
            barrier2.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, NULL, 0, NULL, 1, &barrier2);

            vkEndCommandBuffer(tmpCmd);

            VkFenceCreateInfo fenceCI = {};
            fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &tmpCmd;
            vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, fence);
            vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk.device, fence, NULL);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }
}

static void VK_RT_DestroyHDRImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &hdr = vkRT.hdrScene[i];
        if (hdr.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, hdr.view, NULL);
            hdr.view = VK_NULL_HANDLE;
        }
        if (hdr.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, hdr.image, NULL);
            hdr.image = VK_NULL_HANDLE;
        }
        if (hdr.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, hdr.memory, NULL);
            hdr.memory = VK_NULL_HANDLE;
        }
        hdr.width = 0;
        hdr.height = 0;
    }
}

// ---------------------------------------------------------------------------
// HDR framebuffers (depth + hdrScene as colour attachment)
// ---------------------------------------------------------------------------

static void VK_RT_CreateHDRFramebuffers(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkImageView attachments[2] = {vkRT.hdrScene[i].view, vk.depthView};

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = vk.hdrRenderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = vkRT.hdrScene[i].width;
        fbInfo.height = vkRT.hdrScene[i].height;
        fbInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(vk.device, &fbInfo, NULL, &vk.hdrFramebuffers[i]));
    }
}

static void VK_RT_DestroyHDRFramebuffers(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vk.hdrFramebuffers[i] != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(vk.device, vk.hdrFramebuffers[i], NULL);
            vk.hdrFramebuffers[i] = VK_NULL_HANDLE;
        }
    }
}

// ---------------------------------------------------------------------------
// Tonemap resolve images (RGBA8_UNORM — compute shader output, blitted to swapchain)
// ---------------------------------------------------------------------------

static void VK_RT_CreateTonemapResolveImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &res = vkRT.tonemapResolve[i];
        res.width = width;
        res.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // STORAGE: tonemap.comp writes here (rgba8 format qualifier in shader).
        // TRANSFER_SRC: blitted to the BGRA8 swapchain image after dispatch.
        imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &res.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, res.image, &memReq);

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
            common->Error("VK RT Tonemap: no device-local memory type for tonemap resolve buffer");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &res.memory));
        VK_CHECK(vkBindImageMemory(vk.device, res.image, res.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = res.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &res.view));

        // Transition UNDEFINED → GENERAL: compute shader writes in GENERAL,
        // and vkCmdBlitImage accepts GENERAL as the source layout.
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo cbAlloc = {};
            cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAlloc.commandPool = vk.commandPool;
            cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAlloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmpCmd, &beginInfo);

            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = res.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 NULL, 0, NULL, 1, &barrier);

            vkEndCommandBuffer(tmpCmd);

            VkFenceCreateInfo fenceCI = {};
            fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &tmpCmd;
            vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, fence);
            vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk.device, fence, NULL);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }
}

static void VK_RT_DestroyTonemapResolveImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &res = vkRT.tonemapResolve[i];
        if (res.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, res.view, NULL);
            res.view = VK_NULL_HANDLE;
        }
        if (res.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, res.image, NULL);
            res.image = VK_NULL_HANDLE;
        }
        if (res.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, res.memory, NULL);
            res.memory = VK_NULL_HANDLE;
        }
        res.width = 0;
        res.height = 0;
    }
}

// ---------------------------------------------------------------------------
// Tonemap compute pipeline
// ---------------------------------------------------------------------------

static void VK_RT_CreateTonemapPipeline(void)
{
    // --- Descriptor set layout: binding 0 = hdrIn (readonly), binding 1 = ldrOut ---
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
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.tonemapDescLayout));

    // --- Pipeline layout: descriptor set + 16-byte push constant ---
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 16; // TonemapPC: exposure, toeStrength, linearStart, linearLength

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.tonemapDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.tonemapPipelineLayout));

    // --- Compute shader ---
    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/tonemap.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Tonemap: failed to load tonemap.comp.spv — tonemap disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = compMod;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineCI = {};
    pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage = stage;
    pipelineCI.layout = vkRT.tonemapPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineCI, NULL, &vkRT.tonemapPipeline));
    vkDestroyShaderModule(vk.device, compMod, NULL);

    // --- Descriptor pool (2 storage images × VK_MAX_FRAMES_IN_FLIGHT sets) ---
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2 * VK_MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.tonemapDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.tonemapDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.tonemapDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.tonemapDescSets));

    // Mark as dirty so the first dispatch updates the image views.
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.tonemapDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Tonemap: compute pipeline initialized\n");
}

static void VK_RT_DestroyTonemapPipeline(void)
{
    if (vkRT.tonemapPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.tonemapPipeline, NULL);
        vkRT.tonemapPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.tonemapPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.tonemapPipelineLayout, NULL);
        vkRT.tonemapPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.tonemapDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.tonemapDescPool, NULL);
        vkRT.tonemapDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.tonemapDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.tonemapDescLayout, NULL);
        vkRT.tonemapDescLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VK_RT_InitTonemap(void)
{
    common->Printf("VK: initializing RT tonemapping (Phase 8.1)\n");
    VK_RT_ResizeTonemap(vk.swapchainExtent.width, vk.swapchainExtent.height);
    VK_RT_CreateTonemapPipeline();
}

void VK_RT_ShutdownTonemap(void)
{
    VK_RT_DestroyTonemapPipeline();
    VK_RT_DestroyHDRFramebuffers();
    VK_RT_DestroyHDRImages();
    VK_RT_DestroyTonemapResolveImages();
}

void VK_RT_ResizeTonemap(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyHDRFramebuffers();
    VK_RT_DestroyHDRImages();
    VK_RT_CreateHDRImages(width, height);
    VK_RT_CreateHDRFramebuffers();
    VK_RT_DestroyTonemapResolveImages();
    VK_RT_CreateTonemapResolveImages(width, height);
    // Force descriptor set update on next dispatch (image views have changed).
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.tonemapDescSetLastUpdatedFrameCount[i] = -1;
}

void VK_RT_DispatchTonemap(VkCommandBuffer cmd)
{
    if (!vkRT.isInitialized)
        return;
    if (vkRT.hdrScene[0].image == VK_NULL_HANDLE)
        return;

    const int frameIdx = (int)vk.currentFrame;
    const uint32_t swapIdx = vk.currentImageIdx;

    const bool tonemapEnabled = r_rtTonemap.GetBool() && vkRT.tonemapPipeline != VK_NULL_HANDLE &&
                                vkRT.tonemapResolve[0].image != VK_NULL_HANDLE;

    if (!tonemapEnabled)
    {
        // Bypass: blit hdrScene (RGBA16F) directly to the swapchain.
        // Values > 1.0 are clamped by the UNORM destination format — that is the
        // expected "no tonemapping" behaviour (hard clip instead of filmic roll-off).
        // RGBA16F → BGRA8: Vulkan reads source as (R,G,B,A) and writes it into the
        // destination's memory layout, so colours are semantically preserved.
        VkImageMemoryBarrier hdrToSrc = {};
        hdrToSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hdrToSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        hdrToSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hdrToSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hdrToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        hdrToSrc.image = vkRT.hdrScene[frameIdx].image;
        hdrToSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageMemoryBarrier swapToDst = {};
        swapToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapToDst.srcAccessMask = 0;
        swapToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        swapToDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        swapToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        swapToDst.image = vk.swapchainImages[swapIdx];
        swapToDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageMemoryBarrier preBlit[2] = {hdrToSrc, swapToDst};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, preBlit);

        VkImageBlit region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {(int32_t)vkRT.hdrScene[frameIdx].width, (int32_t)vkRT.hdrScene[frameIdx].height, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {(int32_t)vk.swapchainExtent.width, (int32_t)vk.swapchainExtent.height, 1};
        vkCmdBlitImage(cmd, vkRT.hdrScene[frameIdx].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       vk.swapchainImages[swapIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                       VK_FILTER_NEAREST);

        VkImageMemoryBarrier swapToPresent = {};
        swapToPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapToPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        swapToPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        swapToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        swapToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        swapToPresent.image = vk.swapchainImages[swapIdx];
        swapToPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageMemoryBarrier hdrToCA = {};
        hdrToCA.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hdrToCA.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hdrToCA.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        hdrToCA.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        hdrToCA.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hdrToCA.image = vkRT.hdrScene[frameIdx].image;
        hdrToCA.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageMemoryBarrier postBlit[2] = {swapToPresent, hdrToCA};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                             NULL, 0, NULL, 2, postBlit);
        return;
    }

    // 1. hdrScene COLOR_ATTACHMENT_OPTIMAL → GENERAL for compute shader read.
    //    The finalLayout of hdrRenderPassResume is COLOR_ATTACHMENT_OPTIMAL.
    VkImageMemoryBarrier hdrToGeneral = {};
    hdrToGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    hdrToGeneral.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    hdrToGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    hdrToGeneral.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    hdrToGeneral.image = vkRT.hdrScene[frameIdx].image;
    hdrToGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &hdrToGeneral);

    // 2. Update descriptor sets lazily (after init or resize).
    //    tonemapDescSetLastUpdatedFrameCount < 0 means "needs update".
    if (vkRT.tonemapDescSetLastUpdatedFrameCount[frameIdx] < 0)
    {
        VkDescriptorImageInfo hdrInfo = {};
        hdrInfo.imageView = vkRT.hdrScene[frameIdx].view;
        hdrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo resolveInfo = {};
        resolveInfo.imageView = vkRT.tonemapResolve[frameIdx].view;
        resolveInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = vkRT.tonemapDescSets[frameIdx];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &hdrInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = vkRT.tonemapDescSets[frameIdx];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &resolveInfo;

        vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
        vkRT.tonemapDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // 3. Push constants from CVars.
    struct TonemapPC
    {
        float exposure;
        float toeStrength;
        float linearStart;
        float linearLength;
    } pc;
    pc.exposure = r_rtTonemapExposure.GetFloat();
    pc.toeStrength = r_rtTonemapToe.GetFloat();
    pc.linearStart = r_rtTonemapLinStart.GetFloat();
    pc.linearLength = r_rtTonemapLinLen.GetFloat();

    // 4. Bind pipeline and dispatch (8×8 workgroups).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.tonemapPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.tonemapPipelineLayout, 0, 1,
                            &vkRT.tonemapDescSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.tonemapPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TonemapPC), &pc);

    uint32_t groupsX = (vkRT.tonemapResolve[frameIdx].width + 7) / 8;
    uint32_t groupsY = (vkRT.tonemapResolve[frameIdx].height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // 5. Memory barrier: compute writes → blit read.
    //    tonemapResolve stays in GENERAL throughout (valid source for vkCmdBlitImage).
    VkMemoryBarrier computeDone = {};
    computeDone.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    computeDone.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeDone.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &computeDone,
                         0, NULL, 0, NULL);

    // 6. Swapchain UNDEFINED → TRANSFER_DST_OPTIMAL.
    //    UNDEFINED as source discards old contents, which is correct since
    //    the tonemap blit will overwrite the entire image.
    VkImageMemoryBarrier swapToDst = {};
    swapToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    swapToDst.srcAccessMask = 0;
    swapToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    swapToDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swapToDst.image = vk.swapchainImages[swapIdx];
    swapToDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &swapToDst);

    // 7. Blit tonemapResolve (RGBA8, GENERAL) → swapchain (BGRA8, TRANSFER_DST).
    //    Vulkan maps each named component (R→R, G→G, B→B, A→A) across formats,
    //    so RGBA8→BGRA8 preserves colours correctly.
    VkImageBlit region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {(int32_t)vkRT.tonemapResolve[frameIdx].width, (int32_t)vkRT.tonemapResolve[frameIdx].height,
                            1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {0, 0, 0};
    region.dstOffsets[1] = {(int32_t)vk.swapchainExtent.width, (int32_t)vk.swapchainExtent.height, 1};
    vkCmdBlitImage(cmd, vkRT.tonemapResolve[frameIdx].image, VK_IMAGE_LAYOUT_GENERAL, vk.swapchainImages[swapIdx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    // 8. Swapchain TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR.
    VkImageMemoryBarrier swapToPresent = {};
    swapToPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    swapToPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    swapToPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    swapToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swapToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    swapToPresent.image = vk.swapchainImages[swapIdx];
    swapToPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
                         1, &swapToPresent);

    // 9. hdrScene GENERAL → COLOR_ATTACHMENT_OPTIMAL for the next frame's render pass.
    VkImageMemoryBarrier hdrToCA = {};
    hdrToCA.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    hdrToCA.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    hdrToCA.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    hdrToCA.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    hdrToCA.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrToCA.image = vkRT.hdrScene[frameIdx].image;
    hdrToCA.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &hdrToCA);

    if (r_vkLogRT.GetInteger() >= 2)
        common->Printf("VK RT Tonemap: dispatch frame=%d swap=%u groups=%ux%u exp=%.2f toe=%.2f\n", tr.frameCount,
                       swapIdx, groupsX, groupsY, pc.exposure, pc.toeStrength);
}
