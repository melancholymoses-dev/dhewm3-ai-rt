/*
===========================================================================

dhewm3-rt Vulkan — vk_vol_froxel.cpp — froxel-grid volumetrics.

World-space (well, frustum-space) cache for volumetric in-scattering, replacing
the per-pixel ray march in vk_vol.cpp.  The grid is filled once per cell instead
of once per pixel per step: at the 160x90x64 default that is ~0.92M cell
evaluations against the current half-res march's ~4.1M step evaluations.

Three passes, added over chunks F0-F2 of 20260906_froxel_probe_gi.md Part A:

  1. fill      (this file, F0) — per-cell in-scattering + extinction
  2. integrate (F2)            — per-column front-to-back Beer-Lambert
  3. resolve   (F1 debug, F2)  — trilinear fetch into vkRT.volBuffer

F0 scope: the grid is allocated and filled, and nothing reads it.  The march
remains the only thing feeding the composite, so enabling r_rtVolFroxel here
costs GPU time and changes no pixels — that is deliberate, it isolates the
world-position mapping for validation before anything depends on it.

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

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

static idCVar r_rtVolFroxel("r_rtVolFroxel", "0", CVAR_RENDERER | CVAR_INTEGER,
                            "Volumetric sampling structure: 0 = per-pixel ray march (vol_march.comp), "
                            "1 = froxel grid. F0: the grid is filled but nothing reads it yet.");

static idCVar r_rtVolFroxelResX("r_rtVolFroxelResX", "160", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid width in cells. Raising this is the first mitigation for soft "
                                "shaft edges; cost is linear. Change forces a device-idle realloc.");
static idCVar r_rtVolFroxelResY("r_rtVolFroxelResY", "90", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid height in cells. Change forces a device-idle realloc.");
static idCVar r_rtVolFroxelResZ("r_rtVolFroxelResZ", "64", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid depth in slices, exponentially distributed out to "
                                "r_rtVolMaxDist. Change forces a device-idle realloc.");

static idCVar r_rtVolFroxelDump("r_rtVolFroxelDump", "0", CVAR_RENDERER | CVAR_BOOL,
                                "One-shot dump of the froxel grid: dimensions, memory, the slice->distance "
                                "table, and the params as uploaded. Self-clears after one frame. Compare the "
                                "slice table against vol_march.comp's step distribution at the same maxDist.");

// ---------------------------------------------------------------------------
// Forward declarations from other vk_*.cpp modules
// ---------------------------------------------------------------------------

extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// vk_vol.cpp — the march's medium/phase/strength model, shared verbatim so an
// r_rtVolFroxel 0/1 A/B differs only in sampling structure.
extern idCVar r_rtVol;
extern idCVar r_rtVolMaxDist;
extern idCVar r_rtVolMaxLights;
extern idCVar r_rtVolDensity;
extern idCVar r_rtVolStrength;
extern idCVar r_rtVolAnisotropy;
extern idCVar r_rtVolDirectedDensity;
extern idCVar r_rtVolDirectedStrength;
extern idCVar r_rtVolDirectedAnisotropy;
extern idCVar r_rtVolFlashlightDensity;
extern idCVar r_rtVolFlashlightStrength;
extern idCVar r_rtVolFlashlightAnisotropy;
extern idCVar r_rtVolWhiteNoiseMix;

// ---------------------------------------------------------------------------
// VolFroxelParamsUBO — must match the std140 VolFroxelParams block in
// vol_froxel_common.glsl, which every froxel pass includes at set=0 binding=2.
//
// vec4-packed on purpose: VolParamsUBO's scalar layout has already cost two
// offset-mismatch debugging sessions, and every member here is 16-byte aligned
// by construction so std140 has nothing to surprise us with.
// ---------------------------------------------------------------------------

struct VolFroxelParamsUBO
{
    float invViewProj[16];  //   0
    float cameraPosW[4];    //  64  xyz = camera world position
    float camForwardW[4];   //  80  xyz = viewaxis[0]
    int32_t gridDim[4];     //  96  xyz = Nx,Ny,Nz   w = cluster shift (F3)
    float depthParams[4];   // 112  x=maxDist y=log(maxDist+1) z=linNum w=linAdd
    float densities[4];     // 128  x=point y=directed z=flashlight w=whiteNoiseMix
    float strengths[4];     // 144  x=point y=directed z=flashlight w=temporalAlpha
    float anisos[4];        // 160  x=point y=directed z=flashlight w=unused
    int32_t misc[4];        // 176  x=frameIndex y=maxLights z=debugMode w=debugSlice
    int32_t screen[4];      // 192  x=screenW y=screenH z=marchW w=marchH (resolve)
    int32_t rect[4];        // 208  resolve dispatch rect, march space
    float prevViewProj[16]; // 224  F4 reprojection; identity until then
};
static_assert(sizeof(VolFroxelParamsUBO) == 288, "VolFroxelParamsUBO size mismatch");

// Cached dimensions the images were actually built at, so a mid-session res
// cvar change can be detected and the grid reallocated rather than silently
// running with a mismatched extent (same pattern as s_volMarchScale).
static int32_t s_froxelDim[3] = {0, 0, 0};

// ---------------------------------------------------------------------------
// Dimensions
// ---------------------------------------------------------------------------

static void VK_RT_VolFroxelRequestedDims(int32_t out[3])
{
    out[0] = idMath::ClampInt(16, 512, r_rtVolFroxelResX.GetInteger());
    out[1] = idMath::ClampInt(16, 512, r_rtVolFroxelResY.GetInteger());
    out[2] = idMath::ClampInt(8, 256, r_rtVolFroxelResZ.GetInteger());
}

static bool VK_RT_VolFroxelDimsChanged(void)
{
    int32_t want[3];
    VK_RT_VolFroxelRequestedDims(want);
    return want[0] != s_froxelDim[0] || want[1] != s_froxelDim[1] || want[2] != s_froxelDim[2];
}

// ---------------------------------------------------------------------------
// 3D image lifecycle
// ---------------------------------------------------------------------------

static bool VK_RT_AllocFroxelImage(vkFroxelGrid_t &g, uint32_t w, uint32_t h, uint32_t d)
{
    g.width = w;
    g.height = h;
    g.depth = d;

    VkImageCreateInfo imgCI = {};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_3D;
    imgCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imgCI.extent = {w, h, d};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgCI, NULL, &g.image));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vk.device, g.image, &memReq);

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
        common->Warning("VK RT Froxel: no device-local memory type for the froxel grid");
        vkDestroyImage(vk.device, g.image, NULL);
        g.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocI = {};
    allocI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocI.allocationSize = memReq.size;
    allocI.memoryTypeIndex = memTypeIdx;
    VK_CHECK(vkAllocateMemory(vk.device, &allocI, NULL, &g.memory));
    VK_CHECK(vkBindImageMemory(vk.device, g.image, g.memory, 0));

    VkImageViewCreateInfo viewCI = {};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = g.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewCI, NULL, &g.view));

    // Transition UNDEFINED -> GENERAL and clear to black, so a pass that reads
    // before the first fill (or reads a cell the fill skipped) sees zeroes
    // rather than garbage.  Same one-shot command buffer idiom as vk_vol.cpp.
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
        b1.image = g.image;
        b1.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b1);

        VkClearColorValue clearBlack = {};
        vkCmdClearColorImage(tmpCmd, g.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

        VkImageMemoryBarrier b2 = {};
        b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b2.image = g.image;
        b2.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b2);

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

    return true;
}

static void VK_RT_FreeFroxelImage(vkFroxelGrid_t &g)
{
    if (g.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(vk.device, g.view, NULL);
        g.view = VK_NULL_HANDLE;
    }
    if (g.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(vk.device, g.image, NULL);
        g.image = VK_NULL_HANDLE;
    }
    if (g.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, g.memory, NULL);
        g.memory = VK_NULL_HANDLE;
    }
    g.width = g.height = g.depth = 0;
}

static void VK_RT_DestroyFroxelImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        VK_RT_FreeFroxelImage(vkRT.froxelScatter[i]);
    s_froxelDim[0] = s_froxelDim[1] = s_froxelDim[2] = 0;
}

// Allocates (or reallocates) the grid at the currently requested dimensions.
// Calls vkDeviceWaitIdle — init path, or the one frame a res cvar changes.
static void VK_RT_CreateFroxelImages(void)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyFroxelImages();

    int32_t dim[3];
    VK_RT_VolFroxelRequestedDims(dim);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocFroxelImage(vkRT.froxelScatter[i], (uint32_t)dim[0], (uint32_t)dim[1], (uint32_t)dim[2]))
        {
            VK_RT_DestroyFroxelImages();
            return;
        }
        vkRT.froxelFillDescSetLastUpdatedFrameCount[i] = -1;
    }

    s_froxelDim[0] = dim[0];
    s_froxelDim[1] = dim[1];
    s_froxelDim[2] = dim[2];

    const double cells = (double)dim[0] * (double)dim[1] * (double)dim[2];
    common->Printf("VK RT Froxel: grid %dx%dx%d (%.0f cells, %.1f MiB x%d slots)\n", dim[0], dim[1], dim[2], cells,
                   cells * 8.0 / (1024.0 * 1024.0), VK_MAX_FRAMES_IN_FLIGHT);
}

// ---------------------------------------------------------------------------
// VK_RT_InitFroxelFillPipeline
//
// Descriptor layout mirrors vol_froxel_fill.comp:
//   set 0, binding 0: ACCELERATION_STRUCTURE_KHR (TLAS)
//   set 0, binding 1: STORAGE_IMAGE              (froxelScatter, 3D, write)
//   set 0, binding 2: UNIFORM_BUFFER_DYNAMIC     (VolFroxelParamsUBO)
//   set 0, binding 3: STORAGE_BUFFER             (vkRT.volLightSsbo -- vol's own
//                                                 selection, NOT giLightSsbo)
//   set 1: vkRT.matDescLayout — shared material table, for light cookies. Only
//          its bindless matTextures[4096] is used here, exactly as vol_march.comp.
//
// Binding 2 is the params UBO in EVERY froxel pass: vol_froxel_common.glsl
// declares the block itself, so the slot is part of that file's contract.
// ---------------------------------------------------------------------------

static void VK_RT_InitFroxelFillPipeline(void)
{
    VkDescriptorSetLayoutBinding bindings[4] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.froxelFillDescLayout));

    VkDescriptorSetLayout setLayouts[2] = {vkRT.froxelFillDescLayout, vkRT.matDescLayout};
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.froxelFillPipelineLayout));

    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/vol_froxel_fill.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Froxel: failed to load vol_froxel_fill.comp.spv — froxel path disabled");
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
    pipelineInfo.layout = vkRT.froxelFillPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.froxelFillPipeline));

    vkDestroyShaderModule(vk.device, compMod, NULL);

    VkDescriptorPoolSize poolSizes[4] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.froxelFillDescPool));

    VkDescriptorSetLayout allocLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        allocLayouts[i] = vkRT.froxelFillDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.froxelFillDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = allocLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.froxelFillDescSets));

    common->Printf("VK RT Froxel: fill pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public lifecycle
// ---------------------------------------------------------------------------

void VK_RT_InitVolFroxel(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.froxelFillDescSetLastUpdatedFrameCount[i] = -1;

    VK_RT_InitFroxelFillPipeline();
    if (vkRT.froxelFillPipeline == VK_NULL_HANDLE)
        return;

    VK_RT_CreateFroxelImages();
}

void VK_RT_ShutdownVolFroxel(void)
{
    if (vkRT.froxelFillDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.froxelFillDescPool, NULL);
        vkRT.froxelFillDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.froxelFillDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.froxelFillDescLayout, NULL);
        vkRT.froxelFillDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.froxelFillPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.froxelFillPipeline, NULL);
        vkRT.froxelFillPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.froxelFillPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.froxelFillPipelineLayout, NULL);
        vkRT.froxelFillPipelineLayout = VK_NULL_HANDLE;
    }

    VK_RT_DestroyFroxelImages();
}

bool VK_RT_VolFroxelActive(void)
{
    if (!vkRT.isInitialized || !r_useRayTracing.GetBool() || !r_rtVol.GetBool())
        return false;
    if (r_rtVolFroxel.GetInteger() != 1)
        return false;
    return vkRT.froxelFillPipeline != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolFroxelFill (public)
//
// One invocation per cell.  Reads the TLAS and the vol light selection; does NOT
// read depth, so unlike the march this needs no depth layout round-trip.
// On exit froxelScatter[currentFrame] is in GENERAL and readable by the
// integrate pass (F2) — nothing consumes it in F0.
// ---------------------------------------------------------------------------

void VK_RT_DispatchVolFroxelFill(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!VK_RT_VolFroxelActive())
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;

    const int frameIdx = vk.currentFrame;

    static int s_lastFillFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastFillFrame[frameIdx] == tr.frameCount)
        return;
    s_lastFillFrame[frameIdx] = tr.frameCount;

    // Res cvars changed since the images were built — reallocate before anything
    // references them.  vkDeviceWaitIdle mid-recording is safe (this command
    // buffer is not submitted yet) and only happens on the frame of the change.
    if (VK_RT_VolFroxelDimsChanged())
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Froxel: grid dims changed, reallocating\n");
        VK_RT_CreateFroxelImages();
    }

    vkFroxelGrid_t &grid = vkRT.froxelScatter[frameIdx];
    if (grid.image == VK_NULL_HANDLE)
        return;
    if (vkRT.volLightSsbo[frameIdx] == VK_NULL_HANDLE)
        return;

    // --- Build UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    VolFroxelParamsUBO ubo = {};

    // invViewProj — same construction as the march's, kept identical so cell
    // positions and march step positions agree (that is what F1's overlay tests).
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

        for (int i = 0; i < 16; i++)
            if (ubo.invViewProj[i] != ubo.invViewProj[i])
            {
                common->Warning("VK RT Froxel: invViewProj NaN — skipping fill");
                return;
            }
    }

    const idVec3 camPos = viewDef->renderView.vieworg;
    const idVec3 &camFwd = viewDef->renderView.viewaxis[0];
    ubo.cameraPosW[0] = camPos.x;
    ubo.cameraPosW[1] = camPos.y;
    ubo.cameraPosW[2] = camPos.z;
    ubo.camForwardW[0] = camFwd.x;
    ubo.camForwardW[1] = camFwd.y;
    ubo.camForwardW[2] = camFwd.z;

    ubo.gridDim[0] = (int32_t)grid.width;
    ubo.gridDim[1] = (int32_t)grid.height;
    ubo.gridDim[2] = (int32_t)grid.depth;
    ubo.gridDim[3] = 0; // cluster shift, F3

    const float maxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
    ubo.depthParams[0] = maxDist;
    ubo.depthParams[1] = idMath::Log(maxDist + 1.0f);
    // Depth linearization constants (resolve, F2) — same idiom as BilateralPC.
    ubo.depthParams[2] = -viewDef->projectionMatrix[14];
    ubo.depthParams[3] = viewDef->projectionMatrix[10];

    ubo.densities[0] = idMath::ClampFloat(0.0f, 1.0f, r_rtVolDensity.GetFloat());
    ubo.densities[1] = idMath::ClampFloat(0.0f, 1.0f, r_rtVolDirectedDensity.GetFloat());
    ubo.densities[2] = idMath::ClampFloat(0.0f, 1.0f, r_rtVolFlashlightDensity.GetFloat());
    ubo.densities[3] = idMath::ClampFloat(0.0f, 1.0f, r_rtVolWhiteNoiseMix.GetFloat());

    ubo.strengths[0] = idMath::ClampFloat(0.0f, 8.0f, r_rtVolStrength.GetFloat());
    ubo.strengths[1] = idMath::ClampFloat(0.0f, 8.0f, r_rtVolDirectedStrength.GetFloat());
    ubo.strengths[2] = idMath::ClampFloat(0.0f, 8.0f, r_rtVolFlashlightStrength.GetFloat());
    ubo.strengths[3] = 0.0f; // temporal alpha, F4

    ubo.anisos[0] = idMath::ClampFloat(0.0f, 0.99f, r_rtVolAnisotropy.GetFloat());
    ubo.anisos[1] = idMath::ClampFloat(0.0f, 0.99f, r_rtVolDirectedAnisotropy.GetFloat());
    ubo.anisos[2] = idMath::ClampFloat(0.0f, 0.99f, r_rtVolFlashlightAnisotropy.GetFloat());
    ubo.anisos[3] = 0.0f;

    ubo.misc[0] = (int32_t)tr.frameCount;
    ubo.misc[1] = idMath::ClampInt(1, 128, r_rtVolMaxLights.GetInteger());
    ubo.misc[2] = 0; // debug mode, F1
    ubo.misc[3] = 0; // debug slice, F1

    ubo.screen[0] = (int32_t)vk.swapchainExtent.width;
    ubo.screen[1] = (int32_t)vk.swapchainExtent.height;
    ubo.screen[2] = (int32_t)vk.swapchainExtent.width;
    ubo.screen[3] = (int32_t)vk.swapchainExtent.height;

    // prevViewProj: identity until F4 reprojection needs it.
    ubo.prevViewProj[0] = ubo.prevViewProj[5] = ubo.prevViewProj[10] = ubo.prevViewProj[15] = 1.0f;

    memcpy(uboMapped, &ubo, sizeof(VolFroxelParamsUBO));

    // --- Update descriptor set (once per frame slot, or when resources move) ---
    static VkAccelerationStructureKHR s_lastTlas[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGridView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool resourceChanged =
        (s_lastTlas[frameIdx] != vkRT.tlas[frameIdx].handle) || (s_lastGridView[frameIdx] != grid.view);

    if (vkRT.froxelFillDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount || resourceChanged)
    {
        VkDescriptorSet ds = vkRT.froxelFillDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo gridInfo = {};
        gridInfo.imageView = grid.view;
        gridInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(VolFroxelParamsUBO);

        VkDescriptorBufferInfo lightSsboInfo = {};
        lightSsboInfo.buffer = vkRT.volLightSsbo[frameIdx];
        lightSsboInfo.offset = 0;
        lightSsboInfo.range = VK_WHOLE_SIZE;

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
        writes[1].pImageInfo = &gridInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = ds;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[2].pBufferInfo = &uboInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = ds;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &lightSsboInfo;

        vkUpdateDescriptorSets(vk.device, 4, writes, 0, NULL);
        vkRT.froxelFillDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastTlas[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastGridView[frameIdx] = grid.view;
    }

    // --- Dispatch: local_size is 4x4x4, see vol_froxel_fill.comp ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelFillPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelFillPipelineLayout, 0, 1,
                            &vkRT.froxelFillDescSets[frameIdx], 1, &uboOff);
    // set=1: shared material table — light cookies, same bind as vol_march.comp.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelFillPipelineLayout, 1, 1, &vkRT.matDescSet,
                            0, NULL);

    const uint32_t groupsX = (grid.width + 3) / 4;
    const uint32_t groupsY = (grid.height + 3) / 4;
    const uint32_t groupsZ = (grid.depth + 3) / 4;
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);

    // Compute write -> compute read, for the integrate pass (F2).
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Froxel: fill frame=%d slot=%d grid=%ux%ux%u groups=%ux%ux%u maxLights=%d\n",
                       tr.frameCount, frameIdx, grid.width, grid.height, grid.depth, groupsX, groupsY, groupsZ,
                       ubo.misc[1]);

    // --- r_rtVolFroxelDump ---
    // The slice table is the thing to check first: it must match the distances
    // vol_march.comp's exp(alpha*logFac) stepping visits at the same maxDist.
    if (r_rtVolFroxelDump.GetBool())
    {
        r_rtVolFroxelDump.SetBool(false);

        const double cells = (double)grid.width * (double)grid.height * (double)grid.depth;
        common->Printf("=== [r_rtVolFroxelDump] frame=%d slot=%d ===\n", tr.frameCount, frameIdx);
        common->Printf("  grid=%ux%ux%u  cells=%.0f  %.2f MiB/slot  slots=%d\n", grid.width, grid.height, grid.depth,
                       cells, cells * 8.0 / (1024.0 * 1024.0), VK_MAX_FRAMES_IN_FLIGHT);
        common->Printf("  camera=(%.0f %.0f %.0f)  forward=(%.3f %.3f %.3f)\n", camPos.x, camPos.y, camPos.z, camFwd.x,
                       camFwd.y, camFwd.z);
        common->Printf("  maxDist=%.1f  logFac=%.4f  linNum=%.4f  linAdd=%.4f\n", ubo.depthParams[0],
                       ubo.depthParams[1], ubo.depthParams[2], ubo.depthParams[3]);
        common->Printf("  point:      density=%.5f strength=%.5f aniso=%.4f\n", ubo.densities[0], ubo.strengths[0],
                       ubo.anisos[0]);
        common->Printf("  directed:   density=%.5f strength=%.5f aniso=%.4f\n", ubo.densities[1], ubo.strengths[1],
                       ubo.anisos[1]);
        common->Printf("  flashlight: density=%.5f strength=%.5f aniso=%.4f\n", ubo.densities[2], ubo.strengths[2],
                       ubo.anisos[2]);
        common->Printf("  maxLights=%d  volLights=%d  whiteNoiseMix=%.4f\n", ubo.misc[1],
                       vkRT.volLightSsboMapped[frameIdx] ? *(const int *)vkRT.volLightSsboMapped[frameIdx] : -1,
                       ubo.densities[3]);
        common->Printf("  slice -> planar view distance (cell centres):\n");
        const int nz = (int)grid.depth;
        const int stride = Max(1, nz / 16);
        for (int z = 0; z < nz; z += stride)
        {
            const float zc = ((float)z + 0.5f) / (float)nz;
            const float d = idMath::Exp(zc * ubo.depthParams[1]) - 1.0f;
            common->Printf("    z=%3d  d=%9.2f\n", z, d);
        }
        common->Printf("    z=%3d  d=%9.2f (far face)\n", nz, ubo.depthParams[0]);
        common->Printf("  sizeof(VolFroxelParamsUBO)=%d (GLSL std140 block expects 288)\n",
                       (int)sizeof(VolFroxelParamsUBO));
    }
}
