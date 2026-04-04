/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan ray tracing - environment reflection ray pipeline and dispatch.

Phase 5.3 "cheap approximation":
  Traces one mirror-reflect ray per non-sky pixel.  The closest-hit and
  miss shaders return a direction-based colour tint (no texture/material
  lookup) so metallic/shiny surfaces get plausible environment reflections
  at minimal GPU cost.

The result is stored in an RGBA16F buffer that is sampled by the lighting
interaction fragment shader and blended into the specular contribution,
weighted by the specular map value.

Known limitation: the interaction shader is called once per light, so the
reflection contribution is accumulated N times for N lights illuminating a
pixel.  r_rtReflectionBlend (default 0.1) keeps the result visually
reasonable.  Moving reflections to a dedicated post-process pass is the
correct fix (planned for a later step).

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

// r_rtReflections declared in RenderSystem_init.cpp
extern idCVar r_rtReflections;

static idCVar r_rtReflectionDistance("r_rtReflectionDistance", "2000.0", CVAR_RENDERER | CVAR_FLOAT,
                                     "Max reflection ray travel distance in world units (default 2000)");

static idCVar r_rtReflectionBlend("r_rtReflectionBlend", "0.5", CVAR_RENDERER | CVAR_FLOAT,
                                  "Scale factor for reflection contribution in the interaction shader.\n"
                                  "Lower values compensate for the multi-light accumulation artifact.\n"
                                  "Default 0.5 — decrease if reflections look overbright in heavy-light areas.");

// ---------------------------------------------------------------------------
// UBO layout matching reflect_ray.rgen ReflParams block
//
//   mat4   invViewProj  offset  0  size 64
//   float  maxDist      offset 64  size  4
//   uint   frameIndex   offset 68  size  4
//   ivec2  screenSize   offset 72  size  8  (ivec2 std140 align=8 → 72 is fine)
//   float  reflBlend    offset 80  size  4  (r_rtReflectionBlend, applied at imageStore)
//   pad                 offset 84  size 12
//   total: 96 bytes
// ---------------------------------------------------------------------------

struct ReflParamsUBO
{
    float    invViewProj[16];
    float    maxDist;
    uint32_t frameIndex;
    int32_t  screenWidth;
    int32_t  screenHeight;
    float    reflBlend;   // r_rtReflectionBlend — baked into imageStore in rgen
    float    pad[3];
};
static_assert(sizeof(ReflParamsUBO) == 96, "ReflParamsUBO size mismatch");

// ---------------------------------------------------------------------------
// Forward declarations (defined in other files)
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// VK_RT_CreateReflImages
// Allocates per-frame RGBA16F reflection buffers at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateReflImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &rb = vkRT.reflBuffer[i];
        rb.width = width;
        rb.height = height;

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
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &rb.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, rb.image, &memReq);

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
            common->Error("VK RT Refl: no device-local memory type for reflection image");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &rb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, rb.image, rb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = rb.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &rb.view));

        // Transition UNDEFINED → GENERAL so rgen can imageStore on first dispatch
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
            barrier.image = rb.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &barrier);

            vkEndCommandBuffer(tmpCmd);

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &tmpCmd;
            vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(vk.graphicsQueue);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }
}

static void VK_RT_DestroyReflImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &rb = vkRT.reflBuffer[i];
        if (rb.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, rb.view, NULL);
            rb.view = VK_NULL_HANDLE;
        }
        if (rb.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, rb.image, NULL);
            rb.image = VK_NULL_HANDLE;
        }
        if (rb.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, rb.memory, NULL);
            rb.memory = VK_NULL_HANDLE;
        }
        rb.width = 0;
        rb.height = 0;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflPipeline
// Creates the reflection RT pipeline, SBT, descriptor sets, and sampler.
// ---------------------------------------------------------------------------

