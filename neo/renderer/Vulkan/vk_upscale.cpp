/*
============================================================================
Functions to Upscale images.

Two resolve paths share vk.renderExtent and VK_RT_DispatchUpscale:

  r_fsr 0  U0 — upscale_blit.comp, a bilinear magnify of the render sub-rect.
  r_fsr 1  U1 — AMD FidelityFX Super Resolution 1: fsr_prepare → fsr_easu →
               fsr_rcas.  EASU and RCAS need a perceptual-space input, so the
               chain applies AMD's reversible tonemapper + gamma up front and
               inverts both at the end.  hdrScene therefore still holds linear
               HDR when the chain finishes, and the UI composite, the Uchimura
               tonemap and screenshots downstream are untouched.  That is the
               deliberate difference from the plan's original sketch, which had
               EASU emit already-tonemapped values and bypass r_rtTonemap.

Targeting AMD FidelityFX FSR2 (r_fsr 2) at U3.

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
#include <math.h>
#include <string.h>

extern VkShaderModule VK_LoadSPIRV(const char *path);

// UpscalePC — must match the push_constant block in upscale_blit.comp.
struct UpscalePC
{
    int32_t renderExtent[2];
    int32_t debugMode;
};

// FsrPC — must match the push_constant block in fsr_prepare.comp / fsr_easu.comp.
struct FsrEasuPC
{
    int32_t renderExtent[2];
    int32_t displayExtent[2];
};

// Must match fsr_rcas.comp.
struct FsrRcasPC
{
    float sharpness;
    int32_t debugMode;
};

// Must match motion_debug.comp.
struct MotionDebugPC
{
    int32_t renderExtent[2];
    int32_t displayExtent[2];
    float motionScale;
    float brightness;
};

idCVar r_fsr("r_fsr", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
             "Upscale filter used when r_fsrRenderScale < 1.0.  0 = bilinear resolve, "
             "1 = AMD FidelityFX Super Resolution 1 (EASU + RCAS), 2 = FSR 2 (not implemented yet).");
idCVar r_fsrRenderScale("r_fsrRenderScale", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Linear Render Scale Factor.  Values (0.5=Half-Resolution, 1=no-scale).");
idCVar r_fsrDebug("r_fsrDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "FSR Debug Mode.");
idCVar r_fsrSharpness("r_fsrSharpness", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                      "RCAS sharpening for r_fsr 1.  0 = softest, 1 = sharpest.");
idCVar r_fsrJitter("r_fsrJitter", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                   "Halton(2,3) sub-pixel jitter while upscaling, replacing r_jitter.  Inert until a temporal "
                   "upscaler consumes it (U3), so it only adds shimmer today.");
idCVar r_fsrMotionScale("r_fsrMotionScale", "16", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Render pixels of motion that saturate the r_fsrDebug 7 overlay.");

// ---------------------------------------------------------------------------
// FSR 1 (U1) resources.  Kept file-static: vk_upscale.cpp is the one
// translation unit that owns upscaling, and nothing else needs to see them.
// ---------------------------------------------------------------------------
struct fsrPass_t
{
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout descLayout;
    VkDescriptorPool descPool;
    VkDescriptorSet descSets[VK_MAX_FRAMES_IN_FLIGHT];
};

// Per frame-in-flight for the same reason as vkRT.hdrUpscaled — see vk_raytracing.h.
static vkRTImage_t s_fsrPerceptual[VK_MAX_FRAMES_IN_FLIGHT]; // display-res RGBA16F, perceptual space
static fsrPass_t s_fsrPrepare;
static fsrPass_t s_fsrEasu;
static fsrPass_t s_fsrRcas;
static fsrPass_t s_motionDebug; // U2, r_fsrDebug 7
static bool s_fsr1Ready = false;
static bool s_motionDebugReady = false;
static int s_fsrLoggedMode = -1; // last r_fsr value announced to the console

static void VK_RT_CreateUpscaleImage(vkRTImage_t &up, uint32_t width, uint32_t height, const char *label)
{
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
        common->Error("VK RT Upscale: no device-local memory type for %s", label);
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

static void VK_RT_DestroyUpscaleImage(vkRTImage_t &up)
{
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
into hdrUpscaled at display resolution.  Descriptor sets, and every image they
point at, are per frame-in-flight.
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
===========================================================================
FSR 1 (U1)

Three compute dispatches, all 2-binding: binding 0 reads, binding 1 writes.

  fsr_prepare  hdrScene[slot]          -> s_fsrPerceptual[slot]  (renderExtent + pad)
  fsr_easu     s_fsrPerceptual[slot]   -> hdrUpscaled[slot]      (display extent)
  fsr_rcas     hdrUpscaled[slot]       -> hdrScene[slot]         (display extent)

RCAS writing straight back into hdrScene is what lets the FSR 1 path skip the
full-resolution copy the bilinear path needs.
===========================================================================
*/

