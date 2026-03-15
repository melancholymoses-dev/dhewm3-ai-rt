/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - texture/image management.

Wraps idImage for Vulkan: uploads RGBA8 pixel data to a VkImage with a full
mip chain (generated via vkCmdBlitImage), creates the matching VkImageView
and VkSampler, and provides VK_Image_GetDescriptorInfo() for use in the
interaction descriptor set.

Hooked into idImage::GenerateImage() and idImage::PurgeImage() in
Image_load.cpp via #ifdef DHEWM3_VULKAN guards.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#ifdef DHEWM3_VULKAN

#include "sys/platform.h"
#include "renderer/Image.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"

#include <math.h> // log2, floor
#include <string.h>

// ---------------------------------------------------------------------------
// vkImageData_t — private Vulkan resources per idImage
// The idImage stores only an opaque "struct vkImageData_t *vkData" pointer.
// ---------------------------------------------------------------------------

struct vkImageData_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t CalcMipLevels(int w, int h)
{
    int maxDim = (w > h) ? w : h;
    if (maxDim <= 0)
        return 1;
    uint32_t levels = 1;
    while (maxDim > 1)
    {
        maxDim >>= 1;
        levels++;
    }
    return levels;
}

static VkFilter MapFilter(textureFilter_t f)
{
    switch (f)
    {
    case TF_NEAREST:
        return VK_FILTER_NEAREST;
    default:
        return VK_FILTER_LINEAR;
    }
}

static VkSamplerAddressMode MapRepeat(textureRepeat_t r)
{
    switch (r)
    {
    case TR_CLAMP:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TR_CLAMP_TO_ZERO:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case TR_CLAMP_TO_ZERO_ALPHA:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSamplerMipmapMode MapMipmapMode(textureFilter_t f)
{
    switch (f)
    {
    case TF_NEAREST:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    default:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
}

// ---------------------------------------------------------------------------
// Fallback 1x1 opaque-white image used when an idImage has no vkData yet
// (e.g. the image was generated before the Vulkan device was ready, or the
// upload silently failed).  Prevents unwritten descriptor reads on the GPU.
// ---------------------------------------------------------------------------

static vkImageData_t s_fallback;
static bool s_fallbackValid = false;

void VK_Image_Init(void)
{
    if (s_fallbackValid)
        return;

    const byte white[4] = {255, 255, 255, 255};
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &ci, NULL, &s_fallback.image));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, s_fallback.image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &s_fallback.memory));
    VK_CHECK(vkBindImageMemory(vk.device, s_fallback.image, s_fallback.memory, 0));

    // Upload via staging
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &stagingBuf));
    VkMemoryRequirements smr;
    vkGetBufferMemoryRequirements(vk.device, stagingBuf, &smr);
    VkMemoryAllocateInfo sai = {};
    sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sai.allocationSize = smr.size;
    sai.memoryTypeIndex = VK_FindMemoryType(smr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &sai, NULL, &stagingMem));
    VK_CHECK(vkBindBufferMemory(vk.device, stagingBuf, stagingMem, 0));
    void *ptr;
    VK_CHECK(vkMapMemory(vk.device, stagingMem, 0, 4, 0, &ptr));
    memcpy(ptr, white, 4);
    vkUnmapMemory(vk.device, stagingMem);

    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
    VK_TransitionImageLayout(cmd, s_fallback.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy rgn = {};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, s_fallback.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    VK_TransitionImageLayout(cmd, s_fallback.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_EndSingleTimeCommands(cmd);
    vkDestroyBuffer(vk.device, stagingBuf, NULL);
    vkFreeMemory(vk.device, stagingMem, NULL);

    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s_fallback.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &vci, NULL, &s_fallback.view));

    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &s_fallback.sampler));

    s_fallbackValid = true;
}

