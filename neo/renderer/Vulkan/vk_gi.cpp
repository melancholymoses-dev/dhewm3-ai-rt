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
#include "renderer/RenderWorld_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtGI("r_rtGI", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
              "Enable one-bounce GI (Phase 6.1, ambient colour bleeding)");

idCVar r_rtGIRadius("r_rtGIRadius", "128.0", CVAR_RENDERER | CVAR_FLOAT, "Max GI bounce ray distance in world units");

static idCVar r_rtGISamples("r_rtGISamples", "4", CVAR_RENDERER | CVAR_INTEGER, "GI bounce rays per pixel (1-8)");

// Not static: read from vk_backend.cpp (RB_DetermineLightScale) for the
// r_rtGIAutoDirectScale coupling below.
idCVar r_rtGIStrength("r_rtGIStrength", "0.25", CVAR_RENDERER | CVAR_FLOAT,
                      "Global scale applied to the GI buffer before compositing");

static idCVar r_rtGIContrast(
    "r_rtGIContrast", "0.6", CVAR_RENDERER | CVAR_FLOAT,
    "GI colour contrast boost [0-1]: subtracts minimum channel and rescales to original brightness. "
    "0 = off, 1 = full effect");

idCVar r_rtGIDirectScale("r_rtGIDirectScale", "0.8", CVAR_RENDERER | CVAR_FLOAT,
                         "Baseline multiplier on direct interaction lighting when GI is active, at "
                         "r_rtGIStrength's default (0.25). Reduce below 1.0 to compensate for GI-added "
                         "luminance and keep overall brightness consistent with the original game. "
                         "When r_rtGIAutoDirectScale is on this is the anchor value, not necessarily "
                         "the value actually applied — see that CVar");

// Tuning-comparison aid: scrubbing r_rtGIStrength alone used to require also
// re-tuning r_rtGIDirectScale by hand to keep A/B luminance roughly matched —
// two dials to chase for one comparison. When on, the effective direct-light
// discount is derived from r_rtGIStrength instead of read directly from
// r_rtGIDirectScale, anchored so it reproduces r_rtGIDirectScale's own value
// exactly at r_rtGIStrength's default (0.25) and relaxes to 1.0 (no discount)
// as strength -> 0, matching the giActive==false state continuously.
// This is a scene-independent linear approximation (real GI luminance impact
// depends on local geometry/albedo density, which this can't see) — good
// enough to stop one dial from silently invalidating the other while you're
// scrubbing, not a physically exact auto-exposure.
idCVar r_rtGIAutoDirectScale(
    "r_rtGIAutoDirectScale", "1", CVAR_RENDERER | CVAR_BOOL,
    "Derive the effective r_rtGIDirectScale from the current r_rtGIStrength (anchored at "
    "strength=0.25 -> r_rtGIDirectScale's value, relaxing to 1.0 as strength->0), so scrubbing "
    "r_rtGIStrength alone stays roughly luminance-matched. 0 = use r_rtGIDirectScale as-is (manual)");

static idCVar r_rtGIBounceScale(
    "r_rtGIBounceScale", "2.0", CVAR_RENDERER | CVAR_FLOAT,
    "Multiplier applied to each light's irradiance contribution in the GI bounce (tunes Option B brightness)");

static idCVar r_rtGIEmissiveScale("r_rtGIEmissiveScale", "2.0", CVAR_RENDERER | CVAR_FLOAT,
                                  "Multiplier on emissive surface (SL_AMBIENT) contribution to GI and reflections");

static idCVar r_rtGIMaxBounceLights(
    "r_rtGIMaxBounceLights", "16", CVAR_RENDERER | CVAR_INTEGER,
    "Max uploaded lights evaluated by GI bounce shading (GI pass only; reflections unaffected)");

static idCVar r_rtGIStochasticLights("r_rtGIStochasticLights", "2", CVAR_RENDERER | CVAR_INTEGER,
                                     "P3: shadow rays fired per GI bounce hit (1-2). Lights are importance-sampled by "
                                     "unshadowed contribution and divided by their selection probability (unbiased). "
                                     "0 = legacy path, one shadow ray for every light up to r_rtGIMaxBounceLights");

static idCVar r_rtGIMaxLights("r_rtGIMaxLights", "128", CVAR_RENDERER | CVAR_INTEGER,
                              "Max GI lights to sample per frame (1-128, capped at VK_GI_MAX_LIGHTS)");

static idCVar r_rtGILightStratify(
    "r_rtGILightStratify", "1", CVAR_RENDERER | CVAR_BOOL,
    "L1: stratified GI light selection — distance tiers with per-tier importance sort, quotas and "
    "hysteresis, uploaded importance-first. 0 = legacy nearest-to-camera sort (A/B comparison)");

static idCVar r_rtGILightCollectRadiusScale(
    "r_rtGILightCollectRadiusScale", "4.0", CVAR_RENDERER | CVAR_FLOAT,
    "Camera-space GI light collection radius scale. Effective collect radius = r_rtGIRadius * scale. "
    "DEMOTED (portal_area_lights.md): only used by the legacy sphere collector, i.e. when "
    "r_rtGIAreaLights is 0 or the camera is outside the world (areaNum == -1)");

static idCVar r_rtGIAreaLights(
    "r_rtGIAreaLights", "1", CVAR_RENDERER | CVAR_BOOL,
    "portal_area_lights.md Stage 1: gather GI/vol candidate lights by walking the portal-area graph "
    "from the camera's area (respects walls and closed doors) instead of a camera-centred sphere. "
    "0 = legacy sphere collector (A/B comparison). The sphere path is also the automatic fallback "
    "when the camera is outside the world (viewDef areaNum == -1)");

static idCVar r_rtGIAreaHops("r_rtGIAreaHops", "2", CVAR_RENDERER | CVAR_INTEGER,
                             "portal_area_lights.md: BFS portal-hop depth from the camera's area. "
                             "2 = current room, neighbours, neighbours-of-neighbours");

static idCVar r_rtGIAreaBoundsEpsilon(
    "r_rtGIAreaBoundsEpsilon", "24", CVAR_RENDERER | CVAR_FLOAT,
    "portal_area_lights.md Stage 1.75: BFS seeds from every portal area overlapping a "
    "+/-N unit box around the camera (world->BoundsInAreas), not just the single "
    "PointInArea(camera) classification. Fixes hop-count popping when straddling an area "
    "boundary plane (e.g. standing in a doorway/threshold) — a hard point classification "
    "flips the ENTIRE hop numbering for every light on a sub-unit step. 0 = old single-area seed");

static idCVar r_rtGIMaxLightDist(
    "r_rtGIMaxLightDist", "2048", CVAR_RENDERER | CVAR_FLOAT,
    "portal_area_lights.md: absolute light distance safety cap within BFS-visited areas — guards "
    "pathological mega-areas where a single area spans the whole map. Area-walk path only");

static idCVar r_rtGIWhiteWeight(
    "r_rtGIWhiteWeight", "0.25", CVAR_RENDERER | CVAR_FLOAT,
    "auto_relight.md §0: importance multiplier floor for fully desaturated (white) lights "
    "in GI/vol light selection [0-1]. Saturated lights always score ×1.0; white lights score "
    "×this. Lower = colored accents beat white fills more strongly for the limited light slots. "
    "1.0 = no saturation weighting (legacy)");

static idCVar r_rtGILightDump("r_rtGILightDump", "0", CVAR_RENDERER | CVAR_BOOL,
                              "auto_relight.md §0: dump every in-view lightDef to the console once — name, color, "
                              "saturation, radius, noShadows, classifier verdict, admitted/rejected, importance "
                              "pre/post saturation weight. Self-clearing (prints once then resets to 0)");

static idCVar r_rtGIAlbedo("r_rtGIAlbedo", "1", CVAR_RENDERER | CVAR_BOOL,
                           "docs/plans/gi_albedo_target.md: multiply denoised GI by the receiving surface's "
                           "albedo (gbufAlbedo G-buffer target) before composite. 0 = legacy raw-radiance "
                           "add (A/B) — dark/black materials get the same GI wash as white ones. "
                           "No-op when vk.gbufferSupported is false");

static idCVar r_rtGICheckerboard(
    "r_rtGICheckerboard", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
    "Enable checkerboard GI tracing (updates alternating pixels each frame for lower GI RT cost)");

static idCVar r_rtGIAtrous("r_rtGIAtrous", "1", CVAR_RENDERER | CVAR_BOOL,
                           "Enable À-trous spatial filter on the GI buffer after temporal resolve");
static idCVar r_rtGIAtrousIterations(
    "r_rtGIAtrousIterations", "3", CVAR_RENDERER | CVAR_INTEGER,
    "Number of À-trous filter passes (each doubles the filter radius; 3 → effective radius ~7px)");
static idCVar r_rtGIAtrousSigmaL("r_rtGIAtrousSigmaL", "0.2", CVAR_RENDERER | CVAR_FLOAT,
                                 "À-trous luminance edge-stop bandwidth (smaller = sharper edges preserved)");
static idCVar r_rtGIAtrousSigmaZ("r_rtGIAtrousSigmaZ", "0.01", CVAR_RENDERER | CVAR_FLOAT,
                                 "À-trous depth edge-stop bandwidth (smaller = tighter depth edges)");

// ---------------------------------------------------------------------------
// GI light SSBO — mirrors GILightBuffer in gi_ray.rchit GLSL
// ---------------------------------------------------------------------------

#define VK_GI_MAX_LIGHTS 128

// auto_relight.md AR7 follow-up: bit 0 of GILightEntry::flags marks the first-person
// muzzle-flash light (point light + allowLightInViewID set — see considerLight below).
// vol_march.comp reads it to decide whether this light's shadow ray is allowed to test
// player-body/weapon geometry (r_rtVolMuzzleSelfShadow / r_rtVolMuzzleSelfShadowDebug).
#define GI_LIGHT_FLAG_SELF_SHADOW 0x1u

struct GILightEntry
{
    float posRadius[4];      // xyz = world pos, w = sphere pre-cull radius
    float colorIntensity[4]; // rgb = light colour, a = intensity
    float coneDir[4];        // projected: xyz=normalised dir, w=cos(halfAngle); zeroed for point
    float boxExtents[4];     // point: xyz=AABB half-extents, w=0
                             // projected: w=max reach along cone axis; xyz=0
    uint32_t lightType;      // 0 = point, 1 = projected/spot
    uint32_t flags;          // GI_LIGHT_FLAG_* — see above (was an unused pad slot)
    uint32_t pad[2];         // alignment pad
    // 2026-08-30: idRenderLightLocal::globalLightOrigin — origin + axis * lightCenter.
    // The EMITTER. posRadius.xyz above stays the light's volume centre (parms.origin),
    // because boxExtents/coneDir are expressed relative to that and the attenuation
    // volume does not move when a mapper offsets lightCenter.
    //
    // This is the same split the GL path makes: R_SetLightProject builds the falloff
    // planes around parms.origin (tr_lightrun.cpp:440), then globalLightOrigin is
    // derived separately at :472 and is what the interaction shader's L vector and the
    // shadow frustums use. Doom 3 maps lean on this — mars_city1's light_5253 offsets
    // its emitter by ~920 units, light_4842 by ~5800 — so aiming shadow rays and N·L at
    // parms.origin puts the apparent light source in the wrong place entirely.
    //
    // Deliberately a new field rather than reusing coneDir.xyz (unused for point
    // lights): overloading one slot with two meanings is exactly what made parm3 cost
    // a session, and 16 bytes per light is 2 KB at VK_GI_MAX_LIGHTS.
    float emitPos[4];        // xyz = globalLightOrigin, w unused — pads to 96 bytes
};