static bool VK_RT_CreateFsrPass(fsrPass_t &pass, const char *spvPath, VkDescriptorType srcType, uint32_t pushSize)
{
    memset(&pass, 0, sizeof(pass));

    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = srcType;
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
        common->Warning("VK RT FSR1: failed to load %s", spvPath);
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
    poolSizes[0].descriptorCount = VK_MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = VK_MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &pass.descPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = pass.descLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = pass.descPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, pass.descSets));
    return true;
}

static void VK_RT_DestroyFsrPass(fsrPass_t &pass)
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

// Rewritten every dispatch rather than tracked with a dirty counter: six writes
// per frame is nothing, and the views change on every resize and every bindless
// purge.  Safe because the sets are per frame-in-flight and the slot's fence has
// already been waited on.
static void VK_RT_WriteFsrPassDescriptors(const fsrPass_t &pass, int frameIdx, VkDescriptorType srcType,
                                          VkImageView srcView, VkImageLayout srcLayout, VkImageView dstView)
{
    VkDescriptorImageInfo srcInfo = {};
    srcInfo.sampler = (srcType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ? vkRT.upscaleSampler : VK_NULL_HANDLE;
    srcInfo.imageView = srcView;
    srcInfo.imageLayout = srcLayout;

    VkDescriptorImageInfo dstInfo = {};
    dstInfo.imageView = dstView;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = pass.descSets[frameIdx];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = srcType;
    writes[0].pImageInfo = &srcInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = pass.descSets[frameIdx];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
}

static void VK_RT_CreateFsr1Pipelines(void)
{
    VK_RT_CreateUpscaleSampler();

    s_fsr1Ready = VK_RT_CreateFsrPass(s_fsrPrepare, "glprogs/glsl/fsr_prepare.comp.spv",
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(int32_t) * 2) &&
                  VK_RT_CreateFsrPass(s_fsrEasu, "glprogs/glsl/fsr_easu.comp.spv",
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(FsrEasuPC)) &&
                  VK_RT_CreateFsrPass(s_fsrRcas, "glprogs/glsl/fsr_rcas.comp.spv",
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(FsrRcasPC));

    if (s_fsr1Ready)
        common->Printf("VK RT FSR1: EASU + RCAS pipelines initialized\n");
    else
        common->Warning("VK RT FSR1: pipeline setup failed — r_fsr 1 will fall back to the bilinear resolve");

    // U2 motion-vector overlay — same two-binding shape, so it rides the same helper.
    s_motionDebugReady = VK_RT_CreateFsrPass(s_motionDebug, "glprogs/glsl/motion_debug.comp.spv",
                                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(MotionDebugPC));
    if (!s_motionDebugReady)
        common->Warning("VK RT Upscale: motion_debug.comp failed to load — r_fsrDebug 7 unavailable");
}

static void VK_RT_DestroyFsr1Pipelines(void)
{
    VK_RT_DestroyFsrPass(s_fsrPrepare);
    VK_RT_DestroyFsrPass(s_fsrEasu);
    VK_RT_DestroyFsrPass(s_fsrRcas);
    VK_RT_DestroyFsrPass(s_motionDebug);
    s_fsr1Ready = false;
    s_motionDebugReady = false;
}

// r_fsrDebug 4 is the point-magnify A/B, which lives on the bilinear path, so it
// wins over r_fsr — otherwise there is nothing honest to compare FSR against.
static bool VK_RT_Fsr1Active(void)
{
    return r_fsr.GetInteger() == 1 && r_fsrDebug.GetInteger() != 4 && s_fsr1Ready &&
           s_fsrPerceptual[vk.currentFrame].image != VK_NULL_HANDLE;
}

static void VK_RT_DispatchFsr1(VkCommandBuffer cmd, int frameIdx)
{
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const uint32_t dispW = vk.swapchainExtent.width;
    const uint32_t dispH = vk.swapchainExtent.height;

    VK_RT_WriteFsrPassDescriptors(s_fsrPrepare, frameIdx, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  vkRT.hdrScene[frameIdx].view, VK_IMAGE_LAYOUT_GENERAL,
                                  s_fsrPerceptual[frameIdx].view);
    VK_RT_WriteFsrPassDescriptors(s_fsrEasu, frameIdx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  s_fsrPerceptual[frameIdx].view, VK_IMAGE_LAYOUT_GENERAL,
                                  vkRT.hdrUpscaled[frameIdx].view);
    VK_RT_WriteFsrPassDescriptors(s_fsrRcas, frameIdx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  vkRT.hdrUpscaled[frameIdx].view, VK_IMAGE_LAYOUT_GENERAL,
                                  vkRT.hdrScene[frameIdx].view);

    // 1. hdrScene COLOR_ATTACHMENT_OPTIMAL -> GENERAL.  It stays there for the
    //    whole chain: fsr_prepare reads it, fsr_rcas writes it.
    VkImageMemoryBarrier toGeneral = {};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = vkRT.hdrScene[frameIdx].image;
    toGeneral.subresourceRange = colorRange;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &toGeneral);

    // Between dispatches only a memory dependency is needed — every image
    // involved is already in GENERAL.  The same barrier also orders fsr_prepare's
    // read of hdrScene against fsr_rcas's write of it (WAR).
    VkMemoryBarrier computeChain = {};
    computeChain.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    computeChain.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    computeChain.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    // 2. fsr_prepare over renderExtent plus a pad ring: EASU's 12-tap kernel
    //    reaches two texels past its viewport, and hdrScene outside the sub-rect
    //    was never written this frame.
    {
        const uint32_t padW = (vk.renderExtent.width + 8 < dispW) ? vk.renderExtent.width + 8 : dispW;
        const uint32_t padH = (vk.renderExtent.height + 8 < dispH) ? vk.renderExtent.height + 8 : dispH;

        int32_t pc[2] = {(int32_t)vk.renderExtent.width, (int32_t)vk.renderExtent.height};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsrPrepare.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsrPrepare.layout, 0, 1,
                                &s_fsrPrepare.descSets[frameIdx], 0, NULL);
        vkCmdPushConstants(cmd, s_fsrPrepare.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd, (padW + 7) / 8, (padH + 7) / 8, 1);
    }

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &computeChain, 0, NULL, 0, NULL);

    // 3. EASU over the full display extent.
    {
        FsrEasuPC pc;
        pc.renderExtent[0] = (int32_t)vk.renderExtent.width;
        pc.renderExtent[1] = (int32_t)vk.renderExtent.height;
        pc.displayExtent[0] = (int32_t)dispW;
        pc.displayExtent[1] = (int32_t)dispH;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsrEasu.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsrEasu.layout, 0, 1,
                                &s_fsrEasu.descSets[frameIdx], 0, NULL);
        vkCmdPushConstants(cmd, s_fsrEasu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dispW + 7) / 8, (dispH + 7) / 8, 1);
    }

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &computeChain, 0, NULL, 0, NULL);

    // 4. RCAS sharpens and converts back to linear HDR, straight into hdrScene.
    {
        float s = r_fsrSharpness.GetFloat();
        if (s < 0.0f)
            s = 0.0f;
        if (s > 1.0f)
            s = 1.0f;

        FsrRcasPC pc;
        pc.sharpness = 2.0f * (1.0f - s); // FsrRcasCon takes attenuation in stops; 0 = sharpest
        pc.debugMode = r_fsrDebug.GetInteger();

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsrRcas.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsrRcas.layout, 0, 1,
                                &s_fsrRcas.descSets[frameIdx], 0, NULL);
        vkCmdPushConstants(cmd, s_fsrRcas.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dispW + 7) / 8, (dispH + 7) / 8, 1);
    }

    // 5. hdrScene back to COLOR_ATTACHMENT_OPTIMAL for the UI resume pass.
    VkImageMemoryBarrier toAttach = {};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = vkRT.hdrScene[frameIdx].image;
    toAttach.subresourceRange = colorRange;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &toAttach);
}

