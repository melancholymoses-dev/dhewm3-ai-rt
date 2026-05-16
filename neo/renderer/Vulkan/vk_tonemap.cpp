/*
===========================================================================

dhewm3-rt Vulkan — vk_tonemap.cpp — HDR scene buffer and Uchimura tonemap resolve.

Phase 8.1: allocates a per-frame RGBA16F HDR accumulation image (hdrScene) that
replaces the swapchain as the colour attachment for scene and composite passes.
A final compute dispatch (tonemap.comp) reads hdrScene, applies the Uchimura
filmic S-curve, and writes the mapped result to the swapchain storage image.

Dispatch order in the RT frame:
  [scene render pass  → hdrScene]
  [GI composite       → hdrScene]
  [vol composite      → hdrScene]
  VK_RT_DispatchTonemap   ← this file (tonemap.comp, outside render pass)
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
idCVar r_rtTonemapToe("r_rtTonemapToe", "2.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "Toe strength (c). 1=linear, 2=Doom-dark, 3+=crushed blacks.");
idCVar r_rtTonemapLinStart("r_rtTonemapLinStart", "0.22", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "Luminance at which the linear section begins.");
idCVar r_rtTonemapLinLen("r_rtTonemapLinLen", "0.40", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "Length of the linear section.");

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void VK_RT_CreateHDRImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &hdr = vkRT.hdrScene[i];
        hdr.width  = width;
        hdr.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        imgInfo.extent        = { width, height, 1 };
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // COLOR_ATTACHMENT: render passes write here instead of the swapchain.
        // STORAGE: tonemap.comp reads it as a storage image.
        // SAMPLED: fallback blit path if storage swapchain writes aren't supported.
        // TRANSFER_DST: initial clear on creation.
        imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                      | VK_IMAGE_USAGE_STORAGE_BIT
                      | VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &hdr.memory));
        VK_CHECK(vkBindImageMemory(vk.device, hdr.image, hdr.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image            = hdr.image;
        viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format           = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &hdr.view));

        // Transition UNDEFINED → COLOR_ATTACHMENT_OPTIMAL and clear to black.
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo cbAlloc = {};
            cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAlloc.commandPool        = vk.commandPool;
            cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAlloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmpCmd, &beginInfo);

            VkImageSubresourceRange subRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            // UNDEFINED → GENERAL for the initial clear.
            VkImageMemoryBarrier barrier = {};
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask       = 0;
            barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image               = hdr.image;
            barrier.subresourceRange    = subRange;
            vkCmdPipelineBarrier(tmpCmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &barrier);

            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, hdr.image, VK_IMAGE_LAYOUT_GENERAL,
                &clearBlack, 1, &subRange);

            // GENERAL → COLOR_ATTACHMENT_OPTIMAL: ready for the first render pass.
            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier2.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier2.image               = hdr.image;
            barrier2.subresourceRange    = subRange;
            vkCmdPipelineBarrier(tmpCmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, NULL, 0, NULL, 1, &barrier2);

            vkEndCommandBuffer(tmpCmd);

            VkFenceCreateInfo fenceCI = {};
            fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &tmpCmd;
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
        hdr.width  = 0;
        hdr.height = 0;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VK_RT_InitTonemap(void)
{
    common->Printf("VK: initializing RT tonemapping (Phase 8.1)\n");
    VK_RT_ResizeTonemap(vk.swapchainExtent.width, vk.swapchainExtent.height);
    // Tonemap compute pipeline (tonemap.comp) is created in Step 4.
}

void VK_RT_ShutdownTonemap(void)
{
    // Tonemap pipeline teardown will be added in Step 4.
    VK_RT_DestroyHDRImages();
}

void VK_RT_ResizeTonemap(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyHDRImages();
    VK_RT_CreateHDRImages(width, height);
}

void VK_RT_DispatchTonemap(VkCommandBuffer /*cmd*/)
{
    // Tonemap compute dispatch will be implemented in Step 4 (tonemap.comp pipeline).
    // Until then this is a no-op; the scene renders to hdrScene but is not resolved
    // to the swapchain via this path.
}