struct GILightBuffer
{
    int32_t numLights;
    float bounceScale;   // r_rtGIBounceScale — per-light irradiance multiplier
    float giRadius;      // r_rtGIRadius — hit-point light evaluation window (shader-side)
    float emissiveScale; // r_rtGIEmissiveScale — emissive surface contribution multiplier
    GILightEntry lights[VK_GI_MAX_LIGHTS];
};
static_assert(sizeof(GILightBuffer) == 16 + VK_GI_MAX_LIGHTS * 96, "GILightBuffer size mismatch");

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
//   int   checker           offset 104  size  4
//   int   maxBounceLights   offset 108  size  4
//   float giContrast        offset 112  size  4
//   int   stochasticLights  offset 116  size  4
//   int   useGbufNormal     offset 120  size  4
//   int   checkerPhase      offset 124  size  4
//   total: 128 bytes
//
// gi_ray.rchit declares a matching block (it reads maxBounceLights, frameIndex
// and stochasticLights), so any field added here must be mirrored in *both*
// gi_ray.rgen and gi_ray.rchit — a prefix mismatch silently shifts every
// offset after it.
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
    int32_t checker;
    int32_t maxBounceLights;
    float giContrast;
    int32_t stochasticLights;
    int32_t useGbufNormal;
    // Checkerboard phase — see s_giCheckerPhase.  MUST NOT be derived from
    // frameIndex: the frame slot advances in lockstep with tr.frameCount, so a
    // frameIndex-based parity locks each slot to one fixed half of the image
    // forever (the "GI checkerboard ghost" bug).
    int32_t checkerPhase;
};
static_assert(sizeof(GIParamsUBO) == 128, "GIParamsUBO size mismatch");

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);
// P9: shared 1x1 G-buffer fallback owned by vk_reflections.cpp (alpha 0 = "no data").
extern VkImageView VK_RT_GetNullGbufNormalView(void);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;
extern idCVar r_rtGbufNormals;  // P9 — defined in vk_gbuffer.cpp
extern idCVar r_rtVolMaxLights; // defined in vk_vol.cpp — cap for the vol-only selection built below
extern idCVar r_rtVolMaxDist;   // defined in vk_vol.cpp — vol march reach, used to filter that selection
extern idCVar r_rtVolDump;      // defined in vk_vol.cpp — one-shot verbatim dump of the vol upload
extern bool vkRT_volDumpPending; // defined in vk_vol.cpp — handoff so the params half prints too

// ---------------------------------------------------------------------------
// VK_RT_CreateGILightSsbos / VK_RT_DestroyGILightSsbos
// Per-frame host-visible SSBO for GI Option B light data.
// Size is fixed (VK_GI_MAX_LIGHTS entries) — allocated once, filled every frame.
// ---------------------------------------------------------------------------

static void VK_RT_CreateGILightSsbos(void)
{
    const VkDeviceSize bufSize = sizeof(GILightBuffer);
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_CreateBuffer(bufSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &vkRT.giLightSsbo[i], &vkRT.giLightSsboMemory[i]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.giLightSsboMemory[i], 0, bufSize, 0, &vkRT.giLightSsboMapped[i]));
        // Zero out so numLights=0 by default (Option A fallback until first dispatch).
        memset(vkRT.giLightSsboMapped[i], 0, bufSize);

        // Dedicated vol selection buffer — same layout/size, filled separately below.
        VK_CreateBuffer(bufSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &vkRT.volLightSsbo[i], &vkRT.volLightSsboMemory[i]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.volLightSsboMemory[i], 0, bufSize, 0, &vkRT.volLightSsboMapped[i]));
        memset(vkRT.volLightSsboMapped[i], 0, bufSize);
    }
}

static void VK_RT_DestroyGILightSsbos(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vkRT.giLightSsboMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.giLightSsboMemory[i]);
            vkRT.giLightSsboMapped[i] = NULL;
        }
        if (vkRT.giLightSsbo[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.giLightSsbo[i], NULL);
            vkRT.giLightSsbo[i] = VK_NULL_HANDLE;
        }
        if (vkRT.giLightSsboMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.giLightSsboMemory[i], NULL);
            vkRT.giLightSsboMemory[i] = VK_NULL_HANDLE;
        }

        if (vkRT.volLightSsboMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.volLightSsboMemory[i]);
            vkRT.volLightSsboMapped[i] = NULL;
        }
        if (vkRT.volLightSsbo[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.volLightSsbo[i], NULL);
            vkRT.volLightSsbo[i] = VK_NULL_HANDLE;
        }
        if (vkRT.volLightSsboMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.volLightSsboMemory[i], NULL);
            vkRT.volLightSsboMemory[i] = VK_NULL_HANDLE;
        }
    }
}

