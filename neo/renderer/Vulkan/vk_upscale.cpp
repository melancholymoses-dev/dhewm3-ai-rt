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
  r_fsr 2  U3 — AMD FidelityFX Super Resolution 2.2.1, the vendored AMD library
               (libs/ffx-fsr2-api) driven from VK_RT_DispatchFsr2.  Temporal, so
               it consumes U2's motion vectors and Halton jitter.

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
#include <stdlib.h>
#include <SDL.h>

#if defined(DHEWM3_FSR2)
#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"
#endif

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

// Must match fsr2_bleed_debug.comp (r_fsrDebug 5).
struct Fsr2BleedPC
{
    int32_t renderExtent[2];
    int32_t displayExtent[2];
    float threshold;
};

idCVar r_fsr("r_fsr", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
             "Upscale filter used when the render scale is below 1.0.  0 = bilinear resolve, "
             "1 = AMD FidelityFX Super Resolution 1 (EASU + RCAS), "
             "2 = AMD FidelityFX Super Resolution 2 (temporal).");
idCVar r_fsrRenderScale("r_fsrRenderScale", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Linear Render Scale Factor.  Values (0.5=Half-Resolution, 1=no-scale).  "
                        "Ignored unless r_fsrQuality is 0.");
idCVar r_fsrQuality("r_fsrQuality", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                    "Render-resolution preset, overriding r_fsrRenderScale.  0 = use r_fsrRenderScale, "
                    "1 = Quality (1.5x), 2 = Balanced (1.7x), 3 = Performance (2.0x), "
                    "4 = Ultra Performance (3.0x).");
idCVar r_fsrDebug("r_fsrDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "FSR Debug Mode.");
idCVar r_fsrSharpness("r_fsrSharpness", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                      "RCAS sharpening for r_fsr 1.  0 = softest, 1 = sharpest.");
idCVar r_fsrJitter("r_fsrJitter", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                   "Halton(2,3) sub-pixel jitter while upscaling, replacing r_jitter.  r_fsr 2 needs it and "
                   "goes blurry without it; on the bilinear and FSR 1 paths it only adds shimmer.  Kept "
                   "switchable so \"is the jitter perturbing something?\" stays an A/B rather than an argument.");
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
#if defined(DHEWM3_FSR2)
static fsrPass_t s_fsr2Bleed; // U3, r_fsrDebug 5
static bool s_fsr2BleedReady = false;
#endif
static bool s_fsr1Ready = false;
static bool s_motionDebugReady = false;
static int s_fsrLoggedMode = -1; // last r_fsr value announced to the console

// Set by VK_RT_DispatchUpscale once FSR 2 has actually run.  Read a frame later by
// VK_RT_GetFsrJitter, which is called from the frontend and cannot ask the backend
// whether the context exists yet.  One frame of lag on a mode switch is harmless.
static bool s_fsr2Running = false;

// The 3D view's projection parameters, latched by VK_RT_CaptureFsrViewParams.  The
// resolve runs after the frame's last RC_DRAW_VIEW, by which point backEnd.viewDef may
// be the 2D overlay (zeroed viewaxis, meaningless fov) — see the U0 notes in §4.
struct fsrViewParams_t
{
    bool valid;
    bool prevFrameValid;
    bool cameraCut;
    float nearZ;
    float fovYRad;
    float jitterX; // render pixels, the value R_BuildProjection actually applied
    float jitterY;
};
static fsrViewParams_t s_fsrView;

// FSR 2's own cut state, kept apart from the AO/GI/vol flags: those also clear on a
// render-extent change, which for FSR 2 is already covered by the context rebuild.
static idVec3 s_fsrPrevCamPos(0.0f, 0.0f, 0.0f);
static idVec3 s_fsrPrevCamFwd(1.0f, 0.0f, 0.0f);
static bool s_fsrCamValid = false;

void VK_RT_CaptureFsrViewParams(const viewDef_t *viewDef)
{
    // From the projection, not r_znear: game code writes that cvar directly and drops it
    // to 1.0 for cinematics, so a backend read can disagree with the matrix this frame's
    // depth was rasterised through.  (renderView.cramZNear is dead — never assigned
    // anywhere in the tree — so R_BuildProjection's 0.25 branch never fires.)
    const float zn = VK_RT_EffectiveZNear(viewDef);

    const vkRTCameraCutResult_t cut =
        VK_RT_DetectCameraCut(viewDef, s_fsrPrevCamPos, s_fsrPrevCamFwd, s_fsrCamValid, "FSR");
    s_fsrCamValid = true;

    s_fsrView.valid = true;
    s_fsrView.prevFrameValid = viewDef->prevFrameValid;
    s_fsrView.cameraCut = cut.isCut;
    s_fsrView.nearZ = zn;
    s_fsrView.fovYRad = DEG2RAD(viewDef->renderView.fov_y);
    s_fsrView.jitterX = viewDef->jitterOffset[0];
    s_fsrView.jitterY = viewDef->jitterOffset[1];
}

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

#if defined(DHEWM3_FSR2)
    // U3 bleed overlay (r_fsrDebug 5).  Two bindings is enough: binding 1 is a storage
    // image, so the shader reads FSR2's output and writes the tint back over it.
    s_fsr2BleedReady = VK_RT_CreateFsrPass(s_fsr2Bleed, "glprogs/glsl/fsr2_bleed_debug.comp.spv",
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(Fsr2BleedPC));
    if (!s_fsr2BleedReady)
        common->Warning("VK RT Upscale: fsr2_bleed_debug.comp failed to load — r_fsrDebug 5 unavailable");
#endif
}

