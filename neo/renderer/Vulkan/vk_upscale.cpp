/*
============================================================================
Functions to Upscale images.
Targeting AMD FidelityFX FSR2 later, with simple linear scaling initially.

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
#include "renderer/Vulkan/vk_upscale.h"
#include <string.h>

extern VkShaderModule VK_LoadSPIRV(const char *path);

// UpscalePC — must match the push_constant block in upscale_blit.comp.
struct UpscalePC
{
    int32_t renderExtent[2];
    int32_t debugMode;
};

idCVar r_fsr("r_fsr", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
             "Toggle for FSR upscaling.  Values (0=bilinear AA, 1=FSR2). Off if Renderscale=1.0");
idCVar r_fsrRenderScale("r_fsrRenderScale", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Linear Render Scale Factor.  Values (0.5=Half-Resolution, 1=no-scale).");
idCVar r_fsrDebug("r_fsrDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "FSR Debug Mode.");

static void VK_RT_CreateUpscaleImage(uint32_t width, uint32_t height)
{
    vkRTImage_t &up = vkRT.hdrUpscaled;
    up.width = width;
    up.height = height;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imgInfo.extent = {width, height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &up.image));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vk.device, up.image, &memReq);

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
        common->Error("VK RT Upscale: no device-local memory type for up scene buffer");
        return;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memTypeIdx;
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &up.memory));
    VK_CHECK(vkBindImageMemory(vk.device, up.image, up.memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = up.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &up.view));

    // Clear initial state.
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
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = up.image;
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

static void VK_RT_DestroyUpscaleImage(void)
{
    vkRTImage_t &up = vkRT.hdrUpscaled;
    if (up.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(vk.device, up.view, NULL);
        up.view = VK_NULL_HANDLE;
    }
    if (up.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(vk.device, up.image, NULL);
        up.image = VK_NULL_HANDLE;
    }
    if (up.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, up.memory, NULL);
        up.memory = VK_NULL_HANDLE;
    }
    up.width = 0;
    up.height = 0;
}
// Linear filtering comes from the sampler, not the shader — CLAMP_TO_EDGE stops
// taps running off the texture, but the sub-rect edge is clamped in the shader.
static void VK_RT_CreateUpscaleSampler(void)
{
    if (vkRT.upscaleSampler != VK_NULL_HANDLE)
        return;

    VkSamplerCreateInfo sampCI = {};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.maxLod = 0.0f;
    sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_CHECK(vkCreateSampler(vk.device, &sampCI, NULL, &vkRT.upscaleSampler));
}

/*
Upscale compute pipeline: upscale_blit.comp magnifies hdrScene's render sub-rect
into hdrUpscaled at display resolution.  Descriptor sets are per frame-in-flight
because hdrScene is per-slot; hdrUpscaled is shared.
*/
static void VK_RT_CreateUpscalePipeline(void)
{
    VK_RT_CreateUpscaleSampler();

    // binding 0 = hdrScene + linear sampler, binding 1 = hdrUpscaled storage image.
    // descriptorCount is the array size of the binding (1 = a plain, non-array uniform).
    // stageFlags lists which shader stages may read it; this shader is compute-only.
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.upscaleDescLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(UpscalePC);

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.upscaleDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.upscalePipelineLayout));

    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/upscale_blit.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Upscale: failed to load upscale_blit.comp.spv — upscale disabled");
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
    pipelineCI.layout = vkRT.upscalePipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineCI, NULL, &vkRT.upscalePipeline));
    vkDestroyShaderModule(vk.device, compMod, NULL);

    // One sampler + one storage image per set, one set per frame-in-flight slot.
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = VK_MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = VK_MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.upscaleDescPool));

    // Per-slot sets: binding 0 points at hdrScene[i], which is per frame-in-flight.
    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.upscaleDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.upscaleDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.upscaleDescSets));

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.upscaleDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Upscale: compute pipeline initialized\n");
}

