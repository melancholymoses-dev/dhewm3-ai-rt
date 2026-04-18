/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan ray tracing - ambient occlusion ray pipeline and dispatch.

Outputs a per-pixel AO factor (R8 UNORM, 1=unoccluded, 0=fully occluded)
by shooting cosine-weighted hemisphere rays from each visible surface point.
The AO mask is sampled in the lighting interaction pass to darken diffuse.

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

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

// r_rtAO and r_rtAOSamples declared in RenderSystem_init.cpp — use extern here.
static idCVar r_rtAORadius("r_rtAORadius", "64.0", CVAR_RENDERER | CVAR_FLOAT,
                           "Max AO ray length in world units (default 64)");

// ---------------------------------------------------------------------------
// UBO layout matching ao_ray.rgen AOParams block (std140)
//
//   mat4  invViewProj   offset   0  size 64
//   float aoRadius      offset  64  size  4
//   int   numSamples    offset  68  size  4
//   uint  frameIndex    offset  72  size  4
//   float pad0          offset  76  size  4
//   ivec2 screenSize     offset  80  size  8  (ivec2 std140 align=8)
//   ivec2 scissorOffset  offset  88  size  8
//   ivec2 scissorExtent  offset  96  size  8
//   ivec2 pad2           offset 104  size  8
//   total: 112 bytes
// ---------------------------------------------------------------------------

struct AOParamsUBO
{
    float invViewProj[16]; // column-major 4x4
    float aoRadius;
    int32_t numSamples;
    uint32_t frameIndex;
    float pad0;
    int32_t screenWidth;
    int32_t screenHeight;
    int32_t scissorOffsetX;
    int32_t scissorOffsetY;
    int32_t scissorExtentX;
    int32_t scissorExtentY;
    int32_t pad1[2];
};
static_assert(sizeof(AOParamsUBO) == 112, "AOParamsUBO size mismatch");

// Convert viewDef->scissor (GL Y-up) to VkRect2D (VK Y-down) in framebuffer coordinates.
static VkRect2D VK_RT_ComputeViewDispatchRect(const viewDef_t *viewDef)
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
// Forward declarations (defined in other files)
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// VK_RT_CreateAOMaskImages
// Create per-frame R8_UNORM AO images at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateAOMaskImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkAOMask_t &ao = vkRT.aoMask[i];
        ao.width = width;
        ao.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8_UNORM;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &ao.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, ao.image, &memReq);

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
            common->Error("VK RT AO: no device-local memory type for AO image");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &ao.memory));
        VK_CHECK(vkBindImageMemory(vk.device, ao.image, ao.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ao.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &ao.view));

        // Transition UNDEFINED → GENERAL so the rgen shader can imageStore immediately
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

            // Transition UNDEFINED → GENERAL (required before clear)
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = ao.image;
            barrier.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            // Clear to 1.0 (full ambient, no occlusion) so unwritten pixels don't contain garbage
            VkClearColorValue clearWhite = {};
            clearWhite.float32[0] = 1.0f;
            vkCmdClearColorImage(tmpCmd, ao.image, VK_IMAGE_LAYOUT_GENERAL, &clearWhite, 1, &subRange);

            // Barrier: transfer write → shader write for rgen imageStore
            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.image = ao.image;
            barrier2.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                 0, 0, NULL, 0, NULL, 1, &barrier2);

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

static void VK_RT_DestroyAOMaskImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkAOMask_t &ao = vkRT.aoMask[i];
        if (ao.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, ao.view, NULL);
            ao.view = VK_NULL_HANDLE;
        }
        if (ao.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, ao.image, NULL);
            ao.image = VK_NULL_HANDLE;
        }
        if (ao.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, ao.memory, NULL);
            ao.memory = VK_NULL_HANDLE;
        }
        ao.width = 0;
        ao.height = 0;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitAOPipeline
// Create the AO RT pipeline, SBT, descriptor sets, and samplers.
// ---------------------------------------------------------------------------