static void VK_RT_DestroyFsr1Pipelines(void)
{
    VK_RT_DestroyFsrPass(s_fsrPrepare);
    VK_RT_DestroyFsrPass(s_fsrEasu);
    VK_RT_DestroyFsrPass(s_fsrRcas);
    VK_RT_DestroyFsrPass(s_motionDebug);
    s_fsr1Ready = false;
    s_motionDebugReady = false;
#if defined(DHEWM3_FSR2)
    VK_RT_DestroyFsrPass(s_fsr2Bleed);
    s_fsr2BleedReady = false;
#endif
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
    if (r_fsrDebug.GetInteger() != 7 || !s_motionDebugReady || !vk.gbufferSupported ||
        vkRT.motionVectors[vk.currentFrame].image == VK_NULL_HANDLE)
        return false;

    // The attachment is only ever written by the G-buffer prepass, which stands down
    // when r_useRayTracing is off (VK_RB_FillDepthBuffer).  Without this the overlay
    // would happily paint the whole screen with the cleared zero field — a black frame
    // that reads as "motion vectors are broken" rather than "ray tracing is off".
    if (!r_useRayTracing.GetBool())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            common->Warning("VK RT Upscale: r_fsrDebug 7 needs r_useRayTracing 1 — motion vectors are written by "
                            "the ray-tracing G-buffer prepass; overlay disabled");
        }
        return false;
    }
    return true;
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
FSR 2 (U3)

AMD FidelityFX Super Resolution 2.2.1, driven from the vendored library in
libs/ffx-fsr2-api.  This is the one translation unit that includes its headers.

Inputs, all display-sized images written only inside the top-left renderExtent
sub-rect (§2 of the plan):

  color   hdrScene[slot]        pre-tonemap linear HDR
  depth   vk.depthImage         D32S8, read through vk.depthSampledView
  MV      motionVectors[slot]   GL NDC delta, Y up (U2)
  output  hdrUpscaled[slot]     display-res, copied back over hdrScene

The sub-rect layout works unmodified because FSR2's only UV-space read of the
colour buffer (the luminance pyramid) routes through ClampUv with the resource's
real dimensions; every other input read is a texelFetch.  So the resource
descriptions carry the *display* extent while renderSize carries the sub-rect.

Two conventions are derived rather than guessed, both from
fSrcUnjitteredPos = iPxLrPos + 0.5 - Jitter() (ffx_fsr2_upsample.h):

  motionVectorScale  {+0.5*renderW, -0.5*renderH}.  FSR2 wants a UV offset after
        scaling by motionVectorScale/renderSize, i.e. render pixels before it.
        NDC->pixels is *0.5*extent per axis, and Y flips because our framebuffer
        row is (1-ndc.y)/2 (the negative-height viewport).
  jitterOffset       {-jitterX, +jitterY}.  FSR2's jitterOffset is the image's
        displacement in render pixels, Y down.  R_BuildProjection shifts the
        frustum, which moves the image the *other* way in X, and the Y flip
        cancels the second sign.  Same Y-flip asymmetry as above.

