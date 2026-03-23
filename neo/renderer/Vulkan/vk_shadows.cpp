/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan ray tracing - shadow ray pipeline and dispatch.

Replaces RB_StencilShadowPass() when the Vulkan RT backend is active and
r_rtShadows is enabled.  Outputs a per-pixel shadow mask (R8 UNORM) that
is sampled in the lighting interaction pass instead of a stencil test.

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
// UBO layout matching shadow_ray.rgen ShadowParams block
// ---------------------------------------------------------------------------

struct ShadowParamsUBO
{
    float invViewProj[16]; // column-major 4x4
    float lightOrigin[4];  // xyz = pos, w = radius
    float lightFalloffRadius;
    int numSamples;
    uint32_t frameIndex;
    float pad;
};

// ---------------------------------------------------------------------------
// Forward declarations (defined in vk_buffer.cpp)
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);

// ---------------------------------------------------------------------------
// Helper: load a SPIR-V shader module from the compiled .spv files
// ---------------------------------------------------------------------------

extern VkShaderModule VK_LoadSPIRV(const char *path);

// ---------------------------------------------------------------------------
// VK_RT_InitShadowPipeline
// Builds the VkPipeline for the shadow ray tracing pass.
// Called once, after the swapchain is created.
// ---------------------------------------------------------------------------