static void VK_RT_DestroyUpscalePipeline(void)
{
    if (vkRT.upscalePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.upscalePipeline, NULL);
        vkRT.upscalePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.upscalePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.upscalePipelineLayout, NULL);
        vkRT.upscalePipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.upscaleDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.upscaleDescPool, NULL);
        vkRT.upscaleDescPool = VK_NULL_HANDLE;
        memset(vkRT.upscaleDescSets, 0, sizeof(vkRT.upscaleDescSets));
    }
    if (vkRT.upscaleDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.upscaleDescLayout, NULL);
        vkRT.upscaleDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.upscaleSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.upscaleSampler, NULL);
        vkRT.upscaleSampler = VK_NULL_HANDLE;
    }
}

/*
Function to round size to nearest multiple of 8.  Min size is 64, and maximum is display dim
(which can be not divisible by 8).
*/
static uint32_t VK_SnapExtent8(float f, uint32_t displayDim)
{
    uint32_t fsnap = ((uint32_t)(f + 4.0f) / 8u) * 8u;
    if (fsnap < 64u)
        fsnap = 64u;
    if (fsnap > displayDim)
        fsnap = displayDim;
    return fsnap;
}

void VK_RT_UpdateRenderExtent(void)
{
    const VkExtent2D old = vk.renderExtent;

    // Clamp low so a fat-fingered cvar can't collapse the scene to the 64px floor.
    float scale = r_fsrRenderScale.GetFloat();
    if (scale < 0.3f)
        scale = 0.3f;

    uint32_t wn, hn;
    if (scale >= 1.0f)
    {
        // Identity passes the display extent through untouched: snapping here would
        // round 1366 to 1360 and break the "scale 1.0 is bit-identical" regression test.
        wn = vk.swapchainExtent.width;
        hn = vk.swapchainExtent.height;
    }
    else
    {
        wn = VK_SnapExtent8(vk.swapchainExtent.width * scale, vk.swapchainExtent.width);
        hn = VK_SnapExtent8(vk.swapchainExtent.height * scale, vk.swapchainExtent.height);
    }

    if (wn != old.width || hn != old.height)
        common->Printf("VK RT Upscale: renderExtent %ux%u -> %ux%u (display %ux%u, requested scale %.3f, "
                       "achieved %.4f x %.4f)\n",
                       old.width, old.height, wn, hn, vk.swapchainExtent.width, vk.swapchainExtent.height, scale,
                       (float)wn / (float)vk.swapchainExtent.width, (float)hn / (float)vk.swapchainExtent.height);

    vk.renderExtent.width = wn;
    vk.renderExtent.height = hn;
}