Known inexactness: our projection uses -0.999 rather than -1.0 for the
far-plane-at-infinity row, so device depth tops out at 0.9995 instead of 1.0 and
FSR2's linearisation saturates around 2000x near.  The warp is monotonic and
FSR2 only compares depths relatively, so this costs disocclusion sensitivity at
long range, not correctness.  It cannot be expressed exactly in either of FSR2's
depth families — see the plan's §13/U3 notes.
===========================================================================
*/

#if defined(DHEWM3_FSR2)

static FfxFsr2Context s_fsr2Ctx;
static bool s_fsr2CtxValid = false;
static bool s_fsr2CreateFailed = false; // don't retry every frame; cleared on resize
static void *s_fsr2Scratch = NULL;
static VkExtent2D s_fsr2CtxRender = {0, 0};
static VkExtent2D s_fsr2CtxDisplay = {0, 0};
static uint32_t s_fsr2CtxFlags = 0;
static bool s_fsr2ForceReset = true;
static uint64_t s_fsr2PrevTick = 0;

static void VK_RT_Fsr2Message(FfxFsr2MsgType type, const wchar_t *message)
{
    // AMD's messages are wide; narrow them crudely (they are ASCII in practice).
    char buf[512];
    size_t i = 0;
    for (; message[i] != L'\0' && i < sizeof(buf) - 1; i++)
        buf[i] = (message[i] < 128) ? (char)message[i] : '?';
    buf[i] = '\0';

    if (type == FFX_FSR2_MESSAGE_TYPE_ERROR)
        common->Warning("VK RT FSR2: %s", buf);
    else
        common->Printf("VK RT FSR2: %s\n", buf);
}

static uint32_t VK_RT_Fsr2WantedFlags(void)
{
    // HIGH_DYNAMIC_RANGE: hdrScene is pre-tonemap linear.
    // DEPTH_INFINITE: R_BuildProjection is the far-plane-at-infinity formulation.
    // DEPTH_INVERTED is deliberately absent — our depth is 0 at the near plane.
    // AUTO_EXPOSURE: the engine's tonemap is a fixed curve with no exposure of its
    // own, so there is no application exposure value to hand over.
    uint32_t flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_DEPTH_INFINITE |
                     FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    if (r_fsrDebug.GetInteger() >= 2)
        flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
    return flags;
}

static void VK_RT_Fsr2DestroyContext(void)
{
    if (!s_fsr2CtxValid)
        return;

    // FSR2 owns images and pipelines that in-flight frames may still be reading.
    vkDeviceWaitIdle(vk.device);
    ffxFsr2ContextDestroy(&s_fsr2Ctx);
    s_fsr2CtxValid = false;

    free(s_fsr2Scratch);
    s_fsr2Scratch = NULL;
    s_fsr2ForceReset = true;
    common->Printf("VK RT FSR2: context destroyed\n");
}

static void VK_RT_Fsr2ClearCreateFailure(void)
{
    s_fsr2CreateFailed = false;
}

