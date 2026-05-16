/*
===========================================================================

dhewm3-rt Vulkan — vk_vol.cpp — Volumetric light scattering pipeline.

Phase 7.2: single-scatter ray-marched volumetrics using GL_EXT_ray_query
in a compute shader.  Each view-ray is stepped from the camera to the
depth-reconstructed surface, firing occlusion queries toward nearby lights
and accumulating in-scattering with a Henyey-Greenstein phase function.

Dispatch order in the RT frame (outside render pass):
  ... VK_RT_DispatchAtrousGI ...
  VK_RT_DispatchVolumetrics  ← this file
  (resume render pass)
  VK_RT_CompositeGI
  VK_RT_CompositeVolumetrics ← this file (inside render pass, additive)

Architecture:
  volMarch*    — compute pipeline (vol_march.comp + ray queries via TLAS)
  volComposite* — graphics pipeline (gi_composite.vert + vol_composite.frag)

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtVol("r_rtVol", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
               "Enable volumetric light scattering (Phase 7.2). On by default.");

static idCVar r_rtVolSamples("r_rtVolSamples", "8", CVAR_RENDERER | CVAR_INTEGER,
                             "Ray-march steps per pixel (safe default: 8; high-end: 16)");

static idCVar r_rtVolMaxDist("r_rtVolMaxDist", "512.0", CVAR_RENDERER | CVAR_FLOAT,
                             "Max ray-march distance in world units");

static idCVar r_rtVolMaxLights("r_rtVolMaxLights", "32", CVAR_RENDERER | CVAR_INTEGER,
                               "Nearest lights evaluated per step (pre-sorted, always closest)");

static idCVar r_rtVolDensity("r_rtVolDensity", "0.05", CVAR_RENDERER | CVAR_FLOAT,
                             "Global scattering density (extinction + scattering coefficient)");

static idCVar r_rtVolStrength("r_rtVolStrength", "0.10", CVAR_RENDERER | CVAR_FLOAT,
                              "Final composite scale for point-light scatter");

static idCVar r_rtVolAnisotropy("r_rtVolAnisotropy", "0.25", CVAR_RENDERER | CVAR_FLOAT,
                                "Henyey-Greenstein g parameter (0=isotropic, 0.8=flashlight shaft)");

// Scene directed/spot lights (lightType 1) — separate from the player's flashlight.
static idCVar r_rtVolDirectedDensity("r_rtVolDirectedDensity", "0.05", CVAR_RENDERER | CVAR_FLOAT,
                                     "Scatter contribution scale for scene directed/spot lights.");
static idCVar r_rtVolDirectedStrength("r_rtVolDirectedStrength", "0.1", CVAR_RENDERER | CVAR_FLOAT,
                                      "Final composite multiplier for scene directed light scatter.");
static idCVar r_rtVolDirectedAnisotropy("r_rtVolDirectedAnisotropy", "0.5", CVAR_RENDERER | CVAR_FLOAT,
                                        "Henyey-Greenstein g for scene spot lights (0=iso, 1=full forward).");

// Player flashlight (lightType 2, allowLightInViewID set).
static idCVar r_rtVolFlashlightDensity("r_rtVolFlashlightDensity", "0.05", CVAR_RENDERER | CVAR_FLOAT,
                                       "Scatter contribution scale for the player flashlight.");
static idCVar r_rtVolFlashlightAnisotropy(
    "r_rtVolFlashlightAnisotropy", "0.7", CVAR_RENDERER | CVAR_FLOAT,
    "Henyey-Greenstein g parameter for the flashlight (0=isotropic, 1=full forward).");
static idCVar r_rtVolFlashlightStrength("r_rtVolFlashlightStrength", "0.5", CVAR_RENDERER | CVAR_FLOAT,
                                        "Final composite multiplier for flashlight scatter.");

// ---------------------------------------------------------------------------
// VolParamsUBO — must match the std140 VolParams block in vol_march.comp.
//
//   mat4  invViewProj       offset   0  size 64
//   vec4  cameraPosW        offset  64  size 16  (xyz = camera pos, w unused)
//   uint  frameIndex        offset  80  size  4
//   int   numSamples        offset  84  size  4
//   int   maxLights         offset  88  size  4
//   float density           offset  92  size  4  (point lights — Beer-Lambert too)
//   float anisotropy        offset  96  size  4  (point lights HG)
//   float maxDist           offset 100  size  4
//   float strength          offset 104  size  4  (point lights)
//   int   scissorOffX       offset 108  size  4
//   int   scissorOffY       offset 112  size  4
//   int   scissorExtX       offset 116  size  4
//   int   scissorExtY       offset 120  size  4
//   int   screenWidth       offset 124  size  4
//   int   screenHeight      offset 128  size  4
//   float flashlightDensity    offset 132  size  4  (player flashlight, lightType 2)
//   float flashlightAniso      offset 136  size  4
//   float flashlightStrength   offset 140  size  4
//   float directedDensity      offset 144  size  4  (scene spot/directed, lightType 1)
//   float directedAnisotropy   offset 148  size  4
//   float directedStrength     offset 152  size  4
//   float _pad                 offset 156  size  4  (std140 round to 16)
//   total: 160 bytes
// ---------------------------------------------------------------------------

struct VolParamsUBO
{
    float invViewProj[16];      // 0
    float cameraPosX;           // 64
    float cameraPosY;           // 68
    float cameraPosZ;           // 72
    float cameraPad;            // 76 — pads vec4 cameraPosW in GLSL
    uint32_t frameIndex;        // 80
    int32_t numSamples;         // 84
    int32_t maxLights;          // 88
    float density;              // 92
    float anisotropy;           // 96
    float maxDist;              // 100
    float strength;             // 104
    int32_t scissorOffsetX;     // 108
    int32_t scissorOffsetY;     // 112
    int32_t scissorExtentX;     // 116
    int32_t scissorExtentY;     // 120
    int32_t screenWidth;        // 124
    int32_t screenHeight;       // 128
    float flashlightDensity;    // 132
    float flashlightAnisotropy; // 136
    float flashlightStrength;   // 140
    float directedDensity;      // 144
    float directedAnisotropy;   // 148
    float directedStrength;     // 152
    float _uboPad;              // 156
};
static_assert(sizeof(VolParamsUBO) == 160, "VolParamsUBO size mismatch");

// ---------------------------------------------------------------------------
// Forward declarations from other vk_*.cpp modules
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

static void VK_RT_CreateVolImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &vb = vkRT.volBuffer[i];
        vb.width = width;
        vb.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &vb.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, vb.image, &memReq);

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
            common->Error("VK RT Vol: no device-local memory type for vol buffer");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &vb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, vb.image, vb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vb.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &vb.view));

        // Transition UNDEFINED → GENERAL and clear to black.
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
            barrier.image = vb.image;
            barrier.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, vb.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.image = vb.image;
            barrier2.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 NULL, 0, NULL, 1, &barrier2);

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
        // Default: composite reads raw volBuffer; updated by DispatchTemporalResolveVol each frame.
        vkRT.volReadView[i] = vkRT.volBuffer[i].view;
    }
}

static void VK_RT_DestroyVolImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &vb = vkRT.volBuffer[i];
        if (vb.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, vb.view, NULL);
            vb.view = VK_NULL_HANDLE;
        }
        if (vb.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, vb.image, NULL);
            vb.image = VK_NULL_HANDLE;
        }
        if (vb.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vb.memory, NULL);
            vb.memory = VK_NULL_HANDLE;
        }
        vb.width = 0;
        vb.height = 0;
        vkRT.volReadView[i] = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitVolMarchPipeline
// Compute pipeline for vol_march.comp — reads TLAS + depth, writes volBuf.
// Descriptor layout mirrors vol_march.comp bindings:
//   binding 0: ACCELERATION_STRUCTURE_KHR (TLAS)
//   binding 1: STORAGE_IMAGE              (volBuf write)
//   binding 2: COMBINED_IMAGE_SAMPLER     (depth)
//   binding 3: UNIFORM_BUFFER_DYNAMIC     (VolParamsUBO)
//   binding 4: STORAGE_BUFFER             (GILightBuf SSBO)
// ---------------------------------------------------------------------------

static void VK_RT_InitVolMarchPipeline(void)
{
    // --- Descriptor set layout ---
    VkDescriptorSetLayoutBinding bindings[5] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
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

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.volMarchDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.volMarchDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.volMarchPipelineLayout));

    // --- Compute shader module ---
    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/vol_march.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Vol: failed to load vol_march.comp.spv — volumetrics disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = compMod;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = vkRT.volMarchPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.volMarchPipeline));

    vkDestroyShaderModule(vk.device, compMod, NULL);

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSizes[5] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[3] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.volMarchDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.volMarchDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.volMarchDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.volMarchDescSets));

    // Create the bilinear-clamp sampler shared with the composite pass.
    if (vkRT.volSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.volSampler));
    }

    common->Printf("VK RT Vol: march pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitVolCompositePipeline
// Fullscreen additive pipeline blending volBuf onto the framebuffer.
// Mirrors the GI composite pipeline exactly; uses:
//   gi_composite.vert.spv  (shared fullscreen-triangle generator)
//   vol_composite.frag.spv (samples u_VolMap, passthrough)
// ---------------------------------------------------------------------------

static void VK_RT_InitVolCompositePipeline(void)
{
    // --- Descriptor set layout: 1 combined-image-sampler binding ---
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.volCompositeDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.volCompositeDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.volCompositeLayout));

    // --- Shader modules — share gi_composite.vert.spv ---
    VkShaderModule vertMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.vert.spv");
    VkShaderModule fragMod = VK_LoadSPIRV("glprogs/glsl/vol_composite.frag.spv");
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Vol: failed to load composite shaders — composite pass disabled");
        if (vertMod != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, vertMod, NULL);
        if (fragMod != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, fragMod, NULL);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertInput = {};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa = {};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    // Additive blend: volumetric light is added on top of existing framebuffer.
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = vkRT.volCompositeLayout;
    pipelineInfo.renderPass = vk.renderPass;
    pipelineInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.volCompositePipeline));

    vkDestroyShaderModule(vk.device, vertMod, NULL);
    vkDestroyShaderModule(vk.device, fragMod, NULL);

    // --- Descriptor pool and sets ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.volCompositeDescPool));

    VkDescriptorSetLayout compLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        compLayouts[i] = vkRT.volCompositeDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.volCompositeDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = compLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.volCompositeDescSets));

    common->Printf("VK RT Vol: composite pipeline initialized\n");
}

// ===========================================================================
// Volumetric temporal EMA (Phase 7.2 — step 8)
//
// Reuses gi_temporal_resolve.comp.spv — the shader is identical (rgba16f
// storage images, same push-constant layout).  No new shader file needed.
// ===========================================================================

static idCVar r_rtVolTemporal("r_rtVolTemporal", "1", CVAR_RENDERER | CVAR_BOOL,
                              "Enable temporal EMA accumulation for volumetrics (requires r_rtVol 1).");

static idCVar r_rtVolTemporalAlpha("r_rtVolTemporalAlpha", "0.15", CVAR_RENDERER | CVAR_FLOAT,
                                   "Vol EMA blend factor: 0=history only, 1=current only. "
                                   "0.1-0.2 recommended; lower = smoother but more ghosting.");

extern idCVar r_rtAOTemporalCutThreshold; // camera-cut L-inf threshold, defined in vk_temporal.cpp

static VkRect2D s_volTemporalDispatchRect[VK_MAX_FRAMES_IN_FLIGHT] = {};

// Mirrors VK_RT_ComputeViewDispatchRect from vk_temporal.cpp (static there, duplicated here).
static VkRect2D VK_RT_Vol_ComputeDispatchRect(const viewDef_t *viewDef)
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
// History image lifecycle (mirrors VK_RT_AllocGIHistoryImage pattern)
// ---------------------------------------------------------------------------

static bool VK_RT_AllocVolHistoryImage(vkReflBuffer_t &img, uint32_t width, uint32_t height)
{
    img.width = width;
    img.height = height;

    VkImageCreateInfo imgCI = {};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imgCI.extent = {width, height, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgCI, NULL, &img.image));

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
        common->Warning("VK RT Vol Temporal: no device-local memory for history image");
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

    VkImageViewCreateInfo viewCI = {};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = img.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewCI, NULL, &img.view));

    // Transition UNDEFINED → GENERAL and clear to black.
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

        VkImageSubresourceRange subRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b1 = {};
        b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b1.srcAccessMask = 0;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b1.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b1.image = img.image;
        b1.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b1);

        VkClearColorValue clearBlack = {};
        vkCmdClearColorImage(tmpCmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

        VkImageMemoryBarrier b2 = {};
        b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b2.image = img.image;
        b2.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &b2);

        vkEndCommandBuffer(tmpCmd);
        VkFenceCreateInfo fenceCI = {};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));
        VkSubmitInfo submitI = {};
        submitI.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitI.commandBufferCount = 1;
        submitI.pCommandBuffers = &tmpCmd;
        vkQueueSubmit(vk.graphicsQueue, 1, &submitI, fence);
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk.device, fence, NULL);
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
    }
    return true;
}

static void VK_RT_FreeVolHistoryImage(vkReflBuffer_t &img)
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
    img.width = img.height = 0;
}

static void VK_RT_CreateVolHistoryImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocVolHistoryImage(vkRT.volHistory[i], width, height))
            common->Warning("VK RT Vol Temporal: failed to allocate history slot %d", i);
        vkRT.volHistoryValid[i] = false;
        memset(vkRT.volPrevInvViewProj[i], 0, sizeof(vkRT.volPrevInvViewProj[i]));
        vkRT.volReadView[i] = vkRT.volHistory[i].view;
    }
}

static void VK_RT_DestroyVolHistoryImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeVolHistoryImage(vkRT.volHistory[i]);
        vkRT.volHistoryValid[i] = false;
        // Fall back to raw volBuffer so composite doesn't reference freed history.
        vkRT.volReadView[i] = vkRT.volBuffer[i].view;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitVolTemporalPipeline
// ---------------------------------------------------------------------------

static void VK_RT_InitVolTemporalPipeline(void)
{
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
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.volTemporalDescLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 32; // alpha(4)+pad(12)+scissorOffset(8)+scissorExtent(8)

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.volTemporalDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.volTemporalPipelineLayout));

    // Reuse gi_temporal_resolve.comp.spv — identical rgba16f EMA logic.
    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/gi_temporal_resolve.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Vol Temporal: failed to load gi_temporal_resolve.comp.spv — temporal disabled");
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
    pipeCI.layout = vkRT.volTemporalPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.volTemporalPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * VK_MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.volTemporalDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.volTemporalDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.volTemporalDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.volTemporalDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.volTemporalDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Vol Temporal: EMA pipeline initialized (reusing gi_temporal_resolve.comp.spv)\n");
}

// ---------------------------------------------------------------------------
// Public entry points — vol temporal
// ---------------------------------------------------------------------------

void VK_RT_InitVolTemporal(void)
{
    VK_RT_InitVolTemporalPipeline();
    VK_RT_CreateVolHistoryImages(vk.swapchainExtent.width, vk.swapchainExtent.height);
}

void VK_RT_ShutdownVolTemporal(void)
{
    VK_RT_DestroyVolHistoryImages();
    if (vkRT.volTemporalPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.volTemporalPipeline, NULL);
        vkRT.volTemporalPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.volTemporalPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.volTemporalPipelineLayout, NULL);
        vkRT.volTemporalPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.volTemporalDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.volTemporalDescPool, NULL);
        vkRT.volTemporalDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.volTemporalDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.volTemporalDescLayout, NULL);
        vkRT.volTemporalDescLayout = VK_NULL_HANDLE;
    }
}

void VK_RT_ResizeVolTemporal(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyVolHistoryImages();
    VK_RT_CreateVolHistoryImages(width, height);
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRT.volTemporalDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.volHistoryValid[i] = false;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchTemporalResolveVol
// ---------------------------------------------------------------------------

void VK_RT_DispatchTemporalResolveVol(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;

    const int frameIdx = vk.currentFrame;

    if (!r_useRayTracing.GetBool() || !r_rtVol.GetBool() || !r_rtVolTemporal.GetBool())
    {
        if (vkRT.volBuffer[frameIdx].view != VK_NULL_HANDLE)
            vkRT.volReadView[frameIdx] = vkRT.volBuffer[frameIdx].view;
        return;
    }
    if (vkRT.volTemporalPipeline == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Vol Temporal: pipeline is NULL, skipping dispatch");
        return;
    }

    const VkRect2D dispatchRect = VK_RT_Vol_ComputeDispatchRect(viewDef);
    s_volTemporalDispatchRect[frameIdx] = dispatchRect;
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    vkReflBuffer_t &current = vkRT.volBuffer[frameIdx];
    vkReflBuffer_t &history = vkRT.volHistory[frameIdx];
    if (current.image == VK_NULL_HANDLE || history.image == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Vol Temporal: skip — images not ready (slot %d)\n", frameIdx);
        return;
    }

    // --- Camera-cut detection (same L-inf convention as GI temporal) ---
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

    float effectiveAlpha = 1.0f;
    if (vkRT.volHistoryValid[frameIdx])
    {
        float maxDiff = 0.0f;
        for (int i = 0; i < 16; i++)
        {
            float d = fabsf(invVP[i] - vkRT.volPrevInvViewProj[frameIdx][i]);
            if (d > maxDiff)
                maxDiff = d;
        }
        float cutThresh = Max(0.0f, r_rtAOTemporalCutThreshold.GetFloat());
        if (maxDiff <= cutThresh)
            effectiveAlpha = idMath::ClampFloat(0.0f, 1.0f, r_rtVolTemporalAlpha.GetFloat());
        else if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Vol Temporal: camera cut slot=%d maxDiff=%.4f — resetting history\n", frameIdx,
                           maxDiff);
    }
    memcpy(vkRT.volPrevInvViewProj[frameIdx], invVP, sizeof(invVP));
    vkRT.volHistoryValid[frameIdx] = true;

    // --- Update descriptor set ---
    if (vkRT.volTemporalDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount)
    {
        VkDescriptorImageInfo currInfo = {};
        currInfo.imageView = current.view;
        currInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo histInfo = {};
        histInfo.imageView = history.view;
        histInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = vkRT.volTemporalDescSets[frameIdx];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &currInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = vkRT.volTemporalDescSets[frameIdx];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &histInfo;

        vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
        vkRT.volTemporalDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // --- Push constants ---
    struct
    {
        float alpha;
        float pad[3];
        int32_t sx, sy, ex, ey;
    } pc;
    pc.alpha = effectiveAlpha;
    pc.pad[0] = pc.pad[1] = pc.pad[2] = 0.0f;
    pc.sx = (int32_t)dispatchRect.offset.x;
    pc.sy = (int32_t)dispatchRect.offset.y;
    pc.ex = (int32_t)dispatchRect.extent.width;
    pc.ey = (int32_t)dispatchRect.extent.height;

    // --- Dispatch ---
    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.volTemporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.volTemporalPipelineLayout, 0, 1,
                            &vkRT.volTemporalDescSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.volTemporalPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &pc);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol Temporal: dispatch slot=%d alpha=%.3f rect=(%d,%d %u,%u)\n", frameIdx, effectiveAlpha,
                       dispatchRect.offset.x, dispatchRect.offset.y, (unsigned int)dispatchRect.extent.width,
                       (unsigned int)dispatchRect.extent.height);

    // --- Barrier: volHistory compute write → fragment read (for composite) ---
    {
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &mb, 0, NULL, 0, NULL);
    }

    vkRT.volReadView[frameIdx] = history.view;
}

// ===========================================================================
// Volumetric bilateral filter (Phase 7.2 — step 10)
//
// Spatial Gaussian filter applied after temporal EMA.
// Reads volHistory (storage image), writes to volBlurred.
// CompositeVolumetrics reads volBlurred when this pass is active.
// ===========================================================================

static idCVar r_rtVolBilateral("r_rtVolBilateral", "1", CVAR_RENDERER | CVAR_BOOL,
                               "Enable spatial Gaussian filter on volumetric result (reduces grain).");

static idCVar r_rtVolBilateralSigma("r_rtVolBilateralSigma", "2.0", CVAR_RENDERER | CVAR_FLOAT,
                                    "Gaussian sigma in pixels for the vol spatial filter. "
                                    "Higher = smoother but softer. Radius = ceil(2*sigma); default 2.0 → 9×9 kernel.");

// ---------------------------------------------------------------------------
// VK_RT_InitVolBilateralPipeline
// Descriptor layout mirrors vol_bilateral.comp:
//   binding 0: STORAGE_IMAGE readonly  (volHistory / volIn)
//   binding 1: STORAGE_IMAGE           (volBlurred / volOut)
// Push constants: 32 bytes (offX,offY,extX,extY,screenW,screenH,sigma,pad)
// ---------------------------------------------------------------------------

static void VK_RT_InitVolBilateralPipeline(void)
{
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
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.volBilateralDescLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 32; // int[6] + float + float

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.volBilateralDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.volBilateralPipelineLayout));

    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/vol_bilateral.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Vol Bilateral: failed to load vol_bilateral.comp.spv — filter disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stageCI = {};
    stageCI.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCI.module = compMod;
    stageCI.pName = "main";

    VkComputePipelineCreateInfo pipeCI = {};
    pipeCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.stage = stageCI;
    pipeCI.layout = vkRT.volBilateralPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.volBilateralPipeline));
    vkDestroyShaderModule(vk.device, compMod, NULL);

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u * (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.volBilateralDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.volBilateralDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.volBilateralDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.volBilateralDescSets));

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.volBilateralDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Vol Bilateral: pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public entry points — vol bilateral
// ---------------------------------------------------------------------------

void VK_RT_InitVolBilateral(void)
{
    VK_RT_InitVolBilateralPipeline();
    // Allocate blurred output images at current swapchain size.
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocVolHistoryImage(vkRT.volBlurred[i], vk.swapchainExtent.width, vk.swapchainExtent.height))
            common->Warning("VK RT Vol Bilateral: failed to allocate volBlurred slot %d", i);
    }
}

void VK_RT_ShutdownVolBilateral(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        VK_RT_FreeVolHistoryImage(vkRT.volBlurred[i]);

    if (vkRT.volBilateralPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.volBilateralPipeline, NULL);
        vkRT.volBilateralPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.volBilateralPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.volBilateralPipelineLayout, NULL);
        vkRT.volBilateralPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.volBilateralDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.volBilateralDescPool, NULL);
        vkRT.volBilateralDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.volBilateralDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.volBilateralDescLayout, NULL);
        vkRT.volBilateralDescLayout = VK_NULL_HANDLE;
    }
}

void VK_RT_ResizeVolBilateral(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeVolHistoryImage(vkRT.volBlurred[i]);
        VK_RT_AllocVolHistoryImage(vkRT.volBlurred[i], width, height);
        vkRT.volBilateralDescSetLastUpdatedFrameCount[i] = -1;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolBilateral
// ---------------------------------------------------------------------------

void VK_RT_DispatchVolBilateral(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtVol.GetBool() || !r_rtVolBilateral.GetBool())
        return;
    if (vkRT.volBilateralPipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;

    vkReflBuffer_t &histImg = vkRT.volHistory[frameIdx];
    vkReflBuffer_t &blurredImg = vkRT.volBlurred[frameIdx];

    if (histImg.image == VK_NULL_HANDLE || blurredImg.image == VK_NULL_HANDLE)
        return;

    // Compute dispatch rect (same helper as temporal).
    const VkRect2D dispatchRect = VK_RT_Vol_ComputeDispatchRect(viewDef);
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    // Barrier: ensure temporal compute write to volHistory is visible to our compute read.
    {
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, NULL, 0, NULL);
    }

    // Update descriptor set when resources change.
    if (vkRT.volBilateralDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount)
    {
        VkDescriptorImageInfo inInfo = {};
        inInfo.imageView = histImg.view;
        inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo outInfo = {};
        outInfo.imageView = blurredImg.view;
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = vkRT.volBilateralDescSets[frameIdx];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &inInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = vkRT.volBilateralDescSets[frameIdx];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &outInfo;

        vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
        vkRT.volBilateralDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // Push constants.
    struct BilateralPC
    {
        int32_t offX, offY, extX, extY;
        int32_t screenW, screenH;
        float sigma;
        float pad;
    } pc;
    pc.offX = (int32_t)dispatchRect.offset.x;
    pc.offY = (int32_t)dispatchRect.offset.y;
    pc.extX = (int32_t)dispatchRect.extent.width;
    pc.extY = (int32_t)dispatchRect.extent.height;
    pc.screenW = (int32_t)vk.swapchainExtent.width;
    pc.screenH = (int32_t)vk.swapchainExtent.height;
    pc.sigma = idMath::ClampFloat(0.5f, 8.0f, r_rtVolBilateralSigma.GetFloat());
    pc.pad = 0.0f;

    // Dispatch.
    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.volBilateralPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.volBilateralPipelineLayout, 0, 1,
                            &vkRT.volBilateralDescSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.volBilateralPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &pc);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Barrier: bilateral write → fragment read (for composite).
    {
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &mb, 0, NULL, 0, NULL);
    }

    // Composite now reads the filtered result.
    vkRT.volReadView[frameIdx] = blurredImg.view;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol Bilateral: dispatch slot=%d rect=(%d,%d %u,%u)\n", frameIdx, pc.offX, pc.offY,
                       (unsigned)pc.extX, (unsigned)pc.extY);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VK_RT_InitVolumetrics(void)
{
    VK_RT_InitVolMarchPipeline();
    VK_RT_InitVolCompositePipeline();
    VK_RT_ResizeVolumetrics(vk.swapchainExtent.width, vk.swapchainExtent.height);
    VK_RT_InitVolTemporal();
    VK_RT_InitVolBilateral();
}

void VK_RT_ShutdownVolumetrics(void)
{
    VK_RT_ShutdownVolBilateral();
    VK_RT_ShutdownVolTemporal();

    if (vkRT.volMarchDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.volMarchDescPool, NULL);
        vkRT.volMarchDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.volMarchDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.volMarchDescLayout, NULL);
        vkRT.volMarchDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.volMarchPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.volMarchPipeline, NULL);
        vkRT.volMarchPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.volMarchPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.volMarchPipelineLayout, NULL);
        vkRT.volMarchPipelineLayout = VK_NULL_HANDLE;
    }

    if (vkRT.volCompositeDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.volCompositeDescPool, NULL);
        vkRT.volCompositeDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.volCompositeDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.volCompositeDescLayout, NULL);
        vkRT.volCompositeDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.volCompositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.volCompositePipeline, NULL);
        vkRT.volCompositePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.volCompositeLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.volCompositeLayout, NULL);
        vkRT.volCompositeLayout = VK_NULL_HANDLE;
    }
    if (vkRT.volSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.volSampler, NULL);
        vkRT.volSampler = VK_NULL_HANDLE;
    }

    VK_RT_DestroyVolImages();
}

void VK_RT_ResizeVolumetrics(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyVolImages();
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.volMarchDescSetLastUpdatedFrameCount[i] = -1;
    VK_RT_CreateVolImages(width, height);
    if (vkRT.volTemporalPipeline != VK_NULL_HANDLE)
        VK_RT_ResizeVolTemporal(width, height);
    if (vkRT.volBilateralPipeline != VK_NULL_HANDLE)
        VK_RT_ResizeVolBilateral(width, height);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolumetrics (public)
// Runs vol_march.comp to accumulate in-scattering for the current view.
// Must be called outside a render pass; depth must be in ATTACHMENT_OPTIMAL.
// On exit volBuffer[currentFrame] is in GENERAL, readable by fragment shaders.
// ---------------------------------------------------------------------------

void VK_RT_DispatchVolumetrics(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtVol.GetBool())
        return;
    if (vkRT.volMarchPipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;

    static int s_lastVolDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastVolDispatchFrame[frameIdx] == tr.frameCount)
        return;
    s_lastVolDispatchFrame[frameIdx] = tr.frameCount;

    vkReflBuffer_t &vb = vkRT.volBuffer[frameIdx];
    if (vb.image == VK_NULL_HANDLE)
        return;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol: dispatch frame=%d slot=%d size=%ux%u\n", tr.frameCount, frameIdx, vb.width,
                       vb.height);

    // --- Depth aspect flags ---
    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    // --- Depth barrier: ATTACHMENT → READ_ONLY for compute shader sampling ---
    {
        VkImageMemoryBarrier depthToRead = {};
        depthToRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthToRead.srcAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthToRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthToRead.image = vk.depthImage;
        depthToRead.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Build UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    VolParamsUBO ubo = {};

    // Compute invViewProj (same as GI dispatch).
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
        idMat4 invVP = vpMat.Inverse();
        memcpy(ubo.invViewProj, invVP.ToFloatPtr(), 16 * sizeof(float));
    }

    // Guard against NaN (singular VP on degenerate frame).
    {
        bool hasNaN = false;
        for (int i = 0; i < 16; i++)
            if (ubo.invViewProj[i] != ubo.invViewProj[i])
            {
                hasNaN = true;
                break;
            }
        if (hasNaN)
        {
            common->Warning("VK RT Vol: invViewProj NaN — skipping dispatch");
            VkImageMemoryBarrier restore = {};
            restore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            restore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            restore.dstAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            restore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            restore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            restore.image = vk.depthImage;
            restore.subresourceRange = {depthAspect, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 0, NULL, 0, NULL, 1, &restore);
            return;
        }
    }

    const idVec3 camPos = viewDef->renderView.vieworg;
    ubo.cameraPosX = camPos.x;
    ubo.cameraPosY = camPos.y;
    ubo.cameraPosZ = camPos.z;
    ubo.cameraPad = 0.0f;
    ubo.frameIndex = (uint32_t)tr.frameCount;
    ubo.numSamples = idMath::ClampInt(1, 32, r_rtVolSamples.GetInteger());
    ubo.maxLights = idMath::ClampInt(1, 128, r_rtVolMaxLights.GetInteger());
    ubo.density = idMath::ClampFloat(0.0f, 1.0f, r_rtVolDensity.GetFloat());
    ubo.anisotropy = idMath::ClampFloat(0.0f, 0.99f, r_rtVolAnisotropy.GetFloat());
    ubo.maxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
    ubo.strength = idMath::ClampFloat(0.0f, 8.0f, r_rtVolStrength.GetFloat());
    ubo.flashlightDensity = idMath::ClampFloat(0.0f, 1.0f, r_rtVolFlashlightDensity.GetFloat());
    ubo.flashlightAnisotropy = idMath::ClampFloat(0.0f, 0.99f, r_rtVolFlashlightAnisotropy.GetFloat());
    ubo.flashlightStrength = idMath::ClampFloat(0.0f, 8.0f, r_rtVolFlashlightStrength.GetFloat());
    ubo.directedDensity = idMath::ClampFloat(0.0f, 1.0f, r_rtVolDirectedDensity.GetFloat());
    ubo.directedAnisotropy = idMath::ClampFloat(0.0f, 0.99f, r_rtVolDirectedAnisotropy.GetFloat());
    ubo.directedStrength = idMath::ClampFloat(0.0f, 8.0f, r_rtVolDirectedStrength.GetFloat());
    ubo._uboPad = 0.0f;

    // Scissor rect (GL Y-up → Vulkan Y-down, same conversion as GI).
    const int w = (int)vk.swapchainExtent.width;
    const int h = (int)vk.swapchainExtent.height;
    {
        const idScreenRect &s = viewDef->scissor;
        int offX = idMath::ClampInt(0, w - 1, s.x1);
        int offY = idMath::ClampInt(0, h - 1, h - 1 - s.y2);
        int rw = idMath::ClampInt(1, w - offX, s.x2 - s.x1 + 1);
        int rh = idMath::ClampInt(1, h - offY, s.y2 - s.y1 + 1);
        ubo.scissorOffsetX = offX;
        ubo.scissorOffsetY = offY;
        ubo.scissorExtentX = rw;
        ubo.scissorExtentY = rh;
    }
    ubo.screenWidth = (int32_t)vb.width;
    ubo.screenHeight = (int32_t)vb.height;

    memcpy(uboMapped, &ubo, sizeof(VolParamsUBO));

    // --- Update descriptor set (once per frame slot when resources change) ---
    static VkAccelerationStructureKHR s_lastTlas[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastVolView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastDepthView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool resourceChanged = (s_lastTlas[frameIdx] != vkRT.tlas[frameIdx].handle) ||
                                 (s_lastVolView[frameIdx] != vb.view) ||
                                 (s_lastDepthView[frameIdx] != vk.depthSampledView);

    if (vkRT.volMarchDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount || resourceChanged)
    {
        VkDescriptorSet ds = vkRT.volMarchDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo volImgInfo = {};
        volImgInfo.imageView = vb.view;
        volImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(VolParamsUBO);

        VkDescriptorBufferInfo lightSsboInfo = {};
        lightSsboInfo.buffer = vkRT.giLightSsbo[frameIdx];
        lightSsboInfo.offset = 0;
        lightSsboInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[5] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &tlasWrite;
        writes[0].dstSet = ds;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = ds;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &volImgInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = ds;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &depthInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = ds;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[3].pBufferInfo = &uboInfo;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = ds;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &lightSsboInfo;

        vkUpdateDescriptorSets(vk.device, 5, writes, 0, NULL);
        vkRT.volMarchDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastTlas[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastVolView[frameIdx] = vb.view;
        s_lastDepthView[frameIdx] = vk.depthSampledView;
    }

    // --- Compute dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.volMarchPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.volMarchPipelineLayout, 0, 1,
                            &vkRT.volMarchDescSets[frameIdx], 1, &uboOff);

    uint32_t groupsX = ((uint32_t)ubo.scissorExtentX + 7) / 8;
    uint32_t groupsY = ((uint32_t)ubo.scissorExtentY + 7) / 8;
    if (groupsX > 0 && groupsY > 0)
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // --- Barrier: volBuf compute write → compute read (temporal) + fragment read (composite) ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL ---
    {
        VkImageMemoryBarrier depthRestore = {};
        depthRestore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthRestore.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthRestore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRestore.image = vk.depthImage;
        depthRestore.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, NULL, 0, NULL, 1, &depthRestore);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol: dispatch complete groups=%ux%u density=%.3f samples=%d\n", groupsX, groupsY,
                       ubo.density, ubo.numSamples);
}

// ---------------------------------------------------------------------------
// VK_RT_CompositeVolumetrics (public)
// Additively blends volBuffer onto the framebuffer using a fullscreen triangle.
// Must be called inside the render pass, after VK_RT_CompositeGI.
// ---------------------------------------------------------------------------

void VK_RT_CompositeVolumetrics(VkCommandBuffer cmd)
{
    if (!r_useRayTracing.GetBool() || !r_rtVol.GetBool())
        return;
    if (vkRT.volCompositePipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;
    vkReflBuffer_t &vb = vkRT.volBuffer[frameIdx];
    if (vb.image == VK_NULL_HANDLE || vkRT.volSampler == VK_NULL_HANDLE)
        return;

    // Write the descriptor set for this frame slot.
    // Use volReadView: points to volHistory when temporal is active, volBuffer otherwise.
    VkImageView readView = vkRT.volReadView[frameIdx];
    if (readView == VK_NULL_HANDLE)
        readView = vb.view;

    VkDescriptorImageInfo imgInfo = {};
    imgInfo.sampler = vkRT.volSampler;
    imgInfo.imageView = readView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vkRT.volCompositeDescSets[frameIdx];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.volCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.volCompositeLayout, 0, 1,
                            &vkRT.volCompositeDescSets[frameIdx], 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol: composite drawn frame=%d slot=%d\n", tr.frameCount, frameIdx);
}
