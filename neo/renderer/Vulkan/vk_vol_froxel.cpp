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

r_rtVolFroxel 1 selects this path and stands the march, the volumetric temporal
EMA and the bilateral upsample down (VK_RT_VolFroxelActive) — exactly one of the
two paths writes vkRT.volBuffer.  Measured 2026-09-13: 1.63 -> 0.29 ms median,
and the grid's 64 slices over [znear, dFar] integrate more accurately than the
march's 8 steps over [0, r_rtVolMaxDist], whose last step spanned ~300 units.

r_rtVolFroxelDebug hosts the overlays; r_rtVolFroxelDump prints the grid range,
the slice table and a CPU mirror of the ray reconstruction.

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

static idCVar r_rtVolFroxel("r_rtVolFroxel", "1", CVAR_RENDERER | CVAR_INTEGER,
                            "Volumetric sampling structure: 0 = per-pixel ray march (vol_march.comp), "
                            "1 = froxel grid");

static idCVar r_rtVolFroxelResX("r_rtVolFroxelResX", "240", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid width in cells. Raising this is the first mitigation for soft "
                                "shaft edges; cost is linear. Change forces a device-idle realloc.");
static idCVar r_rtVolFroxelResY("r_rtVolFroxelResY", "135", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid height in cells. Change forces a device-idle realloc.");
static idCVar r_rtVolFroxelResZ("r_rtVolFroxelResZ", "96", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid depth in slices, exponentially distributed out to "
                                "r_rtVolMaxDist. Change forces a device-idle realloc.");

// Overlays (F1). All three replace the composite rather than adding to it —
// VK_RT_CompositeVolumetrics switches to the replace pipeline while any of them
// is active, same as r_rtVolDebugMode does for the march.
static idCVar r_rtVolFroxelDebug("r_rtVolFroxelDebug", "0", CVAR_RENDERER | CVAR_INTEGER,
                                 "Froxel grid overlay: 0=off, 1=one Z slice of the scatter grid "
                                 "(r_rtVolFroxelDebugSlice), 2=per-cell light-count heatmap at the pixel's own "
                                 "depth slice, 3=grid-mapping error vs the depth-reconstructed world position "
                                 "(green=agreement). Requires r_rtVolFroxel 1.");

static idCVar r_rtVolFroxelDebugSlice("r_rtVolFroxelDebugSlice", "32", CVAR_RENDERER | CVAR_INTEGER,
                                      "Which Z slice r_rtVolFroxelDebug 1 displays. r_rtVolFroxelDump prints the "
                                      "slice->distance table to pick one with.");

static idCVar r_rtVolFroxelDebugGain("r_rtVolFroxelDebugGain", "20.0", CVAR_RENDERER | CVAR_FLOAT,
                                     "Mode-1-only pre-tonemap gain, so the tiny raw scatter values survive the "
                                     "Uchimura toe curve instead of reading as black. Mirrors r_rtVolDebugGain; "
                                     "modes 2 and 3 output normalised ramps and ignore it.");

// F2: the grid's far anchor is DERIVED from the medium, not taken from r_rtVolMaxDist.
// Must stay derived rather than a constant: raising density shortens the useful
// range, lowering it extends the range back out toward r_rtVolMaxDist.
static idCVar r_rtVolFroxelFarTransmittance(
    "r_rtVolFroxelFarTransmittance", "0.002", CVAR_RENDERER | CVAR_FLOAT,
    "Transmittance floor that sets the froxel grid's far plane: the grid ends where "
    "exp(-density*d) falls to this, capped by r_rtVolMaxDist. Lower = longer range, "
    "coarser cells. 0 disables the derivation and uses r_rtVolMaxDist directly.");

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
    int32_t gridDim[4];     //  96  xyz = Nx,Ny,Nz   w = unused (std140 pad)
    float depthParams[4];   // 112  x=dNear y=dFar z=linNum w=linAdd
    float rangeParams[4];   // 128  x=logRange y=1/logRange z=maxDist w=unused
    float densities[4];     // 144  x=point y=directed z=flashlight w=whiteNoiseMix
    float strengths[4];     // 160  x=point y=directed z=flashlight w=temporalAlpha
    float anisos[4];        // 176  x=point y=directed z=flashlight w=unused
    int32_t misc[4];        // 192  x=frameIndex y=maxLights z=debugMode w=debugSlice
    int32_t screen[4];      // 208  x=screenW y=screenH z=outW w=outH (resolve target)
    int32_t rect[4];        // 224  resolve dispatch rect, resolve-target space
    float prevViewProj[16]; // 240  F4 reprojection; identity until then
};
static_assert(sizeof(VolFroxelParamsUBO) == 304, "VolFroxelParamsUBO size mismatch");

