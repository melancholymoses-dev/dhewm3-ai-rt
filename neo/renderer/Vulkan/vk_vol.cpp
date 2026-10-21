/*
===========================================================================

dhewm3-rt Vulkan — vk_vol.cpp — Volumetric medium parameters and composite.

Owns the single-scatter medium coefficients (extinction, albedo, per-class
radiance gains and phase parameters), the screen-sized `volBuffer` the
in-scattering lands in, and the fullscreen pass that blends it onto the frame.
The integrator itself is the froxel grid in vk_vol_froxel.cpp, which is the only
thing that writes volBuffer.

Dispatch order in the RT frame (outside render pass):
  ... VK_RT_DispatchGIAlbedoMod ...
  VK_RT_DispatchVolFroxel{Fill,Integrate,Resolve}  ← vk_vol_froxel.cpp
  (resume render pass)
  VK_RT_CompositeGI
  VK_RT_CompositeVolumetrics ← this file (inside render pass, additive)

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
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtVol("r_rtVol", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
               "Enable volumetric light scattering.");

// NOT static, from here down through the flashlight block: vk_vol_froxel.cpp externs
// every medium/phase/strength knob so the froxel fill reads them directly.

// IGN jitter is spatially coherent (beams don't read as noise) but stripes diagonally;
// a small white-noise mix breaks the pattern.  Keep it small.
idCVar r_rtVolWhiteNoiseMix("r_rtVolWhiteNoiseMix", "0.025", CVAR_RENDERER | CVAR_FLOAT,
                            "Fraction of white noise blended into the IGN sampling jitter to soften visible "
                            "dither stripes/fringes (0 = pure IGN, 1 = pure white noise).");

idCVar r_rtVolMaxDist("r_rtVolMaxDist", "512.0", CVAR_RENDERER | CVAR_FLOAT,
                      "Far cap on the volumetric medium's reach in world units.  Also bounds the froxel grid "
                      "and the vol light selection.");

idCVar r_rtVolMaxLights("r_rtVolMaxLights", "96", CVAR_RENDERER | CVAR_INTEGER,
                        "Max lights in the dedicated volumetric light selection (separate from "
                        "GI's own light buffer/cap).");

// --- Medium coefficients ---------------------------------------------------
// Single-scatter transport has exactly two: extinction (drives transmittance) and
// scattering (drives in-scatter), constrained by albedo = sigma_s/sigma_t <= 1.

idCVar r_rtVolExtinction("r_rtVolExtinction", "0.0006", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                         "sigma_t, the medium's extinction coefficient per world unit.  Sets how far you "
                         "can see: transmittance = exp(-sigma_t * distance), so 5e-4 gives 0.78 at 500 "
                         "units.  Medium-wide — extinction is not a property of any one light.");

idCVar r_rtVolAlbedo("r_rtVolAlbedo", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                     "Single-scatter albedo: sigma_s = albedo * sigma_t.  0 = pure absorber (medium "
                     "darkens without glowing), 1 = pure scatterer (bright mist, no net darkening).  "
                     "Values above 1 would create energy and are clamped.");

// Per-class gains scale the light's radiance, not the medium: Doom 3 intensities are
// authored, not radiometric, so this leaves the transport equation alone.
idCVar r_rtVolGain("r_rtVolGain", "25.0", CVAR_RENDERER | CVAR_FLOAT,
                   "Radiance gain for point-light in-scatter (compensates Doom 3's non-radiometric "
                   "light intensities).  Brightness only — does not affect transmittance.");

// 1 is physically correct with a real sigma_t; 0 (additive only) makes fog create light
// rather than redistribute it.
static idCVar r_rtVolAttenuateBackground(
    "r_rtVolAttenuateBackground", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
    "Multiply the scene by the volumetric path transmittance as well as adding "
    "in-scattering (1), or add only (0).  When 1 the composite also moves after the "
    "interaction/shader passes so direct light is attenuated too.");

idCVar r_rtVolAnisotropy("r_rtVolAnisotropy", "0.4", CVAR_RENDERER | CVAR_FLOAT,
                         "Henyey-Greenstein g parameter (0=isotropic, 0.8=flashlight shaft)");

// HG is normalised, so one lobe trades side-on visibility for forward contrast (25x at
// g=0.6).  Global rather than per-class: a visibility floor, not a medium property.
idCVar r_rtVolIsotropicMix("r_rtVolIsotropicMix", "0.45", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                           "Fraction of the volumetric phase function that is isotropic rather than the "
                           "Henyey-Greenstein forward lobe (0 = pure HG, 1 = fully isotropic).  Raises "
                           "side-on/high-angle shaft visibility without lowering r_rtVol*Anisotropy.");

// Scene directed/spot lights (lightType 1) — separate from the player's flashlight.
idCVar r_rtVolDirectedGain("r_rtVolDirectedGain", "70.0", CVAR_RENDERER | CVAR_FLOAT,
                           "Radiance gain for scene directed/spot in-scatter.  Brightness only.");
idCVar r_rtVolDirectedAnisotropy("r_rtVolDirectedAnisotropy", "0.6", CVAR_RENDERER | CVAR_FLOAT,
                                 "Henyey-Greenstein g for scene spot lights (0=iso, 1=full forward).");

// Player flashlight (lightType 2, allowLightInViewID set).
idCVar r_rtVolFlashlightGain("r_rtVolFlashlightGain", "50.0", CVAR_RENDERER | CVAR_FLOAT,
                             "Radiance gain for player-flashlight in-scatter.  Brightness only.");
idCVar r_rtVolFlashlightAnisotropy("r_rtVolFlashlightAnisotropy", "0.7", CVAR_RENDERER | CVAR_FLOAT,
                                   "Henyey-Greenstein g parameter for the flashlight (0=isotropic, 1=full forward).");

// ---------------------------------------------------------------------------
// Medium coefficient helpers.  Defined here rather than in vk_vol_froxel.cpp so the
// composite and the integrator read one source for the same constants.
// ---------------------------------------------------------------------------

bool VK_RT_VolCompositeAfterSurfaces(void)
{
    return r_rtVolAttenuateBackground.GetBool() && vkRT.volCompositeAttenPipeline != VK_NULL_HANDLE;
}

float VK_RT_VolExtinction(void)
{
    return idMath::ClampFloat(0.0f, 1.0f, r_rtVolExtinction.GetFloat());
}

float VK_RT_VolAlbedo(void)
{
    return idMath::ClampFloat(0.0f, 1.0f, r_rtVolAlbedo.GetFloat());
}

// sigma_s * gain for one light class — the single number each integrator needs to
// turn its accumulated (phase * falloff * visibility * dt * T) sum into radiance.
// Folded here rather than in the shader so the per-class density/strength pair
// becomes one uniform slot and one multiply.
float VK_RT_VolScatterScale(int lightClass)
{
    const float sigmaS = VK_RT_VolExtinction() * VK_RT_VolAlbedo();
    float gain;
    switch (lightClass)
    {
    case VK_VOL_CLASS_DIRECTED:
        gain = r_rtVolDirectedGain.GetFloat();
        break;
    case VK_VOL_CLASS_FLASHLIGHT:
        gain = r_rtVolFlashlightGain.GetFloat();
        break;
    default:
        gain = r_rtVolGain.GetFloat();
        break;
    }
    return sigmaS * Max(0.0f, gain);
}

// ---------------------------------------------------------------------------
// Forward declarations from other vk_*.cpp modules
// ---------------------------------------------------------------------------

extern VkShaderModule VK_LoadSPIRV(const char *path);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

static void VK_RT_CreateVolImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &vb = vkRT.volBuffer[i];
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
            VK_Prof_WaitForFences(VKPROF_WAIT_VOL, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk.device, fence, NULL);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
        // Default read view; the froxel resolve re-claims it each frame.
        vkRT.volReadView[i] = vkRT.volBuffer[i].view;
    }
}

static void VK_RT_DestroyVolImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &vb = vkRT.volBuffer[i];
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
    // The fragment shader takes the screen size by push constant rather than
    // textureSize(): the vol image is written identity-mapped into the render
    // sub-rect, which is not necessarily the whole image.
    VkPushConstantRange compPush = {};
    compPush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compPush.offset = 0;
    // vec2 invScreenSize + int debugMode + float debugGain
    compPush.size = sizeof(float) * 2 + sizeof(int32_t) + sizeof(float);

    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.volCompositeDescLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &compPush;
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

    // Attachments 1-3 (gbufNormal, gbufAlbedo, motionVectors) are written only by the
    // G-buffer prepass; every other pipeline on vk.hdrRenderPass supplies write-mask-0
    // fillers so the subpass's per-attachment blend-state count always matches.
    VkPipelineColorBlendAttachmentState blendAttachments[VK_HDR_COLOR_ATTACHMENT_COUNT] = {colorBlend};
    const uint32_t blendCount = VK_FillHdrBlendAttachments(blendAttachments);

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = blendCount;
    blendState.pAttachments = blendAttachments;

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
    pipelineInfo.renderPass = vk.hdrRenderPass;
    pipelineInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.volCompositePipeline));

    // --- Attenuating variant: dst = src.rgb + dst * src.a.  Only dstColorBlendFactor
    // differs, so the toggle swaps handles.  Alpha is untouched: not ours to write. ---
    VkPipelineColorBlendAttachmentState attenBlend = colorBlend;
    attenBlend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

    VkPipelineColorBlendAttachmentState attenAttachments[VK_HDR_COLOR_ATTACHMENT_COUNT] = {attenBlend};
    VK_FillHdrBlendAttachments(attenAttachments);

    VkPipelineColorBlendStateCreateInfo attenBlendState = blendState;
    attenBlendState.pAttachments = attenAttachments;
    pipelineInfo.pColorBlendState = &attenBlendState;

    VK_CHECK(
        vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.volCompositeAttenPipeline));

    // --- Debug variant: blend disabled (replace), so a froxel overlay isn't muddied
    // by additive blending onto the already-lit scene.  Mirrors
    // reflCompositeDebugPipeline in vk_reflections.cpp. ---
    VkPipelineColorBlendAttachmentState replaceBlend = {};
    replaceBlend.blendEnable = VK_FALSE;
    replaceBlend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState replaceAttachments[VK_HDR_COLOR_ATTACHMENT_COUNT] = {replaceBlend};
    VK_FillHdrBlendAttachments(replaceAttachments);

    VkPipelineColorBlendStateCreateInfo replaceBlendState = {};
    replaceBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    replaceBlendState.attachmentCount = blendCount;
    replaceBlendState.pAttachments = replaceAttachments;
    pipelineInfo.pColorBlendState = &replaceBlendState;

    VK_CHECK(
        vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.volCompositeDebugPipeline));

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

    // Bilinear-clamp sampler.  Created here because this is now the first vol pass
    // to initialise; the froxel resolve assumes it already exists.
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

    common->Printf("VK RT Vol: composite pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VK_RT_InitVolumetrics(void)
{
    VK_RT_InitVolCompositePipeline();
    VK_RT_ResizeVolumetrics(vk.swapchainExtent.width, vk.swapchainExtent.height);
    // Froxel grid.  Last, so it can assume
    // volSampler and volBuffer exist; its own images are screen-size independent
    // and so take no part in VK_RT_ResizeVolumetrics.
    VK_RT_InitVolFroxel();
}

void VK_RT_ShutdownVolumetrics(void)
{
    VK_RT_ShutdownVolFroxel();

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
    if (vkRT.volCompositeAttenPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.volCompositeAttenPipeline, NULL);
        vkRT.volCompositeAttenPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.volCompositeDebugPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.volCompositeDebugPipeline, NULL);
        vkRT.volCompositeDebugPipeline = VK_NULL_HANDLE;
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
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_VOL);

    VK_RT_DestroyVolImages();
    VK_RT_CreateVolImages(width, height);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol: resize %ux%u\n", width, height);
}

// ---------------------------------------------------------------------------
// VK_RT_CompositeVolumetrics (public)
// Blends volBuffer onto the framebuffer using a fullscreen triangle.  Must be
// called inside the render pass, after VK_RT_CompositeGI.  Additive by default;
// with r_rtVolAttenuateBackground it also multiplies the scene by src.a and the
// backend calls it from the later, post-shader-passes site instead.
// ---------------------------------------------------------------------------

// Double-composite guard for the two call sites (see below).
static int s_volCompositeFrame = -1;
static const void *s_volCompositeView = NULL;

void VK_RT_CompositeVolumetrics(VkCommandBuffer cmd)
{
    if (!r_useRayTracing.GetBool() || !r_rtVol.GetBool())
        return;
    if (vkRT.volCompositePipeline == VK_NULL_HANDLE)
        return;

    // The froxel resolve is the only writer of volBuffer.  If it stood down, the buffer is
    // black or stale and the attenuating pipeline would blank the screen.
    if (!VK_RT_VolFroxelActive())
        return;

    const int frameIdx = vk.currentFrame;
    vkRTImage_t &vb = vkRT.volBuffer[frameIdx];
    if (vb.image == VK_NULL_HANDLE || vkRT.volSampler == VK_NULL_HANDLE)
        return;

    // Two call sites in VK_RB_DrawView (before the interactions, or after the shader passes
    // when attenuating).  Keyed on the view, since a frame can legitimately draw several.
    if (s_volCompositeFrame == tr.frameCount && s_volCompositeView == (const void *)backEnd.viewDef)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Vol: composite skipped — already drawn this view (frame=%d)\n", tr.frameCount);
        return;
    }

    // Write the descriptor set for this frame slot.  volReadView is claimed by the
    // froxel resolve; volBuffer is the fallback if it never ran.
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

    // Display extent, not renderExtent: the vol image is display-sized and written
    // identity-mapped into the render sub-rect, so fragCoord / displayExtent lands
    // on the right texel.
    struct
    {
        float invScreen[2];
        int32_t debugMode;
    } pc = {{1.0f / Max(1.0f, (float)vk.swapchainExtent.width), 1.0f / Max(1.0f, (float)vk.swapchainExtent.height)}, 0};

    // Froxel overlays arrive already composed in volBuffer.rgb, so the fragment
    // shader's mode 2 — "show rgb as-is under the replace pipeline" — is the right
    // display path.
    if (VK_RT_VolFroxelDebugMode() != 0)
        pc.debugMode = 2;

    // Debug modes win over the attenuating pipeline: a visualization wants the
    // replace blend, not a multiply against the scene.
    const bool debugActive = pc.debugMode >= 1 && pc.debugMode <= 2;
    VkPipeline pipe = vkRT.volCompositePipeline;
    if (debugActive)
        pipe = vkRT.volCompositeDebugPipeline;
    else if (VK_RT_VolCompositeAfterSurfaces())
        pipe = vkRT.volCompositeAttenPipeline;
    if (pipe == VK_NULL_HANDLE)
        return;

    s_volCompositeFrame = tr.frameCount;
    s_volCompositeView = (const void *)backEnd.viewDef;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.volCompositeLayout, 0, 1,
                            &vkRT.volCompositeDescSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.volCompositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Vol: composite drawn frame=%d slot=%d debugMode=%d\n", tr.frameCount, frameIdx,
                       pc.debugMode);
}