static void VK_RT_InitShadowPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: TLAS
    // binding 1: shadow mask storage image
    // binding 2: depth sampler
    // binding 3: shadow params UBO

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
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.shadowDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.shadowDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.shadowPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rgenModule = VK_LoadSPIRV("glprogs/glsl/shadow_ray.rgen.spv");
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/shadow_ray.rmiss.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/shadow_ray.rahit.spv");

    if (rgenModule == VK_NULL_HANDLE || rmissModule == VK_NULL_HANDLE || rahitModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT: failed to load shadow ray shader modules — RT shadows disabled");
        if (rgenModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rgenModule, NULL);
        if (rmissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rmissModule, NULL);
        if (rahitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rahitModule, NULL);
        return;
    }

    // --- Shader stages ---
    VkPipelineShaderStageCreateInfo stages[3] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[2].module = rahitModule;
    stages[2].pName = "main";

    // --- Shader groups ---
    // Group 0: ray gen
    // Group 1: miss
    // Group 2: hit (any-hit only, no closest-hit)
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0; // rgen
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1; // miss
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = 2; // rahit
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount = 3;
    rtPipeInfo.pStages = stages;
    rtPipeInfo.groupCount = 3;
    rtPipeInfo.pGroups = groups;
    rtPipeInfo.maxPipelineRayRecursionDepth = 1; // shadow rays don't recurse
    rtPipeInfo.layout = vkRT.shadowPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.shadowPipeline));

    vkDestroyShaderModule(vk.device, rgenModule, NULL);
    vkDestroyShaderModule(vk.device, rmissModule, NULL);
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

    // One handle per group
    uint32_t sbtSize = 3 * alignUp(handleSizeAligned, baseAlignment);

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vkRT.sbtBuffer,
                    &vkRT.sbtMemory);

    // Retrieve handles
    uint8_t *handles = (uint8_t *)alloca(3 * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.shadowPipeline, 0, 3, 3 * handleSize, handles));

    // Write handles into SBT buffer
    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtMemory, 0, sbtSize, 0, (void **)&sbtData));

    uint32_t stride = alignUp(handleSizeAligned, baseAlignment);
    for (int i = 0; i < 3; i++)
    {
        memcpy(sbtData + i * stride, handles + i * handleSize, handleSize);
    }
    vkUnmapMemory(vk.device, vkRT.sbtMemory);

    // SBT device addresses
    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    vkRT.rgenRegion = {sbtBase + 0 * stride, stride, stride};
    vkRT.missRegion = {sbtBase + 1 * stride, stride, stride};
    vkRT.hitRegion  = {sbtBase + 2 * stride, stride, stride};
    vkRT.callRegion = {0, 0, 0};

    if (r_vkLogRT.GetInteger() >= 1) {
        common->Printf("VK RT SBT: handleSize=%u handleAlignment=%u baseAlignment=%u stride=%u sbtTotalBytes=%u\n",
                       handleSize, handleAlignment, baseAlignment, stride, sbtSize);
        common->Printf("VK RT SBT: sbtBase=0x%llx  rgen=0x%llx  miss=0x%llx  hit=0x%llx\n",
                       (unsigned long long)sbtBase,
                       (unsigned long long)vkRT.rgenRegion.deviceAddress,
                       (unsigned long long)vkRT.missRegion.deviceAddress,
                       (unsigned long long)vkRT.hitRegion.deviceAddress);
        common->Printf("VK RT SBT: rgen base-alignment check: addr%%baseAlign=%llu (must be 0)\n",
                       (unsigned long long)(vkRT.rgenRegion.deviceAddress % baseAlignment));
    }

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
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.shadowDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        layouts[i] = vkRT.shadowDescLayout;
    }
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.shadowDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.shadowDescSets));

    // --- Shadow mask sampler (used by the lighting pass to read the shadow mask) ---
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.shadowMaskSampler));
    }

    // --- Depth sampler (used by the rgen shader to reconstruct world position from depth) ---
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.depthSampler));
    }

    vkRT.isInitialized = true;
    common->Printf("VK RT: shadow ray pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitBlurPipeline
// Builds the compute pipeline for the separable shadow mask blur.
// ---------------------------------------------------------------------------

static void VK_RT_InitBlurPipeline(void)
{
    // --- Descriptor set layout: 2 storage images (input, output) ---
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.blurDescLayout));

    // --- Push constant: direction (int) + radius (int) + 2 floats pad ---
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 16; // 2 ints + 2 floats pad = 16 bytes

    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.blurDescLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.blurPipelineLayout));

    // --- Compute shader ---
    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/shadow_blur.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT: failed to load shadow_blur.comp.spv — blur disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stageInfo = {};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = compModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipeInfo = {};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage = stageInfo;
    pipeInfo.layout = vkRT.blurPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeInfo, NULL, &vkRT.blurPipeline));

    vkDestroyShaderModule(vk.device, compModule, NULL);

    // --- Descriptor pool and sets (2 sets: H pass and V pass) ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}; // 2 images × 2 sets
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.blurDescPool));

    VkDescriptorSetLayout layouts[2] = {vkRT.blurDescLayout, vkRT.blurDescLayout};
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.blurDescPool;
    dsAlloc.descriptorSetCount = 2;
    dsAlloc.pSetLayouts = layouts;
    VkDescriptorSet sets[2];
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, sets));
    vkRT.blurDescSetH = sets[0];
    vkRT.blurDescSetV = sets[1];

    common->Printf("VK RT: shadow blur compute pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchShadowRaysForLight
// Called once per light, outside a render pass, after the depth prepass.
//
// Dispatches shadow rays for a single light and writes to the shadow mask
// (VK_IMAGE_LAYOUT_GENERAL throughout — no layout transition for the mask).
//
// Depth image is transitioned: DEPTH_STENCIL_ATTACHMENT_OPTIMAL →
// DEPTH_STENCIL_READ_ONLY_OPTIMAL for the dispatch, then restored before return
// so the caller can immediately reopen a render pass.
//
// The shadow-params UBO is allocated from the per-frame UBO ring (dynamic binding),
// so per-light data is captured in the command buffer's bind offset — no per-call
// alloc/free and no vkQueueWaitIdle.
// ---------------------------------------------------------------------------

void VK_RT_DispatchShadowRaysForLight(VkCommandBuffer cmd, const viewDef_t *viewDef, const viewLight_t *vLight)
{
    if (!vkRT.isInitialized || !vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtShadows.GetBool())
        return;
    if (!vLight || !vLight->lightDef)
        return;

    uint32_t frameIdx = vk.currentFrame;
    vkShadowMask_t &sm = vkRT.shadowMask[frameIdx];
    if (sm.image == VK_NULL_HANDLE)
        return;

    if (r_vkLogRT.GetInteger() >= 2)
    {
        const renderLight_t &lp = vLight->lightDef->parms;
        common->Printf("VK RT DISPATCH: frame=%u light=(%.1f,%.1f,%.1f) "
                       "pipeline=%s tlas=%s tlasAddr=0x%llx "
                       "shadowMask=%s %ux%u "
                       "rgen=0x%llx miss=0x%llx hit=0x%llx\n",
                       frameIdx,
                       lp.origin.x, lp.origin.y, lp.origin.z,
                       (vkRT.shadowPipeline  != VK_NULL_HANDLE) ? "OK" : "NULL",
                       (vkRT.tlas[frameIdx].handle     != VK_NULL_HANDLE) ? "OK" : "NULL",
                       (unsigned long long)vkRT.tlas[frameIdx].deviceAddress,
                       (sm.image             != VK_NULL_HANDLE) ? "OK" : "NULL",
                       sm.width, sm.height,
                       (unsigned long long)vkRT.rgenRegion.deviceAddress,
                       (unsigned long long)vkRT.missRegion.deviceAddress,
                       (unsigned long long)vkRT.hitRegion.deviceAddress);
        fflush(NULL);
    }

    // --- Depth barrier: render-pass final layout → shader-readable ---
    VkImageMemoryBarrier depthToRead = {};
    depthToRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthToRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthToRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthToRead.image = vk.depthImage;
    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    depthToRead.subresourceRange = {depthAspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, NULL, 0, NULL, 1, &depthToRead);

    // --- Update frame-level descriptor set (TLAS, shadow mask, depth sampler, UBO base) ---
    // The UBO binding is DYNAMIC: the descriptor stores the ring buffer base; the per-light
    // offset is supplied at vkCmdBindDescriptorSets time, so this update only needs to happen
    // once per frame slot (when the ring buffer handle changes, which is never after init).
    // We update unconditionally for safety; it's idempotent.
    {
        VkDescriptorSet ds = vkRT.shadowDescSets[frameIdx];

        // Binding 0: TLAS
        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkWriteDescriptorSet writes[4] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &tlasWrite;
        writes[0].dstSet = ds;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        // Binding 1: shadow mask storage image (always GENERAL)
        VkDescriptorImageInfo smImgInfo = {};
        smImgInfo.imageView = sm.view;
        smImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = ds;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &smImgInfo;

        // Binding 2: depth sampler (cached, created once at init)
        VkDescriptorImageInfo depthImgInfo = {};
        depthImgInfo.sampler = vkRT.depthSampler;
        depthImgInfo.imageView = vk.depthSampledView;  // depth-only view (conformant for sampling)
        depthImgInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = ds;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &depthImgInfo;

        // Binding 3: UBO (DYNAMIC) — stores the ring buffer base; per-light offset at bind time.
        // VK_AllocUBOForShadow is called below to get the per-light slot, but the buffer
        // handle is the same for all lights in this frame slot, so we always use slot 0 offset
        // here as the base; the dynamic offset passed to vkCmdBindDescriptorSets is the real one.
        extern bool VK_AllocUBOForShadow(VkBuffer * outBuf, uint32_t *outOffset, void **outMapped);
        VkBuffer uboBuf;
        uint32_t uboOff;
        void *uboMapped;
        VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0; // base 0; dynamic offset added at bind time
        uboInfo.range = sizeof(ShadowParamsUBO);

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = ds;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[3].pBufferInfo = &uboInfo;

        vkUpdateDescriptorSets(vk.device, 4, writes, 0, NULL);

        // --- Build per-light UBO data into the allocated ring slot ---
        const renderLight_t &light = vLight->lightDef->parms;

        ShadowParamsUBO ubo;
        memset(&ubo, 0, sizeof(ubo));

        {
            const float *proj = viewDef->projectionMatrix;
            const float *mv = viewDef->worldSpace.modelViewMatrix;
            float vp[16];
            for (int r = 0; r < 4; r++)
            {
                for (int c = 0; c < 4; c++)
                {
                    vp[c * 4 + r] = 0.0f;
                    for (int k = 0; k < 4; k++)
                        vp[c * 4 + r] += proj[k * 4 + r] * mv[c * 4 + k];
                }
            }
            idMat4 vpMat(idVec4(vp[0], vp[1], vp[2], vp[3]), idVec4(vp[4], vp[5], vp[6], vp[7]),
                         idVec4(vp[8], vp[9], vp[10], vp[11]), idVec4(vp[12], vp[13], vp[14], vp[15]));
            idMat4 invVP = vpMat.Inverse();
            memcpy(ubo.invViewProj, invVP.ToFloatPtr(), 16 * sizeof(float));
        }

        // Flashlight bias: push shadow origin forward along the view axis to reduce
        // weapon/hand self-shadowing.  Detect the flashlight by checking whether the
        // light origin is very close to the player view origin (within 64 units).
        idVec3 shadowOrigin = light.origin;
        float flashlightBias = r_rtFlashlightBias.GetFloat();
        if (flashlightBias > 0.0f)
        {
            const idVec3 &viewOrg = viewDef->renderView.vieworg;
            float distToView = (light.origin - viewOrg).Length();
            if (distToView < 64.0f)
            {
                // viewaxis[0] is the forward direction in Doom3 (view looks down +X)
                const idVec3 &fwd = viewDef->renderView.viewaxis[0];
                shadowOrigin = light.origin + fwd * flashlightBias;
            }
        }

        ubo.lightOrigin[0] = shadowOrigin.x;
        ubo.lightOrigin[1] = shadowOrigin.y;
        ubo.lightOrigin[2] = shadowOrigin.z;
        ubo.lightOrigin[3] = light.lightRadius.x;
        ubo.lightFalloffRadius = light.lightRadius.Length();
        ubo.numSamples = r_rtShadowSamples.GetInteger();
        ubo.frameIndex = (uint32_t)(vk.currentFrame * 100 + tr.frameCount);
        ubo.pad = 0.0f;

        memcpy(uboMapped, &ubo, sizeof(ShadowParamsUBO));

        // --- Dispatch ---
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.shadowPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.shadowPipelineLayout, 0, 1, &ds, 1,
                                &uboOff);

        if (r_vkLogRT.GetInteger() >= 1)
        {
            common->Printf("VK RT TRACE: frame=%u uboOff=%u dims=%ux%u\n",
                           frameIdx, uboOff, sm.width, sm.height);
            fflush(NULL);
        }

        vkCmdTraceRaysKHR(cmd, &vkRT.rgenRegion, &vkRT.missRegion, &vkRT.hitRegion, &vkRT.callRegion, sm.width,
                          sm.height, 1);
    }

    // --- Shadow mask blur (separable Gaussian, two compute dispatches) ---
    int blurRadius = r_rtShadowBlur.GetInteger();
    bool didBlur = false;
    if (blurRadius > 0 && vkRT.blurPipeline != VK_NULL_HANDLE && sm.blurTempImage != VK_NULL_HANDLE)
    {
        if (blurRadius > 8)
            blurRadius = 8;

        uint32_t groupsX = (sm.width  + 7) / 8;
        uint32_t groupsY = (sm.height + 7) / 8;

        // Barrier: RT write → compute read
        VkMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.blurPipeline);

        // Update H-pass descriptors: input=shadowMask, output=blurTemp
        VkDescriptorImageInfo hInput  = { VK_NULL_HANDLE, sm.view,        VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo hOutput = { VK_NULL_HANDLE, sm.blurTempView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet hWrites[2] = {};
        hWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        hWrites[0].dstSet = vkRT.blurDescSetH;
        hWrites[0].dstBinding = 0;
        hWrites[0].descriptorCount = 1;
        hWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        hWrites[0].pImageInfo = &hInput;
        hWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        hWrites[1].dstSet = vkRT.blurDescSetH;
        hWrites[1].dstBinding = 1;
        hWrites[1].descriptorCount = 1;
        hWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        hWrites[1].pImageInfo = &hOutput;
        vkUpdateDescriptorSets(vk.device, 2, hWrites, 0, NULL);

        // Push constants: direction=0 (horizontal), radius
        struct { int direction; int radius; float pad0; float pad1; } pc;
        pc.direction = 0;
        pc.radius = blurRadius;
        pc.pad0 = 0.f;
        pc.pad1 = 0.f;
        vkCmdPushConstants(cmd, vkRT.blurPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.blurPipelineLayout, 0, 1,
                                &vkRT.blurDescSetH, 0, NULL);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        // Barrier: compute H-pass write → compute V-pass read
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);

        // Update V-pass descriptors: input=blurTemp, output=shadowMask
        VkDescriptorImageInfo vInput  = { VK_NULL_HANDLE, sm.blurTempView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo vOutput = { VK_NULL_HANDLE, sm.view,         VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet vWrites[2] = {};
        vWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vWrites[0].dstSet = vkRT.blurDescSetV;
        vWrites[0].dstBinding = 0;
        vWrites[0].descriptorCount = 1;
        vWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        vWrites[0].pImageInfo = &vInput;
        vWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vWrites[1].dstSet = vkRT.blurDescSetV;
        vWrites[1].dstBinding = 1;
        vWrites[1].descriptorCount = 1;
        vWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        vWrites[1].pImageInfo = &vOutput;
        vkUpdateDescriptorSets(vk.device, 2, vWrites, 0, NULL);

        // Push constants: direction=1 (vertical), same radius
        pc.direction = 1;
        vkCmdPushConstants(cmd, vkRT.blurPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.blurPipelineLayout, 0, 1,
                                &vkRT.blurDescSetV, 0, NULL);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        didBlur = true;
    }

    // --- Barrier: shadow mask → fragment shader read ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkPipelineStageFlags srcStage = didBlur ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &memBarrier, 0, NULL, 0, NULL);
    }

    // --- Depth barrier: restore to DEPTH_STENCIL_ATTACHMENT_OPTIMAL for render pass resume ---
    VkImageMemoryBarrier depthRestore = {};
    depthRestore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthRestore.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthRestore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthRestore.image = vk.depthImage;
    depthRestore.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    VkPipelineStageFlags depthSrcStage = didBlur ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                 : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    vkCmdPipelineBarrier(cmd, depthSrcStage, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, NULL, 0, NULL, 1, &depthRestore);
}

// ---------------------------------------------------------------------------
// VK_RT_Init (public entry from vk_backend.cpp)
// Initializes RT acceleration structure support, shadow pipeline, and
// creates shadow mask images at the current swapchain resolution.
// ---------------------------------------------------------------------------

// Forward declared in vk_raytracing.h; the actual RT state init is in vk_accelstruct.cpp.
// This is the shadow-specific second stage init.
void VK_RT_InitShadows(void)
{
    VK_RT_InitShadowPipeline();
    VK_RT_InitBlurPipeline();
    VK_RT_ResizeShadowMask(vk.swapchainExtent.width, vk.swapchainExtent.height);
}