// Cached dimensions the images were actually built at, so a mid-session res
// cvar change can be detected and the grid reallocated rather than silently
// running with a mismatched extent (same pattern as s_volMarchScale).
static int32_t s_froxelDim[3] = {0, 0, 0};

// Dimensions the last allocation attempt FAILED at.  A failed realloc leaves
// s_froxelDim zeroed, which would make VK_RT_VolFroxelDimsChanged true forever:
// the fill would then call VK_RT_CreateFroxelImages — and with it
// vkDeviceWaitIdle — every single frame.  Latching the failed request makes it
// retry only once the cvars actually move again.
static int32_t s_froxelDimFailed[3] = {0, 0, 0};

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

    if (want[0] == s_froxelDimFailed[0] && want[1] == s_froxelDimFailed[1] && want[2] == s_froxelDimFailed[2])
        return false; // already known-bad, do not retry every frame

    return want[0] != s_froxelDim[0] || want[1] != s_froxelDim[1] || want[2] != s_froxelDim[2];
}

// ---------------------------------------------------------------------------
// 3D image lifecycle
// ---------------------------------------------------------------------------

static void VK_RT_FreeFroxelImage(vkFroxelGrid_t &g);

// Allocation here is deliberately NON-FATAL, unlike most VK_CHECK sites in this
// renderer.  r_rtVolFroxelResX/Y/Z clamp to 512x512x256, which is 512 MiB per
// image and ~2 GiB across both images and both frame slots — a setting the
// CVars accept and a memory-constrained device will refuse.  Under VK_CHECK that
// legal setting terminated the renderer.  Returning false instead lets
// VK_RT_VolFroxelActive stand the froxel path down and the ray march carry the
// frame; that fallback only actually works because Active() now tests the grid
// images and not just the pipelines.
static bool VK_RT_AllocFroxelImage(vkFroxelGrid_t &g, uint32_t w, uint32_t h, uint32_t d)
{
    g.width = w;
    g.height = h;
    g.depth = d;

    VkResult vkr = VK_SUCCESS;

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
    VK_CHECK_NONFATAL(vkCreateImage(vk.device, &imgCI, NULL, &g.image), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT Froxel: vkCreateImage failed (%d) for a %ux%ux%u grid image", (int)vkr, w, h, d);
        g.image = VK_NULL_HANDLE;
        VK_RT_FreeFroxelImage(g);
        return false;
    }

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
        VK_RT_FreeFroxelImage(g);
        return false;
    }

    VkMemoryAllocateInfo allocI = {};
    allocI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocI.allocationSize = memReq.size;
    allocI.memoryTypeIndex = memTypeIdx;
    VK_CHECK_NONFATAL(vkAllocateMemory(vk.device, &allocI, NULL, &g.memory), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT Froxel: out of device memory (%d) for a %ux%ux%u grid image (%.1f MiB) — lower "
                        "r_rtVolFroxelResX/Y/Z",
                        (int)vkr, w, h, d, (double)memReq.size / (1024.0 * 1024.0));
        g.memory = VK_NULL_HANDLE;
        VK_RT_FreeFroxelImage(g);
        return false;
    }

    VK_CHECK_NONFATAL(vkBindImageMemory(vk.device, g.image, g.memory, 0), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT Froxel: vkBindImageMemory failed (%d)", (int)vkr);
        VK_RT_FreeFroxelImage(g);
        return false;
    }

    VkImageViewCreateInfo viewCI = {};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = g.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK_NONFATAL(vkCreateImageView(vk.device, &viewCI, NULL, &g.view), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT Froxel: vkCreateImageView failed (%d)", (int)vkr);
        g.view = VK_NULL_HANDLE;
        VK_RT_FreeFroxelImage(g);
        return false;
    }

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
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &b2);

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
    {
        VK_RT_FreeFroxelImage(vkRT.froxelScatter[i]);
        VK_RT_FreeFroxelImage(vkRT.froxelIntegrated[i]);
    }
    s_froxelDim[0] = s_froxelDim[1] = s_froxelDim[2] = 0;
    s_froxelDimFailed[0] = s_froxelDimFailed[1] = s_froxelDimFailed[2] = 0;
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
        if (!VK_RT_AllocFroxelImage(vkRT.froxelScatter[i], (uint32_t)dim[0], (uint32_t)dim[1], (uint32_t)dim[2]) ||
            !VK_RT_AllocFroxelImage(vkRT.froxelIntegrated[i], (uint32_t)dim[0], (uint32_t)dim[1], (uint32_t)dim[2]))
        {
            VK_RT_DestroyFroxelImages();
            // Latch AFTER the destroy — it clears this on the way out.
            s_froxelDimFailed[0] = dim[0];
            s_froxelDimFailed[1] = dim[1];
            s_froxelDimFailed[2] = dim[2];
            common->Warning("VK RT Froxel: grid allocation failed at %dx%dx%d — falling back to the ray march "
                            "(VK_RT_VolFroxelActive is false while the images are null)",
                            dim[0], dim[1], dim[2]);
            return;
        }
        vkRT.froxelFillDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.froxelIntegrateDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.froxelResolveDescSetLastUpdatedFrameCount[i] = -1;
    }

    s_froxelDim[0] = dim[0];
    s_froxelDim[1] = dim[1];
    s_froxelDim[2] = dim[2];

    const double cells = (double)dim[0] * (double)dim[1] * (double)dim[2];
    common->Printf("VK RT Froxel: grid %dx%dx%d (%.0f cells, %.1f MiB/image, 2 images x%d slots)\n", dim[0], dim[1],
                   dim[2], cells, cells * 8.0 / (1024.0 * 1024.0), VK_MAX_FRAMES_IN_FLIGHT);
}