void VK_Image_Shutdown(void)
{
    if (!s_fallbackValid)
        return;
    vkDestroySampler(vk.device, s_fallback.sampler, NULL);
    vkDestroyImageView(vk.device, s_fallback.view, NULL);
    vkDestroyImage(vk.device, s_fallback.image, NULL);
    vkFreeMemory(vk.device, s_fallback.memory, NULL);
    memset(&s_fallback, 0, sizeof(s_fallback));
    s_fallbackValid = false;
}

void VK_Image_GetFallbackDescriptorInfo(VkDescriptorImageInfo *out)
{
    out->sampler = s_fallback.sampler;
    out->imageView = s_fallback.view;
    out->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Forward declaration — defined later in this file
void VK_Image_Purge(idImage *img);

// ---------------------------------------------------------------------------
// VK_Image_Upload
//
// Called from idImage::GenerateImage() after the OpenGL upload.
// `pic` is the RGBA8 mip-0 data; remaining mips are generated on the GPU
// using vkCmdBlitImage with LINEAR filtering.
// ---------------------------------------------------------------------------

void VK_Image_Upload(idImage *img, const byte *pic, int width, int height)
{
    if (!vk.isInitialized || !pic || width <= 0 || height <= 0)
        return;

    // Free any existing Vulkan resources for this image (e.g. during reload)
    if (img->vkData)
    {
        VK_Image_Purge(img);
    }

    const uint32_t mipLevels = CalcMipLevels(width, height);
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize mip0Size = (VkDeviceSize)width * height * 4;

    // --- Create the VkImage ---
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent = {(uint32_t)width, (uint32_t)height, 1};
    imgInfo.mipLevels = mipLevels;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT   // staging copy
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT // blit source for mip gen
                    | VK_IMAGE_USAGE_SAMPLED_BIT;     // shader sampling
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkImageData_t *vkd = new vkImageData_t();
    memset(vkd, 0, sizeof(*vkd));

    if (vkCreateImage(vk.device, &imgInfo, NULL, &vkd->image) != VK_SUCCESS)
    {
        common->Warning("VK_Image_Upload: vkCreateImage failed for '%s'", img->imgName.c_str());
        delete vkd;
        return;
    }

    // --- Allocate device-local memory ---
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vk.device, vkd->image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &vkd->memory) != VK_SUCCESS)
    {
        vkDestroyImage(vk.device, vkd->image, NULL);
        delete vkd;
        return;
    }
    vkBindImageMemory(vk.device, vkd->image, vkd->memory, 0);

    // --- Upload mip 0 via staging buffer ---
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    {
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = mip0Size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(vk.device, &bi, NULL, &stagingBuf);

        VkMemoryRequirements smr;
        vkGetBufferMemoryRequirements(vk.device, stagingBuf, &smr);
        VkMemoryAllocateInfo sai = {};
        sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        sai.allocationSize = smr.size;
        sai.memoryTypeIndex = VK_FindMemoryType(smr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(vk.device, &sai, NULL, &stagingMem);
        vkBindBufferMemory(vk.device, stagingBuf, stagingMem, 0);

        void *ptr;
        vkMapMemory(vk.device, stagingMem, 0, mip0Size, 0, &ptr);
        memcpy(ptr, pic, (size_t)mip0Size);
        vkUnmapMemory(vk.device, stagingMem);
    }

    // --- Record command buffer for upload + mip generation ---
    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();

    // Transition all mips to TRANSFER_DST for the initial copy
    {
        VkImageMemoryBarrier bar = {};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = vkd->image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &bar);
    }

    // Copy staging buffer → mip 0
    {
        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        vkCmdCopyBufferToImage(cmd, stagingBuf, vkd->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // Generate mips 1..N via vkCmdBlitImage
    int mipW = width, mipH = height;
    for (uint32_t i = 1; i < mipLevels; i++)
    {
        // Transition mip[i-1]: TRANSFER_DST → TRANSFER_SRC
        {
            VkImageMemoryBarrier bar = {};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = vkd->image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1};
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &bar);
        }

        int nextW = (mipW > 1) ? (mipW >> 1) : 1;
        int nextH = (mipH > 1) ? (mipH >> 1) : 1;

        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipW, mipH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {nextW, nextH, 1};
        vkCmdBlitImage(cmd, vkd->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkd->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        // Transition mip[i-1]: TRANSFER_SRC → SHADER_READ_ONLY
        {
            VkImageMemoryBarrier bar = {};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = vkd->image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1};
            bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &bar);
        }

        mipW = nextW;
        mipH = nextH;
    }

    // Transition the last mip: TRANSFER_DST → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier bar = {};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = vkd->image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1, 0, 1};
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &bar);
    }

    VK_EndSingleTimeCommands(cmd);

    // Free staging buffer
    vkDestroyBuffer(vk.device, stagingBuf, NULL);
    vkFreeMemory(vk.device, stagingMem, NULL);

    // --- VkImageView ---
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkd->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk.device, &viewInfo, NULL, &vkd->view) != VK_SUCCESS)
    {
        vkDestroyImage(vk.device, vkd->image, NULL);
        vkFreeMemory(vk.device, vkd->memory, NULL);
        delete vkd;
        return;
    }

    // --- VkSampler ---
    VkFilter magFilter = MapFilter(img->filter);
    VkFilter minFilter = magFilter;
    VkSamplerMipmapMode mipmapMode = MapMipmapMode(img->filter);
    VkSamplerAddressMode addrMode = MapRepeat(img->repeat);

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = magFilter;
    samplerInfo.minFilter = minFilter;
    samplerInfo.mipmapMode = mipmapMode;
    samplerInfo.addressModeU = addrMode;
    samplerInfo.addressModeV = addrMode;
    samplerInfo.addressModeW = addrMode;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_FALSE; // enable later via r_useAnisotropicFiltering
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = (float)(mipLevels - 1);
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(vk.device, &samplerInfo, NULL, &vkd->sampler) != VK_SUCCESS)
    {
        vkDestroyImageView(vk.device, vkd->view, NULL);
        vkDestroyImage(vk.device, vkd->image, NULL);
        vkFreeMemory(vk.device, vkd->memory, NULL);
        delete vkd;
        return;
    }

    img->vkData = vkd;
}