/*
===========================================================================
Motion-vector overlay (U2), r_fsrDebug 7.

Reuses the two-binding fsrPass_t shape: binding 0 texelFetches motionVectors
through the upscale sampler (R16G16_SFLOAT is not a mandatory storage-image
format, so the image carries SAMPLED usage only), binding 1 writes hdrScene.
Dispatched from VK_RB_SwapBuffers after the resolve and before the tonemap,
where hdrScene already holds the display-resolution frame and is in
COLOR_ATTACHMENT_OPTIMAL.
===========================================================================
*/

bool VK_RT_MotionDebugActive(void)
{
    return r_fsrDebug.GetInteger() == 7 && s_motionDebugReady && vk.gbufferSupported &&
           vkRT.motionVectors[vk.currentFrame].image != VK_NULL_HANDLE;
}

void VK_RT_DispatchMotionDebug(VkCommandBuffer cmd)
{
    if (!VK_RT_MotionDebugActive())
        return;

    const int frameIdx = (int)vk.currentFrame;
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const uint32_t dispW = vk.swapchainExtent.width;
    const uint32_t dispH = vk.swapchainExtent.height;

    // GENERAL is a legal layout for a sampled-image descriptor, which keeps the barrier
    // pair below symmetric for both images.
    VK_RT_WriteFsrPassDescriptors(s_motionDebug, frameIdx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  vkRT.motionVectors[frameIdx].view, VK_IMAGE_LAYOUT_GENERAL,
                                  vkRT.hdrScene[frameIdx].view);

    // Both images are colour attachments of the render pass that just ended, so both
    // need moving to GENERAL and putting back — motionVectors' declared initialLayout
    // on vk.hdrRenderPass is COLOR_ATTACHMENT_OPTIMAL and next frame's clear expects it.
    VkImageMemoryBarrier toGeneral[2] = {};
    VkImage images[2] = {vkRT.motionVectors[frameIdx].image, vkRT.hdrScene[frameIdx].image};
    for (int i = 0; i < 2; i++)
    {
        toGeneral[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toGeneral[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toGeneral[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral[i].image = images[i];
        toGeneral[i].subresourceRange = colorRange;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 2, toGeneral);

    MotionDebugPC pc;
    pc.renderExtent[0] = (int32_t)vk.renderExtent.width;
    pc.renderExtent[1] = (int32_t)vk.renderExtent.height;
    pc.displayExtent[0] = (int32_t)dispW;
    pc.displayExtent[1] = (int32_t)dispH;
    pc.motionScale = r_fsrMotionScale.GetFloat();
    if (pc.motionScale < 0.001f)
        pc.motionScale = 0.001f;
    // hdrScene is pre-tonemap linear HDR; the Uchimura curve downstream would otherwise
    // crush the overlay into the toe and make "fast" and "very fast" look alike.
    pc.brightness = 4.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_motionDebug.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_motionDebug.layout, 0, 1,
                            &s_motionDebug.descSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, s_motionDebug.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (dispW + 7) / 8, (dispH + 7) / 8, 1);

    VkImageMemoryBarrier toAttach[2] = {};
    for (int i = 0; i < 2; i++)
    {
        toAttach[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toAttach[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toAttach[i].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        toAttach[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toAttach[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttach[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toAttach[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toAttach[i].image = images[i];
        toAttach[i].subresourceRange = colorRange;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 2, toAttach);
}

/*
===========================================================================
Jitter (U2)

A straight port of FSR 2.2.1's jitter sequence — halton(),
ffxFsr2GetJitterPhaseCount and ffxFsr2GetJitterOffset
(neo/libs/ffx-fsr2-api/ffx_fsr2.cpp:188-201, 1187-1212) — so that U3 switching
to the real symbols does not shift the sequence.  Kept here rather than
including the FSR2 headers so nothing outside U3 depends on that tree.
===========================================================================
*/

static float VK_RT_Halton(int32_t index, int32_t base)
{
    float f = 1.0f, result = 0.0f;
    for (int32_t cur = index; cur > 0;)
    {
        f /= (float)base;
        result = result + f * (float)(cur % base);
        cur = (int32_t)floorf((float)cur / (float)base);
    }
    return result;
}

/*
Sub-pixel offset for this frame, in render-resolution pixels over [-0.5, +0.5],
plus the render extent the caller should measure it against.  Returns false when
the FSR jitter is not in play, leaving the outputs untouched so R_SetupProjection
can fall back to r_jitter.
*/
bool VK_RT_GetFsrJitter(float *jx, float *jy, int *renderW, int *renderH)
{
    if (!r_fsrJitter.GetBool() || !VK_RT_UpscaleActive() || vk.renderExtent.width == 0)
        return false;

    const int32_t phaseCount =
        (int32_t)(8.0f * powf((float)vk.swapchainExtent.width / (float)vk.renderExtent.width, 2.0f));
    if (phaseCount <= 0)
        return false;

    const int32_t index = (int32_t)(((uint32_t)tr.frameCount) % (uint32_t)phaseCount) + 1;
    *jx = VK_RT_Halton(index, 2) - 0.5f;
    *jy = VK_RT_Halton(index, 3) - 0.5f;
    *renderW = (int)vk.renderExtent.width;
    *renderH = (int)vk.renderExtent.height;
    return true;
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

// True once VK_RT_InitUpscale has tried to build the resolve pipelines.  Before that,
// "no pipeline" means "not built yet", not "failed" — the difference matters to the
// fallback in VK_RT_UpdateRenderExtent, which runs once from VK_RT_ResizeUpscale
// before the pipelines exist and then again every frame from VK_RB_DrawView.
static bool s_upscaleInitDone = false;

// Is there any path that can resolve the render sub-rect back to display resolution?
static bool VK_RT_ResolvePathReady(void)
{
    return vkRT.upscalePipeline != VK_NULL_HANDLE || s_fsr1Ready;
}

void VK_RT_UpdateRenderExtent(void)
{
    const VkExtent2D old = vk.renderExtent;

    // Clamp low so a fat-fingered cvar can't collapse the scene to the 64px floor.
    float scale = r_fsrRenderScale.GetFloat();
    if (scale < 0.3f)
        scale = 0.3f;

    // With no resolve path there is nothing to magnify the sub-rect back up, and the
    // callers mark the resolve done regardless — so the UI would draw at display extent
    // over a scene stranded in the top-left corner.  Render native instead: a lost
    // performance option beats a broken frame, and it keeps every display-space divisor
    // in §4 consistent for free.
    if (scale < 1.0f && s_upscaleInitDone && !VK_RT_ResolvePathReady())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            common->Warning("VK RT Upscale: no resolve pipeline loaded — ignoring r_fsrRenderScale %.2f and "
                            "rendering at native resolution",
                            scale);
        }
        scale = 1.0f;
    }

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
    {
        common->Printf("VK RT Upscale: renderExtent %ux%u -> %ux%u (display %ux%u, requested scale %.3f, "
                       "achieved %.4f x %.4f)\n",
                       old.width, old.height, wn, hn, vk.swapchainExtent.width, vk.swapchainExtent.height, scale,
                       (float)wn / (float)vk.swapchainExtent.width, (float)hn / (float)vk.swapchainExtent.height);

        // The AO/GI/vol histories are indexed in render-resolution texels, so after a
        // scale change every one of them refers to the previous raster grid.  Blending
        // against that ghosts until it converges.  Same flags the camera-cut detector
        // uses; this is the same class of event.
        vkRT.aoHistoryValid = false;
        vkRT.giHistoryValid = false;
        vkRT.volHistoryValid = false;
    }

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
    if (!VK_RT_UpscaleActive())
        return;
    if (vkRT.hdrUpscaled[vk.currentFrame].image == VK_NULL_HANDLE ||
        vkRT.hdrScene[vk.currentFrame].image == VK_NULL_HANDLE)
        return;
    if (vkRT.upscalePipeline == VK_NULL_HANDLE && !VK_RT_Fsr1Active())
        return;

    const int frameIdx = (int)vk.currentFrame;
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    const int mode = r_fsr.GetInteger();
    if (mode != s_fsrLoggedMode)
    {
        const char *what = VK_RT_Fsr1Active() ? "AMD FidelityFX Super Resolution 1 (EASU + RCAS)" : "bilinear resolve";
        if (mode == 2)
            common->Warning("VK RT Upscale: r_fsr 2 (FSR 2) is not implemented yet — using the bilinear resolve");
        common->Printf("VK RT Upscale: r_fsr %d -> %s, render %ux%u -> display %ux%u, sharpness %.2f\n", mode, what,
                       vk.renderExtent.width, vk.renderExtent.height, vk.swapchainExtent.width,
                       vk.swapchainExtent.height, r_fsrSharpness.GetFloat());
        s_fsrLoggedMode = mode;
    }

    if (VK_RT_Fsr1Active())
    {
        VK_RT_DispatchFsr1(cmd, frameIdx);
        return;
    }

    // Both views only change on resize, which resets the marker to -1 — so this is a
    // once-per-resize write, not a per-frame one.  (Comparing against tr.frameCount
    // instead, as this used to, is true every frame and never skips anything.)
    if (vkRT.upscaleDescSetLastUpdatedFrameCount[frameIdx] < 0)
    {
        VkDescriptorImageInfo srcInfo = {};
        srcInfo.sampler = vkRT.upscaleSampler;
        srcInfo.imageView = vkRT.hdrScene[frameIdx].view;
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo dstInfo = {};
        dstInfo.imageView = vkRT.hdrUpscaled[frameIdx].view;
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
    preCopy[0].image = vkRT.hdrUpscaled[frameIdx].image;
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
    vkCmdCopyImage(cmd, vkRT.hdrUpscaled[frameIdx].image, VK_IMAGE_LAYOUT_GENERAL, vkRT.hdrScene[frameIdx].image,
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
    VK_RT_CreateUpscalePipeline();
    VK_RT_CreateFsr1Pipelines();
    // Both pipeline sets have now either loaded or failed, so "no resolve path" is a
    // real answer from here on.  Re-run the extent so the fallback can take effect on
    // the very first frame rather than after it.
    s_upscaleInitDone = true;
    VK_RT_UpdateRenderExtent();
}

void VK_RT_ResizeUpscale(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_DestroyUpscaleImage(vkRT.hdrUpscaled[i]);
        VK_RT_DestroyUpscaleImage(s_fsrPerceptual[i]);
        VK_RT_CreateUpscaleImage(vkRT.hdrUpscaled[i], width, height, "hdrUpscaled");
        VK_RT_CreateUpscaleImage(s_fsrPerceptual[i], width, height, "fsrPerceptual");
        // The views changed, so the bound descriptors are stale.  (The FSR 1 passes
        // rewrite theirs every dispatch, so they need nothing here.)
        vkRT.upscaleDescSetLastUpdatedFrameCount[i] = -1;
    }
    // Covers the resize path; the per-frame call in VK_RB_DrawView covers cvar changes.
    VK_RT_UpdateRenderExtent();
    s_fsrLoggedMode = -1;
}

void VK_RT_ShutdownUpscale(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_DestroyUpscaleImage(vkRT.hdrUpscaled[i]);
        VK_RT_DestroyUpscaleImage(s_fsrPerceptual[i]);
    }
    VK_RT_DestroyFsr1Pipelines();
    VK_RT_DestroyUpscalePipeline();
    s_upscaleInitDone = false;
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