// ---------------------------------------------------------------------------
// VK_RT_BuildFroxelParams
//
// One params block shared by every froxel pass, so the fill's cell positions and
// the resolve's lookups cannot drift apart.  Returns false if the view produced
// a singular view-projection.
// ---------------------------------------------------------------------------

static bool VK_RT_BuildFroxelParams(const viewDef_t *viewDef, const vkFroxelGrid_t &grid, VolFroxelParamsUBO &ubo)
{
    memset(&ubo, 0, sizeof(ubo));

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
                return false;
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
    ubo.gridDim[3] = 0; // unused — F3's cluster cull was dropped 2026-09-13

    const float maxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
    const float density = idMath::ClampFloat(0.0f, 1.0f, r_rtVolDensity.GetFloat());

    // Near anchor = the real near plane, read from the projection rather than from
    // r_znear: znear is game-owned and drops to 1.0 in cinematics, and the matrix
    // is authoritative for the frame we are actually rendering.
    //   ndcZ = -proj[10] + proj[14]/d  =>  d(ndcZ = -1) = proj[14] / (proj[10] - 1)
    float dNear = viewDef->projectionMatrix[14] / (viewDef->projectionMatrix[10] - 1.0f);
    if (!(dNear > 0.01f) || dNear != dNear)
        dNear = 1.0f; // degenerate projection — fall back rather than emit a NaN grid

    // Far anchor derived from the medium (see r_rtVolFroxelFarTransmittance).
    float dFar = maxDist;
    const float tFloor = idMath::ClampFloat(0.0f, 0.99f, r_rtVolFroxelFarTransmittance.GetFloat());
    if (tFloor > 1e-5f && density > 1e-5f)
        dFar = Min(maxDist, -idMath::Log(tFloor) / density);
    dFar = Max(dFar, dNear * 2.0f); // never invert or collapse the range

    const float logRange = Max(idMath::Log(dFar / dNear), 1e-4f);

    ubo.depthParams[0] = dNear;
    ubo.depthParams[1] = dFar;
    // Depth linearization constants — same idiom as BilateralPC in vk_vol.cpp.
    ubo.depthParams[2] = -viewDef->projectionMatrix[14];
    ubo.depthParams[3] = viewDef->projectionMatrix[10];

    ubo.rangeParams[0] = logRange;
    ubo.rangeParams[1] = 1.0f / logRange;
    ubo.rangeParams[2] = maxDist;
    ubo.rangeParams[3] = 0.0f;

    ubo.densities[0] = density;
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
    ubo.anisos[3] = Max(0.0f, r_rtVolFroxelDebugGain.GetFloat());

    ubo.misc[0] = (int32_t)tr.frameCount;
    ubo.misc[1] = idMath::ClampInt(1, 128, r_rtVolMaxLights.GetInteger());
    // The fill reads the debug mode too: mode 2 makes it write a light count into
    // alpha instead of extinction.
    ubo.misc[2] = idMath::ClampInt(0, 3, r_rtVolFroxelDebug.GetInteger());
    ubo.misc[3] = idMath::ClampInt(0, (int)grid.depth - 1, r_rtVolFroxelDebugSlice.GetInteger());

    // screen.xy = full res (depth fetch / NDC), screen.zw = the volBuffer the
    // resolve writes. In froxel mode that is full res too — VK_RT_VolRequestedScale
    // forces r_rtVolHalfRes off, because the resolve is a single trilinear fetch.
    const vkReflBuffer_t &vb = vkRT.volBuffer[vk.currentFrame];
    ubo.screen[0] = (int32_t)vk.swapchainExtent.width;
    ubo.screen[1] = (int32_t)vk.swapchainExtent.height;
    ubo.screen[2] = (int32_t)Max(1u, vb.width);
    ubo.screen[3] = (int32_t)Max(1u, vb.height);

    // Resolve dispatch rect: the view scissor, GL Y-up -> Vulkan Y-down, scaled
    // into the output image. Mirrors VK_RT_Vol_ComputeDispatchRect in vk_vol.cpp
    // (static there, and this needs the same conversion).
    {
        const int fullW = (int)vk.swapchainExtent.width;
        const int fullH = (int)vk.swapchainExtent.height;
        const idScreenRect &s = viewDef->scissor;

        int x0 = idMath::ClampInt(0, fullW - 1, s.x1);
        int y0 = idMath::ClampInt(0, fullH - 1, fullH - 1 - s.y2);
        int rw = s.x2 - s.x1 + 1;
        int rh = s.y2 - s.y1 + 1;

        if (rw <= 0 || rh <= 0)
        {
            // Degenerate scissor — dispatch nothing rather than the whole screen.
            ubo.rect[0] = ubo.rect[1] = ubo.rect[2] = ubo.rect[3] = 0;
        }
        else
        {
            rw = idMath::ClampInt(1, fullW - x0, rw);
            rh = idMath::ClampInt(1, fullH - y0, rh);

            const int outW = ubo.screen[2];
            const int outH = ubo.screen[3];
            const int scale = Max(1, fullW / Max(1, outW));

            const int sx0 = x0 / scale;
            const int sy0 = y0 / scale;
            const int sx1 = (x0 + rw + scale - 1) / scale;
            const int sy1 = (y0 + rh + scale - 1) / scale;

            ubo.rect[0] = sx0;
            ubo.rect[1] = sy0;
            ubo.rect[2] = idMath::ClampInt(0, outW - sx0, sx1 - sx0);
            ubo.rect[3] = idMath::ClampInt(0, outH - sy0, sy1 - sy0);
        }
    }

    // prevViewProj: identity until F4 reprojection needs it.
    ubo.prevViewProj[0] = ubo.prevViewProj[5] = ubo.prevViewProj[10] = ubo.prevViewProj[15] = 1.0f;

    return true;
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
// VK_RT_InitFroxelIntegratePipeline
//
// Descriptor layout mirrors vol_froxel_integrate.comp:
//   set 0, binding 0: STORAGE_IMAGE          (froxelScatter, 3D, read)
//   set 0, binding 1: STORAGE_IMAGE          (froxelIntegrated, 3D, write)
//   set 0, binding 2: UNIFORM_BUFFER_DYNAMIC (VolFroxelParamsUBO)
// ---------------------------------------------------------------------------

static void VK_RT_InitFroxelIntegratePipeline(void)
{
    VkDescriptorSetLayoutBinding bindings[3] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
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

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.froxelIntegrateDescLayout));

    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.froxelIntegrateDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.froxelIntegratePipelineLayout));

    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/vol_froxel_integrate.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Froxel: failed to load vol_froxel_integrate.comp.spv — froxel resolve disabled");
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
    pipelineInfo.layout = vkRT.froxelIntegratePipelineLayout;
    VK_CHECK(
        vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.froxelIntegratePipeline));

    vkDestroyShaderModule(vk.device, compMod, NULL);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 2)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.froxelIntegrateDescPool));

    VkDescriptorSetLayout allocLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        allocLayouts[i] = vkRT.froxelIntegrateDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.froxelIntegrateDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = allocLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.froxelIntegrateDescSets));

    common->Printf("VK RT Froxel: integrate pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitFroxelResolvePipeline
//
// Descriptor layout mirrors vol_froxel_resolve.comp:
//   set 0, binding 0: STORAGE_IMAGE          (froxelScatter, 3D, read)
//   set 0, binding 1: STORAGE_IMAGE          (volBuffer, 2D, write)
//   set 0, binding 2: UNIFORM_BUFFER_DYNAMIC (VolFroxelParamsUBO)
//   set 0, binding 3: COMBINED_IMAGE_SAMPLER (depth)
//   set 0, binding 4: COMBINED_IMAGE_SAMPLER (froxelIntegrated, sampler3D)
//
// No set 1 — the resolve samples no materials.  Binding 4's trilinear filter is
// the whole of the real path: it replaces vol_bilateral.comp's depth-aware
// upsample in XY, and performs the final partial cell's fractional weighting
// in Z.
// ---------------------------------------------------------------------------

static void VK_RT_InitFroxelResolvePipeline(void)
{
    VkDescriptorSetLayoutBinding bindings[5] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
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
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.froxelResolveDescLayout));

    // Own trilinear/clamp sampler rather than borrowing vkRT.volSampler: that one
    // is created at the END of VK_RT_InitVolMarchPipeline and is left NULL if the
    // march shader fails to load, which would silently take the froxel path down
    // with it.
    if (vkRT.froxelSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.froxelSampler));
    }

    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.froxelResolveDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.froxelResolvePipelineLayout));

    VkShaderModule compMod = VK_LoadSPIRV("glprogs/glsl/vol_froxel_resolve.comp.spv");
    if (compMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Froxel: failed to load vol_froxel_resolve.comp.spv — overlays disabled");
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
    pipelineInfo.layout = vkRT.froxelResolvePipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.froxelResolvePipeline));

    vkDestroyShaderModule(vk.device, compMod, NULL);

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 2)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 2)};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.froxelResolveDescPool));

    VkDescriptorSetLayout allocLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        allocLayouts[i] = vkRT.froxelResolveDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.froxelResolveDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = allocLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.froxelResolveDescSets));

    common->Printf("VK RT Froxel: resolve pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public lifecycle
// ---------------------------------------------------------------------------

void VK_RT_InitVolFroxel(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRT.froxelFillDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.froxelIntegrateDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.froxelResolveDescSetLastUpdatedFrameCount[i] = -1;
    }

    VK_RT_InitFroxelFillPipeline();
    if (vkRT.froxelFillPipeline == VK_NULL_HANDLE)
        return;

    VK_RT_InitFroxelIntegratePipeline();
    VK_RT_InitFroxelResolvePipeline();
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

    if (vkRT.froxelIntegrateDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.froxelIntegrateDescPool, NULL);
        vkRT.froxelIntegrateDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.froxelIntegrateDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.froxelIntegrateDescLayout, NULL);
        vkRT.froxelIntegrateDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.froxelIntegratePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.froxelIntegratePipeline, NULL);
        vkRT.froxelIntegratePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.froxelIntegratePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.froxelIntegratePipelineLayout, NULL);
        vkRT.froxelIntegratePipelineLayout = VK_NULL_HANDLE;
    }

    if (vkRT.froxelSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.froxelSampler, NULL);
        vkRT.froxelSampler = VK_NULL_HANDLE;
    }

    if (vkRT.froxelResolveDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.froxelResolveDescPool, NULL);
        vkRT.froxelResolveDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.froxelResolveDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.froxelResolveDescLayout, NULL);
        vkRT.froxelResolveDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.froxelResolvePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.froxelResolvePipeline, NULL);
        vkRT.froxelResolvePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.froxelResolvePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.froxelResolvePipelineLayout, NULL);
        vkRT.froxelResolvePipelineLayout = VK_NULL_HANDLE;
    }

    VK_RT_DestroyFroxelImages();
}