// ---------------------------------------------------------------------------
// VK_RT_CreateGIImages
// Allocates per-frame RGBA16F GI buffers at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateGIImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkReflBuffer_t &gb = vkRT.giBuffer[i];
        gb.width = width;
        gb.height = height;

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
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &gb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, gb.image, gb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = gb.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &gb.view));

        // Transition UNDEFINED → GENERAL so rgen can imageStore on first dispatch.
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

            // Transition UNDEFINED → GENERAL (required before clear and imageStore).
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = gb.image;
            barrier.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            // Clear to black so unwritten pixels don't contain garbage.
            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, gb.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

            // Second barrier: transfer write → shader write for rgen imageStore.
            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.image = gb.image;
            barrier2.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                 0, 0, NULL, 0, NULL, 1, &barrier2);

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
        gb.width = 0;
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

    // binding 0: TLAS — rgen fires primary GI rays; rchit fires inline shadow rays
    // binding 1: GI RGBA16F storage image (rgen write)
    // binding 2: depth sampler (rgen read)
    // binding 3: GI params UBO (rgen + rchit read, dynamic)
    // binding 4: GI light list SSBO (rchit read — Option B light evaluation)
    // binding 5: G-buffer normal/F0 sampler (P9 — rgen, replaces rt_ReconstructNormal)
    VkDescriptorSetLayoutBinding bindings[6] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

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
    // CLOSEST_HIT is required, not optional: gi_ray.rchit has read this block since
    // Option B landed (maxBounceLights, and now P3's stochasticLights/frameIndex).
    // Accessing a descriptor from a stage missing from stageFlags is undefined
    // behaviour — it reads garbage or faults depending on driver, which is the
    // likeliest cause of the crashes earlier P3 attempts hit here.
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giDescLayout));

    // --- Pipeline layout ---
    // set=0: per-frame resources (TLAS, GI image, depth, UBO)
    // set=1: material table (shared with reflections/shadows)
    VkDescriptorSetLayout giLayouts[2] = {vkRT.giDescLayout, vkRT.matDescLayout};
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts = giLayouts;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rgenModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rgen.spv");
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rmiss.spv");
    VkShaderModule rchitModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rchit.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rahit.spv");
    VkShaderModule shadowMissModule = VK_LoadSPIRV("glprogs/glsl/gi_shadow.rmiss.spv");

    if (rgenModule == VK_NULL_HANDLE || rmissModule == VK_NULL_HANDLE || rchitModule == VK_NULL_HANDLE ||
        rahitModule == VK_NULL_HANDLE || shadowMissModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI: failed to load GI shader modules — GI disabled");
        if (rgenModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rgenModule, NULL);
        if (rmissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rmissModule, NULL);
        if (rchitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rchitModule, NULL);
        if (rahitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rahitModule, NULL);
        if (shadowMissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, shadowMissModule, NULL);
        return;
    }

    // --- Shader stages ---
    // Stage 0: rgen
    // Stage 1: gi miss (sky ambient)
    // Stage 2: rchit (albedo + Option B light eval + shadow trace)
    // Stage 3: rahit (alpha-discard for GI rays)
    // Stage 4: shadow miss (clears occluded flag)
    VkPipelineShaderStageCreateInfo stages[5] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = "main";

    stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[3].module = rahitModule;
    stages[3].pName = "main";

    stages[4].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[4].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[4].module = shadowMissModule;
    stages[4].pName = "main";

    // --- Shader groups ---
    // Group 0: rgen
    // Group 1: gi miss (missIndex=0 in traceRayEXT)
    // Group 2: hit (rchit + rahit)
    // Group 3: shadow miss (missIndex=1 in shadow traceRayEXT calls from rchit)
    VkRayTracingShaderGroupCreateInfoKHR groups[4] = {};

    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0; // rgen
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1; // gi miss
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = 2; // gi_ray.rchit
    groups[2].anyHitShader = 3;     // gi_ray.rahit — alpha-discard
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 4; // shadow miss
    groups[3].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount = 5;
    rtPipeInfo.pStages = stages;
    rtPipeInfo.groupCount = 4;
    rtPipeInfo.pGroups = groups;
    rtPipeInfo.maxPipelineRayRecursionDepth = 2; // primary GI ray + inline shadow ray
    rtPipeInfo.layout = vkRT.giPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.giPipeline));

    vkDestroyShaderModule(vk.device, rgenModule, NULL);
    vkDestroyShaderModule(vk.device, rmissModule, NULL);
    vkDestroyShaderModule(vk.device, rchitModule, NULL);
    vkDestroyShaderModule(vk.device, rahitModule, NULL);
    vkDestroyShaderModule(vk.device, shadowMissModule, NULL);

    // --- Shader Binding Table ---
    // SBT layout:
    //   slot 0 (rgen region)     : rgen
    //   slot 1 (miss region[0])  : gi miss  (missIndex=0 — GI rays)
    //   slot 2 (miss region[1])  : shadow miss (missIndex=1 — shadow rays from rchit)
    //   slot 3 (hit region[0])   : hit group (rchit + rahit)
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

    // 4 groups total, 6 SBT records:
    //   rgen:  1 record  (group 0)
    //   miss:  2 records (groups 1, 3)  — gi miss + shadow miss
    //   hit:   3 records (group 2, replicated)
    //
    // The hit region is 3 records wide, not 1, because TLAS instances carry a
    // hardcoded instanceShaderBindingTableRecordOffset of 0 or 2 (2 =
    // noSelfShadow player body/viewmodel, see vk_accelstruct.cpp). That offset is
    // per-instance and shared by every pipeline that traces this TLAS, so the hit
    // region must contain record 2 or an out-of-range fetch pulls a garbage
    // shader handle. Record 1 of the region is unused padding — nothing traces
    // with sbtRecordOffset 1 here — but it must exist for record 2 to be
    // addressable.
    uint32_t sbtSize = 6 * stride;

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vkRT.sbtGIBuffer,
                    &vkRT.sbtGIMemory);

    // Fetch all 4 group handles in pipeline order: [rgen, gi-miss, hit, shadow-miss]
    uint8_t *handles = (uint8_t *)alloca(4 * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.giPipeline, 0, 4, 4 * handleSize, handles));

    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtGIMemory, 0, sbtSize, 0, (void **)&sbtData));
    // SBT slot 0 = rgen (group 0)
    memcpy(sbtData + 0 * stride, handles + 0 * handleSize, handleSize);
    // SBT slot 1 = gi miss (group 1)
    memcpy(sbtData + 1 * stride, handles + 1 * handleSize, handleSize);
    // SBT slot 2 = shadow miss (group 3)
    memcpy(sbtData + 2 * stride, handles + 3 * handleSize, handleSize);
    // SBT slots 3..5 = hit group (group 2), replicated so instance record
    // offsets 0..2 all resolve to the same (only) hit group.
    for (int i = 3; i < 6; i++)
        memcpy(sbtData + i * stride, handles + 2 * handleSize, handleSize);
    vkUnmapMemory(vk.device, vkRT.sbtGIMemory);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtGIBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    // rgen:  slot 0
    // miss:  slots 1–2 (stride * 2 region, covers gi-miss[0] and shadow-miss[1])
    // hit:   slots 3–5 (stride * 3 region, same handle in all three)
    vkRT.giRgenRegion = {sbtBase + 0 * stride, stride, stride};
    vkRT.giMissRegion = {sbtBase + 1 * stride, stride, 2 * stride};
    vkRT.giHitRegion = {sbtBase + 3 * stride, stride, 3 * stride};
    vkRT.giCallRegion = {0, 0, 0};

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI SBT: stride=%u sbtBytes=%u base=0x%llx (4 groups: rgen+gi-miss+shadow-miss+hit) "
                       "miss=0x%llx (missRecords=%u) hit=0x%llx (hitRecords=%u)\n",
                       stride, sbtSize, (unsigned long long)sbtBase,
                       (unsigned long long)vkRT.giMissRegion.deviceAddress,
                       (unsigned)(vkRT.giMissRegion.size / vkRT.giMissRegion.stride),
                       (unsigned long long)vkRT.giHitRegion.deviceAddress,
                       (unsigned)(vkRT.giHitRegion.size / vkRT.giHitRegion.stride));

    // --- Descriptor pool and sets ---
    // COMBINED_IMAGE_SAMPLER count is doubled: depth (binding 2) + gbufNormal
    // (binding 5, P9) — two per frame slot.
    VkDescriptorPoolSize poolSizes[5] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MAX_FRAMES_IN_FLIGHT * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giDescSetLastUpdatedFrameCount[i] = -1;

    // --- GI buffer sampler (linear-clamp, used by interaction shader) ---
    if (vkRT.giSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
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
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giCompositeDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.giCompositeDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giCompositeLayout));

    // --- Shader modules ---
    VkShaderModule vertMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.vert.spv");
    VkShaderModule fragMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.frag.spv");
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI: failed to load composite shaders — composite pass disabled");
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

    // No vertex input — triangle is generated from gl_VertexIndex in the vert shader.
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

    // No depth test and no depth write — GI is a post-geometry pass.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    // Additive blend: GI is added on top of whatever is already in the framebuffer.
    // Alpha: ZERO+ONE so we don't disturb the alpha channel.
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    // Attachments 1-2 (gbufNormal, gbufAlbedo) are written only by the G-buffer
    // prepass; every other pipeline on vk.hdrRenderPass supplies write-mask-0
    // fillers so the subpass's per-attachment blend-state count always matches.
    VkPipelineColorBlendAttachmentState blendAttachments[3] = {colorBlend, {}, {}};
    VK_FillSecondBlendAttachment(&blendAttachments[1]);
    VK_FillSecondBlendAttachment(&blendAttachments[2]);

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = vk.gbufferSupported ? 3 : 1;
    blendState.pAttachments = blendAttachments;

    // Dynamic viewport and scissor (composite covers full framebuffer).
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
    pipelineInfo.layout = vkRT.giCompositeLayout;
    pipelineInfo.renderPass = vk.hdrRenderPass;
    pipelineInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.giCompositePipeline));

    vkDestroyShaderModule(vk.device, vertMod, NULL);
    vkDestroyShaderModule(vk.device, fragMod, NULL);

    // --- Descriptor pool and sets (one per frame in flight) ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giCompositeDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giCompositeDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giCompositeDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giCompositeDescSets));

    common->Printf("VK RT GI: composite pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitGI (public entry)
// ---------------------------------------------------------------------------

void VK_RT_InitGI(void)
{
    VK_RT_CreateGILightSsbos();
    VK_RT_InitGIPipeline();
    // P9: ensure the shared null G-buffer image exists at init (idempotent) so its
    // one-time submit + queue wait never lands mid-frame at the first dispatch.
    VK_RT_GetNullGbufNormalView();
    VK_RT_InitGICompositePipeline();
    VK_RT_ResizeGI(vk.swapchainExtent.width, vk.swapchainExtent.height);
    VK_RT_InitGITemporal();
    VK_RT_InitGIAtrous();
    VK_RT_InitGIAlbedoMod();
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

    VK_RT_ShutdownGIAlbedoMod();
    VK_RT_ShutdownGIAtrous();
    VK_RT_ShutdownGITemporal();
    VK_RT_DestroyGIImages();
    VK_RT_DestroyGILightSsbos();
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

    // Only resize temporal/atrous subsystems once their pipelines have been
    // created.  During VK_RT_InitGI the pipeline handles are still NULL here
    // (InitGITemporal/InitGIAtrous are called afterwards), so these are skipped
    // to prevent double-allocation of the history/ping-pong images.
    if (vkRT.giTemporalPipeline != VK_NULL_HANDLE)
        VK_RT_ResizeGITemporal(width, height);
    if (vkRT.giAtrousPipeline != VK_NULL_HANDLE)
        VK_RT_ResizeGIAtrous(width, height);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGI (public)
// Dispatch GI rays for the current view (once per frame).
// Must be outside a render pass.  Depth must be in ATTACHMENT_OPTIMAL on
// entry; this function transitions to READ_ONLY_OPTIMAL and restores.
// On exit giBuffer[currentFrame] is in GENERAL layout, readable by the
// interaction fragment shader (memory barrier issued before returning).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VK_RT_UploadGILights (public)
// Populate the per-frame GI light SSBO with candidate world lights.
// Called every frame BEFORE any RT dispatch that needs light data (reflections,
// GI).  Calling it more than once per frame is safe — the buffer is reset then
// refilled each call.
//
// Gathering (Stage 1, portal_area_lights.md): candidates come from a BFS over
// the portal-area graph rooted at the camera's area (r_rtGIAreaHops deep,
// closed doors block), reading each visited area's lightRefs — id's live,
// volume-exact per-area light lists.  Lights behind sealed walls are never
// admitted; a big room's own lights are admitted regardless of distance.
// The legacy camera-sphere cull remains as fallback (r_rtGIAreaLights 0, or
// viewDef->areaNum == -1 when noclipped outside the world).
//
// Ordering contract (L1, rt_optimization_tuning.md): lights are selected via
// stratified distance tiers and uploaded importance-first, because consumers
// walk a prefix of the buffer (gi_ray.rchit: maxBounceLights, reflect_ray.rchit:
// shadow budget, vol_march.comp: maxLights).
//
// Independent of r_rtGILightBounce so that reflection hit shaders can always
// evaluate lighting even when the GI bounce feature is disabled.
// ---------------------------------------------------------------------------
void VK_RT_UploadGILights(const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized || viewDef == NULL)
        return;

    const int frameIdx = vk.currentFrame;
    if (vkRT.giLightSsboMapped[frameIdx] == NULL)
        return;
    if (tr.primaryWorld == NULL)
        return;

    // Upload once per frame slot from the main world view only.
    // Secondary/subview passes (GUI, mirrors, etc.) can have no world lights
    // and must not clear the already-populated GI light list.
    if (viewDef->renderWorld != tr.primaryWorld || viewDef->isSubview)
        return;

    static int s_lastGIUploadFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastGIUploadFrame[frameIdx] == tr.frameCount)
        return;
    s_lastGIUploadFrame[frameIdx] = tr.frameCount;

    GILightBuffer *lb = (GILightBuffer *)vkRT.giLightSsboMapped[frameIdx];
    lb->numLights = 0;
    lb->bounceScale = idMath::ClampFloat(0.0f, 100.0f, r_rtGIBounceScale.GetFloat());
    lb->giRadius = Max(1.0f, r_rtGIRadius.GetFloat());
    lb->emissiveScale = idMath::ClampFloat(0.0f, 100.0f, r_rtGIEmissiveScale.GetFloat());

    const float giRadius = lb->giRadius;
    const float lightCollectScale = idMath::ClampFloat(0.25f, 8.0f, r_rtGILightCollectRadiusScale.GetFloat());
    const float lightCollectRadius = giRadius * lightCollectScale;
    const idVec3 camPos = viewDef->renderView.vieworg;
    idRenderWorldLocal *world = tr.primaryWorld;
    const int numLightDefs = world->lightDefs.Num();
    const int maxLights = idMath::ClampInt(1, VK_GI_MAX_LIGHTS, r_rtGIMaxLights.GetInteger());

    // Stage 1 (portal_area_lights.md): topology-aware gathering. Instead of a
    // camera-centred Euclidean sphere (admits lights through sealed walls, goes
    // dark in rooms larger than the sphere), BFS the portal graph from the
    // camera's area and gather each visited area's lightRefs — the per-area light
    // lists id's R_CreateLightRefs already maintains by pushing light volumes
    // through the BSP / flooding portals. The sphere path remains as runtime
    // fallback (r_rtGIAreaLights 0) and for camera-outside-world (areaNum == -1).
    static const int kMaxGatherAreas = 4096; // BFS visited-stamp/queue capacity; Doom 3 maps have < 200 areas
    const int areaHops = idMath::ClampInt(0, 64, r_rtGIAreaHops.GetInteger());
    const bool useAreaWalk = r_rtGIAreaLights.GetBool() && viewDef->areaNum >= 0 &&
                             viewDef->areaNum < world->numPortalAreas && world->portalAreas != NULL &&
                             world->numPortalAreas <= kMaxGatherAreas;

    // Area walk: distance is demoted from admission criterion to a generous
    // safety cap (mega-area guard). Sphere fallback: distance IS the admission.
    const float maxLightDist = Max(1.0f, r_rtGIMaxLightDist.GetFloat());
    const float distCullSq = useAreaWalk ? maxLightDist * maxLightDist : lightCollectRadius * lightCollectRadius;

    // Collect candidates (area walk or sphere); selection/ordering happens below (L1).
    struct Candidate
    {
        float distSq;
        float importance; // L1: intensity × luminance × radius² / max(distSq, radius²), + hysteresis boost
        int tier;         // L1: distance tier 0-3 (0-128 / 128-320 / 320-768 / 768+)
        int lightIdx;     // index into lightDefs — stable identity for hysteresis
        GILightEntry entry;
    };

    // Use a fixed-size stack buffer — numLightDefs can be in the hundreds.
    static Candidate s_candidates[1024];
    int numCandidates = 0;

    // L1 hysteresis state: which lightDef indices were uploaded by the previous
    // upload (stamped with that upload's frameCount). A light selected last frame
    // gets a small importance boost so selection doesn't flicker at the cut line.
    static const int kHysteresisMaxLightIdx = 4096;
    static int s_lightSelectedFrame[kHysteresisMaxLightIdx];
    static int s_prevUploadFrameNum = -1;
    const int prevUploadFrame = s_prevUploadFrameNum;

    // §0 (auto_relight.md): one-shot console dump of every light this collector
    // sees, with its classifier verdict — self-clearing so it doesn't spam.
    const bool dumpLights = r_rtGILightDump.GetBool();
    if (dumpLights)
    {
        if (useAreaWalk)
            common->Printf("VK RT GI light dump (area walk: startArea=%d hops=%d maxDist=%.1f, "
                           "+ Stage 1.5 view-flood union + Stage 1.75 boundary-fuzz seed):\n",
                           viewDef->areaNum, areaHops, maxLightDist);
        else
            common->Printf("VK RT GI light dump (camera sphere: collectRadius=%.1f):\n", lightCollectRadius);
    }

    const float whiteWeight = idMath::ClampFloat(0.0f, 1.0f, r_rtGIWhiteWeight.GetFloat());

    // Per-light admission + candidate build, shared by both gather paths.
    // gatherArea/gatherHop are provenance for the dump: hop>=0 means BFS at that
    // depth; hop==-1 with gatherArea>=0 means Stage 1.5 (reached via the view-flood
    // union, not the BFS — on screen but outside hop range); gatherArea==-1 (always
    // paired with hop==-1) means the sphere fallback path.
    const auto considerLight = [&](int li, const idRenderLightLocal *lightLocal, int gatherArea, int gatherHop) {
        const renderLight_t &p = lightLocal->parms;

        // suppressLightInViewID is set on worldMuzzleFlash (third-person weapon light).
        // Skip it — volumetrics should follow the first-person (muzzleFlash) light only.
        if (p.suppressLightInViewID != 0)
            return;

        // Parallel (directional/sky) lights are infinite — no volume boundary or falloff,
        // so they cannot contribute meaningful single-scatter volumetrics.
        if (p.parallel)
            return;

        const float dSq = (p.origin - camPos).LengthSqr();
        if (dSq > distCullSq)
            return;

        float radius = Max(Max(p.lightRadius.x, p.lightRadius.y), p.lightRadius.z);

        float r = p.shaderParms[SHADERPARM_RED];
        float g = p.shaderParms[SHADERPARM_GREEN];
        float b = p.shaderParms[SHADERPARM_BLUE];

        // 2026-08-30: parm3 is the material TIMESCALE on a light, not an intensity.
        //
        // RenderWorld.h:61-62 aliases two names onto slot 3 — SHADERPARM_ALPHA and
        // SHADERPARM_TIMESCALE. Entities/models use the alpha meaning; lights use the
        // other one, and Light.cpp:159 spawns the map key through it explicitly:
        //
        //     args->GetFloat( "shaderParm3", "1", parms[SHADERPARM_TIMESCALE] );
        //
        // So a light's parm3 scales shader time for its material's time-driven
        // expressions. lights/cloudscroll2 is `translate time * .03, time * .03`, and
        // mars_city1's light_5253 sets shaderParm3 9000 as that texture's scroll rate.
        // Reading it as a brightness multiplier gave that one light importance 1631
        // against 0.24 for the next-ranked light: it monopolised the GI/vol upload
        // slots and blew the volumetric march into a full-screen wash that no density,
        // strength or anisotropy could pull back, since the multiplier sits upstream of
        // all three (vol_march.comp's contrib line). 40 retail light entities set parm3
        // outside [0,1], up to 80000 (recycling1 "level_fog") — concentrated in the big
        // maps: recycling1, enpro, cpu, cpuboss, hell1, site3.
        //
        // Nothing is lost by dropping it. A Doom 3 light carries brightness entirely in
        // its RGB registers — tr_render.cpp:872-874 applies backEnd.lightScale to
        // lightColor[0..2] only and draw_common.cpp:2079 pushes the light colour through
        // qglColor3fv, three components; idLight's fade/SetLightLevel paths scale parms
        // 0-2. There is no scalar intensity channel to lose.
        //
        // Why this hid for so long: the spawn default above is "1", so the multiplier is
        // exactly 1.0 on essentially every light, and the old `< 0.001f -> 1.0f` patch
        // covered the zero-timescale case. Both benign values, so the read looked right
        // anywhere it was tested.
        //
        // The one place that used slot 3 as a real intensity was our own auto-relight
        // synthesis (vk_auto_relight.cpp) — it now bakes r_rtAutoRelightIntensity into
        // the RGB parms instead, so brightness arrives here through the same channel for
        // every light regardless of origin, and that cvar finally affects the direct
        // lighting from those fixtures too rather than GI/volumetrics alone.
        //
        // Kept as an explicit 1.0 rather than deleted: GILightEntry.colorIntensity.a and
        // the shaders' lightIntens stay in place for a future source to drive
        // deliberately. The rule is only that it must not silently inherit parm3.
        const float intensity = 1.0f;

        // §0 (auto_relight.md): shared real/fake judgment — replaces the old
        // entity-noShadows-only test, which let material-ambient washes through
        // GI/vol while rejecting small colored accents flagged entity-noShadows.
        const vkRTLightClass_t lightClass = VK_RT_ClassifyLight(p, lightLocal->lightShader);
        const bool admitted = (lightClass == RT_LIGHT_REAL || lightClass == RT_LIGHT_ACCENT) && radius >= 1.0f;

        // §0: saturation weighting for ranking only — a fully white light's importance
        // is scaled by whiteWeight, a fully saturated light is untouched. This does NOT
        // affect admission, only which admitted lights win the limited upload slots.
        const float maxChan = Max(r, Max(g, b));
        const float minChan = Min(r, Min(g, b));
        const float sat = (maxChan > 1e-4f) ? (maxChan - minChan) / maxChan : 0.0f;
        const float satWeight = whiteWeight + (1.0f - whiteWeight) * sat;

        if (dumpLights)
        {
            const float lumDbg = Max(0.0f, 0.299f * r + 0.587f * g + 0.114f * b);
            const float dstSq = Max(dSq, radius * radius);
            const float baseImportanceDbg = (dstSq > 1e-4f) ? intensity * lumDbg * radius * radius / dstSq : 0.0f;
            common->Printf("  %-28s cls=%-12s noShadows=%d radius=%6.1f dist=%6.1f area=%d hop=%d "
                           "color=(%.2f %.2f %.2f) sat=%.2f imp=%.4f->%.4f -> %s\n",
                           lightLocal->lightShader ? lightLocal->lightShader->GetName() : "<null>",
                           VK_RT_LightClassName(lightClass), p.noShadows ? 1 : 0, radius, idMath::Sqrt(dSq), gatherArea,
                           gatherHop, r, g, b, sat, baseImportanceDbg, baseImportanceDbg * satWeight,
                           admitted ? "ADMIT" : "REJECT");
        }

        if (!admitted)
            return;

        if (numCandidates < 1024)
        {
            Candidate &c = s_candidates[numCandidates++];
            c.distSq = dSq;
            c.lightIdx = li;

            // L1 distance tier (world units): 0-128 / 128-320 / 320-768 / 768+.
            const float dist = idMath::Sqrt(dSq);
            c.tier = (dist < 128.0f) ? 0 : (dist < 320.0f) ? 1 : (dist < 768.0f) ? 2 : 3;

            // L1 perceptual importance. The / max(distSq, radius²) term approximates
            // received flux without re-collapsing to pure distance: inside a light's
            // own radius the divisor stops shrinking, so nearby lights compare by
            // intensity·luminance alone.
            const float lum = Max(0.0f, 0.299f * r + 0.587f * g + 0.114f * b);
            const float baseImportance = intensity * lum * radius * radius / Max(dSq, radius * radius);
            c.importance = baseImportance * satWeight;
            if (li < kHysteresisMaxLightIdx && prevUploadFrame >= 0 && s_lightSelectedFrame[li] == prevUploadFrame)
                c.importance *= 1.15f; // hysteresis: incumbents survive small importance dips

            c.entry.posRadius[0] = p.origin.x;
            c.entry.posRadius[1] = p.origin.y;
            c.entry.posRadius[2] = p.origin.z;
            c.entry.posRadius[3] = radius;
            // Emitter, see GILightEntry::emitPos. Taken from the renderer's own derived
            // value rather than recomputed from parms, so it cannot drift from what the
            // GL path uses (R_DeriveLightData sets it, tr_lightrun.cpp:472). The
            // parallel-light branch there (origin + dir * 100000) is unreachable here —
            // considerLight rejects p.parallel above.
            c.entry.emitPos[0] = lightLocal->globalLightOrigin.x;
            c.entry.emitPos[1] = lightLocal->globalLightOrigin.y;
            c.entry.emitPos[2] = lightLocal->globalLightOrigin.z;
            c.entry.emitPos[3] = 0.0f;
            c.entry.colorIntensity[0] = r;
            c.entry.colorIntensity[1] = g;
            c.entry.colorIntensity[2] = b;
            c.entry.colorIntensity[3] = intensity;
            // Fill volume geometry: AABB half-extents for point lights, cone for projected.
            const bool isProjected = (!p.pointLight && !p.parallel);
            if (!isProjected)
            {
                c.entry.boxExtents[0] = p.lightRadius.x;
                c.entry.boxExtents[1] = p.lightRadius.y;
                c.entry.boxExtents[2] = p.lightRadius.z;
                c.entry.boxExtents[3] = 0.0f;
                c.entry.coneDir[0] = c.entry.coneDir[1] = c.entry.coneDir[2] = c.entry.coneDir[3] = 0.0f;
                // Widen sphere pre-cull to cover the 1.5x halo zone outside the box.
                c.entry.posRadius[3] = radius * 1.5f;
            }
            else
            {
                // p.target is LOCAL space; rotate by p.axis to get world-space direction.
                idVec3 toTarget = p.axis * p.target;
                float reach = toTarget.Length();
                idVec3 dir = (reach > 0.001f) ? toTarget / reach : idVec3(0, 0, 1);
                float maxHalf = Max(p.right.Length(), p.up.Length());
                float cosHalf = reach / idMath::Sqrt(reach * reach + maxHalf * maxHalf);
                c.entry.coneDir[0] = dir.x;
                c.entry.coneDir[1] = dir.y;
                c.entry.coneDir[2] = dir.z;
                c.entry.coneDir[3] = cosHalf;
                c.entry.boxExtents[0] = c.entry.boxExtents[1] = c.entry.boxExtents[2] = 0.0f;
                c.entry.boxExtents[3] = reach * 1.1f;
                c.entry.posRadius[3] = reach * 1.1f; // override sphere pre-cull to match cone reach
            }
            // lightType: 0=point, 1=scene directed/spot, 2=player flashlight.
            // allowLightInViewID is set on muzzleFlash (first-person weapon light).
            c.entry.lightType = isProjected ? (p.allowLightInViewID != 0 ? 2u : 1u) : 0u;
            // AR7 follow-up: a first-person point light with allowLightInViewID set is the
            // muzzle flash specifically (the flashlight is projected, so it lands in the
            // lightType==2 branch above instead) — flag it as a volumetric self-shadow
            // candidate. See vol_march.comp's r_rtVolMuzzleSelfShadow handling.
            c.entry.flags = (!isProjected && p.allowLightInViewID != 0) ? GI_LIGHT_FLAG_SELF_SHADOW : 0u;
            c.entry.pad[0] = c.entry.pad[1] = 0u;
        }
    };

    int areasVisited = 0;   // area-walk breadcrumb for the summary log below
    int viewStampAreas = 0; // Stage 1.5 breadcrumb: areas added by the view-flood union, not the BFS
    if (useAreaWalk)
    {
        // BFS the portal graph from the camera's area, gathering each visited
        // area's lightRefs chain. Visited stamps are per-gather-pass counters
        // (NOT tr.frameCount — see feedback_per_slot_counters), so no clearing.
        static int s_areaVisitStamp[kMaxGatherAreas];
        static int s_lightVisitStamp[kHysteresisMaxLightIdx]; // dedupe lights spanning multiple areas
        static int s_gatherStamp = 0;
        s_gatherStamp++; // starts at 1, never matches the zero-initialised arrays

        struct areaHop_t
        {
            int area;
            int hop;
        };
        static areaHop_t s_bfsQueue[kMaxGatherAreas];
        int head = 0, tail = 0;

        // Stage 1.75 (portal_area_lights.md): seed the BFS from every area
        // touching a small box around the camera, not just the single
        // PointInArea(camera) classification. PointInArea is a hard plane
        // test — standing in a doorway/threshold, a sub-unit step can flip
        // which side you're classified on, which flips the hop number of
        // EVERY light in both rooms (hop=0 <-> hop=1, etc.) even though nothing
        // visible changed. Seeding both areas at hop=0 removes the flip.
        const float boundsEps = r_rtGIAreaBoundsEpsilon.GetFloat();
        int numSeedAreas = 0;
        if (boundsEps > 0.0f)
        {
            static int s_seedAreas[64];
            const idBounds seedBounds = idBounds(camPos).Expand(boundsEps);
            numSeedAreas = world->BoundsInAreas(seedBounds, s_seedAreas, 64);
            for (int i = 0; i < numSeedAreas; i++)
            {
                const int a = s_seedAreas[i];
                if (a < 0 || a >= world->numPortalAreas || s_areaVisitStamp[a] == s_gatherStamp)
                    continue;
                s_areaVisitStamp[a] = s_gatherStamp;
                s_bfsQueue[tail].area = a;
                s_bfsQueue[tail].hop = 0;
                tail++;
            }
        }
        if (numSeedAreas == 0)
        {
            // Fallback: bounds probe found nothing (epsilon disabled, or an
            // edge case at the world boundary) — behave exactly as before.
            s_areaVisitStamp[viewDef->areaNum] = s_gatherStamp;
            s_bfsQueue[tail].area = viewDef->areaNum;
            s_bfsQueue[tail].hop = 0;
            tail++;
        }

        if (dumpLights)
        {
            common->Printf("  [Stage 1.75] boundsEps=%.1f seedAreas(%d)=[", boundsEps, tail);
            for (int i = 0; i < tail; i++)
                common->Printf("%s%d", i ? "," : "", s_bfsQueue[i].area);
            common->Printf("]  (PointInArea single classification would have been area=%d)\n", viewDef->areaNum);
        }

        while (head < tail)
        {
            const areaHop_t cur = s_bfsQueue[head++];
            const portalArea_t *area = &world->portalAreas[cur.area];

            for (const areaReference_t *lref = area->lightRefs.areaNext; lref != &area->lightRefs;
                 lref = lref->areaNext)
            {
                const idRenderLightLocal *light = lref->light;
                if (light == NULL)
                    continue;
                const int li = light->index;
                // Dedupe: a light's volume typically spans several areas. Indices
                // beyond the stamp array (never in practice) skip dedupe and may
                // burn a duplicate candidate slot — harmless.
                if (li >= 0 && li < kHysteresisMaxLightIdx)
                {
                    if (s_lightVisitStamp[li] == s_gatherStamp)
                        continue;
                    s_lightVisitStamp[li] = s_gatherStamp;
                }
                considerLight(li, light, cur.area, cur.hop);
            }

            if (cur.hop >= areaHops)
                continue;
            for (const portal_t *portal = area->portals; portal != NULL; portal = portal->next)
            {
                // Closed doors: same PS_BLOCK_VIEW test the view flood uses.
                if (portal->doublePortal->blockingBits & PS_BLOCK_VIEW)
                    continue;
                const int into = portal->intoArea;
                if (into < 0 || into >= world->numPortalAreas)
                    continue;
                if (s_areaVisitStamp[into] == s_gatherStamp)
                    continue;
                s_areaVisitStamp[into] = s_gatherStamp;
                s_bfsQueue[tail].area = into;
                s_bfsQueue[tail].hop = cur.hop + 1;
                tail++;
            }
        }
        areasVisited = tail;

        // Stage 1.5 (portal_area_lights.md): union in every area id's own view-
        // frustum flood found actually visible this frame (portalArea_t::viewCount,
        // stamped by FindViewLightsAndEntities — the same flood the standard
        // renderer already runs to decide what to draw, so this costs nothing new).
        // Fixes what the BFS alone can't: standing still, looking across a room at
        // a light whose area is outside r_rtGIAreaHops range but plainly on screen
        // (a mapper's big open room, or a window/doorway sightline into a room the
        // BFS hasn't reached yet). The BFS handles the complementary case — a light
        // behind a blind corner you can't see yet but are about to walk into.
        // Degrades gracefully if a subview/mirror bumped tr.viewCount again since
        // the primary view's flood by the time this runs: worst case is silently
        // contributing nothing extra that frame (falls back to BFS-only), not a
        // correctness bug — see feedback_per_slot_counters, same per-gather-pass
        // stamp discipline as the BFS above (not tr.frameCount-keyed).
        for (int a = 0; a < world->numPortalAreas; a++)
        {
            if (world->portalAreas[a].viewCount != tr.viewCount)
                continue;
            if (s_areaVisitStamp[a] == s_gatherStamp)
                continue; // already covered by the BFS
            s_areaVisitStamp[a] = s_gatherStamp;
            viewStampAreas++;

            const portalArea_t *vArea = &world->portalAreas[a];
            for (const areaReference_t *lref = vArea->lightRefs.areaNext; lref != &vArea->lightRefs;
                 lref = lref->areaNext)
            {
                const idRenderLightLocal *light = lref->light;
                if (light == NULL)
                    continue;
                const int li = light->index;
                if (li >= 0 && li < kHysteresisMaxLightIdx)
                {
                    if (s_lightVisitStamp[li] == s_gatherStamp)
                        continue;
                    s_lightVisitStamp[li] = s_gatherStamp;
                }
                considerLight(li, light, a, -1); // hop=-1: reached via view-stamp, not the BFS
            }
        }
        areasVisited += viewStampAreas;
    }
    else
    {
        // Legacy/fallback: every lightDef in the world vs. the camera sphere.
        for (int li = 0; li < numLightDefs; li++)
        {
            const idRenderLightLocal *lightLocal = world->lightDefs[li];
            if (lightLocal == NULL)
                continue;
            considerLight(li, lightLocal, -1, -1);
        }
    }

    // Self-clearing: r_rtGILightDump prints one frame's worth of lights, not every frame.
    if (dumpLights)
        r_rtGILightDump.SetBool(false);

    // --- L1 (rt_optimization_tuning.md): stratified selection + importance ordering ---
    //
    // Pure nearest-to-camera selection (legacy path below) lets dim fill lights near
    // the camera crowd out a bright light 200u away, and makes distant sources pop
    // as the player moves. Instead: bucket by distance tier, sort each tier by
    // importance, give each tier a guaranteed slot quota, and upload the final set
    // ordered importance-first — order matters because every consumer walks a
    // *prefix* of this buffer (GI bounce: maxBounceLights, reflection hits: shadow
    // budget, volumetrics: maxLights).
    const int toUpload = Min(numCandidates, maxLights);

    if (!r_rtGILightStratify.GetBool())
    {
        // Legacy nearest-first path, kept for A/B comparison.
        if (numCandidates > 1 && numCandidates > maxLights)
        {
            const auto cmpDist = [](const void *a, const void *b) -> int {
                const Candidate *ca = (const Candidate *)a;
                const Candidate *cb = (const Candidate *)b;
                if (ca->distSq < cb->distSq)
                    return -1;
                if (ca->distSq > cb->distSq)
                    return 1;
                return 0;
            };
            qsort(s_candidates, numCandidates, sizeof(s_candidates[0]), cmpDist);
        }

        if (r_vkLogRT.GetInteger() >= 1)
        {
            if (useAreaWalk)
                common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                               "areaWalk start=%d visited=%d(+%d viewStamp) hops=%d (legacy distance sort)\n",
                               toUpload, numCandidates, maxLights, numLightDefs, giRadius, viewDef->areaNum,
                               areasVisited, viewStampAreas, areaHops);
            else
                common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                               "collectRadius=%.1f (legacy distance sort)\n",
                               toUpload, numCandidates, maxLights, numLightDefs, giRadius, lightCollectRadius);
        }
        for (int i = 0; i < toUpload; i++)
            lb->lights[lb->numLights++] = s_candidates[i].entry;

        // Vol-specific selection (portal_area_lights.md follow-up, 2026-08-22): same
        // pool, walked in the same (here: distance) order, filtered to lights that
        // can possibly reach within r_rtVolMaxDist of the camera, capped separately
        // from GI's own maxLights. See the stratified path below for the full
        // rationale — this legacy A/B path gets the same treatment for consistency.
        {
            GILightBuffer *volLb = (GILightBuffer *)vkRT.volLightSsboMapped[frameIdx];
            volLb->numLights = 0;
            volLb->bounceScale = lb->bounceScale;
            volLb->giRadius = lb->giRadius;
            volLb->emissiveScale = lb->emissiveScale;
            const int volCutoff = idMath::ClampInt(1, VK_GI_MAX_LIGHTS, r_rtVolMaxLights.GetInteger());
            const float volMaxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
            for (int i = 0; i < numCandidates && volLb->numLights < volCutoff; i++)
            {
                const Candidate &c = s_candidates[i];
                const float reach = volMaxDist + c.entry.posRadius[3];
                if (c.distSq > reach * reach)
                    continue; // light's sphere/cone can never reach a march sample — skip, don't burn a slot
                volLb->lights[volLb->numLights++] = c.entry;
            }
        }
        return;
    }

    // Sort tier-ascending, importance-descending: tiers become contiguous runs,
    // each run best-first, so per-tier quota picks are just prefixes.
    if (numCandidates > 1)
    {
        const auto cmpTierImportance = [](const void *a, const void *b) -> int {
            const Candidate *ca = (const Candidate *)a;
            const Candidate *cb = (const Candidate *)b;
            if (ca->tier != cb->tier)
                return ca->tier - cb->tier;
            if (ca->importance > cb->importance)
                return -1;
            if (ca->importance < cb->importance)
                return 1;
            return 0;
        };
        qsort(s_candidates, numCandidates, sizeof(s_candidates[0]), cmpTierImportance);
    }

    int tierStart[4] = {0, 0, 0, 0};
    int tierCount[4] = {0, 0, 0, 0};
    for (int i = 0; i < numCandidates; i++)
        tierCount[s_candidates[i].tier]++;
    for (int t = 1; t < 4; t++)
        tierStart[t] = tierStart[t - 1] + tierCount[t - 1];

    // Quotas 48/40/24/16 of 128 slots, scaled to maxLights. Guaranteed floors so
    // far-tier bright sources always get representation; unused quota flows to
    // other tiers (near-first) in the second pass.
    int quota[4];
    quota[0] = (maxLights * 48) / 128;
    quota[1] = (maxLights * 40) / 128;
    quota[2] = (maxLights * 24) / 128;
    quota[3] = maxLights - quota[0] - quota[1] - quota[2];

    int take[4];
    int used = 0;
    for (int t = 0; t < 4; t++)
    {
        take[t] = Min(quota[t], tierCount[t]);
        used += take[t];
    }
    for (int t = 0; t < 4 && used < toUpload; t++)
    {
        const int extra = Min(toUpload - used, tierCount[t] - take[t]);
        take[t] += extra;
        used += extra;
    }

    // Gather winners (each tier's best-first prefix), then order the final list
    // by importance so prefix-walking consumers see the lights that matter most.
    static const Candidate *s_selected[VK_GI_MAX_LIGHTS];
    int numSelected = 0;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < take[t] && numSelected < VK_GI_MAX_LIGHTS; i++)
            s_selected[numSelected++] = &s_candidates[tierStart[t] + i];

    if (numSelected > 1)
    {
        const auto cmpImportance = [](const void *a, const void *b) -> int {
            const Candidate *ca = *(const Candidate *const *)a;
            const Candidate *cb = *(const Candidate *const *)b;
            if (ca->importance > cb->importance)
                return -1;
            if (ca->importance < cb->importance)
                return 1;
            return 0;
        };
        qsort(s_selected, numSelected, sizeof(s_selected[0]), cmpImportance);
    }

    if (r_vkLogRT.GetInteger() >= 1)
    {
        if (useAreaWalk)
            common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                           "areaWalk start=%d visited=%d(+%d viewStamp) hops=%d tiers=%d/%d/%d/%d "
                           "take=%d/%d/%d/%d\n",
                           numSelected, numCandidates, maxLights, numLightDefs, giRadius, viewDef->areaNum,
                           areasVisited, viewStampAreas, areaHops, tierCount[0], tierCount[1], tierCount[2],
                           tierCount[3], take[0], take[1], take[2], take[3]);
        else
            common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                           "collectRadius=%.1f tiers=%d/%d/%d/%d take=%d/%d/%d/%d\n",
                           numSelected, numCandidates, maxLights, numLightDefs, giRadius, lightCollectRadius,
                           tierCount[0], tierCount[1], tierCount[2], tierCount[3], take[0], take[1], take[2], take[3]);
    }

    // Vol-specific selection (portal_area_lights.md follow-up, 2026-08-22): GI's own
    // selection above (s_selected) can legitimately include a light that's admitted
    // for bounce/reflection purposes but sits far outside anything the volumetric
    // march can ever sample (e.g. a room revealed down a long hallway by the Stage
    // 1.5 view-flood union — real for GI bounce off a surface near it, irrelevant to
    // fog scattered near the camera). Previously vol_march.comp just read a raw
    // prefix of GI's buffer, so a burst of those admitted-but-unreachable lights
    // could silently evict a genuinely nearby, march-relevant light purely by
    // consuming shared slots — the root cause of the hallway on/off pop. Fix: walk
    // GI's own importance-ordered selection (so near/far tier balance and hero-light
    // protection carry over unchanged) and keep only lights whose sphere/cone can
    // possibly reach within r_rtVolMaxDist of the camera, into a SEPARATE buffer
    // vol_march.comp reads instead of GI's.
    static const Candidate *s_volSelected[VK_GI_MAX_LIGHTS];
    int numVolSelected = 0;
    {
        const int volCutoff = idMath::ClampInt(1, VK_GI_MAX_LIGHTS, r_rtVolMaxLights.GetInteger());
        const float volMaxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
        for (int i = 0; i < numSelected && numVolSelected < volCutoff; i++)
        {
            const Candidate *c = s_selected[i];
            const float reach = volMaxDist + c->entry.posRadius[3];
            if (c->distSq > reach * reach)
                continue; // can never reach a march sample — skip, don't burn a vol slot
            s_volSelected[numVolSelected++] = c;
        }
    }

    // r_rtGILightDump's per-candidate log above shows *admission* (area/hop); this
    // shows the two downstream selections so a before/after at a suspected pop can
    // show exactly which light dropped and why (GI-level cut vs. vol-unreachable).
    if (dumpLights)
    {
        common->Printf("  [final order] importance-sorted GI upload (%d):\n", numSelected);
        for (int i = 0; i < numSelected; i++)
        {
            const Candidate *c = s_selected[i];
            const idRenderLightLocal *ld =
                (c->lightIdx >= 0 && c->lightIdx < numLightDefs) ? world->lightDefs[c->lightIdx] : NULL;
            common->Printf("    #%-3d %-28s imp=%.4f tier=%d dist=%6.1f\n", i,
                           (ld && ld->lightShader) ? ld->lightShader->GetName() : "<?>", c->importance, c->tier,
                           idMath::Sqrt(c->distSq));
        }
        common->Printf("  [vol selection] reachable within r_rtVolMaxDist=%.0f, capped at "
                       "r_rtVolMaxLights=%d (%d selected):\n",
                       r_rtVolMaxDist.GetFloat(), r_rtVolMaxLights.GetInteger(), numVolSelected);
        for (int i = 0; i < numVolSelected; i++)
        {
            const Candidate *c = s_volSelected[i];
            const idRenderLightLocal *ld =
                (c->lightIdx >= 0 && c->lightIdx < numLightDefs) ? world->lightDefs[c->lightIdx] : NULL;
            common->Printf("    #%-3d %-28s imp=%.4f dist=%6.1f reach=%.1f\n", i,
                           (ld && ld->lightShader) ? ld->lightShader->GetName() : "<?>", c->importance,
                           idMath::Sqrt(c->distSq), c->entry.posRadius[3]);
        }
    }

    for (int i = 0; i < numSelected; i++)
    {
        lb->lights[lb->numLights++] = s_selected[i]->entry;
        if (s_selected[i]->lightIdx < kHysteresisMaxLightIdx)
            s_lightSelectedFrame[s_selected[i]->lightIdx] = tr.frameCount;
    }
    s_prevUploadFrameNum = tr.frameCount;

    GILightBuffer *volLb = (GILightBuffer *)vkRT.volLightSsboMapped[frameIdx];
    volLb->numLights = 0;
    volLb->bounceScale = lb->bounceScale;
    volLb->giRadius = lb->giRadius;
    volLb->emissiveScale = lb->emissiveScale;
    for (int i = 0; i < numVolSelected; i++)
        volLb->lights[volLb->numLights++] = s_volSelected[i]->entry;

    // r_rtVolDump: read the entries back out of the MAPPED pointer, not out of
    // s_volSelected — the whole point is to show the bytes the march shader will
    // actually read, so a bad copy/stride/aliasing bug shows up here rather than
    // being masked by re-printing the source we copied from.
    //
    // posRadius.xyz is the light entity's origin, which uniquely identifies it in
    // the .map (the [vol selection] dump above can only print the *material* name,
    // and a map reuses one light material across dozens of entities).
    if (r_rtVolDump.GetBool())
    {
        r_rtVolDump.SetBool(false);     // one-shot: this site always runs, so it owns the clear
        vkRT_volDumpPending = true;     // hand off to VK_RT_DispatchVolumetrics for the params half
        common->Printf("=== [r_rtVolDump] vol light SSBO as uploaded (frameIdx=%d) ===\n", frameIdx);
        common->Printf("  numLights=%d  bounceScale=%.4f  giRadius=%.1f  emissiveScale=%.4f\n", volLb->numLights,
                       volLb->bounceScale, volLb->giRadius, volLb->emissiveScale);
        common->Printf("  (GI upload for comparison: numLights=%d;  numSelected=%d, numVolSelected=%d, "
                       "numCandidates=%d)\n",
                       lb->numLights, numSelected, numVolSelected, numCandidates);
        for (int i = 0; i < volLb->numLights; i++)
        {
            const GILightEntry &e = volLb->lights[i];
            const int li = (i < numVolSelected) ? s_volSelected[i]->lightIdx : -1;
            const idRenderLightLocal *ld = (li >= 0 && li < numLightDefs) ? world->lightDefs[li] : NULL;
            common->Printf("  #%-3d type=%u flags=0x%x origin=(%.0f %.0f %.0f) sphereR=%.1f\n"
                           "        emitPos=(%.0f %.0f %.0f) lightCenterOffset=%.1f\n"
                           "        color=(%.3f %.3f %.3f) intensity=%.3f\n"
                           "        boxExtents=(%.1f %.1f %.1f) reach=%.1f  coneDir=(%.3f %.3f %.3f) "
                           "cosHalf=%.3f\n"
                           "        shader=%s\n",
                           i, e.lightType, e.flags, e.posRadius[0], e.posRadius[1], e.posRadius[2], e.posRadius[3],
                           e.emitPos[0], e.emitPos[1], e.emitPos[2],
                           idVec3(e.emitPos[0] - e.posRadius[0], e.emitPos[1] - e.posRadius[1],
                                  e.emitPos[2] - e.posRadius[2])
                               .Length(),
                           e.colorIntensity[0], e.colorIntensity[1], e.colorIntensity[2], e.colorIntensity[3],
                           e.boxExtents[0], e.boxExtents[1], e.boxExtents[2], e.boxExtents[3], e.coneDir[0],
                           e.coneDir[1], e.coneDir[2], e.coneDir[3],
                           (ld && ld->lightShader) ? ld->lightShader->GetName() : "<?>");
        }
    }
}

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

    r.extent.width = (uint32_t)idMath::ClampInt(1, w - r.offset.x, rw);
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

    // --- Build UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    GIParamsUBO ubo = {};

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

    // Guard: skip dispatch if the matrix contains NaN (singular VP on degenerate frame).
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
            common->Warning("VK RT GI: invViewProj contains NaN — skipping dispatch");
            // Restore depth layout before returning.
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
            return;
        }
    }

    VkRect2D dispatchRect = VK_RT_GI_ComputeDispatchRect(viewDef);

    ubo.giRadius = Max(1.0f, r_rtGIRadius.GetFloat());
    ubo.numSamples = idMath::ClampInt(1, 8, r_rtGISamples.GetInteger());
    ubo.frameIndex = (uint32_t)(tr.frameCount);
    ubo.giStrength = idMath::ClampFloat(0.0f, 4.0f, r_rtGIStrength.GetFloat());
    ubo.screenWidth = (int32_t)gb.width;
    ubo.screenHeight = (int32_t)gb.height;
    ubo.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    ubo.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    ubo.scissorExtentX = (int32_t)dispatchRect.extent.width;
    ubo.scissorExtentY = (int32_t)dispatchRect.extent.height;
    ubo.checker = r_rtGICheckerboard.GetBool() ? 1 : 0;

    // Checkerboard phase — per frame SLOT, not per frame.
    //
    // giBuffer is one image per frame-in-flight slot, and vk.currentFrame advances
    // once per frame exactly as tr.frameCount does.  Deriving the parity from
    // frameIndex therefore gives slot 0 only ever even frames and slot 1 only ever
    // odd ones, so each slot traces one fixed half of the checkerboard *forever* and
    // the complementary half of its image is never written again.  giBuffer is only
    // cleared at allocation, so that half keeps whatever was last written to it —
    // a frozen full-res frame from the last time checkerboard was off — and the
    // temporal EMA re-injects it every frame, converging to the ghost instead of
    // washing it out.  That was the "toggling checkerboard leaves a permanent ghost"
    // bug.
    //
    // Counting per slot instead makes each slot alternate halves on successive
    // visits, so every pixel of every slot is refreshed within two visits.
    static uint32_t s_giCheckerPhase[VK_MAX_FRAMES_IN_FLIGHT] = {};
    ubo.checkerPhase = (int32_t)(s_giCheckerPhase[frameIdx]++ & 1u);
    // maxBounceLights: 0 disables Option B in rchit (Option A fallback); otherwise
    // caps the light loop so the GI pass evaluates fewer lights than reflections.
    // The SSBO numLights is NOT modified — reflections always see the full list.
    ubo.maxBounceLights = idMath::ClampInt(0, VK_GI_MAX_LIGHTS, r_rtGIMaxBounceLights.GetInteger());
    ubo.giContrast = idMath::ClampFloat(0.0f, 1.0f, r_rtGIContrast.GetFloat());
    // P3: shadow rays per bounce hit. 0 = legacy (one per light, up to
    // maxBounceLights); 1-2 = importance-sampled picks. Clamped to 2 because the
    // estimator in rt_light_eval.glsl keeps exactly two reservoirs in registers.
    ubo.stochasticLights = idMath::ClampInt(0, 2, r_rtGIStochasticLights.GetInteger());
    // P9: 0 also when the G-buffer isn't available, so the rgen never consults the
    // 1x1 null image it still has bound.
    ubo.useGbufNormal = (vk.gbufferSupported && r_rtGbufNormals.GetBool()) ? 1 : 0;
    memcpy(uboMapped, &ubo, sizeof(GIParamsUBO));

    // P3 mode transitions — one line per change, so a crash/perf report says
    // which estimator was live.
    if (r_vkLogRT.GetInteger() >= 1)
    {
        static int s_lastStochastic = -1;
        static int s_lastSamples = -1;
        if (ubo.stochasticLights != s_lastStochastic || ubo.numSamples != s_lastSamples)
        {
            s_lastStochastic = ubo.stochasticLights;
            s_lastSamples = ubo.numSamples;
            if (ubo.stochasticLights > 0)
                common->Printf("[GI] P3 stochastic: %d shadow ray(s)/hit x %d sample(s) = %d rays/px "
                               "(legacy worst case %d)\n",
                               ubo.stochasticLights, ubo.numSamples, ubo.stochasticLights * ubo.numSamples,
                               ubo.maxBounceLights * ubo.numSamples);
            else
                common->Printf("[GI] P3 stochastic OFF — up to %d shadow rays/hit x %d sample(s) = %d rays/px\n",
                               ubo.maxBounceLights, ubo.numSamples, ubo.maxBounceLights * ubo.numSamples);
        }
    }

    // DEBUG: log light counts once per second (keep for diagnostics).
    if (vkRT.giLightSsboMapped[frameIdx] != NULL && r_vkLogRT.GetInteger() >= 1)
    {
        static int s_lastLogFrame = -9999;
        if (tr.frameCount - s_lastLogFrame > 60)
        {
            s_lastLogFrame = tr.frameCount;
            const GILightBuffer *lb = (const GILightBuffer *)vkRT.giLightSsboMapped[frameIdx];
            int viewLightCount = 0;
            for (const viewLight_t *vl = viewDef->viewLights; vl; vl = vl->next)
                viewLightCount++;

            common->Printf("[GI] viewLights=%d  uploaded=%d  camPos=(%.0f,%.0f,%.0f)\n", viewLightCount, lb->numLights,
                           viewDef->renderView.vieworg.x, viewDef->renderView.vieworg.y, viewDef->renderView.vieworg.z);
        }
    }

    // --- Update descriptor set (once per frame slot when frameCount changes) ---
    static VkAccelerationStructureKHR s_lastGITlasHandle[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGIStorageView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGIDepthView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGIGbufView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    // P9: G-buffer normal/F0 — real per-frame image when supported, else the shared
    // 1x1 null image (a = 0 sentinel → gi_ray.rgen falls back to rt_ReconstructNormal).
    // Already in SHADER_READ_ONLY_OPTIMAL via the once-per-RT-block barrier in
    // vk_backend.cpp, which runs well before the GI dispatch.
    const bool haveGbuf = vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE;
    VkImageView gbufView = haveGbuf ? vkRT.gbufNormal[frameIdx].view : VK_RT_GetNullGbufNormalView();

    const bool resourceChanged =
        (s_lastGITlasHandle[frameIdx] != vkRT.tlas[frameIdx].handle) || (s_lastGIStorageView[frameIdx] != gb.view) ||
        (s_lastGIDepthView[frameIdx] != vk.depthSampledView) || (s_lastGIGbufView[frameIdx] != gbufView);

    bool refreshSet = (vkRT.giDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount) || resourceChanged;
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.giDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo giImgInfo = {};
        giImgInfo.imageView = gb.view;
        giImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(GIParamsUBO);

        VkDescriptorBufferInfo lightSsboInfo = {};
        lightSsboInfo.buffer = vkRT.giLightSsbo[frameIdx];
        lightSsboInfo.offset = 0;
        lightSsboInfo.range = sizeof(GILightBuffer);

        // Binding 5: G-buffer normal/F0 (P9). texelFetch in the rgen, so filter/wrap
        // state is irrelevant — reuse the depth sampler.
        VkDescriptorImageInfo gbufInfo = {};
        gbufInfo.sampler = vkRT.depthSampler;
        gbufInfo.imageView = gbufView;
        gbufInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[6] = {};

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
        writes[1].pImageInfo = &giImgInfo;

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

        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = ds;
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &gbufInfo;

        vkUpdateDescriptorSets(vk.device, 6, writes, 0, NULL);
        vkRT.giDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastGITlasHandle[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastGIStorageView[frameIdx] = gb.view;
        s_lastGIDepthView[frameIdx] = vk.depthSampledView;
        s_lastGIGbufView[frameIdx] = gbufView;
    }

    // --- Dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 0, 1,
                            &vkRT.giDescSets[frameIdx], 1, &uboOff);
    // set=1: material table
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 1, 1, &vkRT.matDescSet,
                            0, NULL);

    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
    {
        // Nothing to dispatch — still restore depth layout.
    }
    else
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI: dispatch %ux%u offset(%d,%d) strength=%.2f\n", dispatchRect.extent.width,
                           dispatchRect.extent.height, dispatchRect.offset.x, dispatchRect.offset.y, ubo.giStrength);

        vkCmdTraceRaysKHR(cmd, &vkRT.giRgenRegion, &vkRT.giMissRegion, &vkRT.giHitRegion, &vkRT.giCallRegion,
                          dispatchRect.extent.width, dispatchRect.extent.height, 1);
    }

    // --- Barrier: GI write -> compute/fragment shader read ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return;
    if (vkRT.giCompositePipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;
    vkReflBuffer_t &gb = vkRT.giBuffer[frameIdx];
    if (gb.image == VK_NULL_HANDLE || vkRT.giSampler == VK_NULL_HANDLE)
        return;

    // When GI temporal is active, giReadView[frameIdx] points to the accumulated
    // giHistory (set by VK_RT_DispatchTemporalResolveGI).  Otherwise it falls
    // back to the raw per-frame giBuffer view.
    VkImageView readView = vkRT.giReadView[frameIdx];
    if (readView == VK_NULL_HANDLE)
        readView = gb.view; // safety fallback

    // Update the descriptor set for this frame slot.  The GI image may have
    // been recreated (resize), so always write it before drawing.
    VkDescriptorImageInfo imgInfo = {};
    imgInfo.sampler = vkRT.giSampler;
    imgInfo.imageView = readView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vkRT.giCompositeDescSets[frameIdx];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    // Viewport and scissor are inherited from the caller (VK_RB_DrawView resumes
    // the render pass with the Y-flipped full viewport and s_viewScissor already
    // bound).  Do not override them here: widening the scissor to the full
    // swapchain extent would leave subsequent interaction draws using the wrong
    // region, and the GI buffer outside the dispatch rect is already cleared to
    // black so blending it adds nothing visually.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.giCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.giCompositeLayout, 0, 1,
                            &vkRT.giCompositeDescSets[frameIdx], 0, NULL);

    // 3 vertices, no vertex buffer — the vert shader generates the triangle from gl_VertexIndex.
    vkCmdDraw(cmd, 3, 1, 0, 0);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: composite drawn frame=%d slot=%d\n", tr.frameCount, frameIdx);
}

