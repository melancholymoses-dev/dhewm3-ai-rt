/*
===========================================================================

dhewm3-rt Vulkan — vk_bloom.cpp — screen-space emissive bloom (B0/B1).

Owns the bloom images, the two B1 source-capture dispatches, and the
r_rtBloomDebug 2 source view.  See vk_bloom.h for the frame ordering and
docs/plans/20260906_bloom_plan.md for why the source is a diff rather than a
luminance threshold.

Images are allocated at half the DISPLAY extent, with only the top-left
(renderExtent / 2) sub-rect valid — the same convention every other U0 buffer
follows.  That keeps r_fsrRenderScale a per-frame push constant instead of a
reallocation.

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
#include "renderer/Vulkan/vk_bloom.h"

#include <string.h>

extern VkShaderModule VK_LoadSPIRV(const char *path);
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtBloom("r_rtBloom", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                 "Screen-space bloom around light-emitting surfaces.  Sourced from what the blend "
                 "stages add to the frame, not from a luminance threshold, so lit walls do not glow.  "
                 "B2's blur and composite are not built yet: for now this only drives the source "
                 "capture that r_rtBloomDebug 2 views.");
idCVar r_rtBloomThreshold("r_rtBloomThreshold", "0.8", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                          "Soft-knee threshold on the lit term, in pre-exposure HDR luminance.  Idle while "
                          "r_rtBloomLitWeight is 0.  Read the bands from r_rtBloomDebug 1 before changing it.");
idCVar r_rtBloomEmissiveThreshold("r_rtBloomEmissiveThreshold", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                  "Soft-knee threshold on the emissive diff.  Keeps blend-add smoke and alpha "
                                  "smoke over dark backgrounds out of the bloom source, which a threshold on the "
                                  "lit term cannot do.");
idCVar r_rtBloomKnee("r_rtBloomKnee", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                     "Width of the soft knee as a fraction of each threshold.  0 = hard cutoff.");
idCVar r_rtBloomLitWeight("r_rtBloomLitWeight", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                          "Weight of the plain luminance-threshold term.  0 = emissive-only, which is the "
                          "default because r_lightScale 2 overbright would otherwise bloom lit walls before "
                          "fixtures.  Raise only if r_rtBloomDebug 1 shows the two in separate bands.");
idCVar r_rtBloomStrength("r_rtBloomStrength", "0.15", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                         "Additive weight of the blurred bloom over the scene.  Consumed by B2's composite, "
                         "which is not built yet.");
idCVar r_rtBloomMips("r_rtBloomMips", "4", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                     "Levels in the bloom blur chain, i.e. the glow radius.  Consumed by B2.  Keep tight: a "
                     "wide radius lifts dark regions.");
idCVar r_rtBloomDebug("r_rtBloomDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                      "Bloom debug view.  1 = luminance bands of the finished frame (<0.5 black, 0.5-1 blue, "
                      "1-2 green, 2-4 yellow, >4 red), 2 = bloomMip[0], the extracted source.");

// ---------------------------------------------------------------------------
// Resources.  File-static: vk_bloom.cpp is the only translation unit that
// touches them, so they stay out of vkRTState_t.
// ---------------------------------------------------------------------------

// Bloom's largest pass reads hdrScene and bloomPre and writes bloomMip[0].
static const uint32_t BLOOM_PASS_MAX_SRC = 2;

struct bloomPass_t
{
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout descLayout;
    VkDescriptorPool descPool;
    // The blur chain dispatches a pass once per level with a different pair of views
    // each time, so one set per frame slot is not enough — a later level would
    // overwrite the descriptors an earlier, still-unsubmitted dispatch points at.
    // Indexed [frameIdx * setsPerFrame + level].
    VkDescriptorSet descSets[VK_MAX_FRAMES_IN_FLIGHT * VK_BLOOM_MAX_MIPS];
    uint32_t numSrc;
    uint32_t setsPerFrame;
};

// Per frame-in-flight, like every other screen-space buffer: the previous frame is
// still on the GPU when this one records.
static vkRTImage_t s_bloomPre[VK_MAX_FRAMES_IN_FLIGHT];
static vkRTImage_t s_bloomMip[VK_MAX_FRAMES_IN_FLIGHT][VK_BLOOM_MAX_MIPS];

static bloomPass_t s_bloomPrepass;
static bloomPass_t s_bloomExtract;
static bloomPass_t s_bloomDebug;
static bloomPass_t s_bloomDown;
static bloomPass_t s_bloomUp;
static bloomPass_t s_bloomComposite;

static bool s_prepassReady;
static bool s_extractReady;
static bool s_debugReady;
static bool s_chainReady;

// Linear clamp sampler for the debug magnify.  Separate from vkRT.upscaleSampler so
// bloom does not depend on VK_RT_InitUpscale having run first.
static VkSampler s_bloomSampler;

// tr.frameCount at which this slot's bloomPre was last filled.  The extract reads a
// stale snapshot as a false diff, so it refuses to run without a matching prepass.
// Per slot, never a single global: the slots advance independently.
static int s_preFrame[VK_MAX_FRAMES_IN_FLIGHT];

// Bounds a single blown emissive texel so one sample cannot dominate the blur chain.
// Generous — muzzle flashes legitimately reach past 10.
static const float BLOOM_CLAMP_MAX = 16.0f;

// Pre-tonemap gain on the debug view; see bloom_debug.comp.
static const float BLOOM_DEBUG_GAIN = 4.0f;

// Upsample tap offset in source texels, and the mix weight toward each blurred
// lower mip.  0.5 scatter keeps the chain's total weight at 1 whatever
// r_rtBloomMips is, so that cvar stays a radius control — see bloom_up.comp.
static const float BLOOM_UP_RADIUS = 1.0f;
static const float BLOOM_UP_SCATTER = 0.5f;

// Must match bloom_prepass.comp.
struct BloomPrepassPC
{
    int32_t srcExtent[2];
    int32_t dstExtent[2];
};

// Must match bloom_extract.comp.
struct BloomExtractPC
{
    int32_t srcExtent[2];
    int32_t dstExtent[2];
    float threshold;
    float emissiveThreshold;
    float knee;
    float emissiveWeight;
    float litWeight;
    float clampMax;
};

// Must match bloom_debug.comp.
struct BloomDebugPC
{
    int32_t srcExtent[2];
    int32_t srcImageExtent[2];
    int32_t displayExtent[2];
    float gain;
};

// Must match bloom_down.comp.
struct BloomDownPC
{
    int32_t srcValid[2];
    int32_t srcImage[2];
    int32_t dstValid[2];
    int32_t karis;
};

// Must match bloom_up.comp.
struct BloomUpPC
{
    int32_t srcValid[2];
    int32_t srcImage[2];
    int32_t dstValid[2];
    float radius;
    float scatter;
};

// Must match bloom_composite.comp.
struct BloomCompositePC
{
    int32_t srcValid[2];
    int32_t srcImage[2];
    int32_t displayExtent[2];
    float strength;
};

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// Mip k is half the display extent at k = 0, quartered at k = 1, and so on.
// Never smaller than 1x1, so the top of the chain stays dispatchable at any resolution.
static uint32_t VK_RT_BloomMipDim(uint32_t displayDim, int mip)
{
    uint32_t d = displayDim;
    for (int i = 0; i <= mip; i++)
        d = (d + 1) / 2;
    return d > 0 ? d : 1;
}

// The VALID sub-rect of mip `mip`, derived from the render extent rather than the
// allocated (display-derived) size.  The two only agree at render scale 1.0, and
// every chain dispatch has to bound itself by this one.
static void VK_RT_BloomLevelValid(int mip, uint32_t *w, uint32_t *h)
{
    *w = VK_RT_BloomMipDim(vk.renderExtent.width, mip);
    *h = VK_RT_BloomMipDim(vk.renderExtent.height, mip);
}

// r_rtBloomMips, clamped to what is allocated and to the 2 levels the chain needs
// to do anything at all.
static int VK_RT_BloomMipCount(void)
{
    int n = r_rtBloomMips.GetInteger();
    if (n < 2)
        n = 2;
    if (n > VK_BLOOM_MAX_MIPS)
        n = VK_BLOOM_MAX_MIPS;
    return n;
}

static void VK_RT_CreateBloomImage(vkRTImage_t &img, uint32_t width, uint32_t height, const char *label)
{
    img.width = width;
    img.height = height;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imgInfo.extent = {width, height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // STORAGE for the extract/prepass writes, SAMPLED for the bilinear reads the
    // debug magnify (and B2's blur chain) need, TRANSFER_DST for the initial clear below.
    imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &img.image));

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
        common->Error("VK RT Bloom: no device-local memory type for %s", label);
        return;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memTypeIdx;
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &img.memory));
    VK_CHECK(vkBindImageMemory(vk.device, img.image, img.memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &img.view));

    // UNDEFINED -> GENERAL once, here.  Every bloom image stays in GENERAL for its
    // whole life: the passes both store to and sample from them, and GENERAL is the
    // one layout that permits both.
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

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img.image;
        barrier.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        // Clear to black: a frame that reads a mip before anything wrote it (the first
        // frame, or the margin outside the render sub-rect) must read "no bloom here"
        // rather than uninitialised memory, which in fp16 can be NaN.
        VkClearColorValue clearBlack = {};
        vkCmdClearColorImage(tmpCmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

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

static void VK_RT_DestroyBloomImage(vkRTImage_t &img)
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
// Pipelines
// ---------------------------------------------------------------------------

// Source bindings at 0..numSrc-1, storage destination at numSrc.  Same shape as
// vk_upscale.cpp's fsrPass_t helper; duplicated rather than shared because that one
// is file-static to the upscaler and reaches for vkRT.upscaleSampler.
static bool VK_RT_CreateBloomPass(bloomPass_t &pass, const char *spvPath, VkDescriptorType srcType, uint32_t pushSize,
                                  uint32_t numSrc, uint32_t setsPerFrame = 1)
{
    memset(&pass, 0, sizeof(pass));
    pass.numSrc = numSrc;
    pass.setsPerFrame = setsPerFrame;
    const uint32_t totalSets = VK_MAX_FRAMES_IN_FLIGHT * setsPerFrame;

    VkDescriptorSetLayoutBinding bindings[BLOOM_PASS_MAX_SRC + 1] = {};
    for (uint32_t b = 0; b < numSrc; b++)
    {
        bindings[b].binding = b;
        bindings[b].descriptorType = srcType;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[numSrc].binding = numSrc;
    bindings[numSrc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[numSrc].descriptorCount = 1;
    bindings[numSrc].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = numSrc + 1;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &pass.descLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = pushSize;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &pass.descLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &pass.layout));

    VkShaderModule compMod = VK_LoadSPIRV(spvPath);
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Bloom: failed to load %s", spvPath);
        return false;
    }

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = compMod;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineCI = {};
    pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage = stage;
    pipelineCI.layout = pass.layout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineCI, NULL, &pass.pipeline));
    vkDestroyShaderModule(vk.device, compMod, NULL);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = srcType;
    poolSizes[0].descriptorCount = totalSets * numSrc;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = totalSets;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = totalSets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &pass.descPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT * VK_BLOOM_MAX_MIPS];
    for (uint32_t i = 0; i < totalSets; i++)
        layouts[i] = pass.descLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = pass.descPool;
    dsAlloc.descriptorSetCount = totalSets;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, pass.descSets));
    return true;
}

static void VK_RT_DestroyBloomPass(bloomPass_t &pass)
{
    if (pass.pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(vk.device, pass.pipeline, NULL);
    if (pass.layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vk.device, pass.layout, NULL);
    if (pass.descPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk.device, pass.descPool, NULL);
    if (pass.descLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vk.device, pass.descLayout, NULL);
    memset(&pass, 0, sizeof(pass));
}

// Rewritten every dispatch: a handful of writes per frame is nothing, and the views
// change on every resize.  Safe because the sets are per frame-in-flight and the
// slot's fence has already been waited on.
// `level` picks among this pass's per-frame sets; 0 for the single-dispatch passes.
static VkDescriptorSet VK_RT_BloomSet(const bloomPass_t &pass, int frameIdx, uint32_t level)
{
    return pass.descSets[frameIdx * pass.setsPerFrame + level];
}

static void VK_RT_WriteBloomDescriptors(const bloomPass_t &pass, int frameIdx, uint32_t level, VkDescriptorType srcType,
                                        const VkImageView *srcViews, uint32_t numSrc, VkImageView dstView)
{
    VkDescriptorImageInfo srcInfo[BLOOM_PASS_MAX_SRC] = {};
    VkWriteDescriptorSet writes[BLOOM_PASS_MAX_SRC + 1] = {};
    const VkDescriptorSet set = VK_RT_BloomSet(pass, frameIdx, level);

    for (uint32_t s = 0; s < numSrc; s++)
    {
        srcInfo[s].sampler = (srcType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ? s_bloomSampler : VK_NULL_HANDLE;
        srcInfo[s].imageView = srcViews[s];
        srcInfo[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writes[s].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[s].dstSet = set;
        writes[s].dstBinding = s;
        writes[s].descriptorCount = 1;
        writes[s].descriptorType = srcType;
        writes[s].pImageInfo = &srcInfo[s];
    }

    VkDescriptorImageInfo dstInfo = {};
    dstInfo.imageView = dstView;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[numSrc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[numSrc].dstSet = set;
    writes[numSrc].dstBinding = numSrc;
    writes[numSrc].descriptorCount = 1;
    writes[numSrc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[numSrc].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(vk.device, numSrc + 1, writes, 0, NULL);
}

static void VK_RT_CreateBloomSampler(void)
{
    if (s_bloomSampler != VK_NULL_HANDLE)
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
    VK_CHECK(vkCreateSampler(vk.device, &sampCI, NULL, &s_bloomSampler));
}

// ---------------------------------------------------------------------------
// Barriers.  hdrScene round-trips COLOR_ATTACHMENT_OPTIMAL <-> GENERAL around each
// dispatch; the bloom images never leave GENERAL.
// ---------------------------------------------------------------------------

static void VK_RT_BloomSceneToGeneral(VkCommandBuffer cmd, int frameIdx, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = dstAccess;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = vkRT.hdrScene[frameIdx].image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &b);
}

static void VK_RT_BloomSceneToAttachment(VkCommandBuffer cmd, int frameIdx, VkAccessFlags srcAccess)
{
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = vkRT.hdrScene[frameIdx].image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &b);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VK_RT_BloomActive(void)
{
    if (!vkRT.isInitialized)
        return false;
    // Debug mode 1 is the tonemap's own band view and needs none of this; mode 2
    // views the extract's output, so it has to pull the capture up on its own.
    if (!r_rtBloom.GetBool() && r_rtBloomDebug.GetInteger() != 2)
        return false;
    if (!s_prepassReady || !s_extractReady)
        return false;
    if (s_bloomPre[vk.currentFrame].image == VK_NULL_HANDLE || s_bloomMip[vk.currentFrame][0].image == VK_NULL_HANDLE)
        return false;
    if (vkRT.hdrScene[vk.currentFrame].image == VK_NULL_HANDLE)
        return false;
    return true;
}

void VK_RT_BloomPrepass(VkCommandBuffer cmd)
{
    if (!VK_RT_BloomActive())
        return;

    const int frameIdx = (int)vk.currentFrame;
    const uint32_t srcW = vk.renderExtent.width;
    const uint32_t srcH = vk.renderExtent.height;
    const uint32_t dstW = (srcW + 1) / 2;
    const uint32_t dstH = (srcH + 1) / 2;

    const VkImageView srcView = vkRT.hdrScene[frameIdx].view;
    VK_RT_WriteBloomDescriptors(s_bloomPrepass, frameIdx, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &srcView, 1,
                                s_bloomPre[frameIdx].view);

    VK_RT_BloomSceneToGeneral(cmd, frameIdx, VK_ACCESS_SHADER_READ_BIT);

    BloomPrepassPC pc;
    pc.srcExtent[0] = (int32_t)srcW;
    pc.srcExtent[1] = (int32_t)srcH;
    pc.dstExtent[0] = (int32_t)dstW;
    pc.dstExtent[1] = (int32_t)dstH;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomPrepass.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomPrepass.layout, 0, 1,
                            &s_bloomPrepass.descSets[frameIdx * s_bloomPrepass.setsPerFrame], 0, NULL);
    vkCmdPushConstants(cmd, s_bloomPrepass.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (dstW + 7) / 8, (dstH + 7) / 8, 1);

    VK_RT_BloomSceneToAttachment(cmd, frameIdx, VK_ACCESS_SHADER_READ_BIT);

    s_preFrame[frameIdx] = tr.frameCount;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Bloom: prepass %ux%u -> %ux%u slot=%d frame=%d\n", srcW, srcH, dstW, dstH, frameIdx,
                       tr.frameCount);
}

void VK_RT_BloomExtract(VkCommandBuffer cmd)
{
    if (!VK_RT_BloomActive())
        return;

    const int frameIdx = (int)vk.currentFrame;

    // No matching snapshot means no diff.  Running anyway would subtract whatever this
    // slot held a frame or two ago, which reads as a bloom source smeared over
    // everything that moved.
    if (s_preFrame[frameIdx] != tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Bloom: extract skipped, no prepass this frame (slot=%d preFrame=%d frame=%d)\n",
                           frameIdx, s_preFrame[frameIdx], tr.frameCount);
        return;
    }

    const uint32_t srcW = vk.renderExtent.width;
    const uint32_t srcH = vk.renderExtent.height;
    const uint32_t dstW = (srcW + 1) / 2;
    const uint32_t dstH = (srcH + 1) / 2;

    const VkImageView srcViews[2] = {vkRT.hdrScene[frameIdx].view, s_bloomPre[frameIdx].view};
    VK_RT_WriteBloomDescriptors(s_bloomExtract, frameIdx, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, srcViews, 2,
                                s_bloomMip[frameIdx][0].view);

    VK_RT_BloomSceneToGeneral(cmd, frameIdx, VK_ACCESS_SHADER_READ_BIT);

    // Orders the prepass's write of bloomPre against this read of it.  The two
    // dispatches are separated by the whole shader-pass block, but nothing in between
    // declared the dependency.
    VkMemoryBarrier preDone = {};
    preDone.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    preDone.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preDone.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &preDone, 0, NULL, 0, NULL);

    BloomExtractPC pc;
    pc.srcExtent[0] = (int32_t)srcW;
    pc.srcExtent[1] = (int32_t)srcH;
    pc.dstExtent[0] = (int32_t)dstW;
    pc.dstExtent[1] = (int32_t)dstH;
    pc.threshold = r_rtBloomThreshold.GetFloat();
    pc.emissiveThreshold = r_rtBloomEmissiveThreshold.GetFloat();
    pc.knee = r_rtBloomKnee.GetFloat();
    // Emissive weight is not a cvar: the whole design is emissive-sourced, and the
    // only honest A/B is r_rtBloomLitWeight against it.
    pc.emissiveWeight = 1.0f;
    pc.litWeight = r_rtBloomLitWeight.GetFloat();
    pc.clampMax = BLOOM_CLAMP_MAX;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomExtract.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomExtract.layout, 0, 1,
                            &s_bloomExtract.descSets[frameIdx * s_bloomExtract.setsPerFrame], 0, NULL);
    vkCmdPushConstants(cmd, s_bloomExtract.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (dstW + 7) / 8, (dstH + 7) / 8, 1);

    VK_RT_BloomSceneToAttachment(cmd, frameIdx, VK_ACCESS_SHADER_READ_BIT);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Bloom: extract %ux%u emThresh=%.2f knee=%.2f litW=%.2f slot=%d\n", dstW, dstH,
                       pc.emissiveThreshold, pc.knee, pc.litWeight, frameIdx);
}

// ---------------------------------------------------------------------------
// B2: blur chain + composite
// ---------------------------------------------------------------------------

bool VK_RT_BloomCompositeActive(void)
{
    // r_rtBloom specifically, NOT VK_RT_BloomActive: r_rtBloomDebug 2 pulls the source
    // capture up on its own so the view has something to show, and it must not start
    // compositing glow the user switched off.
    if (!r_rtBloom.GetBool() || !s_chainReady)
        return false;
    if (!VK_RT_BloomActive())
        return false;
    // The chain reads what the extract wrote this frame in this slot.  Without a
    // matching extract there is nothing to blur, and mip0 holds another frame's source.
    if (s_preFrame[vk.currentFrame] != tr.frameCount)
        return false;
    return true;
}

// Everything between dispatches here touches bloom images only, which never leave
// GENERAL, so a plain memory dependency is all each step needs.  SHADER_READ is in
// both masks because the up chain reads and writes the same image.
static void VK_RT_BloomChainBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

void VK_RT_DispatchBloom(VkCommandBuffer cmd)
{
    if (!VK_RT_BloomCompositeActive())
        return;

    const int frameIdx = (int)vk.currentFrame;
    const int mips = VK_RT_BloomMipCount();
    const uint32_t dispW = vk.swapchainExtent.width;
    const uint32_t dispH = vk.swapchainExtent.height;

    // --- Down: mip[k] -> mip[k+1] -------------------------------------------------
    for (int k = 0; k + 1 < mips; k++)
    {
        uint32_t sw, sh, dw, dh;
        VK_RT_BloomLevelValid(k, &sw, &sh);
        VK_RT_BloomLevelValid(k + 1, &dw, &dh);

        const VkImageView srcView = s_bloomMip[frameIdx][k].view;
        VK_RT_WriteBloomDescriptors(s_bloomDown, frameIdx, (uint32_t)k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    &srcView, 1, s_bloomMip[frameIdx][k + 1].view);

        BloomDownPC pc;
        pc.srcValid[0] = (int32_t)sw;
        pc.srcValid[1] = (int32_t)sh;
        pc.srcImage[0] = (int32_t)s_bloomMip[frameIdx][k].width;
        pc.srcImage[1] = (int32_t)s_bloomMip[frameIdx][k].height;
        pc.dstValid[0] = (int32_t)dw;
        pc.dstValid[1] = (int32_t)dh;
        // Only the first level: see bloom_down.comp.
        pc.karis = (k == 0) ? 1 : 0;

        VK_RT_BloomChainBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomDown.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomDown.layout, 0, 1,
                                &s_bloomDown.descSets[frameIdx * s_bloomDown.setsPerFrame + k], 0, NULL);
        vkCmdPushConstants(cmd, s_bloomDown.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
    }

    // --- Up: mip[k+1] blended into mip[k], coarsest first -------------------------
    for (int k = mips - 2; k >= 0; k--)
    {
        uint32_t sw, sh, dw, dh;
        VK_RT_BloomLevelValid(k + 1, &sw, &sh);
        VK_RT_BloomLevelValid(k, &dw, &dh);

        const VkImageView srcView = s_bloomMip[frameIdx][k + 1].view;
        VK_RT_WriteBloomDescriptors(s_bloomUp, frameIdx, (uint32_t)k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    &srcView, 1, s_bloomMip[frameIdx][k].view);

        BloomUpPC pc;
        pc.srcValid[0] = (int32_t)sw;
        pc.srcValid[1] = (int32_t)sh;
        pc.srcImage[0] = (int32_t)s_bloomMip[frameIdx][k + 1].width;
        pc.srcImage[1] = (int32_t)s_bloomMip[frameIdx][k + 1].height;
        pc.dstValid[0] = (int32_t)dw;
        pc.dstValid[1] = (int32_t)dh;
        pc.radius = BLOOM_UP_RADIUS;
        pc.scatter = BLOOM_UP_SCATTER;

        VK_RT_BloomChainBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomUp.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomUp.layout, 0, 1,
                                &s_bloomUp.descSets[frameIdx * s_bloomUp.setsPerFrame + k], 0, NULL);
        vkCmdPushConstants(cmd, s_bloomUp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
    }

    // --- Composite: hdrScene += mip[0] * strength, at display resolution ----------
    {
        uint32_t sw, sh;
        VK_RT_BloomLevelValid(0, &sw, &sh);

        const VkImageView srcView = s_bloomMip[frameIdx][0].view;
        VK_RT_WriteBloomDescriptors(s_bloomComposite, frameIdx, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcView,
                                    1, vkRT.hdrScene[frameIdx].view);

        // Read-modify-write of hdrScene, so both access bits.
        VK_RT_BloomSceneToGeneral(cmd, frameIdx, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        VK_RT_BloomChainBarrier(cmd);

        BloomCompositePC pc;
        pc.srcValid[0] = (int32_t)sw;
        pc.srcValid[1] = (int32_t)sh;
        pc.srcImage[0] = (int32_t)s_bloomMip[frameIdx][0].width;
        pc.srcImage[1] = (int32_t)s_bloomMip[frameIdx][0].height;
        pc.displayExtent[0] = (int32_t)dispW;
        pc.displayExtent[1] = (int32_t)dispH;
        pc.strength = r_rtBloomStrength.GetFloat();

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomComposite.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomComposite.layout, 0, 1,
                                &s_bloomComposite.descSets[frameIdx * s_bloomComposite.setsPerFrame], 0, NULL);
        vkCmdPushConstants(cmd, s_bloomComposite.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dispW + 7) / 8, (dispH + 7) / 8, 1);

        VK_RT_BloomSceneToAttachment(cmd, frameIdx, VK_ACCESS_SHADER_WRITE_BIT);

        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Bloom: chain mips=%d src=%ux%u disp=%ux%u strength=%.3f slot=%d\n", mips, sw, sh,
                           dispW, dispH, pc.strength, frameIdx);
    }
}

bool VK_RT_BloomDebugActive(void)
{
    if (r_rtBloomDebug.GetInteger() != 2 || !s_debugReady)
        return false;
    if (!vkRT.isInitialized || s_bloomMip[vk.currentFrame][0].image == VK_NULL_HANDLE)
        return false;
    // Without this the overlay would paint the cleared black mip over the frame and
    // read as "the extract produced nothing" rather than "nothing ran it".
    if (s_preFrame[vk.currentFrame] != tr.frameCount)
        return false;
    return true;
}

void VK_RT_DispatchBloomDebug(VkCommandBuffer cmd)
{
    if (!VK_RT_BloomDebugActive())
        return;

    const int frameIdx = (int)vk.currentFrame;
    const uint32_t dispW = vk.swapchainExtent.width;
    const uint32_t dispH = vk.swapchainExtent.height;

    const VkImageView srcView = s_bloomMip[frameIdx][0].view;
    VK_RT_WriteBloomDescriptors(s_bloomDebug, frameIdx, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcView, 1,
                                vkRT.hdrScene[frameIdx].view);

    VK_RT_BloomSceneToGeneral(cmd, frameIdx, VK_ACCESS_SHADER_WRITE_BIT);

    // Orders the extract's write of the mip against this sample of it.
    VkMemoryBarrier mipDone = {};
    mipDone.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mipDone.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mipDone.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &mipDone, 0, NULL, 0, NULL);

    BloomDebugPC pc;
    // Valid sub-rect, which tracks renderExtent, not the allocated image.
    pc.srcExtent[0] = (int32_t)((vk.renderExtent.width + 1) / 2);
    pc.srcExtent[1] = (int32_t)((vk.renderExtent.height + 1) / 2);
    pc.srcImageExtent[0] = (int32_t)s_bloomMip[frameIdx][0].width;
    pc.srcImageExtent[1] = (int32_t)s_bloomMip[frameIdx][0].height;
    pc.displayExtent[0] = (int32_t)dispW;
    pc.displayExtent[1] = (int32_t)dispH;
    pc.gain = BLOOM_DEBUG_GAIN;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomDebug.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bloomDebug.layout, 0, 1,
                            &s_bloomDebug.descSets[frameIdx * s_bloomDebug.setsPerFrame], 0, NULL);
    vkCmdPushConstants(cmd, s_bloomDebug.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (dispW + 7) / 8, (dispH + 7) / 8, 1);

    VK_RT_BloomSceneToAttachment(cmd, frameIdx, VK_ACCESS_SHADER_WRITE_BIT);
}

void VK_RT_InitBloom(void)
{
    common->Printf("VK: initializing RT bloom (B1)\n");
    VK_RT_CreateBloomSampler();
    VK_RT_ResizeBloom(vk.swapchainExtent.width, vk.swapchainExtent.height);

    s_prepassReady = VK_RT_CreateBloomPass(s_bloomPrepass, "glprogs/glsl/bloom_prepass.comp.spv",
                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(BloomPrepassPC), 1);
    s_extractReady = VK_RT_CreateBloomPass(s_bloomExtract, "glprogs/glsl/bloom_extract.comp.spv",
                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(BloomExtractPC), 2);
    s_debugReady = VK_RT_CreateBloomPass(s_bloomDebug, "glprogs/glsl/bloom_debug.comp.spv",
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(BloomDebugPC), 1);

    // The down and up passes run once per level, each against a different pair of
    // views, so they need one descriptor set per level per frame slot.
    s_chainReady = VK_RT_CreateBloomPass(s_bloomDown, "glprogs/glsl/bloom_down.comp.spv",
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(BloomDownPC), 1,
                                         VK_BLOOM_MAX_MIPS) &&
                   VK_RT_CreateBloomPass(s_bloomUp, "glprogs/glsl/bloom_up.comp.spv",
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(BloomUpPC), 1,
                                         VK_BLOOM_MAX_MIPS) &&
                   VK_RT_CreateBloomPass(s_bloomComposite, "glprogs/glsl/bloom_composite.comp.spv",
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(BloomCompositePC), 1);

    if (!s_prepassReady || !s_extractReady)
        common->Warning("VK RT Bloom: source capture unavailable — r_rtBloom will do nothing");
    if (!s_debugReady)
        common->Warning("VK RT Bloom: bloom_debug.comp failed to load — r_rtBloomDebug 2 unavailable");
    if (!s_chainReady)
        common->Warning("VK RT Bloom: blur chain unavailable — r_rtBloom will capture but not composite");
}

void VK_RT_ResizeBloom(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_DestroyBloomImage(s_bloomPre[i]);
        for (int m = 0; m < VK_BLOOM_MAX_MIPS; m++)
            VK_RT_DestroyBloomImage(s_bloomMip[i][m]);

        // bloomPre and bloomMip[0] are the same size by construction — the extract
        // diffs them texel for texel.
        const uint32_t halfW = VK_RT_BloomMipDim(width, 0);
        const uint32_t halfH = VK_RT_BloomMipDim(height, 0);
        VK_RT_CreateBloomImage(s_bloomPre[i], halfW, halfH, "bloomPre");

        // All six mips are allocated regardless of r_rtBloomMips so the cvar stays a
        // dispatch-count change rather than a reallocation.  ~1.33x mip 0 in total.
        for (int m = 0; m < VK_BLOOM_MAX_MIPS; m++)
            VK_RT_CreateBloomImage(s_bloomMip[i][m], VK_RT_BloomMipDim(width, m), VK_RT_BloomMipDim(height, m),
                                   "bloomMip");

        // The views just changed, and any snapshot taken against the old ones is gone.
        s_preFrame[i] = -1;
    }
}

void VK_RT_ShutdownBloom(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_DestroyBloomImage(s_bloomPre[i]);
        for (int m = 0; m < VK_BLOOM_MAX_MIPS; m++)
            VK_RT_DestroyBloomImage(s_bloomMip[i][m]);
        s_preFrame[i] = -1;
    }

    VK_RT_DestroyBloomPass(s_bloomPrepass);
    VK_RT_DestroyBloomPass(s_bloomExtract);
    VK_RT_DestroyBloomPass(s_bloomDebug);
    VK_RT_DestroyBloomPass(s_bloomDown);
    VK_RT_DestroyBloomPass(s_bloomUp);
    VK_RT_DestroyBloomPass(s_bloomComposite);
    s_prepassReady = false;
    s_extractReady = false;
    s_debugReady = false;
    s_chainReady = false;

    if (s_bloomSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, s_bloomSampler, NULL);
        s_bloomSampler = VK_NULL_HANDLE;
    }
}