// True when the froxel path OWNS the volumetric result this frame. vk_vol.cpp
// reads this to stand the march, the temporal EMA and the bilateral upsample
// down, and to force the volBuffer back to full resolution.
//
// Requires the whole chain — fill, integrate and resolve — because standing the
// march down while any link is missing would silently leave volBuffer empty and
// the screen fogless, rather than falling back to the path that works.
//
// The GRID IMAGES are part of that chain, not just the pipelines.  Checking only
// the pipelines made this return true after a failed VK_RT_CreateFroxelImages:
// the march stayed stood down, every froxel dispatch early-outed on a null
// image, and the result was permanently fogless — with the overlays silently
// dead too, since VK_RT_VolFroxelDebugMode gates on this.  This is the invariant
// the comment above already claimed and did not enforce.
// "The froxel path is SELECTED and could run" — everything Active() tests except
// the grid images.  Only the fill uses this, and only so it can still reach its
// realloc when the images are missing: gating the fill on Active() instead would
// mean a failed allocation could never be recovered from, because the one code
// path that retries sits behind the condition the failure just falsified.
static bool VK_RT_VolFroxelSelected(void)
{
    if (!vkRT.isInitialized || !r_useRayTracing.GetBool() || !r_rtVol.GetBool())
        return false;
    if (r_rtVolFroxel.GetInteger() != 1)
        return false;
    return vkRT.froxelFillPipeline != VK_NULL_HANDLE && vkRT.froxelIntegratePipeline != VK_NULL_HANDLE &&
           vkRT.froxelResolvePipeline != VK_NULL_HANDLE;
}