static bool VK_RT_Fsr2CreateContext(void)
{
    const size_t scratchSize = ffxFsr2GetScratchMemorySizeVK(vk.physicalDevice);
    s_fsr2Scratch = malloc(scratchSize);
    if (s_fsr2Scratch == NULL)
    {
        common->Warning("VK RT FSR2: could not allocate %u bytes of backend scratch", (unsigned int)scratchSize);
        return false;
    }

    FfxFsr2ContextDescription desc = {};
    FfxErrorCode err = ffxFsr2GetInterfaceVK(&desc.callbacks, s_fsr2Scratch, scratchSize, vk.physicalDevice,
                                             vkGetDeviceProcAddr);
    if (err != FFX_OK)
    {
        common->Warning("VK RT FSR2: ffxFsr2GetInterfaceVK failed (%d)", (int)err);
        free(s_fsr2Scratch);
        s_fsr2Scratch = NULL;
        return false;
    }

    desc.device = ffxGetDeviceVK(vk.device);
    desc.maxRenderSize.width = vk.renderExtent.width;
    desc.maxRenderSize.height = vk.renderExtent.height;
    desc.displaySize.width = vk.swapchainExtent.width;
    desc.displaySize.height = vk.swapchainExtent.height;
    desc.flags = VK_RT_Fsr2WantedFlags();
    desc.fpMessage = VK_RT_Fsr2Message;

    err = ffxFsr2ContextCreate(&s_fsr2Ctx, &desc);
    if (err != FFX_OK)
    {
        common->Warning("VK RT FSR2: ffxFsr2ContextCreate failed (%d) — falling back", (int)err);
        free(s_fsr2Scratch);
        s_fsr2Scratch = NULL;
        return false;
    }

    s_fsr2CtxValid = true;
    s_fsr2CtxRender = vk.renderExtent;
    s_fsr2CtxDisplay = vk.swapchainExtent;
    s_fsr2CtxFlags = desc.flags;
    s_fsr2ForceReset = true;

    // FSR2 does not expose its internal VRAM total; the scratch figure is the only
    // number it hands back.  Expect ~60-250 MB of device memory on top (plan §3).
    common->Printf("VK RT FSR2: context created, render %ux%u -> display %ux%u, flags 0x%x, host scratch %u KB, "
                   "jitter phases %d\n",
                   vk.renderExtent.width, vk.renderExtent.height, vk.swapchainExtent.width, vk.swapchainExtent.height,
                   desc.flags, (unsigned int)(scratchSize / 1024),
                   ffxFsr2GetJitterPhaseCount((int32_t)vk.renderExtent.width, (int32_t)vk.swapchainExtent.width));
    return true;
}

// Is FSR 2 the path the user asked for?  r_fsrDebug 4 is the point-magnify A/B and
// lives on the bilinear path, so it overrides r_fsr exactly as it does for FSR 1.
static bool VK_RT_Fsr2Requested(void)
{
    return r_fsr.GetInteger() == 2 && r_fsrDebug.GetInteger() != 4;
}

// Could FSR 2 resolve this frame?  Side-effect free, unlike VK_RT_Fsr2Ready — the
// render-extent fallback asks this *before* the extent is settled and must not trigger
// a context build at the old size.
static bool VK_RT_Fsr2Possible(void)
{
    return VK_RT_Fsr2Requested() && vk.fsr2Supported && vk.gbufferSupported && !s_fsr2CreateFailed;
}

// Create / recreate / tear down as needed, and report whether this frame can dispatch.
static bool VK_RT_Fsr2Ready(void)
{
    if (!VK_RT_Fsr2Requested())
    {
        VK_RT_Fsr2DestroyContext();
        return false;
    }

    if (!vk.fsr2Supported || !vk.gbufferSupported)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            common->Warning("VK RT FSR2: unavailable on this device (%s) — r_fsr 2 falls back to the bilinear resolve",
                            !vk.gbufferSupported ? "no G-buffer prepass, so no motion vectors"
                                                 : "see the vk.fsr2Supported warning at startup");
        }
        return false;
    }

    const int slot = (int)vk.currentFrame;
    if (vkRT.motionVectors[slot].image == VK_NULL_HANDLE || vk.depthSampledView == VK_NULL_HANDLE)
        return false;

    const uint32_t wantFlags = VK_RT_Fsr2WantedFlags();
    if (s_fsr2CtxValid && (s_fsr2CtxRender.width != vk.renderExtent.width ||
                           s_fsr2CtxRender.height != vk.renderExtent.height ||
                           s_fsr2CtxDisplay.width != vk.swapchainExtent.width ||
                           s_fsr2CtxDisplay.height != vk.swapchainExtent.height || s_fsr2CtxFlags != wantFlags))
    {
        VK_RT_Fsr2DestroyContext();
        s_fsr2CreateFailed = false; // a different configuration deserves a fresh try
    }

    if (!s_fsr2CtxValid)
    {
        if (s_fsr2CreateFailed)
            return false;
        if (!VK_RT_Fsr2CreateContext())
        {
            s_fsr2CreateFailed = true;
            return false;
        }
    }
    return true;
}

