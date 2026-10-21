/*
===========================================================================

dhewm3-rt Vulkan — vk_vol_froxel.cpp — froxel-grid volumetrics.

Frustum-space cache for volumetric in-scattering; the engine's only volumetric
integrator.  The grid is filled once per cell rather than once per pixel per step.

Three passes:

  1. fill      — per-cell in-scattering + extinction
  2. integrate — per-column front-to-back Beer-Lambert
  3. resolve   — trilinear fetch (or debug overlay) into vkRT.volBuffer

The resolve is the sole writer of vkRT.volBuffer; VK_RT_CompositeVolumetrics
stands itself down when VK_RT_VolFroxelActive is false, since nothing else fills
that image.

r_rtVolFroxelDebug hosts the two overlays: 2 = per-cell light count, 3 = the
grid-to-world mapping check.

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
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

static idCVar r_rtVolFroxelResX("r_rtVolFroxelResX", "240", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid width in cells. Raising this is the first mitigation for soft "
                                "shaft edges; cost is linear. Change forces a device-idle realloc.");
static idCVar r_rtVolFroxelResY("r_rtVolFroxelResY", "135", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid height in cells. Change forces a device-idle realloc.");
static idCVar r_rtVolFroxelResZ("r_rtVolFroxelResZ", "96", CVAR_RENDERER | CVAR_INTEGER,
                                "Froxel grid depth in slices, exponentially distributed out to "
                                "r_rtVolMaxDist. Change forces a device-idle realloc.");

// Overlays replace the composite rather than adding to it.  Retired numbers stay
// reserved; VK_RT_ValidFroxelDebugMode coerces them to off.
static idCVar r_rtVolFroxelDebug("r_rtVolFroxelDebug", "0", CVAR_RENDERER | CVAR_INTEGER,
                                 "Froxel grid overlay: 0=off, 2=per-cell light-count heatmap at the pixel's own "
                                 "depth slice, 3=grid-mapping error vs the depth-reconstructed world position "
                                 "(green=agreement).");

// One place decides which overlay numbers exist, so the UBO and the composite's
// replace-pipeline switch can never disagree about whether an overlay is active.
static int VK_RT_ValidFroxelDebugMode(void)
{
    const int mode = r_rtVolFroxelDebug.GetInteger();
    return (mode == 2 || mode == 3) ? mode : 0;
}

static idCVar r_rtVolProjEmitterBias("r_rtVolProjEmitterBias", "24", CVAR_RENDERER | CVAR_FLOAT,
                                     "Projected lights only: the shadow ray stops this many units short of the "
                                     "emitter, so the lamp housing the light sits inside doesn't occlude its own "
                                     "beam. 0 = stop at the emitter.");

static idCVar r_rtVolProjFalloff("r_rtVolProjFalloff", "2", CVAR_RENDERER | CVAR_INTEGER,
                                 "Projected-light intensity along the beam, x = distance / "
                                 "r_rtVolProjFalloffDist: 0 = linear over the light's full reach (nearly flat "
                                 "inside the fog range), 1 = max(0, 1 - x^2), 2 = 1 / (1 + x^2).");

static idCVar r_rtVolProjFalloffDist("r_rtVolProjFalloffDist", "512", CVAR_RENDERER | CVAR_FLOAT,
                                     "Distance along the beam where r_rtVolProjFalloff modes 1 and 2 reach "
                                     "x = 1 (0 intensity for mode 1, half for mode 2).");

// The grid's far anchor is derived from the medium, so raising density shortens the
// range and lowering it extends it back toward r_rtVolMaxDist.
static idCVar r_rtVolFroxelFarTransmittance(
    "r_rtVolFroxelFarTransmittance", "0.002", CVAR_RENDERER | CVAR_FLOAT,
    "Transmittance floor that sets the froxel grid's far plane: the grid ends where "
    "exp(-density*d) falls to this, capped by r_rtVolMaxDist. Lower = longer range, "
    "coarser cells. 0 disables the derivation and uses r_rtVolMaxDist directly.");

// ---------------------------------------------------------------------------
// Forward declarations from other vk_*.cpp modules
// ---------------------------------------------------------------------------

extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;

// vk_vol.cpp owns the medium/phase/strength model; the fill reads it from there.
extern idCVar r_rtVol;
extern idCVar r_rtVolMaxDist;
extern idCVar r_rtVolMaxLights;
// The medium coefficients themselves come from VK_RT_VolExtinction/Albedo/
// ScatterScale rather than the cvars, so the two integrators cannot drift apart.
extern idCVar r_rtVolAnisotropy;
extern idCVar r_rtVolDirectedAnisotropy;
extern idCVar r_rtVolFlashlightAnisotropy;
extern idCVar r_rtVolWhiteNoiseMix;
extern idCVar r_rtVolIsotropicMix;

// ---------------------------------------------------------------------------
// VolFroxelParamsUBO — must match the std140 VolFroxelParams block in
// vol_froxel_common.glsl, which every froxel pass includes at set=0 binding=2.
//
// vec4-packed on purpose: a scalar std140 layout has already cost two
// offset-mismatch debugging sessions elsewhere, and every member here is 16-byte
// aligned by construction so std140 has nothing to surprise us with.
// ---------------------------------------------------------------------------

struct VolFroxelParamsUBO
{
    float invViewProj[16];  //   0
    float cameraPosW[4];    //  64  xyz = camera world position
    float camForwardW[4];   //  80  xyz = viewaxis[0]
    int32_t gridDim[4];     //  96  xyz = Nx,Ny,Nz   w = projected-light falloff mode
    float depthParams[4];   // 112  x=dNear y=dFar z=linNum w=linAdd
    float rangeParams[4];   // 128  x=logRange y=1/logRange z=maxDist w=unused
    float densities[4];     // 144  x=sigma_t y=albedo(diag) z=unused w=whiteNoiseMix
    float strengths[4];     // 160  xyz=sigma_s*gain per class  w=isotropicMix
    float anisos[4];        // 176  x=point y=directed z=flashlight w=unused
    int32_t misc[4];        // 192  x=frameIndex y=maxLights z=debugMode w=unused
    int32_t screen[4];      // 208  x=screenW y=screenH z=outW w=outH (resolve target)
    int32_t rect[4];        // 224  resolve dispatch rect, resolve-target space
    float prevViewProj[16]; // 240  F4 reprojection; identity until then
};
static_assert(sizeof(VolFroxelParamsUBO) == 304, "VolFroxelParamsUBO size mismatch");

// Cached dimensions the images were actually built at, so a mid-session res
// cvar change can be detected and the grid reallocated rather than silently
// running with a mismatched extent.
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

// 1x1x1 black stand-ins handed out by VK_RT_GetFroxelScatterView/DirView when the
// froxel path is standing down, so consumers always have a valid 3D view to bind.
// Two of them because the pair differ in format.
static vkFroxelGrid_t s_nullFroxelGrid;
static vkFroxelGrid_t s_nullFroxelDirGrid;

// Mean-direction grid: snorm8 is ample for a phase direction (~0.8 deg) and halves memory
// vs rgba16f (12.4 vs 24.9 MiB per slot at 240x135x96).  Storage and linear filtering of
// R8G8B8A8_SNORM are mandatory, so no capability check.
#define VK_FROXEL_SCATTER_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define VK_FROXEL_DIR_FORMAT VK_FORMAT_R8G8B8A8_SNORM

// Allocation here is deliberately NON-FATAL, unlike most VK_CHECK sites in this
// renderer.  r_rtVolFroxelResX/Y/Z clamp to 512x512x256, which is 512 MiB per
// image and ~2 GiB across both images and both frame slots — a setting the
// CVars accept and a memory-constrained device will refuse.  Under VK_CHECK that
// legal setting terminated the renderer.  Returning false instead lets
// VK_RT_VolFroxelActive report false, which stands the whole vol chain down for
// the frame (the composite included) rather than killing the renderer; that only
// works because Active() tests the grid images, not just the pipelines.
static bool VK_RT_AllocFroxelImage(vkFroxelGrid_t &g, uint32_t w, uint32_t h, uint32_t d,
                                   VkFormat format = VK_FROXEL_SCATTER_FORMAT)
{
    g.width = w;
    g.height = h;
    g.depth = d;

    VkResult vkr = VK_SUCCESS;

    VkImageCreateInfo imgCI = {};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_3D;
    imgCI.format = format;
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
    viewCI.format = format;
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
        VK_Prof_WaitForFences(VKPROF_WAIT_VOL, 1, &fence, VK_TRUE, UINT64_MAX);
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
        VK_RT_FreeFroxelImage(vkRT.froxelDir[i]);
        VK_RT_FreeFroxelImage(vkRT.froxelIntegrated[i]);
    }
    s_froxelDim[0] = s_froxelDim[1] = s_froxelDim[2] = 0;
    s_froxelDimFailed[0] = s_froxelDimFailed[1] = s_froxelDimFailed[2] = 0;
}

// Allocates (or reallocates) the grid at the currently requested dimensions.
// Calls vkDeviceWaitIdle — init path, or the one frame a res cvar changes.
static void VK_RT_CreateFroxelImages(void)
{
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_VOL);
    VK_RT_DestroyFroxelImages();

    int32_t dim[3];
    VK_RT_VolFroxelRequestedDims(dim);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocFroxelImage(vkRT.froxelScatter[i], (uint32_t)dim[0], (uint32_t)dim[1], (uint32_t)dim[2]) ||
            !VK_RT_AllocFroxelImage(vkRT.froxelIntegrated[i], (uint32_t)dim[0], (uint32_t)dim[1], (uint32_t)dim[2]) ||
            !VK_RT_AllocFroxelImage(vkRT.froxelDir[i], (uint32_t)dim[0], (uint32_t)dim[1], (uint32_t)dim[2],
                                    VK_FROXEL_DIR_FORMAT))
        {
            VK_RT_DestroyFroxelImages();
            // Latch AFTER the destroy — it clears this on the way out.
            s_froxelDimFailed[0] = dim[0];
            s_froxelDimFailed[1] = dim[1];
            s_froxelDimFailed[2] = dim[2];
            common->Warning("VK RT Froxel: grid allocation failed at %dx%dx%d — volumetrics off this frame "
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

// The view-projection the grid is defined against — GL convention, and jittered,
// because viewDef->projectionMatrix carries the FSR sub-pixel offset. Shared with
// VK_RT_GetFroxelSampleParams so the forward and inverse cannot drift apart.
static void VK_RT_BuildFroxelViewProj(const viewDef_t *viewDef, float vp[16])
{
    const float *proj = viewDef->projectionMatrix;
    const float *mv = viewDef->worldSpace.modelViewMatrix;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            vp[c * 4 + r] = 0.0f;
            for (int k = 0; k < 4; k++)
                vp[c * 4 + r] += proj[k * 4 + r] * mv[c * 4 + k];
        }
}

static bool VK_RT_BuildFroxelParams(const viewDef_t *viewDef, const vkFroxelGrid_t &grid, VolFroxelParamsUBO &ubo)
{
    memset(&ubo, 0, sizeof(ubo));

    // invViewProj, built the same way the resolve reconstructs world position, so
    // cell centres and reconstructed pixel positions agree (overlay 3 tests it).
    {
        float vp[16];
        VK_RT_BuildFroxelViewProj(viewDef, vp);
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
    ubo.gridDim[3] = idMath::ClampInt(0, 2, r_rtVolProjFalloff.GetInteger());

    const float maxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
    const float sigmaT = VK_RT_VolExtinction();

    // Near anchor from the projection, not r_znear (game-owned, drops to 1.0 in cinematics).
    //   ndcZ = -proj[10] + proj[14]/d  =>  d(ndcZ = -1) = proj[14] / (proj[10] - 1)
    float dNear = viewDef->projectionMatrix[14] / (viewDef->projectionMatrix[10] - 1.0f);
    if (!(dNear > 0.01f) || dNear != dNear)
        dNear = 1.0f; // degenerate projection — fall back rather than emit a NaN grid

    // Far anchor from the medium (r_rtVolFroxelFarTransmittance).  At a physical sigma_t
    // the Min() wins and the grid covers r_rtVolMaxDist; the clamp only bites at smoke density.
    float dFar = maxDist;
    const float tFloor = idMath::ClampFloat(0.0f, 0.99f, r_rtVolFroxelFarTransmittance.GetFloat());
    if (tFloor > 1e-5f && sigmaT > 1e-5f)
        dFar = Min(maxDist, -idMath::Log(tFloor) / sigmaT);
    dFar = Max(dFar, dNear * 2.0f); // never invert or collapse the range

    const float logRange = Max(idMath::Log(dFar / dNear), 1e-4f);

    ubo.depthParams[0] = dNear;
    ubo.depthParams[1] = dFar;
    // Depth linearization constants — see vf_LinearDepth in vol_froxel_common.glsl.
    ubo.depthParams[2] = -viewDef->projectionMatrix[14];
    ubo.depthParams[3] = viewDef->projectionMatrix[10];

    ubo.rangeParams[0] = logRange;
    ubo.rangeParams[1] = 1.0f / logRange;
    ubo.rangeParams[2] = maxDist;
    ubo.rangeParams[3] = Max(0.0f, r_rtVolProjEmitterBias.GetFloat());

    // x = sigma_t, the per-cell extinction the fill writes to alpha and the
    // integrate pass exponentiates.  y/z are free since the per-class densities
    // folded into strengths[] below; w is unrelated (jitter).
    ubo.densities[0] = sigmaT;
    ubo.densities[1] = VK_RT_VolAlbedo(); // diagnostic only — dump reads it
    ubo.densities[2] = Max(1.0f, r_rtVolProjFalloffDist.GetFloat());
    ubo.densities[3] = idMath::ClampFloat(0.0f, 1.0f, r_rtVolWhiteNoiseMix.GetFloat());

    // sigma_s * per-class gain, folded CPU-side: the fill now does one multiply
    // instead of density*strength, and the class scale means exactly one thing.
    ubo.strengths[0] = VK_RT_VolScatterScale(VK_VOL_CLASS_POINT);
    ubo.strengths[1] = VK_RT_VolScatterScale(VK_VOL_CLASS_DIRECTED);
    ubo.strengths[2] = VK_RT_VolScatterScale(VK_VOL_CLASS_FLASHLIGHT);
    // Two-lobe phase blend (see PhaseFunction in vol_froxel_fill.comp).
    ubo.strengths[3] = idMath::ClampFloat(0.0f, 1.0f, r_rtVolIsotropicMix.GetFloat());

    ubo.anisos[0] = idMath::ClampFloat(0.0f, 0.99f, r_rtVolAnisotropy.GetFloat());
    ubo.anisos[1] = idMath::ClampFloat(0.0f, 0.99f, r_rtVolDirectedAnisotropy.GetFloat());
    ubo.anisos[2] = idMath::ClampFloat(0.0f, 0.99f, r_rtVolFlashlightAnisotropy.GetFloat());
    ubo.anisos[3] = 0;

    ubo.misc[0] = (int32_t)tr.frameCount;
    ubo.misc[1] = idMath::ClampInt(1, 128, r_rtVolMaxLights.GetInteger());
    // The fill reads the debug mode too: mode 2 makes it write a light count into
    // alpha instead of extinction.
    ubo.misc[2] = VK_RT_ValidFroxelDebugMode();
    ubo.misc[3] = 0;

    // screen.xy = render res (depth fetch / NDC), screen.zw = the volBuffer the
    // resolve writes, which is display-sized.
    const vkRTImage_t &vb = vkRT.volBuffer[vk.currentFrame];
    ubo.screen[0] = (int32_t)vk.renderExtent.width;
    ubo.screen[1] = (int32_t)vk.renderExtent.height;
    ubo.screen[2] = (int32_t)Max(1u, vb.width);
    ubo.screen[3] = (int32_t)Max(1u, vb.height);

    // Resolve dispatch rect: the view scissor, GL Y-up -> Vulkan Y-down, scaled
    // into the output image. Mirrors VK_RT_Vol_ComputeDispatchRect in vk_vol.cpp
    // (static there, and this needs the same conversion).
    {
        const int fullW = (int)vk.renderExtent.width;
        const int fullH = (int)vk.renderExtent.height;
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

    // prevViewProj: reserved for reprojection; identity for now.
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
//   set 0, binding 4: STORAGE_IMAGE              (froxelDir, 3D, write)
//   set 1: vkRT.matDescLayout — shared material table, for light cookies. Only
//          its bindless matTextures[4096] is used here.
//
// Binding 2 is the params UBO in EVERY froxel pass: vol_froxel_common.glsl
// declares the block itself, so the slot is part of that file's contract.
// ---------------------------------------------------------------------------

static void VK_RT_InitFroxelFillPipeline(void)
{
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
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
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
    // froxelScatter + froxelDir
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 2)};
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
//   set 0, binding 3: STORAGE_IMAGE          (froxelDir, 3D, read)
//
// Binding 3 rather than 2 for the direction grid because binding 2 is the params
// UBO in every froxel pass (vol_froxel_common.glsl's contract).
// ---------------------------------------------------------------------------

static void VK_RT_InitFroxelIntegratePipeline(void)
{
    VkDescriptorSetLayoutBinding bindings[4] = {};

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
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
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
    // scatter (read) + integrated (write) + dir (read)
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 3)};
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
// the whole of the real path: it does the depth-aware upsample in XY, and performs
// the final partial cell's fractional weighting in Z.
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

    // Own trilinear/clamp sampler rather than borrowing vkRT.volSampler: this one
    // filters a 3D grid, and owning it keeps the froxel path independent of whether
    // the composite pipeline came up.
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
    // Lazily built by VK_RT_GetFroxelScatterView/DirView when the froxel path is down.
    VK_RT_FreeFroxelImage(s_nullFroxelGrid);
    VK_RT_FreeFroxelImage(s_nullFroxelDirGrid);

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

static bool VK_RT_VolFroxelSelected(void)
{
    if (!vkRT.isInitialized || !r_useRayTracing.GetBool() || !r_rtVol.GetBool())
        return false;
    return vkRT.froxelFillPipeline != VK_NULL_HANDLE && vkRT.froxelIntegratePipeline != VK_NULL_HANDLE &&
           vkRT.froxelResolvePipeline != VK_NULL_HANDLE;
}

bool VK_RT_VolFroxelActive(void)
{
    if (!VK_RT_VolFroxelSelected())
        return false;
    return vkRT.froxelScatter[vk.currentFrame].image != VK_NULL_HANDLE &&
           vkRT.froxelDir[vk.currentFrame].image != VK_NULL_HANDLE &&
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
    return VK_RT_ValidFroxelDebugMode();
}

// ---------------------------------------------------------------------------
// Sampling the scatter grid from outside this file — see vk_raytracing.h.
// ---------------------------------------------------------------------------

VkImageView VK_RT_GetFroxelScatterView(void)
{
    const vkFroxelGrid_t &grid = vkRT.froxelScatter[vk.currentFrame];
    if (VK_RT_VolFroxelActive() && grid.view != VK_NULL_HANDLE)
        return grid.view;

    // Black stand-in: scatter 0 and extinction 0, so a caller that samples it
    // anyway accumulates exactly nothing.
    if (s_nullFroxelGrid.view == VK_NULL_HANDLE)
    {
        if (!VK_RT_AllocFroxelImage(s_nullFroxelGrid, 1, 1, 1))
            return VK_NULL_HANDLE;
    }
    return s_nullFroxelGrid.view;
}

VkImageView VK_RT_GetFroxelDirView(void)
{
    const vkFroxelGrid_t &grid = vkRT.froxelDir[vk.currentFrame];
    if (VK_RT_VolFroxelActive() && grid.view != VK_NULL_HANDLE)
        return grid.view;

    // Zeroed stand-in: no dominant direction, which rt_PhaseFromMeanDir reads as
    // isotropic.  Pairs with the black scatter grid, so a caller that samples both
    // anyway still accumulates nothing.
    if (s_nullFroxelDirGrid.view == VK_NULL_HANDLE)
    {
        if (!VK_RT_AllocFroxelImage(s_nullFroxelDirGrid, 1, 1, 1, VK_FROXEL_DIR_FORMAT))
            return VK_NULL_HANDLE;
    }
    return s_nullFroxelDirGrid.view;
}

VkSampler VK_RT_GetFroxelSampler(void)
{
    return vkRT.froxelSampler;
}

bool VK_RT_GetFroxelSampleParams(const viewDef_t *viewDef, vkFroxelSampleParams_t *out)
{
    if (out == NULL || viewDef == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    if (!VK_RT_VolFroxelActive())
        return false;

    const vkFroxelGrid_t &grid = vkRT.froxelScatter[vk.currentFrame];
    if (grid.image == VK_NULL_HANDLE)
        return false;

    // Through the same builder the fill uses, so the dNear/dFar derivation is
    // shared rather than repeated.
    VolFroxelParamsUBO ubo;
    if (!VK_RT_BuildFroxelParams(viewDef, grid, ubo))
        return false;

    VK_RT_BuildFroxelViewProj(viewDef, out->viewProj);
    for (int i = 0; i < 3; i++)
    {
        out->camPos[i] = ubo.cameraPosW[i];
        out->camFwd[i] = ubo.camForwardW[i];
    }
    out->dNear = ubo.depthParams[0];
    out->logRange = ubo.rangeParams[0];
    out->invLogRange = ubo.rangeParams[1];
    out->nz = ubo.gridDim[2];
    return true;
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolFroxelFill (public)
//
// One invocation per cell.  Reads the TLAS and the vol light selection; does NOT
// read depth, so it needs no depth layout round-trip.
// On exit froxelScatter[currentFrame] is in GENERAL and readable by the
// integrate pass.
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
    vkFroxelGrid_t &dirGrid = vkRT.froxelDir[frameIdx];
    if (grid.image == VK_NULL_HANDLE || dirGrid.image == VK_NULL_HANDLE)
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
    static VkImageView s_lastDirView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool resourceChanged = (s_lastTlas[frameIdx] != vkRT.tlas[frameIdx].handle) ||
                                 (s_lastGridView[frameIdx] != grid.view) || (s_lastDirView[frameIdx] != dirGrid.view);

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

        VkDescriptorImageInfo dirInfo = {};
        dirInfo.imageView = dirGrid.view;
        dirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(VolFroxelParamsUBO);

        VkDescriptorBufferInfo lightSsboInfo = {};
        lightSsboInfo.buffer = vkRT.volLightSsbo[frameIdx];
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

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = ds;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[4].pImageInfo = &dirInfo;

        vkUpdateDescriptorSets(vk.device, 5, writes, 0, NULL);
        vkRT.froxelFillDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastTlas[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastGridView[frameIdx] = grid.view;
        s_lastDirView[frameIdx] = dirGrid.view;
    }

    // --- Dispatch: local_size is 4x4x4, see vol_froxel_fill.comp ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelFillPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelFillPipelineLayout, 0, 1,
                            &vkRT.froxelFillDescSets[frameIdx], 1, &uboOff);
    // set=1: shared material table — light cookies.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.froxelFillPipelineLayout, 1, 1,
                            &vkRT.matDescSet[vk.currentFrame], 0, NULL);

    const uint32_t groupsX = (grid.width + 3) / 4;
    const uint32_t groupsY = (grid.height + 3) / 4;
    const uint32_t groupsZ = (grid.depth + 3) / 4;
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);

    // Compute write -> integrate pass, and -> ray tracing for the reflection rgen, which
    // marches this grid directly.  No layout change: the grids stay in GENERAL.
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Froxel: fill frame=%d slot=%d grid=%ux%ux%u groups=%ux%ux%u maxLights=%d\n",
                       tr.frameCount, frameIdx, grid.width, grid.height, grid.depth, groupsX, groupsY, groupsZ,
                       ubo.misc[1]);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchVolFroxelIntegrate (public)
//
// One invocation per grid COLUMN — 14400 of them at the 160x90 default.  Reads no
// depth, so no layout round-trip.  Must follow the fill's compute->compute barrier.
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
    // src is unphased; the integrate needs the direction grid to apply any phase at all.
    vkFroxelGrid_t &dirGrid = vkRT.froxelDir[frameIdx];
    if (src.image == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE || dirGrid.image == VK_NULL_HANDLE)
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
    static VkImageView s_lastIntegrateDirView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    const bool resourceChanged = (s_lastSrcView[frameIdx] != src.view) || (s_lastDstView[frameIdx] != dst.view) ||
                                 (s_lastIntegrateDirView[frameIdx] != dirGrid.view);

    if (vkRT.froxelIntegrateDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount || resourceChanged)
    {
        VkDescriptorSet ds = vkRT.froxelIntegrateDescSets[frameIdx];

        VkDescriptorImageInfo srcInfo = {};
        srcInfo.imageView = src.view;
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dstInfo = {};
        dstInfo.imageView = dst.view;
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dirInfo = {};
        dirInfo.imageView = dirGrid.view;
        dirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(VolFroxelParamsUBO);

        VkWriteDescriptorSet writes[4] = {};

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

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = ds;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo = &dirInfo;

        vkUpdateDescriptorSets(vk.device, 4, writes, 0, NULL);
        vkRT.froxelIntegrateDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastSrcView[frameIdx] = src.view;
        s_lastDstView[frameIdx] = dst.view;
        s_lastIntegrateDirView[frameIdx] = dirGrid.view;
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
// Trilinear fetch of the integrated grid (or the selected overlay) into
// vkRT.volBuffer[currentFrame], so vol_composite.frag needs no knowledge of the
// grid.  Claims volReadView.
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
    vkRTImage_t &vb = vkRT.volBuffer[frameIdx];
    if (grid.image == VK_NULL_HANDLE || integrated.image == VK_NULL_HANDLE || vb.image == VK_NULL_HANDLE)
        return;

    // Claim volReadView up front, not after the dispatch, so an early-out below
    // leaves the composite reading a stale volBuffer rather than a stale view of
    // some other, possibly differently sized, image.
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

    // We are about to overwrite volBuffer.  This pass is its only writer, so nothing
    // else in the frame touches it — the barrier is what keeps that safe if another
    // pass is ever ordered against it.
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
        common->Printf("VK RT Froxel: resolve frame=%d slot=%d mode=%d rect=(%d,%d %dx%d) out=%ux%u\n",
                       tr.frameCount, frameIdx, ubo.misc[2], ubo.rect[0], ubo.rect[1], ubo.rect[2], ubo.rect[3],
                       vb.width, vb.height);
}