bool VK_RT_VolFroxelActive(void)
{
    if (!VK_RT_VolFroxelSelected())
        return false;
    return vkRT.froxelScatter[vk.currentFrame].image != VK_NULL_HANDLE &&
           vkRT.froxelIntegrated[vk.currentFrame].image != VK_NULL_HANDLE;
}

// Non-zero only while an overlay is actually being drawn — vk_vol.cpp reads this
// to switch the composite to its replace pipeline, so the visualization is not
// muddied by additively blending onto the already-lit scene.
int VK_RT_VolFroxelDebugMode(void)
{
    if (!VK_RT_VolFroxelActive())
        return 0;
    if (vkRT.froxelResolvePipeline == VK_NULL_HANDLE)
        return 0;
    return idMath::ClampInt(0, 3, r_rtVolFroxelDebug.GetInteger());
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
    // Selected(), not Active(): this function owns the realloc below, so it has
    // to run when the grid images are missing. It bails on a null grid after the
    // realloc has had its chance.
    if (!VK_RT_VolFroxelSelected())
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

    VolFroxelParamsUBO ubo;
    if (!VK_RT_BuildFroxelParams(viewDef, grid, ubo))
    {
        common->Warning("VK RT Froxel: invViewProj NaN — skipping fill");
        return;
    }

    const idVec3 camPos = viewDef->renderView.vieworg;
    const idVec3 &camFwd = viewDef->renderView.viewaxis[0];

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
        common->Printf("  grid range: dNear=%.3f dFar=%.1f (r_rtVolMaxDist=%.1f, T floor=%.4f)  logRange=%.4f\n",
                       ubo.depthParams[0], ubo.depthParams[1], ubo.rangeParams[2],
                       r_rtVolFroxelFarTransmittance.GetFloat(), ubo.rangeParams[0]);
        common->Printf("  linNum=%.4f  linAdd=%.4f  (transmittance at dFar = %.5f)\n", ubo.depthParams[2],
                       ubo.depthParams[3], idMath::Exp(-ubo.densities[0] * ubo.depthParams[1]));

        // CPU mirror of vf_RayDirForUV, so a broken unprojection is visible in the
        // log instead of only as a wrong-looking overlay. GLSL reads the matrix
        // column-major, hence m[col*4+row].
        //
        // nearDist must be r_znear (3.0 by default, 1.0 in cinematics) and centre
        // zFac must be ~1.0. The infinite far plane (linAdd ~ -0.999) is why the
        // ray cannot be built from an ndcZ=+1 unprojection — see vf_RayDirForUV.
        {
            const auto unproject = [&](float nx, float ny, float nz) -> idVec3 {
                const float *m = ubo.invViewProj;
                const float v[4] = {nx, ny, nz, 1.0f};
                float r[4];
                for (int row = 0; row < 4; row++)
                {
                    r[row] = 0.0f;
                    for (int col = 0; col < 4; col++)
                        r[row] += m[col * 4 + row] * v[col];
                }
                const float invW = (idMath::Fabs(r[3]) > 1e-8f) ? 1.0f / r[3] : 0.0f;
                return idVec3(r[0] * invW, r[1] * invW, r[2] * invW);
            };

            idVec3 centreDir = unproject(0.0f, 0.0f, -1.0f) - camPos;
            const float nearDist = centreDir.Length();
            centreDir.Normalize();

            idVec3 cornerDir = unproject(-1.0f, 1.0f, -1.0f) - camPos;
            cornerDir.Normalize();

            common->Printf("  near-plane dist=%.3f (expect r_znear)  centre zFac=%.4f (expect ~1.0)  "
                           "corner zFac=%.4f\n",
                           nearDist, centreDir * camFwd, cornerDir * camFwd);
        }
        common->Printf("  point:      density=%.5f strength=%.5f aniso=%.4f\n", ubo.densities[0], ubo.strengths[0],
                       ubo.anisos[0]);
        common->Printf("  directed:   density=%.5f strength=%.5f aniso=%.4f\n", ubo.densities[1], ubo.strengths[1],
                       ubo.anisos[1]);
        common->Printf("  flashlight: density=%.5f strength=%.5f aniso=%.4f\n", ubo.densities[2], ubo.strengths[2],
                       ubo.anisos[2]);
        common->Printf("  maxLights=%d  volLights=%d  whiteNoiseMix=%.4f\n", ubo.misc[1],
                       vkRT.volLightSsboMapped[frameIdx] ? *(const int *)vkRT.volLightSsboMapped[frameIdx] : -1,
                       ubo.densities[3]);
        // Cell depth is what the surface-straddle error scales with, so print it
        // next to the distance rather than making the reader difference the column.
        common->Printf("  slice -> planar view distance (cell centres), and cell depth:\n");
        const int nz = (int)grid.depth;
        const int stride = Max(1, nz / 16);
        const float dNearDump = ubo.depthParams[0];
        const float logRangeDump = ubo.rangeParams[0];
        for (int z = 0; z < nz; z += stride)
        {
            const float dC = dNearDump * idMath::Exp(((float)z + 0.5f) / (float)nz * logRangeDump);
            const float dA = dNearDump * idMath::Exp((float)z / (float)nz * logRangeDump);
            const float dB = dNearDump * idMath::Exp(((float)z + 1.0f) / (float)nz * logRangeDump);
            common->Printf("    z=%3d  d=%9.2f  depth=%7.2f\n", z, dC, dB - dA);
        }
        common->Printf("    z=%3d  d=%9.2f (far face)\n", nz, ubo.depthParams[1]);
        common->Printf("  sizeof(VolFroxelParamsUBO)=%d (GLSL std140 block expects 304)\n",
                       (int)sizeof(VolFroxelParamsUBO));
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolFroxelIntegrate (public)
//
// One invocation per grid COLUMN — 14400 of them at the 160x90 default, against
// the march's ~500k per-pixel integrations.  Reads no depth, so no layout
// round-trip.  Must follow the fill's compute->compute barrier.
// ---------------------------------------------------------------------------

void VK_RT_DispatchVolFroxelIntegrate(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!VK_RT_VolFroxelActive())
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;

    const int frameIdx = vk.currentFrame;

    static int s_lastIntegrateFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastIntegrateFrame[frameIdx] == tr.frameCount)
        return;
    s_lastIntegrateFrame[frameIdx] = tr.frameCount;

    vkFroxelGrid_t &src = vkRT.froxelScatter[frameIdx];
    vkFroxelGrid_t &dst = vkRT.froxelIntegrated[frameIdx];
    if (src.image == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE)
        return;

    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    VolFroxelParamsUBO ubo;
    if (!VK_RT_BuildFroxelParams(viewDef, src, ubo))
    {
        common->Warning("VK RT Froxel: invViewProj NaN — skipping integrate");
        return;
    }
    memcpy(uboMapped, &ubo, sizeof(VolFroxelParamsUBO));

    static VkImageView s_lastSrcView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastDstView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool resourceChanged = (s_lastSrcView[frameIdx] != src.view) || (s_lastDstView[frameIdx] != dst.view);

    if (vkRT.froxelIntegrateDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount || resourceChanged)
    {
        VkDescriptorSet ds = vkRT.froxelIntegrateDescSets[frameIdx];

        VkDescriptorImageInfo srcInfo = {};
        srcInfo.imageView = src.view;
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dstInfo = {};
        dstInfo.imageView = dst.view;
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(VolFroxelParamsUBO);

        VkWriteDescriptorSet writes[3] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = ds;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &srcInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = ds;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dstInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = ds;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[2].pBufferInfo = &uboInfo;

        vkUpdateDescriptorSets(vk.device, 3, writes, 0, NULL);
        vkRT.froxelIntegrateDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastSrcView[frameIdx] = src.view;
        s_lastDstView[frameIdx] = dst.view;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelIntegratePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelIntegratePipelineLayout, 0, 1,
                            &vkRT.froxelIntegrateDescSets[frameIdx], 1, &uboOff);

    const uint32_t groupsX = (src.width + 7) / 8;
    const uint32_t groupsY = (src.height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Compute write -> compute sampled read (resolve). SHADER_READ covers the
    // sampler3D fetch; the image stays in GENERAL throughout.
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Froxel: integrate frame=%d slot=%d columns=%ux%u dNear=%.2f dFar=%.1f\n", tr.frameCount,
                       frameIdx, src.width, src.height, ubo.depthParams[0], ubo.depthParams[1]);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolFroxelResolve (public)