// ===========================================================================
// GI À-trous spatial filter (Phase 6.3)
// ===========================================================================

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

static bool VK_RT_AllocGIAtrousImage(vkReflBuffer_t &img, uint32_t width, uint32_t height)
{
    img.width = width;
    img.height = height;

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
    VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &img.image));

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
        common->Warning("VK RT GI Atrous: no device-local memory for image");
        vkDestroyImage(vk.device, img.image, NULL);
        img.image = VK_NULL_HANDLE;
        img.memory = VK_NULL_HANDLE;
        img.view = VK_NULL_HANDLE;
        img.width = 0;
        img.height = 0;
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memTypeIdx;
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &img.memory));
    VK_CHECK(vkBindImageMemory(vk.device, img.image, img.memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &img.view));

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

static void VK_RT_FreeGIAtrousImage(vkReflBuffer_t &img)
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
    img.width = 0;
    img.height = 0;
}

static void VK_RT_CreateGIAtrousImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocGIAtrousImage(vkRT.giAtrousA[i], width, height))
            common->Warning("VK RT GI Atrous: failed to allocate giAtrousA slot %d", i);
        if (!VK_RT_AllocGIAtrousImage(vkRT.giAtrousB[i], width, height))
            common->Warning("VK RT GI Atrous: failed to allocate giAtrousB slot %d", i);
    }
}

