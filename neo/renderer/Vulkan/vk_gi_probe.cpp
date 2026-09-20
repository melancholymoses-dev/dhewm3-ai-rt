/*
===========================================================================

dhewm3-rt Vulkan — vk_gi_probe.cpp — world-space irradiance probes for GI.

Part B of 20260906_froxel_probe_gi.md: the same move froxels made for
volumetrics, applied to GI.  Per-pixel GI recomputes a hemisphere integral for
every pixel every frame and then spends three denoise passes fighting the noise
that produces; probes compute it once per probe in WORLD space, where temporal
reuse is trivially valid, and reduce the per-pixel cost to a handful of texture
fetches.

Four passes:

  1. trace   (G1) — gi_probe_trace.rgen, the SECOND raygen group in vk_gi.cpp's
                    existing GI ray pipeline.  Fires r_rtGIProbeRays rays for
                    each of r_rtGIProbeUpdatesPerFrame probes into a scratch
                    image.  Shares gi_ray.rchit/rmiss with the per-pixel path,
                    so a probe sees exactly the scene per-pixel GI sees.
  2. blend   (G1) — gi_probe_blend.comp, scratch rays -> octahedral irradiance
                    and visibility atlases, with an EMA.
  3. border  (G1) — gi_probe_border.comp, octahedral seam fill.
  4. resolve (G2) — gi_probe_resolve.comp, per-pixel 8-probe fetch -> giBuffer,
                    and every debug overlay.  Under r_rtGIProbes 1 this stands
                    the per-pixel rgen launch, the temporal EMA and the a-trous
                    chain down; gi_albedo_mod and gi_composite are unchanged and
                    still run after it.

The lattice is ABSOLUTE (cell * spacing) and storage is TOROIDAL
(cell mod gridDim), so the window can follow the camera without probes sliding
and without rewriting the atlas — see gi_probe_common.glsl.  The atlases are a
SINGLE shared pair, not per frame-in-flight slot: they are an EMA accumulator
and per-slot history is what silently halved the AO/GI/Vol update rate before
the 2026-09-06 fix.

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
// CVars (20260906_froxel_probe_gi.md B.4)
// ---------------------------------------------------------------------------

static idCVar r_rtGIProbes("r_rtGIProbes", "1", CVAR_RENDERER | CVAR_INTEGER,
                           "Global illumination sampling structure: 0 = per-pixel GI rays "
                           "(gi_ray.rgen + denoise chain), 1 = world-space irradiance probes. "
                           "At 1 the per-pixel launch, r_rtGITemporal and r_rtGIAtrous are stood "
                           "down;");

static idCVar r_rtGIProbeSpacing("r_rtGIProbeSpacing", "64", CVAR_RENDERER | CVAR_FLOAT,
                                 "World units between probes. Doom 3 interiors are small; 64 is "
                                 "roughly one large step. Change forces a device-idle realloc.");

static idCVar r_rtGIProbeCountX("r_rtGIProbeCountX", "32", CVAR_RENDERER | CVAR_INTEGER,
                                "Probe grid width. Change forces a device-idle realloc.");
static idCVar r_rtGIProbeCountY("r_rtGIProbeCountY", "32", CVAR_RENDERER | CVAR_INTEGER,
                                "Probe grid depth (Doom 3's second horizontal axis). Realloc on change.");
static idCVar r_rtGIProbeCountZ("r_rtGIProbeCountZ", "16", CVAR_RENDERER | CVAR_INTEGER,
                                "Probe grid height. Realloc on change.");

static idCVar r_rtGIProbeRays("r_rtGIProbeRays", "128", CVAR_RENDERER | CVAR_INTEGER,
                              "Rays fired per probe update (1-256). Total rays per frame is this "
                              "times r_rtGIProbeUpdatesPerFrame.");

static idCVar r_rtGIProbeUpdatesPerFrame("r_rtGIProbeUpdatesPerFrame", "1024", CVAR_RENDERER | CVAR_INTEGER,
                                         "Probes refreshed per frame, round-robin. At the 32x32x16 default "
                                         "this is a full refresh every 16 frames.");

static idCVar r_rtGIProbeHysteresis("r_rtGIProbeHysteresis", "0.97", CVAR_RENDERER | CVAR_FLOAT,
                                    "Atlas EMA history weight. A probe that has never been traced since it "
                                    "entered the grid ignores this and takes the new value outright — its "
                                    "atlas tile belongs to a different probe (toroidal storage).");

static idCVar r_rtGIProbeNormalBias("r_rtGIProbeNormalBias", "8.0", CVAR_RENDERER | CVAR_FLOAT,
                                    "Receiver offset along the surface normal before the probe lookup (G2).");

static idCVar r_rtGIProbeViewBias("r_rtGIProbeViewBias", "8.0", CVAR_RENDERER | CVAR_FLOAT,
                                  "Receiver offset toward the eye, on top of r_rtGIProbeNormalBias (G3). This is "
                                  "what stops a wall seen at a grazing angle from registering as its own occluder "
                                  "in the Chebyshev test. Raise it if r_rtGIProbeDebug 3 shows blue "
                                  "(over-darkening) along walls; lower it if raising it turns them red (leaks).");

static idCVar r_rtGIProbeInsideThreshold("r_rtGIProbeInsideThreshold", "0.25", CVAR_RENDERER | CVAR_FLOAT,
                                         "G4: fraction of a probe's rays that must hit BACK faces before it is "
                                         "classified as buried in geometry and given zero weight in the resolve. "
                                         "0 disables the classification.");

static idCVar r_rtGIProbeOutsideThreshold("r_rtGIProbeOutsideThreshold", "0.9", CVAR_RENDERER | CVAR_FLOAT,
                                          "G4: fraction of a probe's rays that must hit NOTHING before it is "
                                          "classified as sitting in the void outside the level and given zero "
                                          "weight. Deliberately high.");

static idCVar r_rtGIProbeVisibility("r_rtGIProbeVisibility", "1", CVAR_RENDERER | CVAR_BOOL,
                                    "Chebyshev visibility weighting in the resolve (G3) — the thin-wall leak "
                                    "mitigation. Set 0 to see the raw "
                                    "leak surface r_rtGIProbeDebug 3 is measuring against.");

static idCVar r_rtGIProbeMaxRayDist("r_rtGIProbeMaxRayDist", "512", CVAR_RENDERER | CVAR_FLOAT,
                                    "Probe ray length, and the value a miss records in the visibility "
                                    "moments. Independent of r_rtGIRadius, which is the per-pixel path's. "
                                    "Also the unit the distance atlas is normalised by (rg16f cannot hold "
                                    "a raw second moment), so changing it re-scales the atlas and the EMA "
                                    "needs about a full refresh to wash the old encoding out.");

static idCVar r_rtGIProbeDistSharpness("r_rtGIProbeDistSharpness", "50.0", CVAR_RENDERER | CVAR_FLOAT,
                                       "Cosine-power lobe width for the visibility moments. Much tighter "
                                       "than the irradiance lobe because these moments must carry geometry "
                                       "EDGES, not a smooth field.");

static idCVar r_rtGIProbeDebug("r_rtGIProbeDebug", "0", CVAR_RENDERER | CVAR_INTEGER,
                               "Probe overlay: 0=off, 1=probe spheres tinted by stored irradiance, "
                               "2=probe weights (blue->red = fraction of weight Chebyshev rejected; "
                               "magenta = no usable probe, GI is black there), "
                               "3=leak detector: Chebyshev vs ray-traced ground truth — green=agree, "
                               "red=LEAK (occluded but let through), blue=over-dark (visible but blocked), "
                               "magenta=all 8 probes excluded, so that surface gets NO probe GI at all, "
                               "4=probe state (green=usable, red=never traced, blue=buried in geometry, "
                               "yellow=in the void outside the level; flagged probes x-ray through walls at half "
                               "brightness, so dim=behind geometry and bright=in open air, i.e. misclassified), "
                               "5=distance atlas (blue->red ramp of mean hit distance / "
                               "r_rtGIProbeMaxRayDist; magenta = impossible second moment), "
                               "6=RAW backface fraction, 7=RAW miss fraction (blue=0 green=0.5 red=1). "
                               "6/7 show the measurement mode 4's verdict is made from: an air probe must "
                               "read BLUE in mode 6, and if it does not, no threshold can fix it. "
                               "8=G5b fast bucket: GREEN = share of this pixel's GI that is being "
                               "flicker-factorized, RED = flickering lights reaching the point / 3. Only the "
                               "highest-importance one is bucketed, so red is what G5b is still leaving "
                               "EMA-smeared — dark red across a level says one bucket covers it.");

static idCVar r_rtGIProbeDebugGain("r_rtGIProbeDebugGain", "4.0", CVAR_RENDERER | CVAR_FLOAT,
                                   "Mode-1-only gain, so stored irradiance survives the Uchimura toe "
                                   "instead of reading as black. Mirrors r_rtVolFroxelDebugGain.");

static idCVar r_rtGIProbeDebugRadius("r_rtGIProbeDebugRadius", "4.0", CVAR_RENDERER | CVAR_FLOAT,
                                     "Radius of the debug probe spheres, world units. Must stay below "
                                     "half r_rtGIProbeSpacing or the overlay's ray stepping can skip one.");

static idCVar r_rtGIProbeDump("r_rtGIProbeDump", "0", CVAR_RENDERER | CVAR_BOOL,
                              "One-shot dump: grid origin/dims, memory, atlas geometry, traced vs "
                              "never-traced counts, update schedule. Self-clears after one frame.");

// ---------------------------------------------------------------------------
// Forward declarations from other vk_*.cpp modules
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern VkImageView VK_RT_GetNullGbufNormalView(void);

// vk_gi.cpp — this frame's GIParams dynamic-UBO binding. gi_ray.rchit reads that
// block, and the probe trace runs the same hit shader, so the probe dispatch has
// to bind set 0 with a valid offset into it. Reusing the per-pixel path's rather
// than allocating a second identical block: every field the hit shader reads
// (maxBounceLights, frameIndex, stochasticLights) is per-frame, not per-launch.
extern bool VK_RT_GIParamsBinding(int frameIdx, uint32_t *outOffset);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;
extern idCVar r_rtGI;
extern idCVar r_rtGIStrength; // vk_gi.cpp — shared so an r_rtGIProbes A/B is at matched strength
extern idCVar r_rtGIContrast; // ditto: the resolve applies the same contrast push gi_ray.rgen does
extern idCVar r_rtGbufNormals;

// ---------------------------------------------------------------------------
// GIProbeParamsUBO — must match the std140 GIProbeParams block in
// gi_probe_common.glsl, which every probe pass includes at binding 2 of its
// probe set (set 2 in the RT pipeline, set 0 in the compute pipelines).
//
// vec4-packed on purpose, same reasoning as VolFroxelParamsUBO.
// ---------------------------------------------------------------------------

struct GIProbeParamsUBO
{
    float invViewProj[16]; //   0
    float cameraPosW[4];   //  64  xyz = eye world position
    float gridOrigin[4];   //  80  xyz = world pos of baseCell, w = spacing
    int32_t gridDim[4];    //  96  xyz = Nx,Ny,Nz  w = total probe count
    int32_t baseCell[4];   // 112  xyz = absolute lattice cell of the window corner
    int32_t atlas[4];      // 128  x=tilesX y=tilesY z=irradiance side w=distance side
    int32_t rays[4];       // 144  x=raysPerProbe y=probesPerFrame z=updateBase w=frameIndex
    float tune[4];         // 160  x=hysteresis y=normalBias z=giStrength w=maxRayDist
    int32_t misc[4];       // 176  x=debugMode y=useGbufNormal z=visibility w=unused
    int32_t screen[4];     // 192  x=screenW y=screenH z=outW w=outH
    int32_t rect[4];       // 208  resolve dispatch rect, output-image space
    float debug[4];        // 224  x=debugGain y=probeRadius z=distSharpness w=unused
    float tune2[4];        // 240  x=giContrast y=viewBias  z/w reserved
};
static_assert(sizeof(GIProbeParamsUBO) == 256, "GIProbeParamsUBO size mismatch");

// Mirrors GIProbeState in gi_probe_common.glsl. std430 gives vec3 a 16-byte
// alignment, so the trailing uint packs into the same 16 bytes.
struct GIProbeStateEntry
{
    float offset[3]; //  0  G4 relocation from the lattice point
    uint32_t flags;  // 12  GIPROBE_FLAG_*
};
static_assert(sizeof(GIProbeStateEntry) == 16, "GIProbeStateEntry size mismatch");

// Mirrors GIProbeStats in gi_probe_blend.comp.  Written by the GPU, read back
// here; the CPU never writes the device copy after creation.
//
// These used to sit in GIProbeStateEntry, which is PER frame-in-flight slot, and
// that was wrong in a way that produced a visible 2-frame oscillation. The blend
// pass writes slot N; the CPU reads slot N+1 the next frame, finds the older
// generation it uploaded itself, and copies that back over its shadow. The
// classification therefore alternated between two generations at frame rate, the
// dead band could not damp it, and every probe in the grid flipped in lockstep —
// probe debug 3 strobed red/green at half the frame rate and mode 0's admitted
// probe set changed every frame. An EMA accumulator cannot live in a per-slot
// buffer; the atlases are shared for exactly the same reason.
struct GIProbeStatsEntry
{
    float backface; // 0  fraction of rays hitting a back face
    float miss;     // 4  fraction of rays hitting nothing
};
static_assert(sizeof(GIProbeStatsEntry) == 8, "GIProbeStatsEntry size mismatch");

#define GIPROBE_FLAG_TRACED 0x1u
#define GIPROBE_FLAG_INSIDE 0x2u  // buried in a solid brush
#define GIPROBE_FLAG_OUTSIDE 0x4u // adrift in the void outside the sealed level hull

// Hard cap, mirrored by GIPROBE_MAX_RAYS in gi_probe_blend.comp's shared-memory
// ray cache. Raising one without the other silently truncates the ray loop.
#define VK_GIPROBE_MAX_RAYS 256

// Bounds the atlas allocation: 65536 probes is a 4608x4608 distance atlas
// (85 MiB) and a 2560x2560 irradiance atlas (52 MiB), already past sane.
#define VK_GIPROBE_MAX_PROBES 65536

// Probe set bindings, shared by the trace rgen (set 2) and the blend/border
// compute passes (set 0). Named because G5b's fast scratch made it grow, and
// the count appears in three places that must agree.
#define VK_GIPROBE_DESC_BINDINGS 7

// Resolve set bindings. Its own layout, and its own count.
#define VK_GIPROBE_RESOLVE_BINDINGS 10

// ---------------------------------------------------------------------------
// CPU-side probe bookkeeping
//
// The traced/inside flags live here rather than being written by the GPU
// because the blend pass has to know whether a probe had history BEFORE this
// frame's trace, and the CPU is the only place that knows both the schedule and
// the grid scroll. The array is uploaded (pre-trace state) and only then are
// this frame's scheduled probes marked traced, so the flag the shader reads is
// always "was this probe traced before now".
// ---------------------------------------------------------------------------

static GIProbeStateEntry *s_probeState = NULL;
static int s_probeStateCount = 0;

// CPU shadow of the GPU-written statistics. Kept separate from the device buffer
// so the CPU never writes the latter behind an in-flight blend dispatch: a probe
// that scrolls into a new tile is zeroed HERE, and the readback below refuses to
// adopt the device value until that probe has been traced again.
static GIProbeStatsEntry *s_probeStats = NULL;

// Geometry the resources were actually built at, so a mid-session cvar change
// can be detected. Same pattern as s_froxelDim in vk_vol_froxel.cpp.
static int32_t s_probeDim[3] = {0, 0, 0};
static int32_t s_probeRays = 0;
static int32_t s_probeUpdates = 0;
// G5b: K as the irradiance atlas was actually sized for. Changing
// r_rtGIProbeFastBuckets changes the atlas height, so it joins the realloc test.
static int32_t s_probeFastBuckets = -1;

// Window state, for scroll invalidation.
static int32_t s_baseCell[3] = {0, 0, 0};
static bool s_windowValid = false;
static float s_spacing = 0.0f;

// Round-robin cursor.
//
// Deliberately a SINGLE counter advanced once per frame, not one per
// frame-in-flight slot. The plan's "per-slot counter" rule exists because a
// tr.frameCount-derived index aliases with vk.currentFrame and pins each slot to
// one fixed subset of a PER-SLOT resource forever (the GI checkerboard ghost).
// The probe atlases are a single shared pair, so there is no per-slot resource
// to pin: what matters here is only that the cursor advances exactly once per
// frame, which the duplicate-dispatch guard below guarantees. Two per-slot
// counters would actually break coverage — each would walk the same stride and
// visit the same probes, leaving the rest never updated.
static uint32_t s_updateCursor = 0;
static int32_t s_updateBase = 0;

// Set only once vkCmdTraceRaysKHR has actually been recorded for this slot.
// The blend pass gates on this rather than on "the descriptor set was refreshed
// this frame": every early-out in the trace below sits AFTER that refresh, and
// blending an unwritten scratch image folds last frame's rays — or garbage on
// the first frame — into the atlas with no way to tell afterwards.
static int s_traceRecordedFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};

// Atlas geometry, derived from the probe count.
static int32_t s_tilesX = 0;
static int32_t s_tilesY = 0;
static const int32_t s_irrSide = 10;  // 8x8 interior + 1-texel border
static const int32_t s_distSide = 18; // 16x16 interior + 1-texel border

// ---------------------------------------------------------------------------
// Requested geometry
// ---------------------------------------------------------------------------

static void VK_RT_GIProbeRequestedDims(int32_t out[3])
{
    out[0] = idMath::ClampInt(2, 128, r_rtGIProbeCountX.GetInteger());
    out[1] = idMath::ClampInt(2, 128, r_rtGIProbeCountY.GetInteger());
    out[2] = idMath::ClampInt(2, 128, r_rtGIProbeCountZ.GetInteger());

    // Clamp the product too — three individually sane axes can still multiply
    // past the atlas budget.
    while ((double)out[0] * (double)out[1] * (double)out[2] > (double)VK_GIPROBE_MAX_PROBES)
    {
        int biggest = (out[0] >= out[1] && out[0] >= out[2]) ? 0 : ((out[1] >= out[2]) ? 1 : 2);
        out[biggest] = Max(2, out[biggest] / 2);
    }
}

static int32_t VK_RT_GIProbeRequestedRays(void)
{
    return idMath::ClampInt(8, VK_GIPROBE_MAX_RAYS, r_rtGIProbeRays.GetInteger());
}

static int32_t VK_RT_GIProbeRequestedUpdates(int32_t probeCount)
{
    return idMath::ClampInt(1, Min(probeCount, 65535), r_rtGIProbeUpdatesPerFrame.GetInteger());
}

// Geometry the last allocation attempt FAILED at, as {Nx, Ny, Nz, rays, updates}.
// Same purpose as s_froxelDimFailed in vk_vol_froxel.cpp: a failed realloc zeroes
// s_probeDim, which would make this predicate true forever and call
// VK_RT_CreateProbeResources — and with it vkDeviceWaitIdle — every frame.
static int32_t s_probeGeomFailed[6] = {0, 0, 0, 0, 0, -1};

static void VK_RT_GIProbeLatchFailure(const int32_t dim[3], int32_t rays, int32_t updates)
{
    s_probeGeomFailed[0] = dim[0];
    s_probeGeomFailed[1] = dim[1];
    s_probeGeomFailed[2] = dim[2];
    s_probeGeomFailed[3] = rays;
    s_probeGeomFailed[4] = updates;
    s_probeGeomFailed[5] = VK_RT_GIFastBucketCount();
    common->Warning("VK RT GIProbe: resource allocation failed at %dx%dx%d, %d rays x %d updates, %d fast bucket(s) "
                    "— probe path stood down (per-pixel GI is unaffected)",
                    dim[0], dim[1], dim[2], rays, updates, s_probeGeomFailed[5]);
}

static bool VK_RT_GIProbeGeometryChanged(void)
{
    int32_t want[3];
    VK_RT_GIProbeRequestedDims(want);
    const int32_t probes = want[0] * want[1] * want[2];
    const int32_t rays = VK_RT_GIProbeRequestedRays();
    const int32_t updates = VK_RT_GIProbeRequestedUpdates(probes);
    // G5b: K sizes the irradiance atlas, so it belongs in both tests below.
    const int32_t fastBuckets = VK_RT_GIFastBucketCount();

    if (want[0] == s_probeGeomFailed[0] && want[1] == s_probeGeomFailed[1] && want[2] == s_probeGeomFailed[2] &&
        rays == s_probeGeomFailed[3] && updates == s_probeGeomFailed[4] && fastBuckets == s_probeGeomFailed[5])
        return false; // already known-bad, do not retry every frame

    return want[0] != s_probeDim[0] || want[1] != s_probeDim[1] || want[2] != s_probeDim[2] || rays != s_probeRays ||
           updates != s_probeUpdates || fastBuckets != s_probeFastBuckets;
}

// ---------------------------------------------------------------------------
// 2D image lifecycle
// ---------------------------------------------------------------------------

static void VK_RT_FreeProbeImage(vkReflBuffer_t &img);

// Non-fatal for the same reason VK_RT_AllocFroxelImage is: at
// VK_GIPROBE_MAX_PROBES the distance atlas is 4608x4608 (85 MiB), the irradiance
// atlas 2560x2560 (52 MiB), and the scratch image 128 MiB per slot — all from
// CVar values the clamps accept. A refusal must stand the probe path down, not
// terminate the renderer.
static bool VK_RT_AllocProbeImage(vkReflBuffer_t &img, uint32_t w, uint32_t h, VkFormat format)
{
    img.width = w;
    img.height = h;

    VkResult vkr = VK_SUCCESS;

    VkImageCreateInfo imgCI = {};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = format;
    imgCI.extent = {w, h, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK_NONFATAL(vkCreateImage(vk.device, &imgCI, NULL, &img.image), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT GIProbe: vkCreateImage failed (%d) for a %ux%u probe image", (int)vkr, w, h);
        img.image = VK_NULL_HANDLE;
        VK_RT_FreeProbeImage(img);
        return false;
    }

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
        common->Warning("VK RT GIProbe: no device-local memory type for a probe image");
        VK_RT_FreeProbeImage(img);
        return false;
    }

    VkMemoryAllocateInfo allocI = {};
    allocI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocI.allocationSize = memReq.size;
    allocI.memoryTypeIndex = memTypeIdx;
    VK_CHECK_NONFATAL(vkAllocateMemory(vk.device, &allocI, NULL, &img.memory), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT GIProbe: out of device memory (%d) for a %ux%u probe image (%.1f MiB) — lower "
                        "r_rtGIProbeCountX/Y/Z, r_rtGIProbeRays or r_rtGIProbeUpdatesPerFrame",
                        (int)vkr, w, h, (double)memReq.size / (1024.0 * 1024.0));
        img.memory = VK_NULL_HANDLE;
        VK_RT_FreeProbeImage(img);
        return false;
    }

    VK_CHECK_NONFATAL(vkBindImageMemory(vk.device, img.image, img.memory, 0), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT GIProbe: vkBindImageMemory failed (%d)", (int)vkr);
        VK_RT_FreeProbeImage(img);
        return false;
    }

    VkImageViewCreateInfo viewCI = {};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = img.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = format;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK_NONFATAL(vkCreateImageView(vk.device, &viewCI, NULL, &img.view), vkr);
    if (vkr != VK_SUCCESS)
    {
        common->Warning("VK RT GIProbe: vkCreateImageView failed (%d)", (int)vkr);
        img.view = VK_NULL_HANDLE;
        VK_RT_FreeProbeImage(img);
        return false;
    }

    // UNDEFINED -> GENERAL + clear, so a probe read before its first trace sees
    // black rather than garbage. Every probe starts with TRACED clear, so the
    // resolve treats those tiles as "no data" regardless.
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
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                             NULL, 0, NULL, 1, &b2);

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

static void VK_RT_FreeProbeImage(vkReflBuffer_t &img)
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

static void VK_RT_DestroyProbeResources(void)
{
    VK_RT_FreeProbeImage(vkRT.giProbeIrradiance);
    VK_RT_FreeProbeImage(vkRT.giProbeDistance);
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeProbeImage(vkRT.giProbeScratch[i]);
        VK_RT_FreeProbeImage(vkRT.giProbeScratchFast[i]);

        if (vkRT.giProbeStateSsboMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.giProbeStateSsboMemory[i]);
            vkRT.giProbeStateSsboMapped[i] = NULL;
        }
        if (vkRT.giProbeStateSsbo[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.giProbeStateSsbo[i], NULL);
            vkRT.giProbeStateSsbo[i] = VK_NULL_HANDLE;
        }
        if (vkRT.giProbeStateSsboMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.giProbeStateSsboMemory[i], NULL);
            vkRT.giProbeStateSsboMemory[i] = VK_NULL_HANDLE;
        }
    }

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vkRT.giProbeStatsReadbackMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.giProbeStatsReadbackMemory[i]);
            vkRT.giProbeStatsReadbackMapped[i] = NULL;
        }
        if (vkRT.giProbeStatsReadback[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.giProbeStatsReadback[i], NULL);
            vkRT.giProbeStatsReadback[i] = VK_NULL_HANDLE;
        }
        if (vkRT.giProbeStatsReadbackMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.giProbeStatsReadbackMemory[i], NULL);
            vkRT.giProbeStatsReadbackMemory[i] = VK_NULL_HANDLE;
        }
    }

    if (vkRT.giProbeStatsSsboMapped != NULL)
    {
        vkUnmapMemory(vk.device, vkRT.giProbeStatsSsboMemory);
        vkRT.giProbeStatsSsboMapped = NULL;
    }
    if (vkRT.giProbeStatsSsbo != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.giProbeStatsSsbo, NULL);
        vkRT.giProbeStatsSsbo = VK_NULL_HANDLE;
    }
    if (vkRT.giProbeStatsSsboMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.giProbeStatsSsboMemory, NULL);
        vkRT.giProbeStatsSsboMemory = VK_NULL_HANDLE;
    }

    if (s_probeState != NULL)
    {
        Mem_Free(s_probeState);
        s_probeState = NULL;
    }
    if (s_probeStats != NULL)
    {
        Mem_Free(s_probeStats);
        s_probeStats = NULL;
    }
    s_probeStateCount = 0;
    s_probeDim[0] = s_probeDim[1] = s_probeDim[2] = 0;
    s_probeRays = 0;
    s_probeUpdates = 0;
    s_tilesX = s_tilesY = 0;
    // -1, not 0: K = 0 is a legal configuration, so zero here would read as
    // "already built at K = 0" and suppress the realloc that turns it back on.
    s_probeFastBuckets = -1;
    s_windowValid = false;
    for (int i = 0; i < 5; i++)
        s_probeGeomFailed[i] = 0;
    s_probeGeomFailed[5] = -1;
}

// Allocates (or reallocates) every probe resource at the currently requested
// geometry. Calls vkDeviceWaitIdle — init path, or the one frame a cvar changes.
static void VK_RT_CreateProbeResources(void)
{
    vkDeviceWaitIdle(vk.device);
    VK_RT_DestroyProbeResources();

    int32_t dim[3];
    VK_RT_GIProbeRequestedDims(dim);
    const int32_t probeCount = dim[0] * dim[1] * dim[2];

    // Square-ish tiling so neither atlas axis runs away from maxImageDimension2D.
    int32_t tx = 1;
    while (tx * tx < probeCount)
        tx++;
    const int32_t ty = (probeCount + tx - 1) / tx;

    // G5b: the irradiance atlas holds 1 + K sets of tiles, bucket k starting at
    // tile index k * probeCount. Deliberately extra ROWS rather than an image
    // array — gip_TileOrigin already turns a linear tile index into a texel
    // origin, so nothing in the addressing changes and no blend/border/resolve
    // declaration has to be retyped to image2DArray. The distance atlas is NOT
    // duplicated: occluder geometry is shared by both buckets.
    const int32_t irrBuckets = 1 + VK_RT_GIFastBucketCount();
    const int32_t irrTilesY = ty * irrBuckets;

    const int32_t rays = VK_RT_GIProbeRequestedRays();
    const int32_t updates = VK_RT_GIProbeRequestedUpdates(probeCount);

    if (!VK_RT_AllocProbeImage(vkRT.giProbeIrradiance, (uint32_t)(tx * s_irrSide), (uint32_t)(irrTilesY * s_irrSide),
                               VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !VK_RT_AllocProbeImage(vkRT.giProbeDistance, (uint32_t)(tx * s_distSide), (uint32_t)(ty * s_distSide),
                               VK_FORMAT_R16G16_SFLOAT))
    {
        VK_RT_DestroyProbeResources();
        // Latch AFTER the destroy — it clears s_probeGeomFailed on the way out.
        VK_RT_GIProbeLatchFailure(dim, rays, updates);
        return;
    }

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        // Scratch is per slot even though the atlases are not: it holds one
        // frame's rays with no cross-frame meaning, so per-slot is simply
        // correct and costs 1 MiB at the defaults.
        if (!VK_RT_AllocProbeImage(vkRT.giProbeScratch[i], (uint32_t)rays, (uint32_t)updates,
                                   VK_FORMAT_R16G16B16A16_SFLOAT))
        {
            VK_RT_DestroyProbeResources();
            VK_RT_GIProbeLatchFailure(dim, rays, updates);
            return;
        }

        // G5b fast-bucket radiance for the same rays. Allocated even at K = 0:
        // the descriptor set writes binding 6 unconditionally, and a shader that
        // declares a binding it never reads still needs a live view there.
        //
        // rgba16f, not the planned r11f_g11f_b10f. STORAGE_IMAGE support for
        // B10G11R11 is optional in Vulkan (it sits behind
        // shaderStorageImageExtendedFormats), and a GLSL storage image's format
        // qualifier has to match its view exactly — so a runtime fallback would
        // mean shipping two variants of gi_probe_trace.rgen and
        // gi_probe_blend.comp. The alpha channel is wasted; 0.5 MiB per slot at
        // the defaults is not worth a device-dependent cliff.
        if (!VK_RT_AllocProbeImage(vkRT.giProbeScratchFast[i], (uint32_t)rays, (uint32_t)updates,
                                   VK_FORMAT_R16G16B16A16_SFLOAT))
        {
            VK_RT_DestroyProbeResources();
            VK_RT_GIProbeLatchFailure(dim, rays, updates);
            return;
        }

        const VkDeviceSize stateBytes = (VkDeviceSize)probeCount * sizeof(GIProbeStateEntry);
        VK_CreateBuffer(stateBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &vkRT.giProbeStateSsbo[i], &vkRT.giProbeStateSsboMemory[i]);
        VK_CHECK(
            vkMapMemory(vk.device, vkRT.giProbeStateSsboMemory[i], 0, stateBytes, 0, &vkRT.giProbeStateSsboMapped[i]));
        memset(vkRT.giProbeStateSsboMapped[i], 0, (size_t)stateBytes);

        vkRT.giProbeDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.giProbeResolveDescSetLastUpdatedFrameCount[i] = -1;
    }

    // One shared stats buffer — see GIProbeStatsEntry on why this must not be
    // per slot. Device-side zeroing happens once, here, with the device idle.
    //
    // Plus a PER-SLOT readback snapshot. Sharing the accumulator removed the
    // accidental synchronisation the per-slot version had: the CPU would
    // otherwise read the live buffer while the previous frame's blend is still
    // writing it, since the fence it waited on is two submissions back. The blend
    // copies into the slot's snapshot, and the CPU reads THAT — recorded in a
    // submission the fence has retired.
    {
        const VkDeviceSize statsBytes = (VkDeviceSize)probeCount * sizeof(GIProbeStatsEntry);
        VK_CreateBuffer(statsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &vkRT.giProbeStatsSsbo, &vkRT.giProbeStatsSsboMemory);
        VK_CHECK(vkMapMemory(vk.device, vkRT.giProbeStatsSsboMemory, 0, statsBytes, 0, &vkRT.giProbeStatsSsboMapped));
        memset(vkRT.giProbeStatsSsboMapped, 0, (size_t)statsBytes);

        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        {
            VK_CreateBuffer(statsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &vkRT.giProbeStatsReadback[i], &vkRT.giProbeStatsReadbackMemory[i]);
            VK_CHECK(vkMapMemory(vk.device, vkRT.giProbeStatsReadbackMemory[i], 0, statsBytes, 0,
                                 &vkRT.giProbeStatsReadbackMapped[i]));
            memset(vkRT.giProbeStatsReadbackMapped[i], 0, (size_t)statsBytes);
        }
    }

    s_probeState = (GIProbeStateEntry *)Mem_Alloc(probeCount * sizeof(GIProbeStateEntry));
    memset(s_probeState, 0, probeCount * sizeof(GIProbeStateEntry));
    s_probeStats = (GIProbeStatsEntry *)Mem_Alloc(probeCount * sizeof(GIProbeStatsEntry));
    memset(s_probeStats, 0, probeCount * sizeof(GIProbeStatsEntry));
    s_probeStateCount = probeCount;

    s_probeDim[0] = dim[0];
    s_probeDim[1] = dim[1];
    s_probeDim[2] = dim[2];
    s_probeRays = rays;
    s_probeUpdates = updates;
    s_tilesX = tx;
    s_tilesY = ty;
    s_probeFastBuckets = irrBuckets - 1;
    s_windowValid = false; // force a full re-anchor, and with it a full invalidate

    const double irrMiB =
        (double)vkRT.giProbeIrradiance.width * vkRT.giProbeIrradiance.height * 8.0 / (1024.0 * 1024.0);
    const double distMiB = (double)vkRT.giProbeDistance.width * vkRT.giProbeDistance.height * 4.0 / (1024.0 * 1024.0);
    common->Printf("VK RT GIProbe: grid %dx%dx%d (%d probes) atlas %dx%d tiles/bucket, irradiance %ux%u (%.1f MiB, "
                   "%d bucket(s): 1 stable + %d fast), distance %ux%u (%.1f MiB), %d rays x %d updates/frame\n",
                   dim[0], dim[1], dim[2], probeCount, tx, ty, vkRT.giProbeIrradiance.width,
                   vkRT.giProbeIrradiance.height, irrMiB, irrBuckets, irrBuckets - 1, vkRT.giProbeDistance.width,
                   vkRT.giProbeDistance.height, distMiB, rays, updates);
}

// ---------------------------------------------------------------------------
// Grid anchoring and scroll invalidation
//
// The window corner is snapped to a whole lattice cell, so probes sit on the
// absolute lattice and never swim. Scrolling only changes WHICH probes exist:
// toroidal storage leaves every surviving probe on its own atlas tile, and the
// cells that just entered the window inherit a tile belonging to the cell that
// just left. Those — and only those — get TRACED cleared, so the blend pass
// takes their first trace outright instead of blending against another room.
// ---------------------------------------------------------------------------

static void VK_RT_GIProbeAnchor(const viewDef_t *viewDef, float spacing)
{
    const idVec3 camPos = viewDef->renderView.vieworg;

    int32_t newBase[3];
    for (int a = 0; a < 3; a++)
    {
        const float c = (a == 0) ? camPos.x : ((a == 1) ? camPos.y : camPos.z);
        newBase[a] = (int32_t)idMath::Floor(c / spacing) - s_probeDim[a] / 2;
    }

    const bool spacingChanged = (spacing != s_spacing);
    if (!s_windowValid || spacingChanged)
    {
        for (int i = 0; i < s_probeStateCount; i++)
        {
            s_probeState[i].flags = 0;
            s_probeState[i].offset[0] = s_probeState[i].offset[1] = s_probeState[i].offset[2] = 0.0f;
            s_probeStats[i].backface = 0.0f;
            s_probeStats[i].miss = 0.0f;
        }
        s_baseCell[0] = newBase[0];
        s_baseCell[1] = newBase[1];
        s_baseCell[2] = newBase[2];
        s_spacing = spacing;
        s_windowValid = true;

        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GIProbe: window re-anchored at cell (%d %d %d), all %d probes invalidated\n",
                           s_baseCell[0], s_baseCell[1], s_baseCell[2], s_probeStateCount);
        return;
    }

    if (newBase[0] == s_baseCell[0] && newBase[1] == s_baseCell[1] && newBase[2] == s_baseCell[2])
        return;

    const int32_t nx = s_probeDim[0];
    const int32_t ny = s_probeDim[1];
    const int32_t nz = s_probeDim[2];

    int cleared = 0;
    for (int lz = 0; lz < nz; lz++)
        for (int ly = 0; ly < ny; ly++)
            for (int lx = 0; lx < nx; lx++)
            {
                const int32_t ax = newBase[0] + lx;
                const int32_t ay = newBase[1] + ly;
                const int32_t az = newBase[2] + lz;

                // Still inside the previous window -> its atlas tile is its own,
                // keep it. Otherwise it just took over a departing probe's tile.
                const int32_t px = ax - s_baseCell[0];
                const int32_t py = ay - s_baseCell[1];
                const int32_t pz = az - s_baseCell[2];
                if (px >= 0 && px < nx && py >= 0 && py < ny && pz >= 0 && pz < nz)
                    continue;

                const int32_t wx = ((ax % nx) + nx) % nx;
                const int32_t wy = ((ay % ny) + ny) % ny;
                const int32_t wz = ((az % nz) + nz) % nz;
                const int idx = wx + nx * (wy + ny * wz);

                s_probeState[idx].flags = 0;
                s_probeState[idx].offset[0] = s_probeState[idx].offset[1] = s_probeState[idx].offset[2] = 0.0f;
                // The tile now belongs to a different probe in a different room;
                // inheriting its predecessor's verdict would classify on the
                // wrong geometry until the EMA washed it out. Clearing TRACED
                // above also stops the readback adopting the device value, and
                // makes the next blend take its measurement outright.
                s_probeStats[idx].backface = 0.0f;
                s_probeStats[idx].miss = 0.0f;
                cleared++;
            }

    s_baseCell[0] = newBase[0];
    s_baseCell[1] = newBase[1];
    s_baseCell[2] = newBase[2];

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GIProbe: window scrolled to cell (%d %d %d), %d probes invalidated\n", s_baseCell[0],
                       s_baseCell[1], s_baseCell[2], cleared);
}

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

static bool VK_RT_BuildProbeParams(const viewDef_t *viewDef, GIProbeParamsUBO &ubo)
{
    memset(&ubo, 0, sizeof(ubo));

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
    ubo.cameraPosW[0] = camPos.x;
    ubo.cameraPosW[1] = camPos.y;
    ubo.cameraPosW[2] = camPos.z;

    ubo.gridOrigin[0] = (float)s_baseCell[0] * s_spacing;
    ubo.gridOrigin[1] = (float)s_baseCell[1] * s_spacing;
    ubo.gridOrigin[2] = (float)s_baseCell[2] * s_spacing;
    ubo.gridOrigin[3] = s_spacing;

    ubo.gridDim[0] = s_probeDim[0];
    ubo.gridDim[1] = s_probeDim[1];
    ubo.gridDim[2] = s_probeDim[2];
    ubo.gridDim[3] = s_probeStateCount;

    ubo.baseCell[0] = s_baseCell[0];
    ubo.baseCell[1] = s_baseCell[1];
    ubo.baseCell[2] = s_baseCell[2];

    ubo.atlas[0] = s_tilesX;
    ubo.atlas[1] = s_tilesY;
    ubo.atlas[2] = s_irrSide;
    ubo.atlas[3] = s_distSide;

    ubo.rays[0] = s_probeRays;
    ubo.rays[1] = s_probeUpdates;
    ubo.rays[2] = s_updateBase;
    ubo.rays[3] = (int32_t)tr.frameCount;

    ubo.tune[0] = idMath::ClampFloat(0.0f, 0.999f, r_rtGIProbeHysteresis.GetFloat());
    ubo.tune[1] = Max(0.0f, r_rtGIProbeNormalBias.GetFloat());
    ubo.tune[2] = idMath::ClampFloat(0.0f, 4.0f, r_rtGIStrength.GetFloat());
    ubo.tune[3] = Max(1.0f, r_rtGIProbeMaxRayDist.GetFloat());

    // Must track VK_RT_GIProbeDebugMode's range. A stale upper bound here does
    // not disable the new mode, it silently renders a DIFFERENT one.
    ubo.misc[0] = idMath::ClampInt(0, 8, r_rtGIProbeDebug.GetInteger());
    ubo.misc[1] = (vk.gbufferSupported && r_rtGbufNormals.GetBool()) ? 1 : 0;
    ubo.misc[2] = r_rtGIProbeVisibility.GetBool() ? 1 : 0;
    // G5b: how many bucket-sized tile blocks the irradiance atlas holds. The
    // shaders need it to normalise their UVs against the taller image — the
    // DISTANCE atlas is not duplicated and keeps using atlas[1] alone.
    ubo.misc[3] = 1 + s_probeFastBuckets;

    const vkReflBuffer_t &gb = vkRT.giBuffer[vk.currentFrame];
    ubo.screen[0] = (int32_t)vk.swapchainExtent.width;
    ubo.screen[1] = (int32_t)vk.swapchainExtent.height;
    ubo.screen[2] = (int32_t)Max(1u, gb.width);
    ubo.screen[3] = (int32_t)Max(1u, gb.height);

    // Resolve dispatch rect: the view scissor, GL Y-up -> Vulkan Y-down. Same
    // conversion VK_RT_GI_ComputeDispatchRect makes (static in vk_gi.cpp).
    {
        const int fullW = (int)vk.swapchainExtent.width;
        const int fullH = (int)vk.swapchainExtent.height;
        const idScreenRect &s = viewDef->scissor;

        const int x0 = idMath::ClampInt(0, fullW - 1, s.x1);
        const int y0 = idMath::ClampInt(0, fullH - 1, fullH - 1 - s.y2);
        const int rw = s.x2 - s.x1 + 1;
        const int rh = s.y2 - s.y1 + 1;

        if (rw <= 0 || rh <= 0)
        {
            ubo.rect[0] = ubo.rect[1] = ubo.rect[2] = ubo.rect[3] = 0;
        }
        else
        {
            ubo.rect[0] = x0;
            ubo.rect[1] = y0;
            ubo.rect[2] = idMath::ClampInt(1, fullW - x0, rw);
            ubo.rect[3] = idMath::ClampInt(1, fullH - y0, rh);
        }
    }

    ubo.debug[0] = Max(0.0f, r_rtGIProbeDebugGain.GetFloat());
    // Clamped below half spacing: the overlay's ray stepping is spacing/2, and a
    // sphere larger than that could be stepped past and vanish at some angles —
    // which would read as "probes are missing", the exact failure mode this
    // overlay exists to rule out.
    ubo.debug[1] = idMath::ClampFloat(0.25f, s_spacing * 0.49f, r_rtGIProbeDebugRadius.GetFloat());
    ubo.debug[2] = Max(1.0f, r_rtGIProbeDistSharpness.GetFloat());

    ubo.tune2[0] = idMath::ClampFloat(0.0f, 1.0f, r_rtGIContrast.GetFloat());
    // Clamped below half spacing: a bias larger than that walks the sample into
    // the next lattice cell, so the trilinear weights would describe a point the
    // receiver is not at.
    ubo.tune2[1] = idMath::ClampFloat(0.0f, s_spacing * 0.5f, r_rtGIProbeViewBias.GetFloat());

    // G5b: the fast buckets' current gains, refreshed by this frame's
    // VK_RT_UploadGILights (vk_backend.cpp calls it before any probe dispatch).
    // This is the ONLY part of the probe path that has to be current-frame
    // fresh — the atlases are deliberately not.
    ubo.tune2[2] = VK_RT_GIFastBucketGain(0);
    ubo.tune2[3] = 0.0f; // bucket 1, when K > 1 is earned

    return true;
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

// Shared by the trace rgen (at set 2 of the GI RT pipeline) and by the blend and
// border compute passes (at their set 0). One layout object bound at two
// different set indices — legal, and it keeps the binding contract in
// gi_probe_common.glsl to a single list.
//
//   0 STORAGE_IMAGE   probe ray scratch  rgba16f
//   1 STORAGE_IMAGE   irradiance atlas   rgba16f
//   2 UNIFORM_BUFFER  GIProbeParams
//   3 STORAGE_BUFFER  probe state        (per slot, CPU-owned)
//   4 STORAGE_IMAGE   distance atlas     rg16f
//   5 STORAGE_BUFFER  probe stats        (shared, GPU-owned; blend only)
//   6 STORAGE_IMAGE   fast ray scratch   rgba16f  (G5b; alpha unused)
static bool VK_RT_InitProbeDescLayout(void)
{
    VkDescriptorSetLayoutBinding bindings[VK_GIPROBE_DESC_BINDINGS] = {};
    const VkShaderStageFlags stages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

    for (int i = 0; i < VK_GIPROBE_DESC_BINDINGS; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = stages;
    }
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; // G5b fast scratch

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = VK_GIPROBE_DESC_BINDINGS;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giProbeDescLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 4)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 2)};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giProbeDescPool));

    VkDescriptorSetLayout allocLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        allocLayouts[i] = vkRT.giProbeDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giProbeDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = allocLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giProbeDescSets));

    return true;
}

static VkPipeline VK_RT_CreateProbeComputePipeline(const char *spv, VkPipelineLayout layout)
{
    VkShaderModule mod = VK_LoadSPIRV(spv);
    if (mod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GIProbe: failed to load %s", spv);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));
    vkDestroyShaderModule(vk.device, mod, NULL);
    return pipeline;
}

static void VK_RT_InitProbeBlendPipelines(void)
{
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.giProbeDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giProbeBlendPipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giProbeBorderPipelineLayout));

    vkRT.giProbeBlendPipeline =
        VK_RT_CreateProbeComputePipeline("glprogs/glsl/gi_probe_blend.comp.spv", vkRT.giProbeBlendPipelineLayout);
    vkRT.giProbeBorderPipeline =
        VK_RT_CreateProbeComputePipeline("glprogs/glsl/gi_probe_border.comp.spv", vkRT.giProbeBorderPipelineLayout);

    if (vkRT.giProbeBlendPipeline != VK_NULL_HANDLE && vkRT.giProbeBorderPipeline != VK_NULL_HANDLE)
        common->Printf("VK RT GIProbe: blend/border pipelines initialized\n");
}

// Resolve set 0 — deliberately keeps the params UBO at binding 2 and the probe
// state at binding 3, matching giProbeDescLayout, so gi_probe_common.glsl can
// declare both for every pass from one place.
//
//   0 COMBINED_IMAGE_SAMPLER  irradiance atlas
//   1 STORAGE_IMAGE           giBuffer (write)
//   2 UNIFORM_BUFFER          GIProbeParams
//   3 STORAGE_BUFFER          probe state
//   4 COMBINED_IMAGE_SAMPLER  distance atlas
//   5 COMBINED_IMAGE_SAMPLER  depth
//   6 COMBINED_IMAGE_SAMPLER  G-buffer normal/F0
//   7 ACCELERATION_STRUCTURE  TLAS — G3's leak overlay traces ray queries for
//     ground-truth probe visibility. Only mode 3 reads it, but the shader
//     references it statically, so it has to be a live handle on every dispatch;
//     the resolve therefore requires a valid TLAS, exactly as VK_RT_DispatchGI
//     already does.
//   8 STORAGE_BUFFER          probe stats (modes 6/7)
//   9 STORAGE_BUFFER          GI light SSBO — G5b's mode 8 counts how many FAST
//     lights reach each receiver, which is the measurement that decides K=1 vs
//     K=2. Same static-reference rule as the TLAS above: debug-only, always bound.
static void VK_RT_InitProbeResolvePipeline(void)
{
    VkDescriptorSetLayoutBinding bindings[VK_GIPROBE_RESOLVE_BINDINGS] = {};
    for (int i = 0; i < VK_GIPROBE_RESOLVE_BINDINGS; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; // stats, modes 6/7
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; // GI lights, G5b mode 8

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = VK_GIPROBE_RESOLVE_BINDINGS;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giProbeResolveDescLayout));

    // Own bilinear/clamp sampler rather than borrowing vkRT.giSampler: that one
    // is created at the end of VK_RT_InitGIPipeline and stays NULL if the GI
    // shaders fail to load, which would take the overlays down with them.
    if (vkRT.giProbeSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.giProbeSampler));
    }

    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.giProbeResolveDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giProbeResolvePipelineLayout));

    vkRT.giProbeResolvePipeline =
        VK_RT_CreateProbeComputePipeline("glprogs/glsl/gi_probe_resolve.comp.spv", vkRT.giProbeResolvePipelineLayout);
    if (vkRT.giProbeResolvePipeline == VK_NULL_HANDLE)
        return;

    VkDescriptorPoolSize poolSizes[5] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 4)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    // 3 storage buffers per set now: probe state, probe stats, and G5b's lights.
    poolSizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)(VK_MAX_FRAMES_IN_FLIGHT * 3)};
    poolSizes[4] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giProbeResolveDescPool));

    VkDescriptorSetLayout allocLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        allocLayouts[i] = vkRT.giProbeResolveDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giProbeResolveDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = allocLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giProbeResolveDescSets));

    common->Printf("VK RT GIProbe: resolve pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public lifecycle
// ---------------------------------------------------------------------------

// Must run BEFORE VK_RT_InitGIPipeline: that function bakes giProbeDescLayout
// into the GI pipeline layout as set 2.
void VK_RT_InitGIProbeLayout(void)
{
    if (vkRT.giProbeDescLayout != VK_NULL_HANDLE)
        return;
    VK_RT_InitProbeDescLayout();
}

void VK_RT_InitGIProbe(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRT.giProbeDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.giProbeResolveDescSetLastUpdatedFrameCount[i] = -1;

        // Fixed size, so unlike everything else here it survives a geometry
        // realloc. Non-dynamic: the probe set is bound at set 2 of the GI RT
        // pipeline alongside set 0's dynamic GIParams, and a second dynamic
        // offset in that bind call is a trap waiting for the next caller.
        if (vkRT.giProbeParamsUbo[i] == VK_NULL_HANDLE)
        {
            VK_CreateBuffer(sizeof(GIProbeParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &vkRT.giProbeParamsUbo[i], &vkRT.giProbeParamsUboMemory[i]);
            VK_CHECK(vkMapMemory(vk.device, vkRT.giProbeParamsUboMemory[i], 0, sizeof(GIProbeParamsUBO), 0,
                                 &vkRT.giProbeParamsUboMapped[i]));
            memset(vkRT.giProbeParamsUboMapped[i], 0, sizeof(GIProbeParamsUBO));
        }
    }

    VK_RT_InitGIProbeLayout();
    VK_RT_InitProbeBlendPipelines();
    VK_RT_InitProbeResolvePipeline();
    VK_RT_CreateProbeResources();
}

void VK_RT_ShutdownGIProbe(void)
{
    if (vkRT.giProbeDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giProbeDescPool, NULL);
        vkRT.giProbeDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giProbeDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giProbeDescLayout, NULL);
        vkRT.giProbeDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giProbeResolveDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giProbeResolveDescPool, NULL);
        vkRT.giProbeResolveDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giProbeResolveDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giProbeResolveDescLayout, NULL);
        vkRT.giProbeResolveDescLayout = VK_NULL_HANDLE;
    }

    VkPipeline pipes[3] = {vkRT.giProbeBlendPipeline, vkRT.giProbeBorderPipeline, vkRT.giProbeResolvePipeline};
    for (int i = 0; i < 3; i++)
        if (pipes[i] != VK_NULL_HANDLE)
            vkDestroyPipeline(vk.device, pipes[i], NULL);
    vkRT.giProbeBlendPipeline = vkRT.giProbeBorderPipeline = vkRT.giProbeResolvePipeline = VK_NULL_HANDLE;

    VkPipelineLayout layouts[3] = {vkRT.giProbeBlendPipelineLayout, vkRT.giProbeBorderPipelineLayout,
                                   vkRT.giProbeResolvePipelineLayout};
    for (int i = 0; i < 3; i++)
        if (layouts[i] != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(vk.device, layouts[i], NULL);
    vkRT.giProbeBlendPipelineLayout = vkRT.giProbeBorderPipelineLayout = vkRT.giProbeResolvePipelineLayout =
        VK_NULL_HANDLE;

    if (vkRT.giProbeSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.giProbeSampler, NULL);
        vkRT.giProbeSampler = VK_NULL_HANDLE;
    }

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vkRT.giProbeParamsUboMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.giProbeParamsUboMemory[i]);
            vkRT.giProbeParamsUboMapped[i] = NULL;
        }
        if (vkRT.giProbeParamsUbo[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.giProbeParamsUbo[i], NULL);
            vkRT.giProbeParamsUbo[i] = VK_NULL_HANDLE;
        }
        if (vkRT.giProbeParamsUboMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.giProbeParamsUboMemory[i], NULL);
            vkRT.giProbeParamsUboMemory[i] = VK_NULL_HANDLE;
        }
    }

    VK_RT_DestroyProbeResources();
}

VkDescriptorSetLayout VK_RT_GIProbeDescLayout(void)
{
    return vkRT.giProbeDescLayout;
}

VkDescriptorSet VK_RT_GIProbeDescSet(int frameIdx)
{
    return vkRT.giProbeDescSets[frameIdx];
}

int VK_RT_GIProbeDebugMode(void)
{
    if (!vkRT.isInitialized || !r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return 0;
    if (vkRT.giProbeResolvePipeline == VK_NULL_HANDLE || vkRT.giProbeIrradiance.image == VK_NULL_HANDLE)
        return 0;
    // Mirror of the clamp in VK_RT_BuildProbeParams' misc[0]. Both must move
    // together: raising only this one gates the new mode in but leaves the UBO
    // telling the shader to render a different one.
    return idMath::ClampInt(0, 8, r_rtGIProbeDebug.GetInteger());
}

// True when the probe path OWNS the GI result — G2 wires this to stand the
// per-pixel rgen, the temporal EMA and the a-trous chain down. It requires the
// whole chain, so a shader that fails to load falls back to the per-pixel path
// rather than leaving the screen with no GI at all (the same rule
// VK_RT_VolFroxelActive enforces for the froxel path).
bool VK_RT_GIProbeActive(void)
{
    if (!vkRT.isInitialized || !r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return false;
    if (r_rtGIProbes.GetInteger() != 1)
        return false;
    // The trace is part of the chain. VK_RT_InitGIPipeline tolerates a missing
    // gi_probe_trace.rgen.spv by zeroing giProbeRgenRegion, and the trace then
    // early-outs on it — so omitting it here stands the per-pixel path down in
    // favour of probes that are never traced, i.e. black GI, which is the exact
    // outcome this predicate exists to prevent.
    return vkRT.giProbeBlendPipeline != VK_NULL_HANDLE && vkRT.giProbeBorderPipeline != VK_NULL_HANDLE &&
           vkRT.giProbeResolvePipeline != VK_NULL_HANDLE && vkRT.giProbeIrradiance.image != VK_NULL_HANDLE &&
           vkRT.giPipeline != VK_NULL_HANDLE && vkRT.giProbeRgenRegion.deviceAddress != 0;
}

// Probes are MAINTAINED (traced + blended) whenever either the resolve wants
// them or an overlay is up: an overlay with nothing traced shows red spheres
// forever and tests nothing.
static bool VK_RT_GIProbeMaintain(void)
{
    if (!vkRT.isInitialized || !r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return false;
    if (vkRT.giProbeIrradiance.image == VK_NULL_HANDLE)
        return false;
    if (r_rtGIProbes.GetInteger() == 0 && r_rtGIProbeDebug.GetInteger() == 0)
        return false;
    return vkRT.giProbeBlendPipeline != VK_NULL_HANDLE && vkRT.giProbeBorderPipeline != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Per-frame CPU update — anchoring, scheduling, state upload, params upload,
// descriptor refresh.  Runs once per frame from the trace dispatch, and from
// the resolve too so the overlays still work with the trace stood down.
// ---------------------------------------------------------------------------

static bool VK_RT_GIProbeUpdate(const viewDef_t *viewDef, GIProbeParamsUBO &outUbo)
{
    const int frameIdx = vk.currentFrame;

    static int s_lastUpdateFrame = -1;
    static GIProbeParamsUBO s_lastUbo;
    static bool s_lastUboValid = false;

    if (s_lastUpdateFrame == tr.frameCount)
    {
        // Second consumer this frame (resolve after trace): reuse verbatim, so
        // the schedule the blend pass reads can never drift from the trace's.
        if (!s_lastUboValid)
            return false;
        outUbo = s_lastUbo;
        return true;
    }

    if (VK_RT_GIProbeGeometryChanged())
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GIProbe: geometry cvars changed, reallocating\n");
        VK_RT_CreateProbeResources();
    }
    if (s_probeStateCount == 0 || vkRT.giProbeIrradiance.image == VK_NULL_HANDLE)
        return false;

    s_lastUpdateFrame = tr.frameCount;
    s_lastUboValid = false;

    // One line per handover, unconditionally: a perf capture or a "GI went
    // black" report has to say which sampling structure was live without
    // needing r_vkLogRT turned on first.
    {
        static int s_lastOwner = -1;
        const int owner = VK_RT_GIProbeActive() ? 1 : 0;
        if (owner != s_lastOwner)
        {
            s_lastOwner = owner;
            common->Printf("VK RT GIProbe: GI is now %s (r_rtGIProbes %d) — per-pixel rgen/temporal/a-trous %s\n",
                           owner ? "PROBE" : "per-pixel", owner, owner ? "stood down" : "live");
        }
    }

    const float spacing = idMath::ClampFloat(8.0f, 512.0f, r_rtGIProbeSpacing.GetFloat());
    VK_RT_GIProbeAnchor(viewDef, spacing);

    s_updateBase = (int32_t)((s_updateCursor * (uint32_t)s_probeUpdates) % (uint32_t)s_probeStateCount);
    s_updateCursor++;

    if (!VK_RT_BuildProbeParams(viewDef, outUbo))
    {
        common->Warning("VK RT GIProbe: invViewProj NaN — skipping probe update");
        return false;
    }

    // --- G4 classification: read back what the GPU measured ---
    //
    // Read this slot's SNAPSHOT, not the live accumulator: the copy was recorded
    // in the submission the frame fence has already retired. We only ever read;
    // the device-side stats are never written from the CPU after creation, so
    // there is nothing to upload back either.
    if (vkRT.giProbeStatsReadbackMapped[frameIdx] != NULL)
    {
        const GIProbeStatsEntry *gpuStats = (const GIProbeStatsEntry *)vkRT.giProbeStatsReadbackMapped[frameIdx];
        const float insideOn = Max(0.0f, r_rtGIProbeInsideThreshold.GetFloat());
        const float insideOff = insideOn * 0.75f; // dead band — see the CVars
        const float outsideOn = Max(0.0f, r_rtGIProbeOutsideThreshold.GetFloat());
        const float outsideOff = outsideOn * 0.75f;
        int inside = 0;
        int outside = 0;

        for (int i = 0; i < s_probeStateCount; i++)
        {
            const bool traced = (s_probeState[i].flags & GIPROBE_FLAG_TRACED) != 0;

            // Untraced since it scrolled in: the device value still describes the
            // tile's previous occupant. NaN guard: never classify on garbage.
            if (traced)
            {
                const float bf = gpuStats[i].backface;
                const float ms = gpuStats[i].miss;
                s_probeStats[i].backface = (bf == bf) ? bf : 0.0f;
                s_probeStats[i].miss = (ms == ms) ? ms : 0.0f;
            }

            if (insideOn > 0.0f && traced)
            {
                if (s_probeStats[i].backface >= insideOn)
                    s_probeState[i].flags |= GIPROBE_FLAG_INSIDE;
                else if (s_probeStats[i].backface < insideOff)
                    s_probeState[i].flags &= ~GIPROBE_FLAG_INSIDE;
            }
            else if (insideOn <= 0.0f)
            {
                s_probeState[i].flags &= ~GIPROBE_FLAG_INSIDE;
            }

            if (outsideOn > 0.0f && traced)
            {
                if (s_probeStats[i].miss >= outsideOn)
                    s_probeState[i].flags |= GIPROBE_FLAG_OUTSIDE;
                else if (s_probeStats[i].miss < outsideOff)
                    s_probeState[i].flags &= ~GIPROBE_FLAG_OUTSIDE;
            }
            else if (outsideOn <= 0.0f)
            {
                s_probeState[i].flags &= ~GIPROBE_FLAG_OUTSIDE;
            }

            if ((s_probeState[i].flags & GIPROBE_FLAG_INSIDE) != 0)
                inside++;
            if ((s_probeState[i].flags & GIPROBE_FLAG_OUTSIDE) != 0)
                outside++;
        }

        // Every probe excluded is a probe the resolve can no longer fall back
        // on, so a runaway threshold shows up as black GI. Report the counts when
        // they move rather than making that a silent failure.
        static int s_lastUnusableLogged = -1;
        const int unusable = inside + outside;
        if (r_vkLogRT.GetInteger() >= 1 && abs(unusable - s_lastUnusableLogged) > Max(1, s_probeStateCount / 100))
        {
            s_lastUnusableLogged = unusable;
            common->Printf("VK RT GIProbe: %d/%d probes unusable (%.1f%%) — %d inside geometry (>= %.2f backface), "
                           "%d outside the level (>= %.2f miss)\n",
                           unusable, s_probeStateCount, 100.0 * unusable / Max(1, s_probeStateCount), inside, insideOn,
                           outside, outsideOn);
        }
    }

    // Upload the PRE-trace flags: gi_probe_blend.comp uses them to decide whether
    // a probe has history to blend against, and this frame's scheduled probes do
    // not yet. They are marked below, after the buffer is filled.
    if (vkRT.giProbeStateSsboMapped[frameIdx] != NULL)
        memcpy(vkRT.giProbeStateSsboMapped[frameIdx], s_probeState, s_probeStateCount * sizeof(GIProbeStateEntry));

    if (vkRT.giProbeParamsUboMapped[frameIdx] != NULL)
        memcpy(vkRT.giProbeParamsUboMapped[frameIdx], &outUbo, sizeof(GIProbeParamsUBO));

    s_lastUbo = outUbo;
    s_lastUboValid = true;

    // --- Descriptor refresh: probe set (RT set 2 / compute set 0) ---
    {
        VkDescriptorSet ds = vkRT.giProbeDescSets[frameIdx];

        VkDescriptorImageInfo scratchInfo = {};
        scratchInfo.imageView = vkRT.giProbeScratch[frameIdx].view;
        scratchInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo scratchFastInfo = {};
        scratchFastInfo.imageView = vkRT.giProbeScratchFast[frameIdx].view;
        scratchFastInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo irrInfo = {};
        irrInfo.imageView = vkRT.giProbeIrradiance.view;
        irrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo distInfo = {};
        distInfo.imageView = vkRT.giProbeDistance.view;
        distInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo paramsInfo = {};
        paramsInfo.buffer = vkRT.giProbeParamsUbo[frameIdx];
        paramsInfo.offset = 0;
        paramsInfo.range = sizeof(GIProbeParamsUBO);

        VkDescriptorBufferInfo stateInfo = {};
        stateInfo.buffer = vkRT.giProbeStateSsbo[frameIdx];
        stateInfo.offset = 0;
        stateInfo.range = VK_WHOLE_SIZE;

        // Shared across slots by design — see GIProbeStatsEntry.
        VkDescriptorBufferInfo statsInfo = {};
        statsInfo.buffer = vkRT.giProbeStatsSsbo;
        statsInfo.offset = 0;
        statsInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[VK_GIPROBE_DESC_BINDINGS] = {};
        for (int i = 0; i < VK_GIPROBE_DESC_BINDINGS; i++)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = ds;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &scratchInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &irrInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &paramsInfo;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &stateInfo;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[4].pImageInfo = &distInfo;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].pBufferInfo = &statsInfo;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; // G5b fast scratch
        writes[6].pImageInfo = &scratchFastInfo;

        vkUpdateDescriptorSets(vk.device, VK_GIPROBE_DESC_BINDINGS, writes, 0, NULL);
        vkRT.giProbeDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GIProbe: frame=%d slot=%d base=(%d %d %d) updateBase=%d rays=%d updates=%d debug=%d\n",
                       tr.frameCount, frameIdx, s_baseCell[0], s_baseCell[1], s_baseCell[2], s_updateBase, s_probeRays,
                       s_probeUpdates, outUbo.misc[0]);

    // --- r_rtGIProbeDump ---
    if (r_rtGIProbeDump.GetBool())
    {
        r_rtGIProbeDump.SetBool(false);

        int traced = 0;
        int inside = 0;
        int outside = 0;
        double bfSum = 0.0;
        double msSum = 0.0;
        float bfMax = 0.0f;
        float msMax = 0.0f;
        for (int i = 0; i < s_probeStateCount; i++)
        {
            if (s_probeState[i].flags & GIPROBE_FLAG_TRACED)
                traced++;
            if (s_probeState[i].flags & GIPROBE_FLAG_INSIDE)
                inside++;
            if (s_probeState[i].flags & GIPROBE_FLAG_OUTSIDE)
                outside++;
            bfSum += s_probeStats[i].backface;
            msSum += s_probeStats[i].miss;
            bfMax = Max(bfMax, s_probeStats[i].backface);
            msMax = Max(msMax, s_probeStats[i].miss);
        }

        const idVec3 camPos = viewDef->renderView.vieworg;
        const double irrMiB =
            (double)vkRT.giProbeIrradiance.width * vkRT.giProbeIrradiance.height * 8.0 / (1024.0 * 1024.0);
        const double distMiB =
            (double)vkRT.giProbeDistance.width * vkRT.giProbeDistance.height * 4.0 / (1024.0 * 1024.0);
        const double scratchMiB = (double)vkRT.giProbeScratch[frameIdx].width * vkRT.giProbeScratch[frameIdx].height *
                                  8.0 / (1024.0 * 1024.0);
        const double stateMiB = (double)s_probeStateCount * sizeof(GIProbeStateEntry) / (1024.0 * 1024.0);
        const double statsMiB = (double)s_probeStateCount * sizeof(GIProbeStatsEntry) / (1024.0 * 1024.0);

        common->Printf("=== [r_rtGIProbeDump] frame=%d slot=%d ===\n", tr.frameCount, frameIdx);
        common->Printf("  grid=%dx%dx%d (%d probes)  spacing=%.1f  coverage=%.0fx%.0fx%.0f units\n", s_probeDim[0],
                       s_probeDim[1], s_probeDim[2], s_probeStateCount, s_spacing, s_probeDim[0] * s_spacing,
                       s_probeDim[1] * s_spacing, s_probeDim[2] * s_spacing);
        common->Printf("  camera=(%.0f %.0f %.0f)  baseCell=(%d %d %d)  origin=(%.0f %.0f %.0f)\n", camPos.x, camPos.y,
                       camPos.z, s_baseCell[0], s_baseCell[1], s_baseCell[2], outUbo.gridOrigin[0],
                       outUbo.gridOrigin[1], outUbo.gridOrigin[2]);
        common->Printf("  atlas tiles=%dx%d/bucket  irradiance %ux%u side=%d (%.1f MiB)  distance %ux%u side=%d "
                       "(%.1f MiB)\n",
                       s_tilesX, s_tilesY, vkRT.giProbeIrradiance.width, vkRT.giProbeIrradiance.height, s_irrSide,
                       irrMiB, vkRT.giProbeDistance.width, vkRT.giProbeDistance.height, s_distSide, distMiB);
        common->Printf("  scratch %ux%u (%.2f MiB/slot x2: primary + G5b fast)  state %.2f MiB/slot  stats %.2f MiB "
                       "shared  total ~%.1f MiB\n",
                       vkRT.giProbeScratch[frameIdx].width, vkRT.giProbeScratch[frameIdx].height, scratchMiB, stateMiB,
                       statsMiB,
                       irrMiB + distMiB + statsMiB + (scratchMiB * 2.0 + stateMiB) * VK_MAX_FRAMES_IN_FLIGHT);
        // G5b. A gain pinned at 1.000 with a nonzero fast count means every fast
        // light happens to be at its peak this instant; a gain of 0.000 with a
        // count of 0 is the no-flickering-light-in-range case and the fast atlas
        // is correctly contributing nothing.
        common->Printf("  G5b: fastBuckets=%d (requested %d)  fastLights=%d  gain[0]=%.3f  irradiance atlas is "
                       "%dx its single-bucket size\n",
                       s_probeFastBuckets, VK_RT_GIFastBucketCount(), VK_RT_GIFastLightCount(),
                       VK_RT_GIFastBucketGain(0), 1 + s_probeFastBuckets);
        const int usable = traced - inside - outside;
        common->Printf("  traced=%d/%d  never traced=%d\n", traced, s_probeStateCount, s_probeStateCount - traced);
        common->Printf("  usable=%d (%.1f%%)  insideGeometry=%d (%.1f%%)  outsideLevel=%d (%.1f%%)\n", usable,
                       100.0 * usable / Max(1, s_probeStateCount), inside, 100.0 * inside / Max(1, s_probeStateCount),
                       outside, 100.0 * outside / Max(1, s_probeStateCount));
        // A near-zero mean with a max near 1.0 means the statistic is cleanly
        // BIMODAL — a crisp classifier. A mean near the threshold means probes
        // are strewn across it and the verdict is a coin flip.
        common->Printf("  backface: mean=%.3f max=%.3f  threshold=%.2f on / %.2f off\n",
                       bfSum / Max(1, s_probeStateCount), bfMax, Max(0.0f, r_rtGIProbeInsideThreshold.GetFloat()),
                       Max(0.0f, r_rtGIProbeInsideThreshold.GetFloat()) * 0.75f);
        common->Printf("  miss:     mean=%.3f max=%.3f  threshold=%.2f on / %.2f off\n",
                       msSum / Max(1, s_probeStateCount), msMax, Max(0.0f, r_rtGIProbeOutsideThreshold.GetFloat()),
                       Max(0.0f, r_rtGIProbeOutsideThreshold.GetFloat()) * 0.75f);
        common->Printf("  schedule: base=%d  %d probes/frame -> full refresh every %.1f frames  %d rays each "
                       "(%d rays/frame)\n",
                       s_updateBase, s_probeUpdates, (double)s_probeStateCount / (double)Max(1, s_probeUpdates),
                       s_probeRays, s_probeRays * s_probeUpdates);
        common->Printf("  maxRayDist=%.1f  hysteresis=%.3f  normalBias=%.1f  distSharpness=%.1f\n", outUbo.tune[3],
                       outUbo.tune[0], outUbo.tune[1], outUbo.debug[2]);
        common->Printf("  maintain=%d  probeResolveActive=%d  debugMode=%d\n", VK_RT_GIProbeMaintain() ? 1 : 0,
                       VK_RT_GIProbeActive() ? 1 : 0, outUbo.misc[0]);
        common->Printf("  sizeof(GIProbeParamsUBO)=%d (GLSL std140 block expects 256)\n",
                       (int)sizeof(GIProbeParamsUBO));
        common->Printf("  resolve: giStrength=%.3f giContrast=%.3f (mode 0 owns giBuffer=%d)\n", outUbo.tune[2],
                       outUbo.tune2[0], (VK_RT_GIProbeActive() && outUbo.misc[0] == 0) ? 1 : 0);
        common->Printf("  leak control: chebyshev=%s normalBias=%.1f viewBias=%.1f (spacing %.0f, so %.2f/%.2f "
                       "cells)\n",
                       outUbo.misc[2] ? "ON" : "OFF", outUbo.tune[1], outUbo.tune2[1], s_spacing,
                       outUbo.tune[1] / Max(1.0f, s_spacing), outUbo.tune2[1] / Max(1.0f, s_spacing));
    }

    return true;
}

// Mark this frame's scheduled probes as having history.  Called only once the
// trace is actually recorded, and always AFTER the state SSBO upload above:
// gi_probe_blend.comp needs "was this probe traced BEFORE now" to decide
// whether it has history worth blending against, and the flag it reads is the
// one that went into the buffer a moment earlier.
static void VK_RT_GIProbeMarkScheduledTraced(void)
{
    for (int s = 0; s < s_probeUpdates; s++)
    {
        const int li = (s_updateBase + s) % s_probeStateCount;
        const int lx = li % s_probeDim[0];
        const int ly = (li / s_probeDim[0]) % s_probeDim[1];
        const int lz = li / (s_probeDim[0] * s_probeDim[1]);

        const int32_t ax = s_baseCell[0] + lx;
        const int32_t ay = s_baseCell[1] + ly;
        const int32_t az = s_baseCell[2] + lz;
        const int32_t wx = ((ax % s_probeDim[0]) + s_probeDim[0]) % s_probeDim[0];
        const int32_t wy = ((ay % s_probeDim[1]) + s_probeDim[1]) % s_probeDim[1];
        const int32_t wz = ((az % s_probeDim[2]) + s_probeDim[2]) % s_probeDim[2];

        s_probeState[wx + s_probeDim[0] * (wy + s_probeDim[1] * wz)].flags |= GIPROBE_FLAG_TRACED;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGIProbeTrace (public)
//
// Second raygen group of the GI RT pipeline.  Binds set 0 with the GIParams
// offset vk_gi.cpp built this frame (gi_ray.rchit reads that block), set 1 with
// the material table, and set 2 with the probe resources.
//
// Must be called outside a render pass, after VK_RT_DispatchGI — it depends on
// that function having refreshed set 0 for this frame.
// ---------------------------------------------------------------------------

void VK_RT_DispatchGIProbeTrace(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!VK_RT_GIProbeMaintain())
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (vkRT.giPipeline == VK_NULL_HANDLE || vkRT.giProbeRgenRegion.deviceAddress == 0)
        return;

    const int frameIdx = vk.currentFrame;

    static int s_lastTraceFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastTraceFrame[frameIdx] == tr.frameCount)
        return;
    s_lastTraceFrame[frameIdx] = tr.frameCount;

    GIProbeParamsUBO ubo;
    if (!VK_RT_GIProbeUpdate(viewDef, ubo))
        return;

    uint32_t giParamsOffset = 0;
    if (!VK_RT_GIParamsBinding(frameIdx, &giParamsOffset))
    {
        // vk_gi.cpp did not build set 0 this frame (GI disabled, or it early-outed),
        // so binding 3/4 would point at a stale buffer and gi_ray.rchit would read
        // last frame's light list. Skip rather than trace against it.
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GIProbe: no GIParams this frame — probe trace skipped\n");
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 0, 1,
                            &vkRT.giDescSets[frameIdx], 1, &giParamsOffset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 1, 1, &vkRT.matDescSet,
                            0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.giPipelineLayout, 2, 1,
                            &vkRT.giProbeDescSets[frameIdx], 0, NULL);

    vkCmdTraceRaysKHR(cmd, &vkRT.giProbeRgenRegion, &vkRT.giMissRegion, &vkRT.giHitRegion, &vkRT.giCallRegion,
                      (uint32_t)s_probeRays, (uint32_t)s_probeUpdates, 1);
    s_traceRecordedFrame[frameIdx] = tr.frameCount;
    VK_RT_GIProbeMarkScheduledTraced();

    // RT write -> compute read (blend).
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &memBarrier, 0, NULL, 0, NULL);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GIProbe: trace frame=%d slot=%d launch=%dx%d base=%d\n", tr.frameCount, frameIdx,
                       s_probeRays, s_probeUpdates, s_updateBase);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGIProbeBlend (public)
//
// scratch -> atlases (EMA), then the octahedral border fill.  Both run over the
// same probesPerFrame update slots and share one descriptor set.
// ---------------------------------------------------------------------------

void VK_RT_DispatchGIProbeBlend(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!VK_RT_GIProbeMaintain())
        return;
    if (vkRT.giProbeBlendPipeline == VK_NULL_HANDLE || vkRT.giProbeBorderPipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;

    static int s_lastBlendFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastBlendFrame[frameIdx] == tr.frameCount)
        return;

    // The blend is meaningless without the trace that fills the scratch.
    if (s_traceRecordedFrame[frameIdx] != tr.frameCount)
        return;
    s_lastBlendFrame[frameIdx] = tr.frameCount;

    GIProbeParamsUBO ubo;
    if (!VK_RT_GIProbeUpdate(viewDef, ubo))
        return;

    // Write-after-read across the frame boundary. The atlases are SHARED, not per
    // slot, so the previous frame's resolve may still be sampling them when this
    // blend starts overwriting: submission order does not imply an execution
    // dependency, and the frame fence is two submissions back. Nothing else
    // covers it — the trace->blend barrier below is about the scratch image, and
    // the post-resolve barrier only reaches fragment work.
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memBarrier, 0, NULL, 0, NULL);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giProbeBlendPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giProbeBlendPipelineLayout, 0, 1,
                            &vkRT.giProbeDescSets[frameIdx], 0, NULL);
    vkCmdDispatch(cmd, (uint32_t)s_probeUpdates, 1, 1);

    // Blend writes the interiors; the border pass reads them. It also writes the
    // stats accumulator, which is copied to this slot's readback snapshot below.
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memBarrier,
                             0, NULL, 0, NULL);
    }

    // Snapshot for the CPU's G4 classification. Reading the live accumulator
    // instead would race the previous frame's blend — the fence the CPU waited on
    // is two submissions back, not one.
    if (vkRT.giProbeStatsReadback[frameIdx] != VK_NULL_HANDLE)
    {
        VkBufferCopy region = {};
        region.size = (VkDeviceSize)s_probeStateCount * sizeof(GIProbeStatsEntry);
        vkCmdCopyBuffer(cmd, vkRT.giProbeStatsSsbo, vkRT.giProbeStatsReadback[frameIdx], 1, &region);

        VkMemoryBarrier hostBarrier = {};
        hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0,
                             NULL, 0, NULL);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giProbeBorderPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giProbeBorderPipelineLayout, 0, 1,
                            &vkRT.giProbeDescSets[frameIdx], 0, NULL);
    // y covers every irradiance bucket plus the single distance map. G5b: this
    // used to be a constant 2 and MUST track s_probeFastBuckets — passing 2 with
    // K = 1 leaves the fast bucket's octahedral borders unwritten, which reads as
    // a seam cross that shows up only on flickering lights.
    vkCmdDispatch(cmd, (uint32_t)s_probeUpdates, (uint32_t)(s_probeFastBuckets + 2), 1);

    // Border writes -> resolve's sampled read.
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GIProbe: blend+border frame=%d slot=%d probes=%d alpha=%.3f\n", tr.frameCount, frameIdx,
                       s_probeUpdates, 1.0f - ubo.tune[0]);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGIProbeResolve (public)
//
// Writes vkRT.giBuffer and repoints giReadView at it, so gi_composite.frag
// keeps sampling one 2D image and knows nothing about probes.  giReadView has
// to be re-pointed because temporal/a-trous aimed it at their own outputs
// earlier in the frame (they stand down under r_rtGIProbes 1, but the overlays
// run with the per-pixel path live).
//
// Mode 0 is the shipping G2 resolve; any other mode is an overlay, and
// VK_RT_DispatchGIAlbedoMod skips so a measurement reaches the composite
// unmodulated.
//
// Must be called outside a render pass; depth must be in ATTACHMENT_OPTIMAL.
// ---------------------------------------------------------------------------

void VK_RT_DispatchGIProbeResolve(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    const int debugMode = VK_RT_GIProbeDebugMode();
    if (debugMode == 0 && !VK_RT_GIProbeActive())
        return;

    const int frameIdx = vk.currentFrame;

    // G3 bound the TLAS into the resolve set for the leak overlay's ray queries.
    // Only mode 3 reads it, but the shader references it statically, so binding
    // set 0 at all requires a live handle. VK_RT_DispatchGI has the same
    // requirement, so a frame without a TLAS already had no GI.
    if (!vkRT.tlas[frameIdx].isValid || vkRT.tlas[frameIdx].handle == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GIProbe: resolve skipped — no valid TLAS this frame\n");
        return;
    }

    static int s_lastResolveFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastResolveFrame[frameIdx] == tr.frameCount)
        return;
    s_lastResolveFrame[frameIdx] = tr.frameCount;

    vkReflBuffer_t &gb = vkRT.giBuffer[frameIdx];
    if (gb.image == VK_NULL_HANDLE || vkRT.giProbeResolvePipeline == VK_NULL_HANDLE)
        return;

    GIProbeParamsUBO ubo;
    if (!VK_RT_GIProbeUpdate(viewDef, ubo))
        return;

    // Claimed up front, like the froxel resolve claims volReadView: the denoise
    // chain aims giReadView at its own scratch images, and any early-out below
    // would otherwise leave the composite reading those instead of the overlay.
    vkRT.giReadView[frameIdx] = gb.view;

    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

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

    // --- Descriptor set ---
    {
        VkDescriptorSet ds = vkRT.giProbeResolveDescSets[frameIdx];

        const bool haveGbuf = vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE;
        VkImageView gbufView = haveGbuf ? vkRT.gbufNormal[frameIdx].view : VK_RT_GetNullGbufNormalView();

        VkDescriptorImageInfo irrInfo = {vkRT.giProbeSampler, vkRT.giProbeIrradiance.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo distInfo = {vkRT.giProbeSampler, vkRT.giProbeDistance.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo outInfo = {VK_NULL_HANDLE, gb.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo depthInfo = {vkRT.depthSampler, vk.depthSampledView,
                                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo gbufInfo = {vkRT.depthSampler, gbufView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkDescriptorBufferInfo paramsInfo = {vkRT.giProbeParamsUbo[frameIdx], 0, sizeof(GIProbeParamsUBO)};
        VkDescriptorBufferInfo stateInfo = {vkRT.giProbeStateSsbo[frameIdx], 0, VK_WHOLE_SIZE};

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorBufferInfo statsInfo = {vkRT.giProbeStatsSsbo, 0, VK_WHOLE_SIZE};

        // G5b mode 8 only, but statically referenced by the shader, so it is
        // bound on every dispatch. Same slot's buffer gi_ray.rchit reads.
        VkDescriptorBufferInfo lightInfo = {vkRT.giLightSsbo[frameIdx], 0, VK_WHOLE_SIZE};

        VkWriteDescriptorSet writes[VK_GIPROBE_RESOLVE_BINDINGS] = {};
        for (int i = 0; i < VK_GIPROBE_RESOLVE_BINDINGS; i++)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = ds;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &irrInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &outInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &paramsInfo;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &stateInfo;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo = &distInfo;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &depthInfo;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = &gbufInfo;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[7].pNext = &tlasWrite;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[8].pBufferInfo = &statsInfo;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[9].pBufferInfo = &lightInfo;

        vkUpdateDescriptorSets(vk.device, VK_GIPROBE_RESOLVE_BINDINGS, writes, 0, NULL);
        vkRT.giProbeResolveDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // We are about to overwrite giBuffer, which the denoise chain read earlier
    // in the frame.
    {
        VkMemoryBarrier giBarrier = {};
        giBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        giBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        giBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &giBarrier, 0, NULL, 0, NULL);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giProbeResolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giProbeResolvePipelineLayout, 0, 1,
                            &vkRT.giProbeResolveDescSets[frameIdx], 0, NULL);

    const uint32_t groupsX = ((uint32_t)ubo.rect[2] + 7) / 8;
    const uint32_t groupsY = ((uint32_t)ubo.rect[3] + 7) / 8;
    if (groupsX > 0 && groupsY > 0)
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

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
        common->Printf("VK RT GIProbe: resolve frame=%d slot=%d %s(mode=%d) rect=(%d,%d %dx%d)\n", tr.frameCount,
                       frameIdx, (ubo.misc[0] == 0) ? "shipping" : "overlay", ubo.misc[0], ubo.rect[0], ubo.rect[1],
                       ubo.rect[2], ubo.rect[3]);
}
