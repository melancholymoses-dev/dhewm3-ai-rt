/*
===========================================================================

dhewm3-rt Vulkan ray tracing - one-bounce global illumination pipeline.

Phase 6.1 (Option A — Ambient-only):
  Shoots one cosine-weighted hemisphere ray per pixel from each visible
  surface point.  At the secondary hit the real diffuse albedo is sampled
  via the material table (Phase 5.4 infrastructure).  The averaged albedo
  is scaled by r_rtGIStrength and stored in an RGBA16F GI buffer.

  The interaction fragment shader samples this buffer and adds it to the
  diffuse term, providing colour bleeding and contact brightening.

  The pipeline layout mirrors vk_reflections.cpp:
    set=0  per-frame: TLAS, GI storage image, depth, GI params UBO
    set=1  material table (matDescLayout — shared with reflections)

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

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtGI("r_rtGI", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
              "Enable one-bounce GI (Phase 6.1, ambient colour bleeding)");

static idCVar r_rtGIRadius("r_rtGIRadius", "512.0", CVAR_RENDERER | CVAR_FLOAT,
                           "Max GI bounce ray distance in world units");

static idCVar r_rtGISamples("r_rtGISamples", "1", CVAR_RENDERER | CVAR_INTEGER,
                            "GI bounce rays per pixel (1-8)");

static idCVar r_rtGIStrength("r_rtGIStrength", "0.03", CVAR_RENDERER | CVAR_FLOAT,
                             "Scale applied to the GI contribution per-light (accumulates; default 0.03 ≈ 0.3 for ~10 lights)");

// ---------------------------------------------------------------------------
// GI UBO layout matching gi_ray.rgen GIParams block (std140)
//
//   mat4  invViewProj    offset   0  size 64
//   float giRadius       offset  64  size  4
//   int   numSamples     offset  68  size  4
//   uint  frameIndex     offset  72  size  4
//   float giStrength     offset  76  size  4
//   ivec2 screenSize     offset  80  size  8  (ivec2 std140 align=8)
//   ivec2 scissorOffset  offset  88  size  8
//   ivec2 scissorExtent  offset  96  size  8
//   ivec2 pad2           offset 104  size  8
//   total: 112 bytes
// ---------------------------------------------------------------------------

struct GIParamsUBO
{
    float invViewProj[16]; // column-major 4x4
    float giRadius;
    int32_t numSamples;
    uint32_t frameIndex;
    float giStrength;
    int32_t screenWidth;
    int32_t screenHeight;
    int32_t scissorOffsetX;
    int32_t scissorOffsetY;
    int32_t scissorExtentX;
    int32_t scissorExtentY;
    int32_t pad[2];
};
static_assert(sizeof(GIParamsUBO) == 112, "GIParamsUBO size mismatch");

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// VK_RT_CreateGIImages
// Allocates per-frame RGBA16F GI buffers at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateGIImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &gb = vkRT.giBuffer[i];
        gb.width  = width;
        gb.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        imgInfo.extent        = {width, height, 1};
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &gb.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, gb.image, &memReq);

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
            common->Error("VK RT GI: no device-local memory type for GI image");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &gb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, gb.image, gb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image            = gb.image;
        viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format           = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &gb.view));

        // Transition UNDEFINED → GENERAL so rgen can imageStore on first dispatch.
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo cbAlloc = {};
            cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAlloc.commandPool        = vk.commandPool;
            cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAlloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmpCmd, &beginInfo);

            VkImageSubresourceRange subRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            // Transition UNDEFINED → GENERAL (required before clear and imageStore).
            VkImageMemoryBarrier barrier = {};
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask       = 0;
            barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image               = gb.image;
            barrier.subresourceRange    = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

            // Clear to black so unwritten pixels don't contain garbage.
            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, gb.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

            // Second barrier: transfer write → shader write for rgen imageStore.
            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            barrier2.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.image               = gb.image;
            barrier2.subresourceRange    = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &barrier2);

            vkEndCommandBuffer(tmpCmd);

            VkSubmitInfo submitInfo         = {};
            submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount   = 1;
            submitInfo.pCommandBuffers      = &tmpCmd;
            vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(vk.graphicsQueue);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }
}

static void VK_RT_DestroyGIImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &gb = vkRT.giBuffer[i];
        if (gb.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, gb.view, NULL);
            gb.view = VK_NULL_HANDLE;
        }
        if (gb.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, gb.image, NULL);
            gb.image = VK_NULL_HANDLE;
        }
        if (gb.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, gb.memory, NULL);
            gb.memory = VK_NULL_HANDLE;
        }
        gb.width  = 0;
        gb.height = 0;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitGIPipeline
// Creates the GI RT pipeline, SBT, descriptor sets, and sampler.
//
// Shader groups:
//   Group 0: rgen   (gi_ray.rgen)
//   Group 1: miss   (gi_ray.rmiss)
//   Group 2: hit    (gi_ray.rchit + gi_ray.rahit for alpha-discard)
//
// SBT layout: [0]=rgen  [1]=miss  [2]=hit
// ---------------------------------------------------------------------------

static void VK_RT_InitGIPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: TLAS
    // binding 1: GI RGBA16F storage image
    // binding 2: depth sampler (COMBINED_IMAGE_SAMPLER)
    // binding 3: GI params UBO (dynamic)

    VkDescriptorSetLayoutBinding bindings[4] = {};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giDescLayout));

    // --- Pipeline layout ---
    // set=0: per-frame resources (TLAS, GI image, depth, UBO)
    // set=1: material table (shared with reflections/shadows)
    VkDescriptorSetLayout giLayouts[2] = {vkRT.giDescLayout, vkRT.matDescLayout};
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts    = giLayouts;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rgenModule  = VK_LoadSPIRV("glprogs/glsl/gi_ray.rgen.spv");
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rmiss.spv");
    VkShaderModule rchitModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rchit.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rahit.spv");

    if (rgenModule == VK_NULL_HANDLE || rmissModule == VK_NULL_HANDLE ||
        rchitModule == VK_NULL_HANDLE || rahitModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI: failed to load GI shader modules — GI disabled");
        if (rgenModule  != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rgenModule,  NULL);
        if (rmissModule != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rmissModule, NULL);
        if (rchitModule != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rchitModule, NULL);
        if (rahitModule != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, rahitModule, NULL);
        return;
    }

    // --- Shader stages ---
    // Stage 0: rgen
    // Stage 1: rmiss
    // Stage 2: rchit
    // Stage 3: rahit
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
    // Group 0: rgen
    // Group 1: miss
    // Group 2: hit (rchit + rahit for alpha-discard)
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    groups[0].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader      = 0; // rgen
    groups[0].closestHitShader   = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader      = 1; // rmiss
    groups[1].closestHitShader   = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader      = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader   = 2; // gi_ray.rchit — albedo lookup
    groups[2].anyHitShader       = 3; // gi_ray.rahit  — alpha-discard
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType                          = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount                     = 4;
    rtPipeInfo.pStages                        = stages;
    rtPipeInfo.groupCount                     = 3;
    rtPipeInfo.pGroups                        = groups;
    rtPipeInfo.maxPipelineRayRecursionDepth   = 1; // one bounce — no recursion
    rtPipeInfo.layout                         = vkRT.giPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.giPipeline));

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

    const uint32_t handleSize      = rtProps.shaderGroupHandleSize;
    const uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    const uint32_t baseAlignment   = rtProps.shaderGroupBaseAlignment;

    auto alignUp = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
    uint32_t handleSizeAligned = alignUp(handleSize, handleAlignment);
    uint32_t stride            = alignUp(handleSizeAligned, baseAlignment);
    // Layout: [0]=rgen  [1]=miss  [2]=hit
    uint32_t sbtSize = 3 * stride;

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &vkRT.sbtGIBuffer, &vkRT.sbtGIMemory);

    uint8_t *handles = (uint8_t *)alloca(3 * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.giPipeline, 0, 3, 3 * handleSize, handles));

    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtGIMemory, 0, sbtSize, 0, (void **)&sbtData));
    for (int i = 0; i < 3; i++)
        memcpy(sbtData + i * stride, handles + i * handleSize, handleSize);
    vkUnmapMemory(vk.device, vkRT.sbtGIMemory);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtGIBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    vkRT.giRgenRegion = {sbtBase + 0 * stride, stride, stride};
    vkRT.giMissRegion = {sbtBase + 1 * stride, stride, stride};
    vkRT.giHitRegion  = {sbtBase + 2 * stride, stride, stride};
    vkRT.giCallRegion = {0, 0, 0};

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI SBT: stride=%u sbtBytes=%u base=0x%llx (3 groups)\n",
                       stride, sbtSize, (unsigned long long)sbtBase);

    // --- Descriptor pool and sets ---
    VkDescriptorPoolSize poolSizes[4] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,     VK_MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes    = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = vkRT.giDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giDescSetLastUpdatedFrameCount[i] = -1;

    // --- GI buffer sampler (linear-clamp, used by interaction shader) ---
    if (vkRT.giSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.giSampler));
    }

    common->Printf("VK RT GI: pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitGICompositePipeline
// Fullscreen additive pipeline that blends the GI buffer onto the framebuffer
// once per view, before the per-light interaction draws.
// ---------------------------------------------------------------------------

static void VK_RT_InitGICompositePipeline(void)
{
    // --- Descriptor set layout: 1 sampler binding ---
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giCompositeDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &vkRT.giCompositeDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giCompositeLayout));

    // --- Shader modules ---
    VkShaderModule vertMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.vert.spv");
    VkShaderModule fragMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.frag.spv");
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI: failed to load composite shaders — composite pass disabled");
        if (vertMod != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, vertMod, NULL);
        if (fragMod != VK_NULL_HANDLE) vkDestroyShaderModule(vk.device, fragMod, NULL);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    // No vertex input — triangle is generated from gl_VertexIndex in the vert shader.
    VkPipelineVertexInputStateCreateInfo vertInput = {};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa = {};
    msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No depth test and no depth write — GI is a post-geometry pass.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

    // Additive blend: GI is added on top of whatever is already in the framebuffer.
    // Alpha: ZERO+ONE so we don't disturb the alpha channel.
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable         = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.colorBlendOp        = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.alphaBlendOp        = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments    = &colorBlend;

    // Dynamic viewport and scissor (composite covers full framebuffer).
    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &msaa;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &blendState;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = vkRT.giCompositeLayout;
    pipelineInfo.renderPass          = vk.renderPass;
    pipelineInfo.subpass             = 0;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.giCompositePipeline));

    vkDestroyShaderModule(vk.device, vertMod, NULL);
    vkDestroyShaderModule(vk.device, fragMod, NULL);

    // --- Descriptor pool and sets (one per frame in flight) ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giCompositeDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giCompositeDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = vkRT.giCompositeDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giCompositeDescSets));

    common->Printf("VK RT GI: composite pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitGI (public entry)
// ---------------------------------------------------------------------------

void VK_RT_InitGI(void)
{
    VK_RT_InitGIPipeline();
    VK_RT_InitGICompositePipeline();
    VK_RT_ResizeGI(vk.swapchainExtent.width, vk.swapchainExtent.height);
}

// ---------------------------------------------------------------------------
// VK_RT_ShutdownGI (public)
// ---------------------------------------------------------------------------

void VK_RT_ShutdownGI(void)
{
    if (vkRT.giDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giDescPool, NULL);
        vkRT.giDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giDescLayout, NULL);
        vkRT.giDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giPipeline, NULL);
        vkRT.giPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giPipelineLayout, NULL);
        vkRT.giPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.sbtGIBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.sbtGIBuffer, NULL);
        vkRT.sbtGIBuffer = VK_NULL_HANDLE;
    }
    if (vkRT.sbtGIMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.sbtGIMemory, NULL);
        vkRT.sbtGIMemory = VK_NULL_HANDLE;
    }
    if (vkRT.giSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.giSampler, NULL);
        vkRT.giSampler = VK_NULL_HANDLE;
    }

    // Composite pipeline cleanup.
    if (vkRT.giCompositeDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giCompositeDescPool, NULL);
        vkRT.giCompositeDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositeDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giCompositeDescLayout, NULL);
        vkRT.giCompositeDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giCompositePipeline, NULL);
        vkRT.giCompositePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositeLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giCompositeLayout, NULL);
        vkRT.giCompositeLayout = VK_NULL_HANDLE;
    }

    VK_RT_DestroyGIImages();
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeGI (public)
// ---------------------------------------------------------------------------

void VK_RT_ResizeGI(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyGIImages();
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giDescSetLastUpdatedFrameCount[i] = -1;
    VK_RT_CreateGIImages(width, height);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGI (public)
// Dispatch GI rays for the current view (once per frame).
// Must be outside a render pass.  Depth must be in ATTACHMENT_OPTIMAL on
// entry; this function transitions to READ_ONLY_OPTIMAL and restores.
// On exit giBuffer[currentFrame] is in GENERAL layout, readable by the
// interaction fragment shader (memory barrier issued before returning).
// ---------------------------------------------------------------------------

// Convert viewDef->scissor (GL Y-up) to VkRect2D (VK Y-down).
static VkRect2D VK_RT_GI_ComputeDispatchRect(const viewDef_t *viewDef)
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

    r.extent.width  = (uint32_t)idMath::ClampInt(1, w - r.offset.x, rw);
    r.extent.height = (uint32_t)idMath::ClampInt(1, h - r.offset.y, rh);
    return r;
}

void VK_RT_DispatchGI(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return;
    if (vkRT.giPipeline == VK_NULL_HANDLE)
    {
        common->Printf("VK RT GI: skip — pipeline is NULL\n");
        return;
    }

    const int frameIdx = vk.currentFrame;

    // Guard against duplicate dispatch in the same frame.
    static int s_lastGIDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastGIDispatchFrame[frameIdx] == tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI: skip duplicate dispatch frame=%d slot=%d\n", tr.frameCount, frameIdx);
        return;
    }
    s_lastGIDispatchFrame[frameIdx] = tr.frameCount;

    vkReflBuffer_t &gb = vkRT.giBuffer[frameIdx];
    if (gb.image == VK_NULL_HANDLE)
    {
        common->Printf("VK RT GI: skip — giBuffer[%d] image is NULL\n", frameIdx);
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: frame=%d slot=%d size=%ux%u\n", tr.frameCount, frameIdx, gb.width, gb.height);

    // --- Depth barrier: ATTACHMENT → READ_ONLY for rgen depth sampling ---
    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    {
        VkImageMemoryBarrier depthToRead = {};
        depthToRead.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthToRead.srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthToRead.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        depthToRead.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthToRead.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthToRead.image            = vk.depthImage;
        depthToRead.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Build UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void    *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    GIParamsUBO ubo = {};

    {
        const float *proj = viewDef->projectionMatrix;
        const float *mv   = viewDef->worldSpace.modelViewMatrix;
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

    // Guard: skip dispatch if the matrix contains NaN (singular VP on degenerate frame).
    {
        bool hasNaN = false;
        for (int i = 0; i < 16; i++)
            if (ubo.invViewProj[i] != ubo.invViewProj[i]) { hasNaN = true; break; }
        if (hasNaN)
        {
            common->Warning("VK RT GI: invViewProj contains NaN — skipping dispatch");
            // Restore depth layout before returning.
            VkImageMemoryBarrier depthRestore = {};
            depthRestore.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            depthRestore.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            depthRestore.dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthRestore.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthRestore.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthRestore.image            = vk.depthImage;
            depthRestore.subresourceRange = {depthAspect, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &depthRestore);
            return;
        }
    }

    VkRect2D dispatchRect = VK_RT_GI_ComputeDispatchRect(viewDef);

    ubo.giRadius      = Max(1.0f, r_rtGIRadius.GetFloat());
    ubo.numSamples    = idMath::ClampInt(1, 8, r_rtGISamples.GetInteger());
    ubo.frameIndex    = (uint32_t)(tr.frameCount);
    ubo.giStrength    = idMath::ClampFloat(0.0f, 4.0f, r_rtGIStrength.GetFloat());
    ubo.screenWidth   = (int32_t)gb.width;
    ubo.screenHeight  = (int32_t)gb.height;
    ubo.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    ubo.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    ubo.scissorExtentX = (int32_t)dispatchRect.extent.width;
    ubo.scissorExtentY = (int32_t)dispatchRect.extent.height;
    memcpy(uboMapped, &ubo, sizeof(GIParamsUBO));

    // --- Update descriptor set (once per frame slot when frameCount changes) ---
    static VkAccelerationStructureKHR s_lastGITlasHandle[VK_MAX_FRAMES_IN_FLIGHT]  = {};
    static VkImageView                s_lastGIStorageView[VK_MAX_FRAMES_IN_FLIGHT]  = {};
    static VkImageView                s_lastGIDepthView[VK_MAX_FRAMES_IN_FLIGHT]    = {};

    const bool resourceChanged = (s_lastGITlasHandle[frameIdx] != vkRT.tlas[frameIdx].handle) ||
                                 (s_lastGIStorageView[frameIdx] != gb.view)                    ||
                                 (s_lastGIDepthView[frameIdx]   != vk.depthSampledView);

    bool refreshSet = (vkRT.giDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount) || resourceChanged;
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.giDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures    = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo giImgInfo = {};
        giImgInfo.imageView   = gb.view;
        giImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler     = vkRT.depthSampler;
        depthInfo.imageView   = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range  = sizeof(GIParamsUBO);

        VkWriteDescriptorSet writes[4] = {};

        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext           = &tlasWrite;
        writes[0].dstSet          = ds;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = ds;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo      = &giImgInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = ds;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &depthInfo;

        writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet          = ds;
        writes[3].dstBinding      = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[3].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(vk.device, 4, writes, 0, NULL);
        vkRT.giDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastGITlasHandle[frameIdx]  = vkRT.tlas[frameIdx].handle;
        s_lastGIStorageView[frameIdx] = gb.view;
        s_lastGIDepthView[frameIdx]   = vk.depthSampledView;
    }

    // --- Dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 0, 1,
                            &vkRT.giDescSets[frameIdx], 1, &uboOff);
    // set=1: material table
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 1, 1,
                            &vkRT.matDescSet, 0, NULL);

    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
    {
        // Nothing to dispatch — still restore depth layout.
    }
    else
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI: dispatch %ux%u offset(%d,%d) strength=%.2f\n",
                           dispatchRect.extent.width, dispatchRect.extent.height,
                           dispatchRect.offset.x, dispatchRect.offset.y, ubo.giStrength);

        vkCmdTraceRaysKHR(cmd, &vkRT.giRgenRegion, &vkRT.giMissRegion, &vkRT.giHitRegion, &vkRT.giCallRegion,
                          dispatchRect.extent.width, dispatchRect.extent.height, 1);
    }

    // --- Barrier: GI write -> fragment shader read ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &memBarrier, 0, NULL, 0, NULL);
    }

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL ---
    {
        VkImageMemoryBarrier depthRestore = {};
        depthRestore.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthRestore.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        depthRestore.dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthRestore.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthRestore.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRestore.image            = vk.depthImage;
        depthRestore.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &depthRestore);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: dispatch complete\n");
}

// ---------------------------------------------------------------------------
// VK_RT_CompositeGI (public)
// Additively blends the GI buffer onto the current framebuffer using a
// fullscreen triangle.  Must be called inside the main render pass, before
// the first per-light interaction draw.  Rebinds no pipeline state that the
// caller cannot restore with a simple vkCmdBindPipeline.
// ---------------------------------------------------------------------------

void VK_RT_CompositeGI(VkCommandBuffer cmd)
{
    if (!r_rtGI.GetBool())
        return;
    if (vkRT.giCompositePipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;
    vkReflBuffer_t &gb = vkRT.giBuffer[frameIdx];
    if (gb.image == VK_NULL_HANDLE || vkRT.giSampler == VK_NULL_HANDLE)
        return;

    // Update the descriptor set for this frame slot.  The GI image may have
    // been recreated (resize), so always write it before drawing.
    VkDescriptorImageInfo imgInfo = {};
    imgInfo.sampler     = vkRT.giSampler;
    imgInfo.imageView   = gb.view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write = {};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = vkRT.giCompositeDescSets[frameIdx];
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    // Full-framebuffer viewport and scissor — GI covers the entire render target.
    // Use the Y-flipped viewport (y = H, height = -H) to match the renderer's
    // OpenGL-convention coordinate system.  This is critical: leaving a non-flipped
    // viewport bound after this draw breaks all subsequent geometry and GUI passes.
    VkViewport vp = {};
    vp.x        = 0.0f;
    vp.y        = (float)vk.swapchainExtent.height;
    vp.width    = (float)vk.swapchainExtent.width;
    vp.height   = -(float)vk.swapchainExtent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D sc = {{0, 0}, vk.swapchainExtent};

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.giCompositePipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.giCompositeLayout,
                            0, 1, &vkRT.giCompositeDescSets[frameIdx], 0, NULL);

    // 3 vertices, no vertex buffer — the vert shader generates the triangle from gl_VertexIndex.
    vkCmdDraw(cmd, 3, 1, 0, 0);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: composite drawn frame=%d slot=%d\n", tr.frameCount, frameIdx);
}