static void VK_RT_InitReflPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: TLAS
    // binding 1: reflection RGBA16F storage image
    // binding 2: depth sampler (COMBINED_IMAGE_SAMPLER)
    // binding 3: reflection params UBO (dynamic)

    VkDescriptorSetLayoutBinding bindings[4] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.reflDescLayout));

    // --- Pipeline layout ---
    // set=0: per-frame resources (TLAS, storage image, depth, UBO)
    // set=1: material table (MatTable, VtxAddrTable, IdxAddrTable, bindless textures)
    VkDescriptorSetLayout reflLayouts[2] = {vkRT.reflDescLayout, vkRT.matDescLayout};
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts = reflLayouts;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.reflPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rgenModule  = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rgen.spv");
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rmiss.spv");
    VkShaderModule rchitModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rchit.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rahit.spv");

    if (rgenModule == VK_NULL_HANDLE || rmissModule == VK_NULL_HANDLE ||
        rchitModule == VK_NULL_HANDLE || rahitModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Refl: failed to load reflection shader modules — reflections disabled");
        if (rgenModule  != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rgenModule,  NULL);
        if (rmissModule != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rmissModule, NULL);
        if (rchitModule != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rchitModule, NULL);
        if (rahitModule != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rahitModule, NULL);
        return;
    }

    // --- Shader stages ---
    // Stage 0: rgen  Stage 1: rmiss  Stage 2: rchit  Stage 3: rahit
    VkPipelineShaderStageCreateInfo stages[4] = {};

    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName  = "main";

    stages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage  = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName  = "main";

    stages[3].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage  = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[3].module = rahitModule;
    stages[3].pName  = "main";

    // --- Shader groups ---
    // Group 0: ray gen
    // Group 1: miss
    // Group 2: triangles hit group — closest-hit shades the surface,
    //          any-hit alpha-discards transparent (perforated) geometry.
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    groups[0].sType            = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader    = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader     = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType            = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader    = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader     = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType            = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader    = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = 2; // rchit
    groups[2].anyHitShader     = 3; // rahit — alpha-discard for perforated geometry
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount                   = 4;
    rtPipeInfo.pStages                      = stages;
    rtPipeInfo.groupCount                   = 3;
    rtPipeInfo.pGroups                      = groups;
    rtPipeInfo.maxPipelineRayRecursionDepth = 1; // single-bounce reflection
    rtPipeInfo.layout                       = vkRT.reflPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.reflPipeline));

    vkDestroyShaderModule(vk.device, rgenModule,  NULL);
    vkDestroyShaderModule(vk.device, rmissModule, NULL);
    vkDestroyShaderModule(vk.device, rchitModule, NULL);
    vkDestroyShaderModule(vk.device, rahitModule, NULL);

    // --- Shader Binding Table ---
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(vk.physicalDevice, &props2);

    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    const uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;

    auto alignUp = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
    uint32_t handleSizeAligned = alignUp(handleSize, handleAlignment);
    uint32_t stride = alignUp(handleSizeAligned, baseAlignment);
    uint32_t sbtSize = 3 * stride;

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vkRT.sbtReflBuffer,
                    &vkRT.sbtReflMemory);

    uint8_t *handles = (uint8_t *)alloca(3 * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.reflPipeline, 0, 3, 3 * handleSize, handles));

    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtReflMemory, 0, sbtSize, 0, (void **)&sbtData));
    for (int i = 0; i < 3; i++)
        memcpy(sbtData + i * stride, handles + i * handleSize, handleSize);
    vkUnmapMemory(vk.device, vkRT.sbtReflMemory);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtReflBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    vkRT.reflRgenRegion = {sbtBase + 0 * stride, stride, stride};
    vkRT.reflMissRegion = {sbtBase + 1 * stride, stride, stride};
    vkRT.reflHitRegion = {sbtBase + 2 * stride, stride, stride};
    vkRT.reflCallRegion = {0, 0, 0};

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl SBT: stride=%u sbtBytes=%u base=0x%llx\n", stride, sbtSize,
                       (unsigned long long)sbtBase);

    // --- Descriptor pool and sets ---
    VkDescriptorPoolSize poolSizes[4] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.reflDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.reflDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.reflDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.reflDescSetLastUpdatedFrameCount[i] = -1;

    // --- Reflection buffer sampler (linear-clamp, used by interaction shader) ---
    if (vkRT.reflSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.reflSampler));
    }

    common->Printf("VK RT Refl: pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflections (public entry)
// ---------------------------------------------------------------------------

void VK_RT_InitReflections(void)
{
    VK_RT_InitReflPipeline();
    VK_RT_ResizeReflections(vk.swapchainExtent.width, vk.swapchainExtent.height);
}

// ---------------------------------------------------------------------------
// VK_RT_ShutdownReflections (public)
// ---------------------------------------------------------------------------

void VK_RT_ShutdownReflections(void)
{
    if (vkRT.reflDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.reflDescPool, NULL);
        vkRT.reflDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.reflDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.reflDescLayout, NULL);
        vkRT.reflDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.reflPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflPipeline, NULL);
        vkRT.reflPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.reflPipelineLayout, NULL);
        vkRT.reflPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.sbtReflBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.sbtReflBuffer, NULL);
        vkRT.sbtReflBuffer = VK_NULL_HANDLE;
    }
    if (vkRT.sbtReflMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.sbtReflMemory, NULL);
        vkRT.sbtReflMemory = VK_NULL_HANDLE;
    }
    if (vkRT.reflSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.reflSampler, NULL);
        vkRT.reflSampler = VK_NULL_HANDLE;
    }
    VK_RT_DestroyReflImages();
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeReflections (public)
// ---------------------------------------------------------------------------