static void VK_RT_InitAOPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: TLAS
    // binding 1: AO mask storage image
    // binding 2: depth sampler
    // binding 3: AO params UBO (dynamic)

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
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.aoDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.aoDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.aoPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rgenModule = VK_LoadSPIRV("glprogs/glsl/ao_ray.rgen.spv");
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/ao_ray.rmiss.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/ao_ray.rahit.spv");

    if (rgenModule == VK_NULL_HANDLE || rmissModule == VK_NULL_HANDLE || rahitModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT AO: failed to load AO ray shader modules — RTAO disabled");
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
    // Group 2: hit (any-hit only, no closest-hit — AO only needs binary occlusion)
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = 2;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount = 3;
    rtPipeInfo.pStages = stages;
    rtPipeInfo.groupCount = 3;
    rtPipeInfo.pGroups = groups;
    rtPipeInfo.maxPipelineRayRecursionDepth = 1; // AO rays don't recurse
    rtPipeInfo.layout = vkRT.aoPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.aoPipeline));

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
    uint32_t stride = alignUp(handleSizeAligned, baseAlignment);
    uint32_t sbtSize = 3 * stride;

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vkRT.sbtAOBuffer,
                    &vkRT.sbtAOMemory);

    uint8_t *handles = (uint8_t *)alloca(3 * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.aoPipeline, 0, 3, 3 * handleSize, handles));

    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtAOMemory, 0, sbtSize, 0, (void **)&sbtData));
    for (int i = 0; i < 3; i++)
        memcpy(sbtData + i * stride, handles + i * handleSize, handleSize);
    vkUnmapMemory(vk.device, vkRT.sbtAOMemory);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtAOBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    vkRT.aoRgenRegion = {sbtBase + 0 * stride, stride, stride};
    vkRT.aoMissRegion = {sbtBase + 1 * stride, stride, stride};
    vkRT.aoHitRegion = {sbtBase + 2 * stride, stride, stride};
    vkRT.aoCallRegion = {0, 0, 0};

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT AO SBT: stride=%u sbtBytes=%u base=0x%llx\n", stride, sbtSize,
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
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.aoDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.aoDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.aoDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.aoDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.aoDescSetLastUpdatedFrameCount[i] = -1;

    // --- AO mask sampler (nearest-clamp, used by interaction shader) ---
    if (vkRT.aoMaskSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.aoMaskSampler));
    }

    common->Printf("VK RT AO: pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitAO (public entry)
// ---------------------------------------------------------------------------

void VK_RT_InitAO(void)
{
    VK_RT_InitAOPipeline();
    VK_RT_ResizeAOMask(vk.swapchainExtent.width, vk.swapchainExtent.height);
    // Temporal history images and EMA pipeline (Step 5.2)
    VK_RT_InitTemporal();
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeAOMask
// ---------------------------------------------------------------------------

void VK_RT_ResizeAOMask(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyAOMaskImages();
    VK_RT_CreateAOMaskImages(width, height);

    // Descriptor sets must be refreshed at the next dispatch (new image views)
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.aoDescSetLastUpdatedFrameCount[i] = -1;
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchAO
// Dispatch hemisphere AO rays for the current view scissor rect.
// Called once per frame after TLAS rebuild, outside a render pass.
// Depth must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL on entry.
// ---------------------------------------------------------------------------

void VK_RT_DispatchAO(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
    {
        common->Printf("VK RT AO: skip — RT not initialized\n");
        return;
    }
    if (!vkRT.tlas[vk.currentFrame].isValid & (r_vkLogRT.GetInteger() >= 1))
    {
        common->Printf("VK RT AO: skip — TLAS[%d] not valid\n", vk.currentFrame);
        return;
    }
    if (!r_useRayTracing.GetBool() || !r_rtAO.GetBool())
        return;
    if (vkRT.aoPipeline == VK_NULL_HANDLE)
    {
        common->Printf("VK RT AO: skip — pipeline is NULL\n");
        return;
    }

    const int frameIdx = vk.currentFrame;

    const VkRect2D dispatchRect = VK_RT_ComputeViewDispatchRect(viewDef);
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT AO: skip empty dispatch rect frame=%d slot=%d\n", tr.frameCount, frameIdx);
        return;
    }

    vkAOMask_t &ao = vkRT.aoMask[frameIdx];

    if (ao.image == VK_NULL_HANDLE & (r_vkLogRT.GetInteger() >= 1))
    {
        common->Printf("VK RT AO: skip — AO image[%d] is NULL\n", frameIdx);
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT AO: frame=%d slot=%d mask=%ux%u rect=(%d,%d %u,%u) tlas=%p pipeline=%p\n", tr.frameCount,
                       frameIdx, ao.width, ao.height, dispatchRect.offset.x, dispatchRect.offset.y,
                       (unsigned int)dispatchRect.extent.width, (unsigned int)dispatchRect.extent.height,
                       (void *)vkRT.tlas[frameIdx].handle, (void *)vkRT.aoPipeline);

    // --- Depth barrier: ATTACHMENT → READ_ONLY for rgen depth sampling ---
    {
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Build / fill UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    AOParamsUBO ubo = {};

    // Inverse view-projection (GL convention — same as shadow dispatch).
    // The rgen shader remaps Vulkan depth [0,1] → GL NDC Z [-1,1] before applying this matrix.
    // Use viewDef->projectionMatrix (GL convention, NOT the VK-remapped variant).
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

    ubo.aoRadius = Max(1.0f, r_rtAORadius.GetFloat());
    ubo.numSamples = idMath::ClampInt(1, 16, r_rtAOSamples.GetInteger());
    ubo.frameIndex = (uint32_t)(tr.frameCount);
    ubo.screenWidth = (int32_t)ao.width;
    ubo.screenHeight = (int32_t)ao.height;
    ubo.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    ubo.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    ubo.scissorExtentX = (int32_t)dispatchRect.extent.width;
    ubo.scissorExtentY = (int32_t)dispatchRect.extent.height;

    memcpy(uboMapped, &ubo, sizeof(AOParamsUBO));

    // Sanity-check the matrix for NaN (a singular VP matrix produces NaN and crashes the rgen)
    {
        bool hasNaN = false;
        for (int i = 0; i < 16; i++)
            if (ubo.invViewProj[i] != ubo.invViewProj[i])
            {
                hasNaN = true;
                break;
            }
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf(
                "VK RT AO: UBO radius=%.1f samples=%d screenSize=%dx%d scissor=(%d,%d %d,%d) matNaN=%d uboOff=%u\n",
                ubo.aoRadius, ubo.numSamples, ubo.screenWidth, ubo.screenHeight, ubo.scissorOffsetX, ubo.scissorOffsetY,
                ubo.scissorExtentX, ubo.scissorExtentY, hasNaN ? 1 : 0, uboOff);
        if (hasNaN)
            common->Warning("VK RT AO: invViewProj contains NaN — ray directions will be degenerate!");
    }

    // --- Update descriptor set (once per frame slot when frameCount changes).
    // Also force refresh when bound resources changed inside the same frame, which
    // protects against stale cached descriptors after RT resource churn.
    static VkAccelerationStructureKHR s_lastAOTlasHandle[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastAOStorageView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastAODepthView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool aoResourceChanged = (s_lastAOTlasHandle[frameIdx] != vkRT.tlas[frameIdx].handle) ||
                                   (s_lastAOStorageView[frameIdx] != ao.view) ||
                                   (s_lastAODepthView[frameIdx] != vk.depthSampledView);

    if (aoResourceChanged && vkRT.aoDescSetLastUpdatedFrameCount[frameIdx] == tr.frameCount &&
        r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK RT AO: forcing descriptor refresh due to in-frame resource change frame=%d slot=%d tlas=%p "
                       "aoView=%p depthView=%p\n",
                       tr.frameCount, frameIdx, (void *)vkRT.tlas[frameIdx].handle, (void *)ao.view,
                       (void *)vk.depthSampledView);
    }

    bool refreshSet = (vkRT.aoDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount) || aoResourceChanged;
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.aoDescSets[frameIdx];

        // Binding 0: TLAS
        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        // Binding 1: AO image (storage)
        VkDescriptorImageInfo aoImgInfo = {};
        aoImgInfo.imageView = ao.view;
        aoImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Binding 2: depth sampler
        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        // Binding 3: UBO base (dynamic offset supplied at bind time)
        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(AOParamsUBO);

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
        writes[1].pImageInfo = &aoImgInfo;

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
        vkRT.aoDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastAOTlasHandle[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastAOStorageView[frameIdx] = ao.view;
        s_lastAODepthView[frameIdx] = vk.depthSampledView;
    }

    // --- Dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.aoPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.aoPipelineLayout, 0, 1,
                            &vkRT.aoDescSets[frameIdx], 1, &uboOff);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT AO: dispatch %ux%u samples=%d radius=%.1f (rect origin=%d,%d)\n",
                       (unsigned int)dispatchRect.extent.width, (unsigned int)dispatchRect.extent.height,
                       ubo.numSamples, ubo.aoRadius, dispatchRect.offset.x, dispatchRect.offset.y);

    vkCmdTraceRaysKHR(cmd, &vkRT.aoRgenRegion, &vkRT.aoMissRegion, &vkRT.aoHitRegion, &vkRT.aoCallRegion,
                      dispatchRect.extent.width, dispatchRect.extent.height, 1);
    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT AO: traceRaysKHR recorded\n");

    // --- Barrier: AO write → fragment shader read (and compute when temporal is active) ---
    // Include COMPUTE_SHADER in the dst stage so the temporal resolve pass (if enabled)
    // can read aoMask[] without an additional barrier.
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &memBarrier, 0, NULL, 0, NULL);
    }

    // --- Temporal EMA resolve + atrous filter ---
    // Both passes sample the depth image (READ_ONLY_OPTIMAL layout).
    // Run them BEFORE restoring depth to ATTACHMENT_OPTIMAL so the layout matches.
    VK_RT_DispatchTemporalResolveAO(cmd, viewDef);
    VK_RT_DispatchAtrousAO(cmd);

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL for render pass resume ---
    // Use the same conditional aspect mask as the initial barrier.
    // srcStageMask includes COMPUTE_SHADER since temporal/atrous just ran.
    {
        VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
            vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
            depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

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
        common->Printf("VK RT AO: dispatch complete\n");
}