static void VK_RT_DestroyGIAtrousImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeGIAtrousImage(vkRT.giAtrousA[i]);
        VK_RT_FreeGIAtrousImage(vkRT.giAtrousB[i]);
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitGIAtrousPipeline
// ---------------------------------------------------------------------------

static void VK_RT_InitGIAtrousPipeline(void)
{
    // gi_atrous.comp bindings:
    //   binding 0: COMBINED_IMAGE_SAMPLER — input GI buffer (giSrc, sampler2D)
    //   binding 1: STORAGE_IMAGE          — output GI buffer (giDst, rgba16f image2D)
    //   binding 2: COMBINED_IMAGE_SAMPLER — depth sampler
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.giAtrousDescLayout));

    // Push constants: 40 bytes matching gi_atrous.comp PC block.
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 40;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.giAtrousDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.giAtrousPipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/gi_atrous.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI Atrous: failed to load gi_atrous.comp.spv — GI spatial filter disabled");
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
    pipeCI.layout = vkRT.giAtrousPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.giAtrousPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    // Pool: 3 sets per frame slot × VK_MAX_FRAMES_IN_FLIGHT
    //   COMBINED_IMAGE_SAMPLER: 2 per set (input + depth) × 3 × 2 slots = 12
    //   STORAGE_IMAGE:          1 per set                  × 3 × 2 slots = 6
    const int totalSets = VK_MAX_FRAMES_IN_FLIGHT * 3;
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2 * totalSets;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1 * totalSets;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = totalSets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.giAtrousDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT * 3];
    for (int i = 0; i < totalSets; i++)
        layouts[i] = vkRT.giAtrousDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giAtrousDescPool;
    dsAlloc.descriptorSetCount = totalSets;
    dsAlloc.pSetLayouts = layouts;
    // Allocate flat: [0][0],[0][1],[0][2],[1][0],[1][1],[1][2]
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, &vkRT.giAtrousDescSets[0][0]));

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giAtrousDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT GI Atrous: spatial filter pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public entry points — GI À-trous
// ---------------------------------------------------------------------------

