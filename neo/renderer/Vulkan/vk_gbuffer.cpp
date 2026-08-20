/*
===========================================================================

dhewm3-rt Vulkan — vk_gbuffer.cpp — G-buffer normal/F0 + albedo image lifetime.

Stage 3.5 (see docs/plans/gbuffer_normal_pass.md): two thin per-frame images
written by the depth prepass (VK_RB_FillDepthBuffer) — gbufNormal, sampled by
the RT reflection/AO/GI rgens, and gbufAlbedo (docs/plans/gi_albedo_target.md),
sampled by the GI albedo-modulate compute pass. Both R8G8B8A8_UNORM:
  gbufNormal: rgb = world-space bump-mapped shading normal (n * 0.5 + 0.5),
              a = normalized F0 (the Stage-2 specular remap); 0 means "no
              G-buffer data here" so consumers fall back to rt_ReconstructNormal.
  gbufAlbedo: rgb = diffuse map sample; unwritten pixels stay at the WHITE
              clear value (vk_backend.cpp) so GI passes through unmodulated
              rather than being killed to black.

Requires VkPhysicalDeviceFeatures.independentBlend (multiple colour attachments
with differing per-attachment blend/write-mask state — see Step 0 in the
plan). vk.gbufferSupported latches that check at device-creation time; when
false, every function here is a no-op and vkRT.gbufNormal/gbufAlbedo stay
all-NULL, leaving the existing depth-only prepass path untouched.

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
#include "renderer/Vulkan/vk_gbuffer.h"

extern idCVar r_vkLogRT;

// P9 (docs/plans/rt_optimization_tuning.md): AO and GI read their shading normal
// from this buffer instead of rebuilding it from depth gradients. Set 0 to force
// both back onto rt_ReconstructNormal for an A/B — the ALU cost returns, and so
// does the flat-polygon look (no bump detail in AO/GI). Reflections are NOT
// affected; they consume the G-buffer unconditionally (Wave 1b).
idCVar r_rtGbufNormals("r_rtGbufNormals", "1", CVAR_RENDERER | CVAR_BOOL,
                       "P9: AO/GI take their shading normal from the G-buffer "
                       "(0 = legacy depth-gradient reconstruction, for A/B)");

// ---------------------------------------------------------------------------
// VK_RT_CreateGBufferImages
// Allocates per-frame R8G8B8A8_UNORM normal/F0 buffers at the given resolution,
// plus the receiver-albedo buffers (gi_albedo_target.md) with identical
// lifetime: attachment 2 of the HDR render pass, written by the same prepass,
// sampled by the GI albedo-modulate compute pass.
// ---------------------------------------------------------------------------

static void VK_RT_CreateGBufferImages(uint32_t width, uint32_t height)
{
    for (int slot = 0; slot < 2 * VK_MAX_FRAMES_IN_FLIGHT; slot++)
    {
        // First VK_MAX_FRAMES_IN_FLIGHT slots: gbufNormal; then gbufAlbedo.
        const bool isAlbedo = slot >= VK_MAX_FRAMES_IN_FLIGHT;
        const int i = slot % VK_MAX_FRAMES_IN_FLIGHT;
        vkReflBuffer_t &gb = isAlbedo ? vkRT.gbufAlbedo[i] : vkRT.gbufNormal[i];
        gb.width = width;
        gb.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // COLOR_ATTACHMENT: written as a second attachment on the HDR render passes.
        // SAMPLED: read back by the reflection (and later AO/GI) RT shaders.
        imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &gb.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, gb.image, &memReq);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = VK_FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &gb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, gb.image, gb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = gb.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &gb.view));

        // Transition UNDEFINED → COLOR_ATTACHMENT_OPTIMAL so the render pass's
        // COLOR_ATTACHMENT_OPTIMAL initial/final layout (Step 2) is honest from
        // the first frame. One-time submit, same pattern as VK_RT_CreateReflImages.
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
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.image = gb.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

            vkEndCommandBuffer(tmpCmd);

            VkFenceCreateInfo fenceCI = {};
            fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &tmpCmd;
            int submitResult = vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, fence);
            if (submitResult != 0)
            {
                common->Printf("VK: vkQueueSubmit failed with error %d in VK_RT_CreateGBufferImages\n", submitResult);
                fflush(NULL);
            }

            vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk.device, fence, NULL);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GBuffer: allocated %dx%d R8G8B8A8_UNORM normal/F0 + albedo images (%d frames in flight)\n",
                       width, height, VK_MAX_FRAMES_IN_FLIGHT);
}

static void VK_RT_DestroyGBufferImages(void)
{
    for (int slot = 0; slot < 2 * VK_MAX_FRAMES_IN_FLIGHT; slot++)
    {
        const bool isAlbedo = slot >= VK_MAX_FRAMES_IN_FLIGHT;
        const int i = slot % VK_MAX_FRAMES_IN_FLIGHT;
        vkReflBuffer_t &gb = isAlbedo ? vkRT.gbufAlbedo[i] : vkRT.gbufNormal[i];
        if (gb.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, gb.view, NULL);
            gb.view = VK_NULL_HANDLE;
        }
        if (gb.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, gb.image, NULL);
            gb.image = VK_NULL_HANDLE;
        }
        if (gb.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, gb.memory, NULL);
            gb.memory = VK_NULL_HANDLE;
        }
        gb.width = 0;
        gb.height = 0;
    }
}

// ---------------------------------------------------------------------------
// Public entry points
//
// No separate "Init": VK_RT_InitTonemap's first call is VK_RT_ResizeTonemap,
// which calls VK_RT_ResizeGBuffer below before creating the HDR framebuffers
// that reference gbufNormal[i].view — that first call is this module's init.
// ---------------------------------------------------------------------------

void VK_RT_ShutdownGBuffer(void)
{
    VK_RT_DestroyGBufferImages();
}

void VK_RT_ResizeGBuffer(uint32_t width, uint32_t height)
{
    if (!vk.gbufferSupported)
    {
        static bool warned = false;
        if (!warned)
        {
            common->Printf("VK RT GBuffer: independentBlend not supported — G-buffer normal pass disabled\n");
            warned = true;
        }
        return;
    }

    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyGBufferImages();
    VK_RT_CreateGBufferImages(width, height);
}