void VK_RT_ResizeReflections(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyReflImages();
    VK_RT_CreateReflImages(width, height);

    // Force descriptor set refresh at next dispatch
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.reflDescSetLastUpdatedFrameCount[i] = -1;
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflections (public)
// Dispatch reflection rays for the full screen.  Called once per frame.
// Must be outside a render pass.  On entry depth is ATTACHMENT_OPTIMAL;
// transitions to READ_ONLY_OPTIMAL for the dispatch, then restores.
// On exit reflBuffer[currentFrame] is in GENERAL layout, readable by
// the interaction fragment shader (barrier issued before returning).
// ---------------------------------------------------------------------------

void VK_RT_DispatchReflections(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtReflections.GetBool())
        return;
    if (vkRT.reflPipeline == VK_NULL_HANDLE)
    {
        common->Printf("VK RT Refl: skip — pipeline is NULL\n");
        return;
    }

    const int frameIdx = vk.currentFrame;

    // Guard against duplicate dispatch in the same frame (weapon subview etc.)
    static int s_lastReflDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastReflDispatchFrame[frameIdx] == tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Refl: skip duplicate dispatch frame=%d slot=%d\n", tr.frameCount, frameIdx);
        return;
    }
    s_lastReflDispatchFrame[frameIdx] = tr.frameCount;

    vkReflBuffer_t &rb = vkRT.reflBuffer[frameIdx];
    if (rb.image == VK_NULL_HANDLE)
    {
        common->Printf("VK RT Refl: skip — reflBuffer[%d] image is NULL\n", frameIdx);
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl: frame=%d slot=%d size=%ux%u tlas=%p pipeline=%p\n", tr.frameCount, frameIdx,
                       rb.width, rb.height, (void *)vkRT.tlas[frameIdx].handle, (void *)vkRT.reflPipeline);

    // --- Depth barrier: ATTACHMENT → READ_ONLY for rgen depth sampling ---
    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    {
        VkImageMemoryBarrier depthToRead = {};
        depthToRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthToRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthToRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthToRead.image = vk.depthImage;
        depthToRead.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Build / fill UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    ReflParamsUBO ubo = {};

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

    ubo.maxDist = Max(1.0f, r_rtReflectionDistance.GetFloat());
    ubo.frameIndex = (uint32_t)(tr.frameCount);
    ubo.screenWidth = (int32_t)rb.width;
    ubo.screenHeight = (int32_t)rb.height;    ubo.reflBlend    = idMath::ClampFloat(0.0f, 2.0f, r_rtReflectionBlend.GetFloat());
    memcpy(uboMapped, &ubo, sizeof(ReflParamsUBO));

    // --- Update descriptor set (once per frame slot when frameCount changes) ---
    bool refreshSet = (vkRT.reflDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount);
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.reflDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo reflImgInfo = {};
        reflImgInfo.imageView = rb.view;
        reflImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(ReflParamsUBO);

        VkWriteDescriptorSet writes[4] = {};

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
        writes[1].pImageInfo = &reflImgInfo;

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

        vkUpdateDescriptorSets(vk.device, 4, writes, 0, NULL);
        vkRT.reflDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // --- Dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipelineLayout, 0, 1,
                            &vkRT.reflDescSets[frameIdx], 1, &uboOff);
    // set=1: material table (MatTable SSBO, VtxAddrTable, IdxAddrTable, bindless textures)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipelineLayout, 1, 1,
                            &vkRT.matDescSet, 0, NULL);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl: dispatch %ux%u maxDist=%.1f\n", rb.width, rb.height, ubo.maxDist);

    vkCmdTraceRaysKHR(cmd, &vkRT.reflRgenRegion, &vkRT.reflMissRegion, &vkRT.reflHitRegion, &vkRT.reflCallRegion,
                      rb.width, rb.height, 1);

    // --- Barrier: reflection write → fragment shader read ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &memBarrier, 0, NULL, 0, NULL);
    }

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL for the upcoming render pass ---
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &depthRestore);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl: dispatch complete\n");
}