void VK_RT_InitGIAtrous(void)
{
    VK_RT_InitGIAtrousPipeline();
    if (vkRT.giAtrousPipeline == VK_NULL_HANDLE)
    {
        VK_RT_DestroyGIAtrousImages();
        return;
    }
    VK_RT_CreateGIAtrousImages(vk.swapchainExtent.width, vk.swapchainExtent.height);
}

void VK_RT_ShutdownGIAtrous(void)
{
    VK_RT_DestroyGIAtrousImages();

    if (vkRT.giAtrousPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giAtrousPipeline, NULL);
        vkRT.giAtrousPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giAtrousPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giAtrousPipelineLayout, NULL);
        vkRT.giAtrousPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giAtrousDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giAtrousDescPool, NULL);
        vkRT.giAtrousDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giAtrousDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giAtrousDescLayout, NULL);
        vkRT.giAtrousDescLayout = VK_NULL_HANDLE;
    }
}

void VK_RT_ResizeGIAtrous(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyGIAtrousImages();
    VK_RT_CreateGIAtrousImages(width, height);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giAtrousDescSetLastUpdatedFrameCount[i] = -1;
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchAtrousGI
// ---------------------------------------------------------------------------

void VK_RT_DispatchAtrousGI(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return;

    const int frameIdx = vk.currentFrame;

    const int iters = idMath::ClampInt(0, 8, r_rtGIAtrousIterations.GetInteger());
    if (!r_rtGIAtrous.GetBool() || iters == 0)
        return; // No filter: composite reads the current giReadView unchanged.

    if (vkRT.giAtrousPipeline == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI Atrous: pipeline is NULL, skipping dispatch");
        return;
    }

    vkReflBuffer_t &bufA = vkRT.giAtrousA[frameIdx];
    vkReflBuffer_t &bufB = vkRT.giAtrousB[frameIdx];

    if (bufA.image == VK_NULL_HANDLE || bufB.image == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI Atrous: skip — images not ready (slot %d)\n", frameIdx);
        return;
    }

    const VkRect2D dispatchRect = VK_RT_GI_ComputeDispatchRect(viewDef);
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    // --- Depth barrier: ATTACHMENT_OPTIMAL → READ_ONLY for depth edge-stopping ---
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Update descriptor sets (once per tr.frameCount per slot) ---
    // DS[frameIdx][0]: giReadView(sampler) → giAtrousA  — rebuilt each frame (giReadView may change)
    // DS[frameIdx][1]: giAtrousA(sampler)  → giAtrousB  — stable after images created
    // DS[frameIdx][2]: giAtrousB(sampler)  → giAtrousA  — stable after images created
    if (vkRT.giAtrousDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount)
    {
        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo srcInfos[3] = {};
        srcInfos[0] = {vkRT.giSampler, vkRT.giReadView[frameIdx], VK_IMAGE_LAYOUT_GENERAL}; // giReadView → A
        srcInfos[1] = {vkRT.giSampler, bufA.view, VK_IMAGE_LAYOUT_GENERAL};                 // A → B
        srcInfos[2] = {vkRT.giSampler, bufB.view, VK_IMAGE_LAYOUT_GENERAL};                 // B → A

        VkDescriptorImageInfo dstInfos[3] = {};
        dstInfos[0] = {VK_NULL_HANDLE, bufA.view, VK_IMAGE_LAYOUT_GENERAL}; // → A
        dstInfos[1] = {VK_NULL_HANDLE, bufB.view, VK_IMAGE_LAYOUT_GENERAL}; // → B
        dstInfos[2] = {VK_NULL_HANDLE, bufA.view, VK_IMAGE_LAYOUT_GENERAL}; // → A

        VkWriteDescriptorSet writes[9] = {};
        for (int s = 0; s < 3; s++)
        {
            VkDescriptorSet ds = vkRT.giAtrousDescSets[frameIdx][s];

            writes[s * 3 + 0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[s * 3 + 0].dstSet = ds;
            writes[s * 3 + 0].dstBinding = 0;
            writes[s * 3 + 0].descriptorCount = 1;
            writes[s * 3 + 0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[s * 3 + 0].pImageInfo = &srcInfos[s];

            writes[s * 3 + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[s * 3 + 1].dstSet = ds;
            writes[s * 3 + 1].dstBinding = 1;
            writes[s * 3 + 1].descriptorCount = 1;
            writes[s * 3 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[s * 3 + 1].pImageInfo = &dstInfos[s];

            writes[s * 3 + 2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[s * 3 + 2].dstSet = ds;
            writes[s * 3 + 2].dstBinding = 2;
            writes[s * 3 + 2].descriptorCount = 1;
            writes[s * 3 + 2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[s * 3 + 2].pImageInfo = &depthInfo;
        }
        vkUpdateDescriptorSets(vk.device, 9, writes, 0, NULL);
        vkRT.giAtrousDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    const float sigmaL = r_rtGIAtrousSigmaL.GetFloat();
    const float sigmaZ = r_rtGIAtrousSigmaZ.GetFloat();

    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giAtrousPipeline);

    for (int pass = 0; pass < iters; pass++)
    {
        // Descriptor set selection:
        //   pass 0          → DS[0] (giReadView → A)
        //   pass 1, 3, 5 …  → DS[1] (A → B)
        //   pass 2, 4, 6 …  → DS[2] (B → A)
        const int descIdx = (pass == 0) ? 0 : (1 + (pass - 1) % 2);

        struct AtrousGIPC
        {
            int32_t stepSize;
            float sigmaL;
            float sigmaZ;
            float pad;
            int32_t screenWidth;
            int32_t screenHeight;
            int32_t scissorOffsetX;
            int32_t scissorOffsetY;
            int32_t scissorExtentX;
            int32_t scissorExtentY;
        } pc;
        static_assert(sizeof(pc) == 40, "AtrousGIPC size mismatch");

        pc.stepSize = 1 << pass;
        pc.sigmaL = sigmaL;
        pc.sigmaZ = sigmaZ;
        pc.pad = 0.0f;
        pc.screenWidth = (int32_t)bufA.width;
        pc.screenHeight = (int32_t)bufA.height;
        pc.scissorOffsetX = (int32_t)dispatchRect.offset.x;
        pc.scissorOffsetY = (int32_t)dispatchRect.offset.y;
        pc.scissorExtentX = (int32_t)dispatchRect.extent.width;
        pc.scissorExtentY = (int32_t)dispatchRect.extent.height;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giAtrousPipelineLayout, 0, 1,
                                &vkRT.giAtrousDescSets[frameIdx][descIdx], 0, NULL);
        vkCmdPushConstants(cmd, vkRT.giAtrousPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 40, &pc);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        // Barrier: output becomes input for the next pass; final pass transfers to FRAGMENT.
        const bool isLastPass = (pass == iters - 1);
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             isLastPass ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }

    // Update giReadView to whichever buffer the final pass wrote.
    // pass 0 → A, pass 1 → B, pass 2 → A, pass 3 → B …
    // When (iters-1) is even the last pass wrote A; when odd it wrote B.
    const bool finalInA = ((iters - 1) % 2 == 0);
    vkRT.giReadView[frameIdx] = finalInA ? bufA.view : bufB.view;

    // --- Restore depth to ATTACHMENT_OPTIMAL ---
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
        common->Printf("VK RT GI Atrous: %d passes slot=%d final=%s step_max=%d\n", iters, frameIdx,
                       finalInA ? "A" : "B", 1 << (iters - 1));
}

// ---------------------------------------------------------------------------
// VK_RT_InitGIAlbedoModPipeline (docs/plans/gi_albedo_target.md)
// ---------------------------------------------------------------------------

static void VK_RT_InitGIAlbedoModPipeline(void)
{
    // gi_albedo_mod.comp bindings:
    //   binding 0: COMBINED_IMAGE_SAMPLER — denoised GI (giSrc, sampler2D)
    //   binding 1: COMBINED_IMAGE_SAMPLER — receiver albedo (albedoSampler, sampler2D)
    //   binding 2: STORAGE_IMAGE          — output GI (giDst, rgba16f image2D)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.giAlbedoModDescLayout));

    // Push constants: 24 bytes matching gi_albedo_mod.comp's PC block (6 int32s).
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 24;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.giAlbedoModDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.giAlbedoModPipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/gi_albedo_mod.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI Albedo Mod: failed to load gi_albedo_mod.comp.spv — GI albedo modulation disabled");
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
    pipeCI.layout = vkRT.giAlbedoModPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.giAlbedoModPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    // Pool: 1 set per frame slot, rewritten every dispatch (giReadView/albedo/scratch
    // all vary frame to frame — no benefit to the atrous-style "reuse if unchanged").
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2 * VK_MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1 * VK_MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.giAlbedoModDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giAlbedoModDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giAlbedoModDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giAlbedoModDescSets));

    common->Printf("VK RT GI Albedo Mod: pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public entry points — GI receiver-albedo modulation
// ---------------------------------------------------------------------------

void VK_RT_InitGIAlbedoMod(void)
{
    VK_RT_InitGIAlbedoModPipeline();
}

void VK_RT_ShutdownGIAlbedoMod(void)
{
    if (vkRT.giAlbedoModPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giAlbedoModPipeline, NULL);
        vkRT.giAlbedoModPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giAlbedoModPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giAlbedoModPipelineLayout, NULL);
        vkRT.giAlbedoModPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giAlbedoModDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giAlbedoModDescPool, NULL);
        vkRT.giAlbedoModDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giAlbedoModDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giAlbedoModDescLayout, NULL);
        vkRT.giAlbedoModDescLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGIAlbedoMod (public)
// ---------------------------------------------------------------------------

void VK_RT_DispatchGIAlbedoMod(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool() || !r_rtGIAlbedo.GetBool())
        return;
    if (!vk.gbufferSupported)
        return; // no gbufAlbedo target to read — legacy raw-radiance composite
    if (vkRT.giAlbedoModPipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;

    const vkReflBuffer_t &albedoBuf = vkRT.gbufAlbedo[frameIdx];
    if (albedoBuf.view == VK_NULL_HANDLE)
        return;

    vkReflBuffer_t &bufA = vkRT.giAtrousA[frameIdx];
    vkReflBuffer_t &bufB = vkRT.giAtrousB[frameIdx];
    if (bufA.image == VK_NULL_HANDLE || bufB.image == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI Albedo Mod: skip — scratch images not ready (slot %d)\n", frameIdx);
        return;
    }

    // Scratch selection: whichever of A/B giReadView is NOT currently pointing at.
    // Covers all upstream states — à-trous ran (giReadView is A or B already) or
    // was skipped (giReadView is giHistory/giBuffer, matches neither, default A).
    const bool srcIsA = (vkRT.giReadView[frameIdx] == bufA.view);
    vkReflBuffer_t &dst = srcIsA ? bufB : bufA;

    const VkRect2D dispatchRect = VK_RT_GI_ComputeDispatchRect(viewDef);
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    // Prior writer (à-trous compute, or temporal resolve if à-trous is disabled) may
    // have left its last barrier targeting FRAGMENT_SHADER (for the legacy composite
    // path); make the result visible to this dispatch's COMPUTE_SHADER read too.
    {
        VkMemoryBarrier srcBarrier = {};
        srcBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        srcBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &srcBarrier, 0, NULL, 0, NULL);
    }

    // Rewritten every call — giReadView/scratch/albedo view all vary frame to frame.
    VkDescriptorSet ds = vkRT.giAlbedoModDescSets[frameIdx];

    VkDescriptorImageInfo srcInfo = {vkRT.giSampler, vkRT.giReadView[frameIdx], VK_IMAGE_LAYOUT_GENERAL};
    // texelFetch in the shader — filter/wrap state is irrelevant; reuse the depth
    // sampler, same convention as gbufNormal reads elsewhere (P9).
    VkDescriptorImageInfo albedoInfo = {vkRT.depthSampler, albedoBuf.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dstInfo = {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &albedoInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = ds;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(vk.device, 3, writes, 0, NULL);

    struct AlbedoModPC
    {
        int32_t screenWidth;
        int32_t screenHeight;
        int32_t scissorOffsetX;
        int32_t scissorOffsetY;
        int32_t scissorExtentX;
        int32_t scissorExtentY;
    } pc;
    static_assert(sizeof(pc) == 24, "AlbedoModPC size mismatch");
    pc.screenWidth = (int32_t)dst.width;
    pc.screenHeight = (int32_t)dst.height;
    pc.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    pc.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    pc.scissorExtentX = (int32_t)dispatchRect.extent.width;
    pc.scissorExtentY = (int32_t)dispatchRect.extent.height;

    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giAlbedoModPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giAlbedoModPipelineLayout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cmd, vkRT.giAlbedoModPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24, &pc);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Output becomes the new giReadView; composite reads it via FRAGMENT sampling.
    VkMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    dstBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &dstBarrier, 0, NULL, 0, NULL);

    vkRT.giReadView[frameIdx] = dst.view;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI Albedo Mod: applied slot=%d dst=%s\n", frameIdx, srcIsA ? "B" : "A");
}