/*
Magnify hdrScene's render sub-rect to the full display extent and copy the result
back over hdrScene, so the UI can composite on top at native resolution.

Must be called OUTSIDE a render pass, after all 3D work for the frame and before
the UI pass.  No-op unless the extents actually differ.
*/
void VK_RT_DispatchUpscale(VkCommandBuffer cmd)
{
    if (!VK_RT_UpscaleActive() || vkRT.upscalePipeline == VK_NULL_HANDLE)
        return;
    if (vkRT.hdrUpscaled.image == VK_NULL_HANDLE || vkRT.hdrScene[vk.currentFrame].image == VK_NULL_HANDLE)
        return;

    const int frameIdx = (int)vk.currentFrame;
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Descriptor views only change on resize; the counter guard skips the rewrite.
    if (vkRT.upscaleDescSetLastUpdatedFrameCount[frameIdx] != (int)tr.frameCount)
    {
        VkDescriptorImageInfo srcInfo = {};
        srcInfo.sampler = vkRT.upscaleSampler;
        srcInfo.imageView = vkRT.hdrScene[frameIdx].view;
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo dstInfo = {};
        dstInfo.imageView = vkRT.hdrUpscaled.view;
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = vkRT.upscaleDescSets[frameIdx];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &srcInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = vkRT.upscaleDescSets[frameIdx];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dstInfo;

        vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
        vkRT.upscaleDescSetLastUpdatedFrameCount[frameIdx] = (int)tr.frameCount;
    }

    // 1. hdrScene COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY for the sampler.
    VkImageMemoryBarrier toRead = {};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = vkRT.hdrScene[frameIdx].image;
    toRead.subresourceRange = colorRange;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &toRead);

    // 2. Dispatch over the full display extent — that is what we are writing.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.upscalePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.upscalePipelineLayout, 0, 1,
                            &vkRT.upscaleDescSets[frameIdx], 0, NULL);

    UpscalePC pc;
    pc.renderExtent[0] = (int32_t)vk.renderExtent.width;
    pc.renderExtent[1] = (int32_t)vk.renderExtent.height;
    pc.debugMode = r_fsrDebug.GetInteger();
    vkCmdPushConstants(cmd, vkRT.upscalePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    const uint32_t groupsX = (vk.swapchainExtent.width + 7) / 8;
    const uint32_t groupsY = (vk.swapchainExtent.height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // 3. hdrUpscaled stays in GENERAL (legal for both storage write and transfer read);
    //    only the memory dependency needs expressing.  hdrScene -> TRANSFER_DST.
    VkImageMemoryBarrier preCopy[2] = {};
    preCopy[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preCopy[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preCopy[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    preCopy[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preCopy[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[0].image = vkRT.hdrUpscaled.image;
    preCopy[0].subresourceRange = colorRange;

    preCopy[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preCopy[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    preCopy[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    preCopy[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    preCopy[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    preCopy[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[1].image = vkRT.hdrScene[frameIdx].image;
    preCopy[1].subresourceRange = colorRange;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         2, preCopy);

    // 4. Same size and format, so a copy rather than a blit.
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.extent = {vk.swapchainExtent.width, vk.swapchainExtent.height, 1};
    vkCmdCopyImage(cmd, vkRT.hdrUpscaled.image, VK_IMAGE_LAYOUT_GENERAL, vkRT.hdrScene[frameIdx].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // 5. hdrScene back to COLOR_ATTACHMENT_OPTIMAL for the UI resume pass.
    VkImageMemoryBarrier toAttach = {};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = vkRT.hdrScene[frameIdx].image;
    toAttach.subresourceRange = colorRange;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
                         0, NULL, 1, &toAttach);
}

void VK_RT_InitUpscale()
{
    VK_RT_ResizeUpscale(vk.swapchainExtent.width, vk.swapchainExtent.height);
    // Init for VK pipeline for FSR etc goes here.
    VK_RT_CreateUpscalePipeline();
}

void VK_RT_ResizeUpscale(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyUpscaleImage();
    VK_RT_CreateUpscaleImage(width, height);
    // Covers the resize path; the per-frame call in VK_RB_DrawView covers cvar changes.
    VK_RT_UpdateRenderExtent();
    // hdrUpscaled's view changed, so the bound descriptors are stale.
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.upscaleDescSetLastUpdatedFrameCount[i] = -1;
}

void VK_RT_ShutdownUpscale(void)
{
    VK_RT_DestroyUpscaleImage();
    VK_RT_DestroyUpscalePipeline();
}

float VK_RT_RenderScaleX(void)
{
    return (float)vk.renderExtent.width / (float)vk.swapchainExtent.width;
}
float VK_RT_RenderScaleY(void)
{
    return (float)vk.renderExtent.height / (float)vk.swapchainExtent.height;
}

idScreenRect VK_RT_ScaleDisplayRect(const idScreenRect &s)
{
    const float sx = VK_RT_RenderScaleX();
    const float sy = VK_RT_RenderScaleY();

    idScreenRect sr;
    sr.x1 = (short)idMath::Floor(sx * s.x1);
    sr.x2 = (short)idMath::Ceil(sx * s.x2);
    sr.y1 = (short)idMath::Floor(sy * s.y1);
    sr.y2 = (short)idMath::Ceil(sy * s.y2);
    sr.zmin = s.zmin;
    sr.zmax = s.zmax;
    return sr;
}

bool VK_RT_UpscaleActive(void)
{
    return (vk.renderExtent.width != vk.swapchainExtent.width) || (vk.renderExtent.height != vk.swapchainExtent.height);
}