// r_fsrDebug 5: pillar 2's black-level gate.  Tints pixels where the upscaled output
// is brighter than the sub-rect input it came from by more than a threshold — i.e.
// where FSR2's history has leaked light into a pixel that should be dark.
static void VK_RT_DispatchFsr2BleedDebug(VkCommandBuffer cmd, int frameIdx)
{
    if (!s_fsr2BleedReady)
        return;

    // hdrScene is in SHADER_READ_ONLY_OPTIMAL and hdrUpscaled in GENERAL, which is
    // exactly where ffxFsr2ContextDispatch left them.
    VK_RT_WriteFsrPassDescriptors(s_fsr2Bleed, frameIdx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  vkRT.hdrScene[frameIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  vkRT.hdrUpscaled[frameIdx].view);

    Fsr2BleedPC pc;
    pc.renderExtent[0] = (int32_t)vk.renderExtent.width;
    pc.renderExtent[1] = (int32_t)vk.renderExtent.height;
    pc.displayExtent[0] = (int32_t)vk.swapchainExtent.width;
    pc.displayExtent[1] = (int32_t)vk.swapchainExtent.height;
    pc.threshold = 0.02f; // linear HDR luminance; the black floor we care about

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsr2Bleed.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsr2Bleed.layout, 0, 1,
                            &s_fsr2Bleed.descSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, s_fsr2Bleed.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (vk.swapchainExtent.width + 7) / 8, (vk.swapchainExtent.height + 7) / 8, 1);

    VkMemoryBarrier mem = {};
    mem.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mem.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mem.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mem, 0,
                         NULL, 0, NULL);
}

static VkImageAspectFlags VK_RT_DepthAspect(void)
{
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return aspect;
}