//
// F1: runs only while an overlay is selected, and writes the overlay into
// vkRT.volBuffer[currentFrame] so vol_composite.frag can display it with no
// knowledge of the grid.  volReadView is repointed at volBuffer because the
// temporal/bilateral passes will have aimed it at their own outputs earlier in
// the frame.
//
// Ordering note: this runs AFTER the temporal EMA has consumed volBuffer, so
// overwriting it here cannot poison the history — the march refills volBuffer at
// the top of the next frame, before temporal reads it again.
//
// Must be called outside a render pass; depth must be in ATTACHMENT_OPTIMAL.
// ---------------------------------------------------------------------------

void VK_RT_DispatchVolFroxelResolve(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!VK_RT_VolFroxelActive())
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;

    const int frameIdx = vk.currentFrame;

    static int s_lastResolveFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastResolveFrame[frameIdx] == tr.frameCount)
        return;
    s_lastResolveFrame[frameIdx] = tr.frameCount;

    vkFroxelGrid_t &grid = vkRT.froxelScatter[frameIdx];
    vkFroxelGrid_t &integrated = vkRT.froxelIntegrated[frameIdx];
    vkReflBuffer_t &vb = vkRT.volBuffer[frameIdx];
    if (grid.image == VK_NULL_HANDLE || integrated.image == VK_NULL_HANDLE || vb.image == VK_NULL_HANDLE)
        return;

    // Claim volReadView up front, not after the dispatch: temporal and bilateral
    // stand down in froxel mode, so whatever they last pointed it at (volHistory
    // or volBlurred) would otherwise persist — and at the old half resolution —
    // through any early-out below. A stale volBuffer is the right failure here;
    // a stale differently-sized image is not.
    vkRT.volReadView[frameIdx] = vb.view;

    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    // --- Depth barrier: ATTACHMENT -> READ_ONLY for compute sampling ---
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

    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    VolFroxelParamsUBO ubo;
    if (!VK_RT_BuildFroxelParams(viewDef, grid, ubo))
    {
        common->Warning("VK RT Froxel: invViewProj NaN — skipping resolve");
        VkImageMemoryBarrier restore = {};
        restore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        restore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        restore.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        restore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        restore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        restore.image = vk.depthImage;
        restore.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, NULL, 0, NULL, 1, &restore);
        return;
    }
    memcpy(uboMapped, &ubo, sizeof(VolFroxelParamsUBO));

    // --- Update descriptor set ---
    static VkImageView s_lastGridView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastIntView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastVolView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastDepthView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool resourceChanged = (s_lastGridView[frameIdx] != grid.view) ||
                                 (s_lastIntView[frameIdx] != integrated.view) || (s_lastVolView[frameIdx] != vb.view) ||
                                 (s_lastDepthView[frameIdx] != vk.depthSampledView);

    if (vkRT.froxelResolveDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount || resourceChanged)
    {
        VkDescriptorSet ds = vkRT.froxelResolveDescSets[frameIdx];

        VkDescriptorImageInfo gridInfo = {};
        gridInfo.imageView = grid.view;
        gridInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo volInfo = {};
        volInfo.imageView = vb.view;
        volInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(VolFroxelParamsUBO);

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo integratedInfo = {};
        integratedInfo.sampler = vkRT.froxelSampler;
        integratedInfo.imageView = integrated.view;
        integratedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[5] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = ds;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &gridInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = ds;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &volInfo;

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
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &depthInfo;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = ds;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo = &integratedInfo;

        vkUpdateDescriptorSets(vk.device, 5, writes, 0, NULL);
        vkRT.froxelResolveDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastGridView[frameIdx] = grid.view;
        s_lastIntView[frameIdx] = integrated.view;
        s_lastVolView[frameIdx] = vb.view;
        s_lastDepthView[frameIdx] = vk.depthSampledView;
    }

    // We are about to overwrite volBuffer. VK_RT_VolFroxelActive stands the march
    // and the temporal EMA down whenever this runs, so today nothing else in the
    // frame touches it — but this image is the hand-off point between two paths
    // and the barrier is what keeps that true if the ordering is ever revisited.
    {
        VkMemoryBarrier volBarrier = {};
        volBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        volBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        volBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &volBarrier, 0, NULL, 0, NULL);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelResolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelResolvePipelineLayout, 0, 1,
                            &vkRT.froxelResolveDescSets[frameIdx], 1, &uboOff);

    const uint32_t groupsX = ((uint32_t)ubo.rect[2] + 7) / 8;
    const uint32_t groupsY = ((uint32_t)ubo.rect[3] + 7) / 8;
    if (groupsX > 0 && groupsY > 0)
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Compute write -> fragment read (composite).
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
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
        common->Printf("VK RT Froxel: resolve frame=%d slot=%d mode=%d slice=%d rect=(%d,%d %dx%d) out=%ux%u\n",
                       tr.frameCount, frameIdx, ubo.misc[2], ubo.misc[3], ubo.rect[0], ubo.rect[1], ubo.rect[2],
                       ubo.rect[3], vb.width, vb.height);
}
