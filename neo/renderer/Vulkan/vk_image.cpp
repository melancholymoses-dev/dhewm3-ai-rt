/*
===========================================================================

dhewm3-rt Vulkan backend - texture/image management.

Wraps idImage for Vulkan: uploads RGBA8 pixel data to a VkImage with a full
mip chain (generated via vkCmdBlitImage), creates the matching VkImageView
and VkSampler, and provides VK_Image_GetDescriptorInfo() for use in the
interaction descriptor set.

Hooked into idImage via the backendData void* field (see Image.h).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/Image.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"

#include <math.h> // log2, floor
#include <string.h>

// Positive LOD bias on bump maps reduces high-frequency normal aliasing
// (black/white moire shimmer) on grazing-angle lit surfaces.
static idCVar r_vkBumpMipBias("r_vkBumpMipBias", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                              "Extra positive mip LOD bias for Vulkan bump-map samplers");

// ---------------------------------------------------------------------------
// vkImageData_t — private Vulkan resources per idImage
// The idImage stores only an opaque "struct vkImageData_t *vkData" pointer.
// ---------------------------------------------------------------------------

struct vkImageData_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageView cubeView;
    VkSampler sampler;
    uint32_t width;
    uint32_t height;
};

// ---------------------------------------------------------------------------
// Deferred image deletion garbage ring
// Images purged while the GPU may still be reading them are queued here.
// VK_Image_DrainGarbage(frameIdx) is called after the per-frame fence fires.
// ---------------------------------------------------------------------------

static const int VK_IMAGE_GARBAGE_MAX = 512; // max purges per frame slot

static vkImageData_t *s_imageGarbage[VK_MAX_FRAMES_IN_FLIGHT][VK_IMAGE_GARBAGE_MAX];
static int s_imageGarbageCount[VK_MAX_FRAMES_IN_FLIGHT] = {};
static uint32_t s_imageGarbageOverflowCount[VK_MAX_FRAMES_IN_FLIGHT] = {};

static float s_cachedMaxSamplerAnisotropy = 1.0f;
static float s_cachedMaxSamplerLodBias = 0.0f;
static bool s_samplerQualityLimitsCached = false;

static void VK_DestroyImageData(vkImageData_t *vkd)
{
    if (vkd->sampler != VK_NULL_HANDLE)
        vkDestroySampler(vk.device, vkd->sampler, NULL);
    if (vkd->cubeView != VK_NULL_HANDLE)
        vkDestroyImageView(vk.device, vkd->cubeView, NULL);
    if (vkd->view != VK_NULL_HANDLE)
        vkDestroyImageView(vk.device, vkd->view, NULL);
    if (vkd->image != VK_NULL_HANDLE)
        vkDestroyImage(vk.device, vkd->image, NULL);
    if (vkd->memory != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, vkd->memory, NULL);
    delete vkd;
}

void VK_Image_DrainGarbage(uint32_t frameIdx)
{
    for (int i = 0; i < s_imageGarbageCount[frameIdx]; i++)
        VK_DestroyImageData(s_imageGarbage[frameIdx][i]);
    s_imageGarbageCount[frameIdx] = 0;

    if (s_imageGarbageOverflowCount[frameIdx] > 0)
    {
        common->Printf("VK: image garbage overflow summary for frame slot %u: %u immediate destroys after ring-full\n",
                       frameIdx, s_imageGarbageOverflowCount[frameIdx]);
        s_imageGarbageOverflowCount[frameIdx] = 0;
    }
}

// Drain all frame slots — called at shutdown.
void VK_Image_DrainAllGarbage(void)
{
    for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
        VK_Image_DrainGarbage(f);
}

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

static void VK_GetSamplerQualityLimits(float *outMaxAnisotropy, float *outMaxLodBias)
{
    if (!s_samplerQualityLimitsCached)
    {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(vk.physicalDevice, &props);
        s_cachedMaxSamplerAnisotropy = props.limits.maxSamplerAnisotropy;
        s_cachedMaxSamplerLodBias = props.limits.maxSamplerLodBias;
        s_samplerQualityLimitsCached = true;
    }

    if (outMaxAnisotropy)
        *outMaxAnisotropy = s_cachedMaxSamplerAnisotropy;
    if (outMaxLodBias)
        *outMaxLodBias = s_cachedMaxSamplerLodBias;
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

// ---------------------------------------------------------------------------
// Cinematic (video) image — one shared 2D image re-uploaded each frame.
// Matches the role of globalImages->cinematicImage in the GL path.
// ---------------------------------------------------------------------------

static vkImageData_t s_cinImage;
static bool s_cinImageValid = false;
static int s_cinImageW = 0;
static int s_cinImageH = 0;
static VkBuffer s_cinStaging = VK_NULL_HANDLE;
static VkDeviceMemory s_cinStagingMem = VK_NULL_HANDLE;
static void *s_cinStagingPtr = NULL;
static VkImageLayout s_cinLayout = VK_IMAGE_LAYOUT_UNDEFINED;

// Destroy the current cinematic image and staging buffer (called on shutdown or resize).
static void VK_DestroyCinematicImage(void)
{
    if (!s_cinImageValid)
        return;

    if (s_cinStagingPtr)
        vkUnmapMemory(vk.device, s_cinStagingMem);
    if (s_cinStaging != VK_NULL_HANDLE)
        vkDestroyBuffer(vk.device, s_cinStaging, NULL);
    if (s_cinStagingMem != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, s_cinStagingMem, NULL);

    vkDestroySampler(vk.device, s_cinImage.sampler, NULL);
    vkDestroyImageView(vk.device, s_cinImage.view, NULL);
    vkDestroyImage(vk.device, s_cinImage.image, NULL);
    vkFreeMemory(vk.device, s_cinImage.memory, NULL);

    memset(&s_cinImage, 0, sizeof(s_cinImage));
    s_cinImageValid = false;
    s_cinStaging = VK_NULL_HANDLE;
    s_cinStagingMem = VK_NULL_HANDLE;
    s_cinStagingPtr = NULL;
    s_cinImageW = 0;
    s_cinImageH = 0;
    s_cinLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VK_Image_Shutdown(void)
{
    VK_DestroyCinematicImage();
    s_samplerQualityLimitsCached = false;

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

// Ensure the cinematic VkImage + persistent staging buffer exist at the given size.
// Recreates if the size has grown.
static void VK_EnsureCinematicImage(int w, int h)
{
    if (s_cinImageValid && w <= s_cinImageW && h <= s_cinImageH)
        return;

    // Drain before destroying — we might be in flight
    vkDeviceWaitIdle(vk.device);
    VK_DestroyCinematicImage();

    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize sz = (VkDeviceSize)w * h * 4;

    // --- VkImage ---
    VkImageCreateInfo ic = {};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = fmt;
    ic.extent = {(uint32_t)w, (uint32_t)h, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &ic, NULL, &s_cinImage.image));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, s_cinImage.image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &s_cinImage.memory));
    VK_CHECK(vkBindImageMemory(vk.device, s_cinImage.image, s_cinImage.memory, 0));

    // --- Staging buffer (persistently mapped host-coherent) ---
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sz;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &s_cinStaging));

    VkMemoryRequirements smr;
    vkGetBufferMemoryRequirements(vk.device, s_cinStaging, &smr);
    VkMemoryAllocateInfo sai = {};
    sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sai.allocationSize = smr.size;
    sai.memoryTypeIndex = VK_FindMemoryType(smr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &sai, NULL, &s_cinStagingMem));
    VK_CHECK(vkBindBufferMemory(vk.device, s_cinStaging, s_cinStagingMem, 0));
    VK_CHECK(vkMapMemory(vk.device, s_cinStagingMem, 0, sz, 0, &s_cinStagingPtr));

    // --- View ---
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s_cinImage.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(vk.device, &vci, NULL, &s_cinImage.view));

    // --- Sampler ---
    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &s_cinImage.sampler));

    s_cinImageValid = true;
    s_cinImageW = w;
    s_cinImageH = h;
    s_cinLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// Record a cinematic frame upload into cmd (must be outside any render pass).
// rgba is w*h*4 RGBA bytes; ownership stays with the caller (idCinematic).
bool VK_Image_UpdateCinematic(VkCommandBuffer cmd, const byte *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0)
        return false;

    VK_EnsureCinematicImage(w, h);
    if (!s_cinImageValid)
        return false;

    // Copy pixel data into the persistently-mapped staging buffer
    memcpy(s_cinStagingPtr, rgba, (size_t)w * h * 4);

    // Transition: current layout → TRANSFER_DST
    VkImageMemoryBarrier toXfer = {};
    toXfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toXfer.oldLayout = s_cinLayout;
    toXfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toXfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toXfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toXfer.image = s_cinImage.image;
    toXfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toXfer.srcAccessMask = (s_cinLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toXfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         (s_cinLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                    : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toXfer);

    // Copy staging → image
    VkBufferImageCopy rgn = {};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, s_cinStaging, s_cinImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);

    // Transition: TRANSFER_DST → SHADER_READ_ONLY
    VkImageMemoryBarrier toRead = toXfer;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &toRead);

    s_cinLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void VK_Image_GetCinematicDescriptorInfo(VkDescriptorImageInfo *out)
{
    if (s_cinImageValid)
    {
        out->sampler = s_cinImage.sampler;
        out->imageView = s_cinImage.view;
        out->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    else
    {
        // Nothing uploaded yet — fall back to black (don't flash white)
        VK_Image_GetFallbackDescriptorInfo(out);
    }
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
    if (img->backendData)
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
        common->Warning("VK_Image_Upload: vkAllocateMemory failed for '%s'", img->imgName.c_str());
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

    // Staging buffer must stay alive until the GPU copy completes.
    VK_DeferStagingFree(stagingBuf, stagingMem);

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
        common->Warning("VK_Image_Upload: vkCreateImageView failed for '%s'", img->imgName.c_str());
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
    float maxAniso = 1.0f;
    float maxLodBias = 0.0f;
    VK_GetSamplerQualityLimits(&maxAniso, &maxLodBias);

    float lodBias = idImageManager::image_lodbias.GetFloat();
    if (img->depth == TD_BUMP)
        lodBias += r_vkBumpMipBias.GetFloat();
    if (lodBias > maxLodBias)
        lodBias = maxLodBias;
    if (lodBias < -maxLodBias)
        lodBias = -maxLodBias;
    samplerInfo.mipLodBias = lodBias;

    float requestedAniso = idImageManager::image_anisotropy.GetFloat();
    if (requestedAniso < 1.0f)
        requestedAniso = 1.0f;
    if (requestedAniso > maxAniso)
        requestedAniso = maxAniso;

    const bool canUseAniso = (img->filter != TF_NEAREST && mipLevels > 1 && requestedAniso > 1.0f);
    samplerInfo.anisotropyEnable = canUseAniso ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = canUseAniso ? requestedAniso : 1.0f;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = (float)(mipLevels - 1);
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(vk.device, &samplerInfo, NULL, &vkd->sampler) != VK_SUCCESS)
    {
        common->Warning("VK_Image_Upload: vkCreateSampler failed for '%s'", img->imgName.c_str());
        vkDestroyImageView(vk.device, vkd->view, NULL);
        vkDestroyImage(vk.device, vkd->image, NULL);
        vkFreeMemory(vk.device, vkd->memory, NULL);
        delete vkd;
        return;
    }

    vkd->width = (uint32_t)width;
    vkd->height = (uint32_t)height;

    img->backendData = vkd;
}

// ---------------------------------------------------------------------------
// VK_Image_UploadCubemap
//
// Called from idImage::GenerateCubeImage() on the Vulkan path.
// Uploads all 6 RGBA8 faces (each size x size) as a VkImage cube array.
// Creates both a cube view (for samplerCube stages) and a 2D view of face 0
// for legacy sampler2D paths.
// ---------------------------------------------------------------------------

void VK_Image_UploadCubemap(idImage *img, const byte *const pic[6], int size)
{
    if (!vk.isInitialized || !pic || size <= 0)
        return;
    for (int i = 0; i < 6; i++)
        if (!pic[i])
            return;

    if (img->backendData)
        VK_Image_Purge(img);

    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize faceSize = (VkDeviceSize)size * size * 4;
    const VkDeviceSize totalSize = faceSize * 6;

    // --- Create cube-compatible VkImage (2D array, 6 layers) ---
    VkImageCreateInfo imgCI = {};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = format;
    imgCI.extent = {(uint32_t)size, (uint32_t)size, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 6;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkImageData_t *vkd = new vkImageData_t();
    memset(vkd, 0, sizeof(*vkd));

    if (vkCreateImage(vk.device, &imgCI, NULL, &vkd->image) != VK_SUCCESS)
    {
        common->Warning("VK_Image_UploadCubemap: vkCreateImage failed for '%s'", img->imgName.c_str());
        delete vkd;
        return;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vk.device, vkd->image, &memReqs);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &vkd->memory) != VK_SUCCESS)
    {
        common->Warning("VK_Image_UploadCubemap: vkAllocateMemory failed for '%s'", img->imgName.c_str());
        vkDestroyImage(vk.device, vkd->image, NULL);
        delete vkd;
        return;
    }
    vkBindImageMemory(vk.device, vkd->image, vkd->memory, 0);

    // --- Staging buffer: all 6 faces contiguous ---
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    {
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = totalSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &stagingBuf));

        VkMemoryRequirements smr;
        vkGetBufferMemoryRequirements(vk.device, stagingBuf, &smr);
        VkMemoryAllocateInfo sai = {};
        sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        sai.allocationSize = smr.size;
        sai.memoryTypeIndex = VK_FindMemoryType(smr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &sai, NULL, &stagingMem));
        VK_CHECK(vkBindBufferMemory(vk.device, stagingBuf, stagingMem, 0));

        void *ptr;
        VK_CHECK(vkMapMemory(vk.device, stagingMem, 0, totalSize, 0, &ptr));
        for (int i = 0; i < 6; i++)
            memcpy((byte *)ptr + i * faceSize, pic[i], (size_t)faceSize);
        vkUnmapMemory(vk.device, stagingMem);
    }

    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();

    // Transition all 6 layers to TRANSFER_DST
    {
        VkImageMemoryBarrier bar = {};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = vkd->image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &bar);
    }

    // Copy each face from the staging buffer
    VkBufferImageCopy regions[6];
    for (int i = 0; i < 6; i++)
    {
        regions[i] = {};
        regions[i].bufferOffset = (VkDeviceSize)i * faceSize;
        regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[i].imageSubresource.mipLevel = 0;
        regions[i].imageSubresource.baseArrayLayer = (uint32_t)i;
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageExtent = {(uint32_t)size, (uint32_t)size, 1};
    }
    vkCmdCopyBufferToImage(cmd, stagingBuf, vkd->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

    // Transition all 6 layers to SHADER_READ_ONLY
    {
        VkImageMemoryBarrier bar = {};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = vkd->image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &bar);
    }

    VK_EndSingleTimeCommands(cmd);

    // Staging buffer must stay alive until the GPU copy completes.
    VK_DeferStagingFree(stagingBuf, stagingMem);

    // --- Cube view (for samplerCube binding) ---
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkd->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;
    if (vkCreateImageView(vk.device, &viewInfo, NULL, &vkd->cubeView) != VK_SUCCESS)
    {
        common->Warning("VK_Image_UploadCubemap: vkCreateImageView(cube) failed for '%s'", img->imgName.c_str());
        vkDestroyImage(vk.device, vkd->image, NULL);
        vkFreeMemory(vk.device, vkd->memory, NULL);
        delete vkd;
        return;
    }

    // --- 2D view of face 0 (for sampler2D compatibility paths) ---
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk.device, &viewInfo, NULL, &vkd->view) != VK_SUCCESS)
    {
        common->Warning("VK_Image_UploadCubemap: vkCreateImageView(2D) failed for '%s'", img->imgName.c_str());
        vkDestroyImageView(vk.device, vkd->cubeView, NULL);
        vkDestroyImage(vk.device, vkd->image, NULL);
        vkFreeMemory(vk.device, vkd->memory, NULL);
        delete vkd;
        return;
    }

    // --- Sampler (clamp-to-edge, linear, no mips) ---
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(vk.device, &samplerInfo, NULL, &vkd->sampler) != VK_SUCCESS)
    {
        common->Warning("VK_Image_UploadCubemap: vkCreateSampler failed for '%s'", img->imgName.c_str());
        vkDestroyImageView(vk.device, vkd->cubeView, NULL);
        vkDestroyImageView(vk.device, vkd->view, NULL);
        vkDestroyImage(vk.device, vkd->image, NULL);
        vkFreeMemory(vk.device, vkd->memory, NULL);
        delete vkd;
        return;
    }

    vkd->width = (uint32_t)size;
    vkd->height = (uint32_t)size;

    img->backendData = vkd;
}

// ---------------------------------------------------------------------------
// VK_Image_Purge
// Called from idImage::PurgeImage() to destroy Vulkan resources.
// ---------------------------------------------------------------------------

void VK_Image_Purge(idImage *img)
{
    if (!img->backendData)
        return;

    vkImageData_t *vkd = (vkImageData_t *)img->backendData;
    img->backendData = NULL;

    // If the Vulkan device isn't up yet (or we're at shutdown after draining),
    // destroy immediately.  Otherwise queue for the current frame slot so the
    // GPU cannot still be reading the resource.
    if (!vk.isInitialized)
    {
        VK_DestroyImageData(vkd);
        return;
    }

    uint32_t frameIdx = vk.currentFrame;
    if (s_imageGarbageCount[frameIdx] < VK_IMAGE_GARBAGE_MAX)
    {
        s_imageGarbage[frameIdx][s_imageGarbageCount[frameIdx]++] = vkd;
    }
    else
    {
        // Garbage ring full — destroy immediately. Log once per frame slot to avoid spam.
        // If the device is already lost (e.g., shutdown after a crash), skip the
        // stall to avoid spamming the log; the process is exiting anyway.
        s_imageGarbageOverflowCount[frameIdx]++;
        VkResult waitResult = vkDeviceWaitIdle(vk.device);
        if (waitResult == VK_SUCCESS && s_imageGarbageOverflowCount[frameIdx] == 1)
        {
            common->DPrintf(
                "VK: image garbage ring full for frame %u, stalling (additional overflows this slot suppressed)\n",
                frameIdx);
        }
        VK_DestroyImageData(vkd);
    }
}

// ---------------------------------------------------------------------------
// VK_Image_GetDescriptorInfo
// Fills a VkDescriptorImageInfo for use in a combined-image-sampler write.
// Returns false if the Vulkan image is not yet uploaded (caller should use
// a fallback/default white texture descriptor instead).
// ---------------------------------------------------------------------------

bool VK_Image_GetDescriptorInfo(idImage *img, VkDescriptorImageInfo *out)
{
    if (!img || !img->backendData)
        return false;
    vkImageData_t *vkd = (vkImageData_t *)img->backendData;
    out->sampler = vkd->sampler;
    out->imageView = vkd->view;
    out->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool VK_Image_GetDescriptorInfoCube(idImage *img, VkDescriptorImageInfo *out)
{
    if (!img || !img->backendData)
        return false;

    vkImageData_t *vkd = (vkImageData_t *)img->backendData;
    if (vkd->cubeView == VK_NULL_HANDLE)
        return false;

    out->sampler = vkd->sampler;
    out->imageView = vkd->cubeView;
    out->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool VK_Image_GetHandle(idImage *img, VkImage *out)
{
    if (!img || !img->backendData || !out)
        return false;

    vkImageData_t *vkd = (vkImageData_t *)img->backendData;
    *out = vkd->image;
    return true;
}

bool VK_Image_GetExtent(idImage *img, int *outW, int *outH)
{
    if (!img || !img->backendData || !outW || !outH)
        return false;

    vkImageData_t *vkd = (vkImageData_t *)img->backendData;
    *outW = (int)vkd->width;
    *outH = (int)vkd->height;
    return true;
}