static void VK_RT_DispatchFsr2(VkCommandBuffer cmd, int frameIdx)
{
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const uint32_t dispW = vk.swapchainExtent.width;
    const uint32_t dispH = vk.swapchainExtent.height;

    // --- 1. Hand FSR2 the layouts its FfxResourceStates name ---------------------
    // COMPUTE_READ maps to SHADER_READ_ONLY_OPTIMAL and UNORDERED_ACCESS to GENERAL
    // (getVKImageLayoutFromResourceState, ffx_fsr2_vk.cpp:352).  FSR2 emits its own
    // barrier from the state we declare, so the declaration has to be the truth —
    // hence moving all three inputs there first and restoring them afterwards.
    VkImageMemoryBarrier toRead[2] = {};
    VkImage colorImages[2] = {vkRT.hdrScene[frameIdx].image, vkRT.motionVectors[frameIdx].image};
    for (int i = 0; i < 2; i++)
    {
        toRead[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toRead[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = colorImages[i];
        toRead[i].subresourceRange = colorRange;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 2, toRead);

    // Depth needs SHADER_READ_ONLY rather than the engine's usual
    // DEPTH_STENCIL_READ_ONLY, because that is the only read layout FSR2 knows.
    VkImageMemoryBarrier depthToRead = {};
    depthToRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthToRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthToRead.image = vk.depthImage;
    depthToRead.subresourceRange = {VK_RT_DepthAspect(), 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &depthToRead);

    // --- 2. Per-frame dispatch description --------------------------------------
    FfxFsr2DispatchDescription dd = {};
    dd.commandList = ffxGetCommandListVK(cmd);

    // Resource dimensions are the images' real (display) size; renderSize below is
    // the sub-rect FSR2 should actually read.
    dd.color = ffxGetTextureResourceVK(&s_fsr2Ctx, vkRT.hdrScene[frameIdx].image, vkRT.hdrScene[frameIdx].view, dispW,
                                       dispH, VK_FORMAT_R16G16B16A16_SFLOAT, L"hdrScene",
                                       FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.depth = ffxGetTextureResourceVK(&s_fsr2Ctx, vk.depthImage, vk.depthSampledView, dispW, dispH, vk.depthFormat,
                                       L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors = ffxGetTextureResourceVK(&s_fsr2Ctx, vkRT.motionVectors[frameIdx].image,
                                               vkRT.motionVectors[frameIdx].view, dispW, dispH, VK_FORMAT_R16G16_SFLOAT,
                                               L"motionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.output = ffxGetTextureResourceVK(&s_fsr2Ctx, vkRT.hdrUpscaled[frameIdx].image, vkRT.hdrUpscaled[frameIdx].view,
                                        dispW, dispH, VK_FORMAT_R16G16B16A16_SFLOAT, L"hdrUpscaled",
                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    // exposure / reactive / transparencyAndComposition stay null: auto exposure is on
    // and the masks are U4.

    dd.renderSize.width = vk.renderExtent.width;
    dd.renderSize.height = vk.renderExtent.height;

    // See the block comment above for both sign derivations.
    dd.jitterOffset.x = -s_fsrView.jitterX;
    dd.jitterOffset.y = s_fsrView.jitterY;
    dd.motionVectorScale.x = 0.5f * (float)vk.renderExtent.width;
    dd.motionVectorScale.y = -0.5f * (float)vk.renderExtent.height;

    float sharp = r_fsrSharpness.GetFloat();
    if (sharp < 0.0f)
        sharp = 0.0f;
    if (sharp > 1.0f)
        sharp = 1.0f;
    dd.enableSharpening = sharp > 0.0f;
    dd.sharpness = sharp;

    // Wall-clock, not tr.frameShaderTime: that one stops in the menu and FSR2 uses
    // the delta for its exposure EMA and lock decay.
    const uint64_t tick = SDL_GetPerformanceCounter();
    const uint64_t freq = SDL_GetPerformanceFrequency();
    float deltaMs = 16.6f;
    if (s_fsr2PrevTick != 0 && freq != 0 && tick > s_fsr2PrevTick)
        deltaMs = (float)((double)(tick - s_fsr2PrevTick) * 1000.0 / (double)freq);
    s_fsr2PrevTick = tick;
    if (deltaMs < 0.1f)
        deltaMs = 0.1f;
    if (deltaMs > 1000.0f)
        deltaMs = 1000.0f;
    dd.frameTimeDelta = deltaMs;

    dd.preExposure = 1.0f; // must be > 0; the engine has no pre-exposure of its own
    dd.cameraNear = s_fsrView.nearZ;
    // Large but finite rather than FLT_MAX: DEPTH_INFINITE means FSR2 ignores it, and a
    // finite value keeps setupDeviceDepthToViewSpaceDepthParams' unused fQ*fMin term
    // from overflowing to infinity.
    dd.cameraFar = 1.0e6f;
    dd.cameraFovAngleVertical = s_fsrView.fovYRad;
    dd.viewSpaceToMetersFactor = 0.0254f; // Doom 3 units are inches

    // reset: a discontinuity means the history is worthless rather than merely stale.
    // VK_RT_CaptureFsrViewParams runs the shared cut detector on the 3D view.
    dd.reset = s_fsr2ForceReset || !s_fsrView.valid || !s_fsrView.prevFrameValid || s_fsrView.cameraCut;
    if (dd.reset && r_fsrDebug.GetInteger() >= 2)
        common->Printf("VK RT FSR2: history reset (forced=%d viewValid=%d prevFrame=%d cut=%d)\n",
                       s_fsr2ForceReset ? 1 : 0, s_fsrView.valid ? 1 : 0, s_fsrView.prevFrameValid ? 1 : 0,
                       s_fsrView.cameraCut ? 1 : 0);
    s_fsr2ForceReset = false;

    const FfxErrorCode err = ffxFsr2ContextDispatch(&s_fsr2Ctx, &dd);
    if (err != FFX_OK)
        common->Warning("VK RT FSR2: ffxFsr2ContextDispatch failed (%d)", (int)err);

    if (r_fsrDebug.GetInteger() == 5)
        VK_RT_DispatchFsr2BleedDebug(cmd, frameIdx);

    // --- 3. Copy the display-res result back over hdrScene ----------------------
    // FSR2 left color/depth/MV in SHADER_READ_ONLY_OPTIMAL and output in GENERAL.
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

    preCopy[1] = preCopy[0];
    preCopy[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    preCopy[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    preCopy[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    preCopy[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    preCopy[1].image = vkRT.hdrScene[frameIdx].image;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         2, preCopy);

    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.extent = {dispW, dispH, 1};
    vkCmdCopyImage(cmd, vkRT.hdrUpscaled[frameIdx].image, VK_IMAGE_LAYOUT_GENERAL, vkRT.hdrScene[frameIdx].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // --- 4. Restore every layout the resume pass and next frame expect ----------
    VkImageMemoryBarrier restore[2] = {};
    restore[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restore[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    restore[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    restore[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    restore[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    restore[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore[0].image = vkRT.hdrScene[frameIdx].image;
    restore[0].subresourceRange = colorRange;

    restore[1] = restore[0];
    restore[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    restore[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    restore[1].image = vkRT.motionVectors[frameIdx].image;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 2, restore);

    VkImageMemoryBarrier depthRestore = depthToRead;
    depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthRestore.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    depthRestore.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0,
                         NULL, 0, NULL, 1, &depthRestore);
}

#else // !DHEWM3_FSR2

static bool VK_RT_Fsr2Possible(void)
{
    return false;
}
static bool VK_RT_Fsr2Ready(void)
{
    if (r_fsr.GetInteger() == 2)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            common->Warning("VK RT FSR2: built without DHEWM3_FSR2 — r_fsr 2 falls back to the bilinear resolve");
        }
    }
    return false;
}
static void VK_RT_DispatchFsr2(VkCommandBuffer, int)
{
}
static void VK_RT_Fsr2DestroyContext(void)
{
}
static void VK_RT_Fsr2ClearCreateFailure(void)
{
}

#endif // DHEWM3_FSR2

/*
===========================================================================
Jitter (U2)

U3 calls FSR 2's real ffxFsr2GetJitterPhaseCount / ffxFsr2GetJitterOffset.  U2's
hand port of the same functions (neo/libs/ffx-fsr2-api/ffx_fsr2.cpp:188-201,
1187-1212) stays as the DHEWM3_FSR2=OFF fallback, so the sequence is unchanged
either way.
===========================================================================
*/

#if !defined(DHEWM3_FSR2)
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
#endif

/*
Sub-pixel offset for this frame, in render-resolution pixels over [-0.5, +0.5],
plus the render extent the caller should measure it against.  Returns false when
the FSR jitter is not in play, leaving the outputs untouched so R_SetupProjection
can fall back to r_jitter.

r_fsr 2 wants it on — FSR 2 reconstructs from the jittered sample grid, and without
jitter it has nothing to resolve and reads as a blur — but it is deliberately NOT forced.
Jitter is the one lever that separates "a screen-space input is changing frame to frame
and nothing reconciles it" from "FSR 2's own history is unstable", and an A/B you cannot
run is not a debug lever.  r_fsrJitter defaults to 1 instead; turning it off under r_fsr 2
warns once and is expected to look soft.
*/
bool VK_RT_GetFsrJitter(float *jx, float *jy, int *renderW, int *renderH)
{
    if (!r_fsrJitter.GetBool())
    {
        if (s_fsr2Running)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                common->Warning("VK RT FSR2: r_fsrJitter 0 with r_fsr 2 — FSR 2 has no sub-pixel samples to "
                                "reconstruct from and will read as a blur.  This is the A/B, not a usable setting.");
            }
        }
        return false;
    }
    if (!VK_RT_UpscaleActive() || vk.renderExtent.width == 0)
        return false;

#if defined(DHEWM3_FSR2)
    const int32_t phaseCount =
        ffxFsr2GetJitterPhaseCount((int32_t)vk.renderExtent.width, (int32_t)vk.swapchainExtent.width);
#else
    const int32_t phaseCount =
        (int32_t)(8.0f * powf((float)vk.swapchainExtent.width / (float)vk.renderExtent.width, 2.0f));
#endif
    if (phaseCount <= 0)
        return false;

#if defined(DHEWM3_FSR2)
    // ffxFsr2GetJitterOffset does its own (index % phaseCount) + 1, so hand it the raw
    // frame counter rather than a pre-reduced one.
    if (ffxFsr2GetJitterOffset(jx, jy, (int32_t)(((uint32_t)tr.frameCount) & 0x7fffffffu), phaseCount) != FFX_OK)
        return false;
#else
    const int32_t index = (int32_t)(((uint32_t)tr.frameCount) % (uint32_t)phaseCount) + 1;
    *jx = VK_RT_Halton(index, 2) - 0.5f;
    *jy = VK_RT_Halton(index, 3) - 0.5f;
#endif
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

// Can the path that would ACTUALLY run this frame resolve the sub-rect?  This must stay
// the exact negation of VK_RT_DispatchUpscale's pipeline early-out, not a looser "some
// pipeline loaded" test: with a broken bilinear blit but working FSR 1 shaders, the
// looser test keeps the reduced extent alive while r_fsr 0 (the default) and
// r_fsrDebug 4 both still dispatch nothing — recreating the stranded-in-the-corner
// frame this fallback exists to prevent.
static bool VK_RT_ResolvePathReady(void)
{
    return vkRT.upscalePipeline != VK_NULL_HANDLE || VK_RT_Fsr1Active() || VK_RT_Fsr2Possible();
}

/*
The render scale to aim for.  r_fsrQuality overrides r_fsrRenderScale when set.

These are the divisors FSR, DLSS and XeSS have all converged on (§8.1), written out
rather than calling ffxFsr2GetRenderResolutionFromQualityMode so that the presets also
work with DHEWM3_FSR2 off and so VK_SnapExtent8 below stays the single rounding rule.

Deviation from the plan: the default is 0 (use r_fsrRenderScale), not 1.  Defaulting to
Quality would silently turn upscaling on for every existing config, and "r_fsr 0 /
native is the default until U3 passes its gate" is a hygiene rule of this arc.
*/
static float VK_RT_FsrTargetScale(void)
{
    switch (r_fsrQuality.GetInteger())
    {
    case 1:
        return 1.0f / 1.5f; // Quality
    case 2:
        return 1.0f / 1.7f; // Balanced
    case 3:
        return 1.0f / 2.0f; // Performance
    case 4:
        return 1.0f / 3.0f; // Ultra Performance
    default:
        return r_fsrRenderScale.GetFloat();
    }
}

void VK_RT_UpdateRenderExtent(void)
{
    const VkExtent2D old = vk.renderExtent;

    // Clamp low so a fat-fingered cvar can't collapse the scene to the 64px floor.
    float scale = VK_RT_FsrTargetScale();
    if (scale < 0.3f)
        scale = 0.3f;

    // With no resolve path there is nothing to magnify the sub-rect back up, and the
    // callers mark the resolve done regardless — so the UI would draw at display extent
    // over a scene stranded in the top-left corner.  Render native instead: a lost
    // performance option beats a broken frame, and it keeps every display-space divisor
    // in §4 consistent for free.
    if (scale < 1.0f && s_upscaleInitDone && !VK_RT_ResolvePathReady())
    {
        // Warn per r_fsr value, not once: the readiness test is mode-dependent, so
        // switching modes can start or stop hitting this and the user needs to know why.
        static int warnedMode = -1;
        if (warnedMode != r_fsr.GetInteger())
        {
            warnedMode = r_fsr.GetInteger();
            common->Warning("VK RT Upscale: no resolve pipeline for r_fsr %d — ignoring render scale %.2f and "
                            "rendering at native resolution",
                            warnedMode, scale);
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

    // VK_RT_DispatchUpscale is only reached while the extents differ, so it cannot be
    // the one that notices "we are back at native" or "r_fsr left 2".  Free FSR 2's
    // internal resources here instead — they are 60-250 MB of VRAM doing nothing.
    if (!VK_RT_Fsr2Possible() || wn == vk.swapchainExtent.width)
    {
        s_fsr2Running = false;
        VK_RT_Fsr2DestroyContext();
    }
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

    // Asked first because it owns the FSR 2 context lifetime: it tears the context down
    // when r_fsr moves away from 2, and builds it when it moves to 2.
    const bool fsr2 = VK_RT_Fsr2Ready();
    s_fsr2Running = fsr2;

    if (!fsr2 && vkRT.upscalePipeline == VK_NULL_HANDLE && !VK_RT_Fsr1Active())
        return;

    const int frameIdx = (int)vk.currentFrame;
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    const int mode = r_fsr.GetInteger();
    if (mode != s_fsrLoggedMode)
    {
        const char *what = fsr2 ? "AMD FidelityFX Super Resolution 2"
                                : (VK_RT_Fsr1Active() ? "AMD FidelityFX Super Resolution 1 (EASU + RCAS)"
                                                      : "bilinear resolve");
        common->Printf("VK RT Upscale: r_fsr %d -> %s, render %ux%u -> display %ux%u, sharpness %.2f\n", mode, what,
                       vk.renderExtent.width, vk.renderExtent.height, vk.swapchainExtent.width,
                       vk.swapchainExtent.height, r_fsrSharpness.GetFloat());
        s_fsrLoggedMode = mode;
    }

    if (fsr2)
    {
        VK_RT_DispatchFsr2(cmd, frameIdx);
        return;
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
    // Both extents move and every view FSR 2 holds is about to be destroyed.  A failed
    // creation also gets a second chance: the new size may fit where the old did not.
    VK_RT_Fsr2DestroyContext();
    VK_RT_Fsr2ClearCreateFailure();
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
    VK_RT_Fsr2DestroyContext();
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