// ---------------------------------------------------------------------------
// VK_Image_Purge
// Called from idImage::PurgeImage() to destroy Vulkan resources.
// ---------------------------------------------------------------------------

void VK_Image_Purge(idImage *img)
{
    if (!img->vkData)
        return;

    // Wait for the device to be idle before destroying resources that may
    // still be referenced by in-flight frames.  A proper implementation
    // should use per-frame deferred deletion queues.
    vkDeviceWaitIdle(vk.device);

    vkImageData_t *vkd = img->vkData;
    if (vkd->sampler != VK_NULL_HANDLE)
        vkDestroySampler(vk.device, vkd->sampler, NULL);
    if (vkd->view != VK_NULL_HANDLE)
        vkDestroyImageView(vk.device, vkd->view, NULL);
    if (vkd->image != VK_NULL_HANDLE)
        vkDestroyImage(vk.device, vkd->image, NULL);
    if (vkd->memory != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, vkd->memory, NULL);

    delete vkd;
    img->vkData = NULL;
}

// ---------------------------------------------------------------------------
// VK_Image_GetDescriptorInfo
// Fills a VkDescriptorImageInfo for use in a combined-image-sampler write.
// Returns false if the Vulkan image is not yet uploaded (caller should use
// a fallback/default white texture descriptor instead).
// ---------------------------------------------------------------------------

bool VK_Image_GetDescriptorInfo(idImage *img, VkDescriptorImageInfo *out)
{
    if (!img || !img->vkData)
        return false;
    vkImageData_t *vkd = img->vkData;
    out->sampler = vkd->sampler;
    out->imageView = vkd->view;
    out->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

#endif // DHEWM3_VULKAN
