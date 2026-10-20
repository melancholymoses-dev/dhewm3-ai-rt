/*
===========================================================================

dhewm3-rt Vulkan ray tracing - types and declarations.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#ifndef __VK_RAYTRACING_H__
#define __VK_RAYTRACING_H__

#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_buffer_pool.h"

// ---------------------------------------------------------------------------
// RT extension function pointers (always loaded at runtime via vkGetDeviceProcAddr)
//
// vulkan-1.lib does not export KHR extension symbols — they must be fetched
// from the driver at runtime regardless of SDK version.  We use a pfn_ prefix
// to avoid clashing with the SDK's own function declarations, then redirect
// all call sites via the macros below.
// ---------------------------------------------------------------------------

extern PFN_vkCreateAccelerationStructureKHR pfn_vkCreateAccelerationStructureKHR;
extern PFN_vkDestroyAccelerationStructureKHR pfn_vkDestroyAccelerationStructureKHR;
extern PFN_vkGetAccelerationStructureBuildSizesKHR pfn_vkGetAccelerationStructureBuildSizesKHR;
extern PFN_vkCmdBuildAccelerationStructuresKHR pfn_vkCmdBuildAccelerationStructuresKHR;
extern PFN_vkGetAccelerationStructureDeviceAddressKHR pfn_vkGetAccelerationStructureDeviceAddressKHR;
extern PFN_vkCreateRayTracingPipelinesKHR pfn_vkCreateRayTracingPipelinesKHR;
extern PFN_vkGetRayTracingShaderGroupHandlesKHR pfn_vkGetRayTracingShaderGroupHandlesKHR;
extern PFN_vkCmdTraceRaysKHR pfn_vkCmdTraceRaysKHR;
extern PFN_vkGetBufferDeviceAddressKHR pfn_vkGetBufferDeviceAddressKHR;

#define vkCreateAccelerationStructureKHR pfn_vkCreateAccelerationStructureKHR
#define vkDestroyAccelerationStructureKHR pfn_vkDestroyAccelerationStructureKHR
#define vkGetAccelerationStructureBuildSizesKHR pfn_vkGetAccelerationStructureBuildSizesKHR
#define vkCmdBuildAccelerationStructuresKHR pfn_vkCmdBuildAccelerationStructuresKHR
#define vkGetAccelerationStructureDeviceAddressKHR pfn_vkGetAccelerationStructureDeviceAddressKHR
#define vkCreateRayTracingPipelinesKHR pfn_vkCreateRayTracingPipelinesKHR
#define vkGetRayTracingShaderGroupHandlesKHR pfn_vkGetRayTracingShaderGroupHandlesKHR
#define vkCmdTraceRaysKHR pfn_vkCmdTraceRaysKHR
#define vkGetBufferDeviceAddressKHR pfn_vkGetBufferDeviceAddressKHR

// ---------------------------------------------------------------------------
// Material table constants and types (Phase 5.4)
// ---------------------------------------------------------------------------

static const uint32_t VK_MAT_MAX_TEXTURES = 4096; // max bindless texture slots
static const uint32_t VK_MAT_MAX_GEOMS = 16384;   // max total geometry slots across all BLAS instances

// TLAS instance mask bits. A ray cull mask of VK_RT_MASK_NO_PLAYER (0xFD) skips the player
// (body, head, held weapon) only; 0xFF sees everything. GLSL mirrors the literal 0xFD.
#define VK_RT_MASK_WORLD 0x01u
#define VK_RT_MASK_PLAYER 0x02u
#define VK_RT_MASK_NO_PLAYER 0xFDu

// Per-material-entry flags (must match GLSL definition in rt_material.glsl)
#define VK_MAT_FLAG_ALPHA_TESTED 0x01u // MC_PERFORATED — alpha discard in any-hit
#define VK_MAT_FLAG_TWO_SIDED 0x02u    // CT_TWO_SIDED — no back-face cull
#define VK_MAT_FLAG_GLASS 0x04u        // MC_TRANSLUCENT — thin glass, flat F0=0.04 reflectance
#define VK_MAT_FLAG_PLAYER_BODY 0x08u  // renderEntity_t::rtPlayer — routed to player_reflect hit group
#define VK_MAT_FLAG_EMISSIVE 0x10u     // SL_AMBIENT stage present — surface emits its own light
#define VK_MAT_FLAG_GUI_EMISSIVE 0x20u // guiSurf/entity GUI surface — boost emissive in RT GI/refs
// R4: MC_TRANSLUCENT *and* SURFTYPE_GLASS. VK_MAT_FLAG_GLASS alone is every translucent
// (smoke, force fields, liquids), which must not act as a reflection plane.
#define VK_MAT_FLAG_REAL_GLASS 0x40u
// GUI reflections (docs/plans/20261015_gui_reflections.md). GUI_SURF = HasGui() && MC_TRANSLUCENT;
// GUI_LIVE = emissive index points at a live GUI texture slot (Stage 2).
#define VK_MAT_FLAG_GUI_SURF 0x80u
#define VK_MAT_FLAG_GUI_LIVE 0x100u

// One entry per BLAS geometry slot.  Indexed by
//   gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT
// in shaders (instanceCustomIndex == baseGeomIdx of the instance).
// std430-compatible: all fields are 4 bytes, total = 32 bytes.
struct VkMaterialEntry
{
    uint32_t diffuseTexIndex;  // index into bindless texture array (0 = white fallback)
    uint32_t normalTexIndex;   // index into bindless texture array (0 = flat normal)
    float roughness;           // GGX roughness [0,1]; default 1.0 (fully diffuse)
    uint32_t flags;            // VK_MAT_FLAG_*
    uint32_t baseGeomIdx;      // offset into per-geometry VtxAddrTable/IdxAddrTable
    float alphaThreshold;      // alpha test cutoff (MC_PERFORATED); default 0.5
    uint32_t maxVertex;        // numVerts-1 for this geometry; used for bounds check in rt_InterpolateUV
    uint32_t emissiveTexIndex; // bindless index of SL_AMBIENT stage image; 0 = no emissive
    uint32_t maxPrimId;        // numTriangles-1 for this geometry; 0xFFFFFFFF = no check.
                               // Must be checked BEFORE indexing the index buffer by primId —
                               // maxVertex only validates the values read, after an
                               // out-of-bounds index-buffer read has already happened.
};
static_assert(sizeof(VkMaterialEntry) == 36, "VkMaterialEntry size mismatch");

// ---------------------------------------------------------------------------
// BLAS (Bottom-Level Acceleration Structure) - one per unique mesh
// ---------------------------------------------------------------------------

struct vkBLAS_t
{
    VkAccelerationStructureKHR handle;
    vkPoolAlloc_t storage; // AS storage range in the BLAS storage pool
    VkDeviceAddress deviceAddress;
    // Per-surface geometry ranges in the host-mapped BLAS geometry pool, length geomCount.
    // buf == VK_NULL_HANDLE for zero-copy geometry read straight from the vertex cache.
    // Released with the BLAS, after every command buffer that read them has retired.
    uint32_t geomCount;
    vkPoolAlloc_t *geomVertRanges;
    vkPoolAlloc_t *geomIdxRanges;
    // Per-surface geometry sizes — used to validate reuse for BLAS update.
    VkDeviceSize *geomVertSizes;
    VkDeviceSize *geomIdxSizes;
    // Per-surface primitive counts — must match exactly for MODE_UPDATE to be valid.
    uint32_t *geomPrimCounts;
    // Per-surface device addresses of vertex/index data (vertex 0 / index 0).
    // Populated at BLAS build time; used by VK_RT_MakeMaterialEntry to fill
    // the VtxAddrTable and IdxAddrTable SSBOs for UV interpolation in shaders.
    VkDeviceAddress *geomVertAddrs; // [geomCount], NULL if geomCount == 0
    VkDeviceAddress *geomIdxAddrs;  // [geomCount], NULL if geomCount == 0
    // Per-geometry surface shaders — one per BLAS geometry entry, in the same
    // order as the valid surfaces selected during VK_RT_BuildBLASForModel.
    // Used by the TLAS loop to write per-surface VkMaterialEntry values.
    // idMaterial* pointers are stable for the lifetime of a map session.
    const idMaterial **geomShaders; // [geomCount], may hold NULLs for missing shaders
    // Build/update scratch range in the BLAS scratch pool. Empty for static BLASes, which
    // release it after the build (they never refit).
    vkPoolAlloc_t scratch;
    bool allowUpdate; // built with ALLOW_UPDATE; MODE_UPDATE refits are only legal when set
    bool isValid;
    // Set when the model BLAS cache owns this entry.
    // VK_RT_DestroyBLAS is a no-op for cached BLASes; only the cache clear
    // function frees them (after all in-flight references are gone).
    bool cachedByModel;
};

// ---------------------------------------------------------------------------
// TLAS (Top-Level Acceleration Structure) - rebuilt each frame from BLASes
// ---------------------------------------------------------------------------

struct vkTLAS_t
{
    VkAccelerationStructureKHR handle;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress deviceAddress;
    VkDeviceSize bufferSize;

    VkBuffer instanceBuffer; // VkAccelerationStructureInstanceKHR array
    VkDeviceMemory instanceMemory;
    uint32_t instanceCount;
    VkDeviceSize instanceBufferSize;

    VkBuffer scratchBuffer; // build scratch
    VkDeviceMemory scratchMemory;
    VkDeviceSize scratchBufferSize;

    bool isValid;
    int lastBuiltFrameCount; // skip redundant rebuilds within same frame
};

// ---------------------------------------------------------------------------
// Shadow mask layer budget (P1b — batched shadow masks, rt_optimization_tuning.md)
//
// The shadow mask is an R8_UNORM 2D *array*: one layer per shadow-casting light
// in a batch.  Batch 0 is traced back-to-back outside the render pass; each later
// batch reuses the same layers and costs one render-pass break (Stage S3).  Each
// light's draw samples its own layer via u_ShadowMaskLayer.
// ---------------------------------------------------------------------------
#define VK_RT_SHADOW_LAYERS 8

// Shadow mask buffer (R8 UNORM 2D array, one pixel per screen pixel, one layer per light)
struct vkShadowMask_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view; // VK_IMAGE_VIEW_TYPE_2D_ARRAY covering all layers
    // Temp image for blur ping-pong (same format/size/layer count as shadow mask)
    VkImage blurTempImage;
    VkDeviceMemory blurTempMemory;
    VkImageView blurTempView;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
};

// General Image Buffer for RT
// Has image, memory, view and sizing.
struct vkRTImage_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width;
    uint32_t height;
};

// Volumetric froxel grid: a 3D image (20260906_froxel_probe_gi.md Part A).
// Separate from vkRTImage_t only because it needs a depth extent.
struct vkFroxelGrid_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

// ---------------------------------------------------------------------------
// vkReflChain_t — one reflection trace + denoise chain
//
// Bundles everything that is per-population rather than per-renderer: the traced
// image, the temporal EMA's cross-frame ping-pong history/moments, the à-trous
// scratch, the descriptor sets for all three passes, and the bookkeeping those
// passes need (launch rect, duplicate-dispatch guards, cached bound views).
// Pipelines, layouts, pools and samplers stay shared in vkRTState_t — two chains
// run the same shaders over different images.
//
// Stage 4a of docs/plans/20261017_reflection_pipeline_refactor.md introduces this
// with exactly one instance; 4b adds a second so glass and opaque can be traced
// and filtered at different rates and resolutions.
// ---------------------------------------------------------------------------
struct vkReflChain_t
{
    const char *name; // appears in this chain's log lines

    // Whether the à-trous filter runs on this chain. False for glass: refl_atrous.comp
    // excludes negative-alpha pixels from both the centre test and the 3x3 taps, so
    // every pass over a glass image is a copy. The temporal EMA does run on glass —
    // that is what r_rtReflDenoiseAlphaGlass feeds.
    bool spatialFilter;

    vkRTImage_t trace[VK_MAX_FRAMES_IN_FLIGHT]; // RGBA16F, full res, what the denoisers read

    // Stage 4c: when half-res tracing is on for this chain, reflect_ray.rgen writes
    // HERE instead and refl_upsample.comp reconstructs trace[] from it. Allocated only
    // for a chain with halfResCapable; image NULL means "trace at full res this frame".
    vkRTImage_t traceHalf[VK_MAX_FRAMES_IN_FLIGHT];
    bool halfResCapable; // opaque only — glass must stay sharp and is already rect-bound

    // Temporal EMA state. Shared across frame slots (the recurrence is sequential
    // across frames), ping-ponged across frames because the resolve reprojects —
    // it reads a different pixel than it writes, so one read-write image races.
    // pingPong is the index most recently WRITTEN, i.e. what consumers read.
    vkRTImage_t history[2]; // rgb = EMA'd radiance, a = this frame's Fresnel weight
    vkRTImage_t moments[2]; // mean, mean^2, history depth, age
    int pingPong;
    bool historyValid;
    idVec3 prevCamPos;
    idVec3 prevCamFwd;

    // À-trous scratch: per-slot ping-pong across the N passes of ONE frame, unlike
    // history's cross-frame ping-pong. atrousReadView is VK_NULL_HANDLE when the
    // filter didn't run this frame.
    vkRTImage_t atrousA[VK_MAX_FRAMES_IN_FLIGHT];
    vkRTImage_t atrousB[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView atrousReadView[VK_MAX_FRAMES_IN_FLIGHT];

    VkDescriptorSet traceDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int traceDescSetLastUpdated[VK_MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSet upsampleDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int upsampleDescSetLastUpdated[VK_MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSet temporalDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int temporalDescSetLastUpdated[VK_MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSet atrousDescSets[VK_MAX_FRAMES_IN_FLIGHT][3];
    int atrousDescSetLastUpdated[VK_MAX_FRAMES_IN_FLIGHT];

    // The rect the rgen actually launched over and the frame it launched on, so both
    // filters touch exactly the pixels that were traced and nothing stale outside.
    VkRect2D dispatchRect[VK_MAX_FRAMES_IN_FLIGHT];
    int dispatchRectFrame[VK_MAX_FRAMES_IN_FLIGHT];

    // Duplicate-dispatch guards: the backend can issue a second RC_DRAW_VIEW in one
    // frame (weapon subview, 2D overlay), which would re-run the EMA on its own output.
    int lastTraceFrame[VK_MAX_FRAMES_IN_FLIGHT];
    int lastTemporalFrame[VK_MAX_FRAMES_IN_FLIGHT];
    int lastAtrousFrame[VK_MAX_FRAMES_IN_FLIGHT];

    // Cached bound resources for the trace set's in-frame change detection — a purge
    // or toggle can swap a view inside a frame the cache would otherwise call current.
    VkAccelerationStructureKHR lastTlas[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView lastStorageView[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView lastDepthView[VK_MAX_FRAMES_IN_FLIGHT];
    VkBuffer lastLightSsbo[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView lastGbufView[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView lastGbufAlbedoView[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView lastFroxelView[VK_MAX_FRAMES_IN_FLIGHT];
    VkImageView lastFroxelDirView[VK_MAX_FRAMES_IN_FLIGHT];
};

// Chain 0 = opaque, chain 1 = glass (Stage 4b). The descriptor pools are sized by
// this, so adding a population is a matter of the count plus its dispatch routing.
#define VK_REFL_CHAIN_COUNT 2

// Global RT state
struct vkRTState_t
{
    vkTLAS_t tlas[VK_MAX_FRAMES_IN_FLIGHT];
    vkShadowMask_t shadowMask[VK_MAX_FRAMES_IN_FLIGHT];
    VkSampler shadowMaskSampler; // nearest-clamp, used when sampling shadow mask in lighting pass
    VkSampler depthSampler;      // nearest-clamp, for sampling depth in the RT shadow rgen

    // RT pipeline for shadow rays
    VkPipeline shadowPipeline;
    VkPipelineLayout shadowPipelineLayout;
    VkDescriptorSetLayout shadowDescLayout;
    VkDescriptorPool shadowDescPool;
    VkDescriptorSet shadowDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int shadowDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // AO image and pipeline
    vkRTImage_t aoMask[VK_MAX_FRAMES_IN_FLIGHT];
    // Set by VK_RT_DispatchAO only once vkCmdTraceRaysKHR is recorded for this slot,
    // cleared before every early-out.  The interaction pass must gate on this, not on
    // aoMask[].image: the image can exist and hold nothing written this frame.
    bool aoValid[VK_MAX_FRAMES_IN_FLIGHT];
    VkSampler aoMaskSampler; // nearest-clamp, used when sampling AO in the lighting pass

    VkPipeline aoPipeline;
    VkPipelineLayout aoPipelineLayout;
    VkDescriptorSetLayout aoDescLayout;
    VkDescriptorPool aoDescPool;
    VkDescriptorSet aoDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int aoDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // AO SBT (separate from shadow SBT)
    VkBuffer sbtAOBuffer;
    VkDeviceMemory sbtAOMemory;
    VkStridedDeviceAddressRegionKHR aoRgenRegion;
    VkStridedDeviceAddressRegionKHR aoMissRegion;
    VkStridedDeviceAddressRegionKHR aoHitRegion;
    VkStridedDeviceAddressRegionKHR aoCallRegion;

    // Shadow mask blur (compute pipeline)
    VkPipeline blurPipeline;
    VkPipelineLayout blurPipelineLayout;
    VkDescriptorSetLayout blurDescLayout;
    VkDescriptorPool blurDescPool;
    VkDescriptorSet blurDescSetH[VK_MAX_FRAMES_IN_FLIGHT]; // horizontal pass: shadowMask→blurTemp
    VkDescriptorSet blurDescSetV[VK_MAX_FRAMES_IN_FLIGHT]; // vertical pass: blurTemp→shadowMask
    int blurDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // SHARED, not per slot: the EMA recurrence is sequential across frames, so a
    // per-slot history would only update every other frame and double the ghosting.
    // Safe because all RT work goes through one queue, in submission order.
    vkRTImage_t aoHistory; // accumulated EMA history, R8_UNORM

    // Temporal compute pipeline (temporal_resolve.comp)
    VkPipeline temporalPipeline;
    VkPipelineLayout temporalPipelineLayout;
    VkDescriptorSetLayout temporalDescLayout;
    VkDescriptorPool temporalDescPool;
    // One descriptor set per frame slot: binding 0=aoMask (readonly), binding 1=aoHistory (rw)
    VkDescriptorSet temporalDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int temporalDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // History validity and camera-state cache for cut detection — single instance,
    // matching aoHistory itself now being single instance (see above).
    // See VK_RT_DetectCameraCut (vk_temporal.cpp) — position/orientation deltas,
    // not a raw inverse-view-projection matrix diff (rt_temporal_cut_detection.md).
    bool aoHistoryValid;
    idVec3 aoPrevCamPos;
    idVec3 aoPrevCamFwd;

    // --------------------------------------------------------------------------
    // Ambient Occlusion Atrous spatial filter
    //
    // Ping-pong between aoHistory and aoScratch.  Descriptor set index alternates
    // per pass: [frameIdx][0] reads history → writes scratch (odd pass outputs);
    //           [frameIdx][1] reads scratch → writes history (even pass outputs).
    // aoReadView[i] is updated after each Atrous dispatch to point at whichever
    // image holds the final result.  The backend always samples aoReadView[i].
    // --------------------------------------------------------------------------
    vkRTImage_t aoScratch[VK_MAX_FRAMES_IN_FLIGHT]; // ping-pong scratch, R8_UNORM, GENERAL

    VkPipeline atrousPipeline;
    VkPipelineLayout atrousPipelineLayout;
    VkDescriptorSetLayout atrousDescLayout;
    VkDescriptorPool atrousDescPool;
    VkDescriptorSet atrousDescSets[VK_MAX_FRAMES_IN_FLIGHT][2];
    int atrousDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    VkImageView aoReadView[VK_MAX_FRAMES_IN_FLIGHT];

    // --------------------------------------------------------------------------
    // RT Reflections (Step 5.3)
    //
    // Per-chain resources live in reflChain[]; see vkReflChain_t. Everything the
    // chains share (pipelines, layouts, pools, samplers) stays here.
    // --------------------------------------------------------------------------
    vkReflChain_t reflChain[VK_REFL_CHAIN_COUNT];
    VkSampler reflSampler; // linear-clamp for interaction shader

    // --------------------------------------------------------------------------
    // RT Global Illumination
    //
    // One RGBA16F buffer per frame-in-flight slot.
    // --------------------------------------------------------------------------
    vkRTImage_t giBuffer[VK_MAX_FRAMES_IN_FLIGHT]; // RGBA16F GI colour (raw per-frame)
    VkSampler giSampler;                           // linear-clamp for interaction shader

    // --------------------------------------------------------------------------
    // GI result view, and the scratch pair the chain hands it between.
    //
    // giReadView: per-slot, repointed as the chain runs — the probe resolve aims
    //   it at giBuffer, then the albedo-mod pass aims it at its own output. The
    //   composite pass reads whatever it ends up at, so it never needs to know
    //   which passes ran.
    // giScratchA/B: RGBA16F per frame slot. The albedo-mod pass cannot write in
    //   place (its source is a read-only sampler), so it writes to whichever of
    //   the pair giReadView is NOT currently pointing at. Both are allocated so
    //   that choice is always available.
    // --------------------------------------------------------------------------
    VkImageView giReadView[VK_MAX_FRAMES_IN_FLIGHT]; // composite reads from here
    vkRTImage_t giScratchA[VK_MAX_FRAMES_IN_FLIGHT];
    vkRTImage_t giScratchB[VK_MAX_FRAMES_IN_FLIGHT];

    // --------------------------------------------------------------------------
    // GI light list SSBO
    //
    // One host-visible, persistently-mapped storage buffer per frame-in-flight
    // slot.  Filled each frame from viewDef->viewLights; consumed by
    // gi_ray.rchit to evaluate direct lighting at the secondary hit point.
    // --------------------------------------------------------------------------
    VkBuffer giLightSsbo[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory giLightSsboMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *giLightSsboMapped[VK_MAX_FRAMES_IN_FLIGHT];

    // Dedicated volumetric light selection
    // Same GILightBuffer layout as giLightSsbo above, but filled with a SEPARATE
    // selection: the same admitted-candidate pool, filtered to lights that can
    // possibly reach within r_rtVolMaxDist of the camera, capped at r_rtVolMaxLights.
    // vol_froxel_fill.comp reads this buffer, not giLightSsbo.
    VkBuffer volLightSsbo[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory volLightSsboMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *volLightSsboMapped[VK_MAX_FRAMES_IN_FLIGHT];

    VkPipeline giPipeline;
    VkPipelineLayout giPipelineLayout;
    VkDescriptorSetLayout giDescLayout;
    VkDescriptorPool giDescPool;
    VkDescriptorSet giDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int giDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    VkBuffer sbtGIBuffer;
    VkDeviceMemory sbtGIMemory;
    VkStridedDeviceAddressRegionKHR giMissRegion;
    VkStridedDeviceAddressRegionKHR giHitRegion;
    VkStridedDeviceAddressRegionKHR giCallRegion;

    // Fullscreen composite pipeline — additively blends the GI buffer onto the
    // framebuffer once per view, before the per-light interaction draws.
    VkPipeline giCompositePipeline;
    VkPipeline giCompositeDebugPipeline; // replace-blend variant for r_rtAODebug
    VkPipelineLayout giCompositeLayout;
    VkDescriptorSetLayout giCompositeDescLayout;
    VkDescriptorPool giCompositeDescPool;
    VkDescriptorSet giCompositeDescSets[VK_MAX_FRAMES_IN_FLIGHT];

    VkPipeline reflPipeline;
    VkPipelineLayout reflPipelineLayout;
    VkDescriptorSetLayout reflDescLayout;
    VkDescriptorPool reflDescPool;

    VkBuffer sbtReflBuffer;
    VkDeviceMemory sbtReflMemory;
    VkStridedDeviceAddressRegionKHR reflRgenRegion;
    VkStridedDeviceAddressRegionKHR reflMissRegion;
    VkStridedDeviceAddressRegionKHR reflHitRegion;
    VkStridedDeviceAddressRegionKHR reflCallRegion;

    // Fullscreen composite pipeline
    // reflCompositeDebugPipeline is used when r_rtReflectionDebugMode is
    // 2-7 so the visualization replaces the scene (since blend is off).
    VkPipeline reflCompositePipeline;
    VkPipeline reflCompositeDebugPipeline;
    VkPipelineLayout reflCompositeLayout;
    VkDescriptorSetLayout reflCompositeDescLayout;
    VkDescriptorPool reflCompositeDescPool;
    VkDescriptorSet reflCompositeDescSets[VK_MAX_FRAMES_IN_FLIGHT];

    // Reflection temporal denoiser (20261013_reflection_roughness.md Stage 1).
    // Images and ping-pong state are per chain — see vkReflChain_t.
    VkPipeline reflTemporalPipeline;
    VkPipelineLayout reflTemporalPipelineLayout;
    VkDescriptorSetLayout reflTemporalDescLayout;
    VkDescriptorPool reflTemporalDescPool;

    // Half-res reconstruction (Stage 4c, r_rtReflOpaqueHalfRes). Compute pipeline for
    // refl_upsample.comp; runs between the trace and the temporal resolve.
    VkPipeline reflUpsamplePipeline;
    VkPipelineLayout reflUpsamplePipelineLayout;
    VkDescriptorSetLayout reflUpsampleDescLayout;
    VkDescriptorPool reflUpsampleDescPool;

    // Reflection spatial filter (r_rtReflDenoise 2), à-trous wavelet, roughness-gated
    // (20261013_reflection_roughness.md Stage 1 addendum). Runs on top of the temporal
    // resolve's output. Scratch images and the read view are per chain.
    VkPipeline reflAtrousPipeline;
    VkPipelineLayout reflAtrousPipelineLayout;
    VkDescriptorSetLayout reflAtrousDescLayout;
    VkDescriptorPool reflAtrousDescPool;

    // SBT buffers
    VkBuffer sbtBuffer;
    VkDeviceMemory sbtMemory;
    VkStridedDeviceAddressRegionKHR rgenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callRegion;

    // --------------------------------------------------------------------------
    // Material table  — set=2 in RT shaders
    //
    // Three persistently-mapped HOST_VISIBLE|HOST_COHERENT SSBOs:
    //   matTableSSBO  — VkMaterialEntry[], one per geometry slot, indexed by
    //                   gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT
    //   vtxAddrSSBO   — uint64_t[],        vertex buffer device address per instance
    //   idxAddrSSBO   — uint64_t[],        index  buffer device address per instance
    //
    // One copy per frame slot, indexed by vk.currentFrame: the CPU rewrites them
    // while recording, and the previous frame's TLAS still indexes the old layout.
    // --------------------------------------------------------------------------
    VkBuffer matTableSSBO[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory matTableSSBOMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *matTableMapped[VK_MAX_FRAMES_IN_FLIGHT]; // persistently mapped

    VkBuffer vtxAddrSSBO[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory vtxAddrSSBOMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *vtxAddrMapped[VK_MAX_FRAMES_IN_FLIGHT]; // persistently mapped

    VkBuffer idxAddrSSBO[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory idxAddrSSBOMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *idxAddrMapped[VK_MAX_FRAMES_IN_FLIGHT]; // persistently mapped

    VkDescriptorSetLayout matDescLayout; // set=2: mat SSBO + vtx/idx addr + bindless textures
    VkDescriptorPool matDescPool;
    VkDescriptorSet matDescSet[VK_MAX_FRAMES_IN_FLIGHT]; // bind matDescSet[vk.currentFrame]
    VkSampler matSampler;                                // bilinear-clamp, used for all bindless slots

    bool matTableInitialized;

    // --------------------------------------------------------------------------
    // Volumetric Lighting
    //
    // volBuffer: RGBA16F per-slot scatter accumulation image (full resolution).
    //   Written by the froxel resolve, its only writer.
    //   Read by the composite pass (additive blend onto framebuffer).
    // volComposite*: graphics pipeline for additive composite (shares
    //   gi_composite.vert; separate vol_composite.frag).
    // volReadView: what the composite samples — claimed by the froxel resolve each
    //   frame, defaulting to volBuffer's own view.
    // --------------------------------------------------------------------------
    vkRTImage_t volBuffer[VK_MAX_FRAMES_IN_FLIGHT]; // RGBA16F volumetric scatter
    VkSampler volSampler;                           // bilinear-clamp for composite
    VkImageView volReadView[VK_MAX_FRAMES_IN_FLIGHT];

    VkPipeline volCompositePipeline;
    VkPipeline volCompositeDebugPipeline;
    VkPipeline volCompositeAttenPipeline;
    VkPipelineLayout volCompositeLayout;
    VkDescriptorSetLayout volCompositeDescLayout;
    VkDescriptorPool volCompositeDescPool;
    VkDescriptorSet volCompositeDescSets[VK_MAX_FRAMES_IN_FLIGHT];

    // --------------------------------------------------------------------------
    // Volumetric froxel grid — the volumetric integrator.
    //
    //  froxelScatter holds per-cell in-scattering (rgb, per unit length) + extinction (a);
    //  froxelDir (Stage 5) holds the mean scattering direction, |xyz| = mean cosine g;
    //  froxelIntegrated (F2) holds the accumulated camera->cell integral.
    // All are per frame-in-flight slot because each is written and consumed inside
    // the same frame.  The froxel-space EMA history(F4) will be a SINGLE shared image.
    //
    // froxelScatter is UNPHASED since Stage 5 and is meaningless without froxelDir
    // beside it — the two are written and read as a pair.
    // --------------------------------------------------------------------------
    vkFroxelGrid_t froxelScatter[VK_MAX_FRAMES_IN_FLIGHT];
    vkFroxelGrid_t froxelDir[VK_MAX_FRAMES_IN_FLIGHT];
    vkFroxelGrid_t froxelIntegrated[VK_MAX_FRAMES_IN_FLIGHT];
    VkSampler froxelSampler; // trilinear + clamp;

    VkPipeline froxelFillPipeline;
    VkPipelineLayout froxelFillPipelineLayout;
    VkDescriptorSetLayout froxelFillDescLayout;
    VkDescriptorPool froxelFillDescPool;
    VkDescriptorSet froxelFillDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int froxelFillDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    VkPipeline froxelIntegratePipeline;
    VkPipelineLayout froxelIntegratePipelineLayout;
    VkDescriptorSetLayout froxelIntegrateDescLayout;
    VkDescriptorPool froxelIntegrateDescPool;
    VkDescriptorSet froxelIntegrateDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int froxelIntegrateDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // Resolve: grid -> vkRT.volBuffer, so nothing downstream of the volumetric
    // composite has to learn about the grid.
    VkPipeline froxelResolvePipeline;
    VkPipelineLayout froxelResolvePipelineLayout;
    VkDescriptorSetLayout froxelResolveDescLayout;
    VkDescriptorPool froxelResolveDescPool;
    VkDescriptorSet froxelResolveDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int froxelResolveDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // --------------------------------------------------------------------------
    // GI irradiance probes
    //
    // The two atlases are SINGLE, so they accumulate over all updates every frame.
    // The scratch image is per slot — it holds one frame's rays with no cross-frame meaning,
    //
    // giProbeDescLayout binds at set 2 in the GI RT pipeline and set 0 in the
    // blend/border/resolve compute ones; the GLSL side keys on GIPROBE_SET.
    // --------------------------------------------------------------------------
    vkRTImage_t giProbeIrradiance; // rgba16f octahedral irradiance atlas, SHARED, (1+K) buckets tall
    vkRTImage_t giProbeDistance;   // rg16f visibility moments (mean, mean^2), SHARED
    vkRTImage_t giProbeScratch[VK_MAX_FRAMES_IN_FLIGHT];     // rgba16f, raysPerProbe x probesPerFrame
    vkRTImage_t giProbeScratchFast[VK_MAX_FRAMES_IN_FLIGHT]; // G5b fast-bucket radiance, rgba16f
    VkSampler giProbeSampler;                                // bilinear-clamp for atlas fetches

    // CPU-owned (offset + flags), re-uploaded every frame, hence per slot.
    VkBuffer giProbeStateSsbo[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory giProbeStateSsboMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *giProbeStateSsboMapped[VK_MAX_FRAMES_IN_FLIGHT];

    // GPU-owned G4 classification statistics, read back by the CPU.
    VkBuffer giProbeStatsSsbo;
    VkDeviceMemory giProbeStatsSsboMemory;
    void *giProbeStatsSsboMapped;

    // Per-slot snapshot of the above, filled by a copy at the end of the blend
    // pass.  The CPU reads THIS, not the live accumulator, which the previous
    // frame's blend may still be writing.
    VkBuffer giProbeStatsReadback[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory giProbeStatsReadbackMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *giProbeStatsReadbackMapped[VK_MAX_FRAMES_IN_FLIGHT];

    VkBuffer giProbeParamsUbo[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory giProbeParamsUboMemory[VK_MAX_FRAMES_IN_FLIGHT];
    void *giProbeParamsUboMapped[VK_MAX_FRAMES_IN_FLIGHT];

    VkDescriptorSetLayout giProbeDescLayout;
    VkDescriptorPool giProbeDescPool;
    VkDescriptorSet giProbeDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int giProbeDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // The GI SBT's only raygen record — gi_probe_trace.rgen.  Shares the pipeline,
    // and therefore the miss/hit regions, with gi_ray.rchit.
    VkStridedDeviceAddressRegionKHR giProbeRgenRegion;

    VkPipeline giProbeBlendPipeline;
    VkPipelineLayout giProbeBlendPipelineLayout;
    VkPipeline giProbeBorderPipeline;
    VkPipelineLayout giProbeBorderPipelineLayout;

    VkPipeline giProbeResolvePipeline;
    VkPipelineLayout giProbeResolvePipelineLayout;
    VkDescriptorSetLayout giProbeResolveDescLayout;
    VkDescriptorPool giProbeResolveDescPool;
    VkDescriptorSet giProbeResolveDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int giProbeResolveDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // --------------------------------------------------------------------------
    // HDR scene buffer and Uchimura tonemap pipeline (Phase 8.1)
    //
    // hdrScene: RGBA16F per-slot image used as the colour attachment for all
    //   scene and composite render passes instead of the swapchain.
    // tonemapResolve: RGBA8_UNORM per-slot storage target that tonemap.comp writes
    //   to; blitted to the BGRA8 swapchain image after dispatch.
    // --------------------------------------------------------------------------
    vkRTImage_t hdrScene[VK_MAX_FRAMES_IN_FLIGHT];       // RGBA16F HDR accumulator
    vkRTImage_t tonemapResolve[VK_MAX_FRAMES_IN_FLIGHT]; // RGBA8_UNORM resolve target

    VkPipeline tonemapPipeline;
    VkPipelineLayout tonemapPipelineLayout;
    VkDescriptorSetLayout tonemapDescLayout;
    VkDescriptorPool tonemapDescPool;
    VkDescriptorSet tonemapDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int tonemapDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // Resolution Upscaler bounce-buffer.
    // hdrScene (sub-rect) -> hdrUpscaled -> hdrScene (full)
    vkRTImage_t hdrUpscaled[VK_MAX_FRAMES_IN_FLIGHT];

    // U4 FSR 2 masks (docs/plans/completed/20260918_fsr_upscaling.md §9).  All display-sized with
    // only the render sub-rect valid, like every other U0 buffer.
    //   preAlphaColor: hdrScene snapshotted before the blend stages draw
    //   reactiveMask:  ffxFsr2ContextGenerateReactiveMask's output — auto-detected
    //                  alpha-blended pixels (particles, glass, fog)
    //   tcMask:        transparency-and-composition, extracted from gbufAlbedo's alpha by
    //                  fsr_tc_mask.comp — surfaces the prepass flagged as having motion
    //                  vectors FSR 2 should not trust (skinned meshes, the viewmodel)
    vkRTImage_t reactiveMask[VK_MAX_FRAMES_IN_FLIGHT];
    vkRTImage_t preAlphaColor[VK_MAX_FRAMES_IN_FLIGHT];
    vkRTImage_t tcMask[VK_MAX_FRAMES_IN_FLIGHT];

    VkPipeline upscalePipeline;
    VkPipelineLayout upscalePipelineLayout;
    VkDescriptorSetLayout upscaleDescLayout;
    VkDescriptorPool upscaleDescPool;
    VkDescriptorSet upscaleDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int upscaleDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];
    VkSampler upscaleSampler; // linear clamp-to-edge, samples hdrScene

    // --------------------------------------------------------------------------
    // G-buffer normal/F0 image, written by the depth prepass
    //  rgb = world-space bump-mapped normal (× 0.5 + 0.5),
    // a = normalized F0 (0 = "no data, use rt_ReconstructNormal").
    // --------------------------------------------------------------------------
    vkRTImage_t gbufNormal[VK_MAX_FRAMES_IN_FLIGHT];

    // Receiver-albedo G-buffer target (docs/plans/gi_albedo_target.md): diffuse map
    // sample written by the same prepass as gbufNormal (attachment 2). Cleared to
    // WHITE — an unwritten pixel modulates GI by 1.0, i.e. legacy behavior.
    // Same vk.gbufferSupported gating and lifetime as gbufNormal.
    vkRTImage_t gbufAlbedo[VK_MAX_FRAMES_IN_FLIGHT]; // R8G8B8A8_UNORM diffuse albedo

    // Motion vectors (U2, docs/plans/completed/20260918_fsr_upscaling.md §13): per pixel, the
    // screen-space offset from this frame's position to last frame's, in GL NDC units
    // (Y up), written as attachment 3 by the same prepass. Cleared to (0,0), so sky,
    // translucent surfaces and anything the prepass skips read as "not moving".
    // Same vk.gbufferSupported gating and lifetime as gbufNormal.
    vkRTImage_t motionVectors[VK_MAX_FRAMES_IN_FLIGHT]; // R16G16_SFLOAT

    // GI albedo modulate pass (gi_albedo_target.md): multiplies the resolved GI
    // radiance by gbufAlbedo into whichever of giScratchA/B is NOT the current
    // giReadView, then repoints giReadView at it. One descriptor set per frame
    // slot, rewritten unconditionally every dispatch, since this pass runs
    // exactly once per frame.
    VkPipeline giAlbedoModPipeline;
    VkPipelineLayout giAlbedoModPipelineLayout;
    VkDescriptorSetLayout giAlbedoModDescLayout;
    VkDescriptorPool giAlbedoModDescPool;
    VkDescriptorSet giAlbedoModDescSets[VK_MAX_FRAMES_IN_FLIGHT];

    // Recomputed each TLAS build in vk_accelstruct.cpp.
    bool sceneHasGlass;

    bool isInitialized;
};

extern vkRTState_t vkRT;

// ---------------------------------------------------------------------------
// Function declarations
// ---------------------------------------------------------------------------

// Load RT extension function pointers
void VK_RT_LoadFunctionPointers(void);

// Initialize the RT pipeline (called once after device creation when RT is available)
void VK_RT_Init(void);
void VK_RT_Shutdown(void);
void VK_RT_BeginLevelLoad(void); // clears BLAS cache at level transitions

// Initialize the shadow ray pipeline and shadow mask images (called after swapchain creation)
void VK_RT_InitShadows(void);

// Initialize the AO ray pipeline and AO mask images (called after swapchain creation)
void VK_RT_InitAO(void);

// Dispatch AO rays for the current view (once per frame, after TLAS rebuild, outside render pass).
// Depth must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL on entry.
// Transitions depth to READ_ONLY_OPTIMAL for the dispatch, then restores to ATTACHMENT_OPTIMAL.
void VK_RT_DispatchAO(VkCommandBuffer cmd, const viewDef_t *viewDef);

// AO image for fragment sampling this frame (denoised view or raw mask).  False, with the
// white fallback filled in, when AO was not written this frame.
bool VK_RT_GetAODescriptor(VkDescriptorImageInfo *out);

// Resize AO mask when resolution changes
void VK_RT_ResizeAOMask(uint32_t width, uint32_t height);

// ---------------------------------------------------------------------------
// Temporal EMA resolve (Step 5.2)
// ---------------------------------------------------------------------------

// Initialize temporal history images and compute pipeline.
// Called from VK_RT_InitAO after AO pipeline is ready.
void VK_RT_InitTemporal(void);

// Destroy all temporal resources.  Device must be idle before calling.
void VK_RT_ShutdownTemporal(void);

// Resize history images when the render resolution changes.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeTemporal(uint32_t width, uint32_t height);

// Dispatch the temporal EMA blend for AO.
// Called from VK_RT_DispatchAO after the AO ray dispatch ends.
// On entry aoMask[currentFrame] has been written and a memory barrier
// (RAY_TRACING → COMPUTE|FRAGMENT) has already been issued by the AO dispatch.
// On exit aoHistory contains the blended result and a
// (COMPUTE_WRITE → COMPUTE|FRAGMENT_READ) barrier is issued so either Atrous
// or the interaction pass can consume it.
void VK_RT_DispatchTemporalResolveAO(VkCommandBuffer cmd, const viewDef_t *viewDef);

// ---------------------------------------------------------------------------
// Atrous spatial filter (Step 5.2b)
// ---------------------------------------------------------------------------

// Initialize Atrous scratch images and filter pipeline.
// Called internally from VK_RT_InitTemporal.
void VK_RT_InitAtrous(void);

// Destroy all Atrous resources.  Device must be idle before calling.
void VK_RT_ShutdownAtrous(void);

// Resize Atrous scratch images when render resolution changes.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeAtrous(uint32_t width, uint32_t height);

// Run the edge-stopped À-trous spatial filter on the EMA history image.
// Called from VK_RT_DispatchAO after VK_RT_DispatchTemporalResolveAO.
// On exit aoReadView[currentFrame] points to the final denoised image and
// a (COMPUTE_WRITE → FRAGMENT_READ) barrier has been issued.
// If r_rtAtrousIterations == 0, returns early after setting aoReadView to aoHistory.
void VK_RT_DispatchAtrousAO(VkCommandBuffer cmd);

// ---------------------------------------------------------------------------
// RT Reflections (Step 5.3) — cheap environment approximation
// ---------------------------------------------------------------------------

// Create the RGBA16F reflection buffer, RT pipeline, and SBT.
// Called once from VK_InitVulkan after VK_RT_InitAO.
void VK_RT_InitReflections(void);

// Destroy all reflection resources.  Device must be idle before calling.
void VK_RT_ShutdownReflections(void);

// Resize reflection buffer when render resolution changes.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeReflections(uint32_t width, uint32_t height);

// Dispatch reflection rays for the current view (once per frame).
// Must be outside a render pass.  Depth must be in ATTACHMENT_OPTIMAL on entry;
// this function transitions to READ_ONLY_OPTIMAL and back.
// Output: the chain's trace[currentFrame] is ready for FRAGMENT sampling on return.
void VK_RT_DispatchReflections(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Temporal EMA denoise of the traced image (Stage 1, 20261013_reflection_roughness.md).
// Must be called after VK_RT_DispatchReflections and before any consumer of the
// reflection buffer (composite, interaction, glass overlay). No-op when
// r_rtReflDenoise is 0 — VK_RT_GetReflReadView then just returns the traced image,
// so r_rtReflDenoise 0 is bit-identical to the pre-Stage-1 path.
void VK_RT_DispatchReflTemporal(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Roughness-gated à-trous spatial filter (Stage 1 addendum, r_rtReflDenoise 2).
// Must be called after VK_RT_DispatchReflTemporal and before any consumer of the
// reflection buffer. No-op below r_rtReflDenoise 2 — VK_RT_GetReflReadView then
// falls back to the temporal history (1) or the traced image (0).
void VK_RT_DispatchReflAtrous(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Coerces r_rtReflectionDebugMode to a mode that still exists, 0 otherwise. Every
// consumer must go through this: a nonzero return forces the full-screen launch grid,
// the replace-blend composite and suppression of the per-surface glass overlay, so a
// retired number reading as "active" would route an ordinary image down that path.
// Live: 2/3 G-buffer normal + F0, 11 denoiser history (VK_RT_DispatchReflTemporal),
// 12 roughness, 13 GI handoff, 15 à-trous edge stops (refl_atrous.comp).
// Numbers are kept stable across retirements so plan docs keep resolving.
int VK_RT_ValidReflDebugMode(void);

// Additively composite reflBuffer onto the framebuffer (Step 8, see
// docs/plans/gbuffer_normal_pass.md). Must be called INSIDE the main render
// pass, after VK_RT_CompositeGI. No-op if vk.gbufferSupported is false.
// Selects a blend-disabled variant while VK_RT_ValidReflDebugMode is nonzero, so the
// debug visualization replaces rather than adds onto the lit scene.
void VK_RT_CompositeReflections(VkCommandBuffer cmd);

// View to sample for any downstream reflection consumer (composite, interaction
// binding 9, glass overlay): the à-trous output when r_rtReflDenoise is 2 and it
// ran this frame, else the temporal history when the denoiser is active and valid
// for this slot, else the traced image directly.
VkImageView VK_RT_GetReflReadView(int frameIdx);

// Same, for glass_refl_overlay.frag's per-pane draw (Stage 4b). Returns the glass
// chain when the populations were split this frame, else the opaque chain — where the
// pane's radiance sits behind the negative-alpha sentinel, as before the split.
VkImageView VK_RT_GetReflGlassReadView(int frameIdx);

// Whether VK_RT_GetReflReadView has a real image to hand out for this slot. Callers
// binding the reflection map must check this rather than reaching into the chain's
// images, which Stage 4b will split by population.
bool VK_RT_ReflReadReady(int frameIdx);

void VK_RT_DestroyBLAS(vkBLAS_t *blas);

// Build a multi-geometry BLAS covering all non-translucent surfaces of a model.
// Produces one TLAS instance per entity so shadow rays intersect every surface,
// not just Surface(0).  Used by VK_RT_RebuildTLAS.
// prevBlas: if non-NULL and geometry is compatible, performs an in-place update
// (reuses geometry buffers and uses MODE_UPDATE) instead of a full rebuild.
// Returns prevBlas on successful update, or a new vkBLAS_t on full rebuild.
// allowUpdate=false (static models) builds without ALLOW_UPDATE and frees scratch after the build.
vkBLAS_t *VK_RT_BuildBLASForModel(idRenderModel *model, VkCommandBuffer cmd, vkBLAS_t *prevBlas = NULL,
                                  bool allowUpdate = true);

// Drain deferred BLAS deletions (call after fence wait when frame slot is safe)
void VK_RT_DrainBLASGarbage(void);

// Rebuild TLAS from all visible entities this frame
void VK_RT_RebuildTLAS(VkCommandBuffer cmd, const viewDef_t *viewDef);

// r_vkRTProfile: BLAS work for the current tr.frameCount, valid after VK_RT_RebuildTLAS.
struct vkRTBlasFrameStats_t
{
    int updates;          // in-place MODE_UPDATE refits
    int rebuilds;         // full builds (new buffers + AS)
    int tris;             // triangles across both
    int dynamicInstances; // TLAS instances with a per-frame BLAS
    double cpuMs;         // CPU time inside VK_RT_BuildBLASForModel
};
void VK_RT_GetBLASFrameStats(vkRTBlasFrameStats_t *out);

// r_vkRTProfile GPU span over this frame's BLAS builds (vk_backend.cpp).  Begin is
// idempotent within a frame; the backend closes a span left open by an early return.
void VK_RTProfile_BLASGpuBegin(VkCommandBuffer cmd);
void VK_RTProfile_BLASGpuEnd(VkCommandBuffer cmd);

// True when the TLAS will be rebuilt and consumed this frame (RT supported,
// initialized, enabled, and at least one effect that needs it turned on).
// Callable from the frontend; gates work that only RT geometry requires.
bool VK_RT_TLASActive(void);

// Resize shadow mask when resolution changes
void VK_RT_ResizeShadowMask(uint32_t width, uint32_t height);

// ---------------------------------------------------------------------------
// Shared light classifier (auto_relight.md §0)
//
// Single real/fake judgment for a light, used by GI/volumetric admission
// (vk_gi.cpp) and, later, the noShadows shadow unlock (§6).  Never test
// parms.noShadows or lightShader->LightCastsShadows() directly in a new
// consumer — call VK_RT_ClassifyLight so all passes agree.
// ---------------------------------------------------------------------------
enum vkRTLightClass_t
{
    RT_LIGHT_REAL = 0,     // shadow-casting map light — full GI/vol/shadow eligibility
    RT_LIGHT_ACCENT,       // noShadows, radius < r_rtLightAccentMaxRadius — placed colored
                           // accent; admitted to GI/vol, eligible for the noShadows unlock
    RT_LIGHT_AMBIENT_FILL, // ambientLight material, or noShadows at/above the accent
                           // radius threshold — semantic mapper fill; never admitted
    RT_LIGHT_FOG_BLEND,    // fogLight or blendLight material — no RT falloff/occlusion volume
    RT_LIGHT_SPECTRUM      // material "spectrum N" (N != 0) — only lights matching-spectrum
                           // surfaces (teleport pentagram reveal); never admitted
};

vkRTLightClass_t VK_RT_ClassifyLight(const renderLight_t &parms, const idMaterial *lightShader);
const char *VK_RT_LightClassName(vkRTLightClass_t cls);

// ---------------------------------------------------------------------------
// Shared camera-cut detection (rt_temporal_cut_detection.md)
//
// Used by the AO/GI/volumetric temporal EMA passes (vk_temporal.cpp, vk_vol.cpp)
// to decide whether to blend with history or replace it outright. Tests the
// camera's actual position/orientation delta rather than a raw inverse-view-
// projection matrix diff — that matrix is ill-conditioned under Doom 3's
// infinite-far-Z projection (amd_vulkan_cleanup.md A12) and was firing on
// ordinary mouselook (confirmed in-game: maxDiff ~472 vs. a 0.5 threshold,
// even standing still).
//
// prevPos/prevFwd are updated in place to the current frame's values on every
// call, regardless of historyValid — callers own the per-slot storage
// (vkRT.{ao,gi,vol}PrevCamPos/Fwd) and pass historyValid from their own
// per-slot flag (vkRT.{ao,gi,vol}HistoryValid).
// ---------------------------------------------------------------------------
struct vkRTCameraCutResult_t
{
    bool isCut;
    float posDelta;   // world units moved since the last call for this slot
    float angleDelta; // degrees rotated since the last call for this slot
};

vkRTCameraCutResult_t VK_RT_DetectCameraCut(const viewDef_t *viewDef, idVec3 &prevPos, idVec3 &prevFwd,
                                            bool historyValid, const char *tag);

// ---------------------------------------------------------------------------
// Effective near plane for THIS view, read off the projection instead of the
// r_znear cvar.
//
// Game code writes r_znear directly (3.0 normally, 1.0 for cinematics —
// Game_local.cpp:4532), so a backend GetFloat() can return a value the frontend
// never rasterised this frame's depth buffer with.  Everything keyed on znear —
// the A12 depth-reconstruction bias floor (d^2*ulp/znear), the AO fade band,
// FSR 2's cameraNear — is then wrong for exactly the frames where znear moved,
// which is the cinematic transition.  The matrix is authoritative.
//
//   ndcZ = -proj[10] + proj[14]/d  =>  d(ndcZ = -1) = proj[14] / (proj[10] - 1)
//
// Jitter does not affect this: R_BuildProjection only shifts the frustum's x/y
// edges, leaving rows 2 and 3 alone.  vk_vol_froxel.cpp has used this since the
// froxel work; this is the same expression, shared.
// ---------------------------------------------------------------------------
inline float VK_RT_EffectiveZNear(const viewDef_t *viewDef)
{
    const float d = viewDef->projectionMatrix[14] / (viewDef->projectionMatrix[10] - 1.0f);
    return (d > 0.01f && d == d) ? d : 3.0f; // degenerate projection: Doom 3's default
}

// ---------------------------------------------------------------------------
// P1b — batched shadow masks
//
// Per-view flow (all from vk_backend.cpp):
//   1. VK_RT_ShadowBatchBegin()                     — reset the layer table
//   2. VK_RT_ShadowBatchAddLight(vLight, rect) × L  — assign one layer per light,
//      in the same order and with the same filters as the interaction loop
//   3. VK_RT_ShadowBatchClearLayers(cmd)            — clear only the layers in use
//   4. VK_RT_DispatchShadowBatch(cmd, viewDef)      — trace + blur batch 0 with a
//      single depth round-trip and no render-pass breaks
//   5. interaction loop: VK_RT_GetShadowLayerForLight(vLight) picks the layer.
//      S3: when VK_RT_GetShadowBatchForLight rises, end the pass and call
//      VK_RT_DispatchShadowBatchInLoop for that batch (one break per batch).
// ---------------------------------------------------------------------------

// Reset the per-view layer assignment table.
void VK_RT_ShadowBatchBegin(void);

// Assign a shadow mask layer to one light.  Returns the layer index, or -1 when
// the light casts no shadows (noShadows / material "noShadows") — those lights
// sample no mask at all instead of a cleared-to-white one.
int VK_RT_ShadowBatchAddLight(const viewLight_t *vLight, VkRect2D dispatchRect);

// Shadow-casting lights this view, how many are in batch 0, and how many are in later
// batches (reported as lights.serial).  In-loop dispatch time is timed by the nested
// Interactions.Shadows span, so a capture with serial=0 says nothing about its cost.
void VK_RT_GetShadowBatchStats(int *outCasters, int *outBatched, int *outSerial);

// Layer assigned by VK_RT_ShadowBatchAddLight, or -1 if the light casts no shadows
// or was never added this view.
int VK_RT_GetShadowLayerForLight(const viewLight_t *vLight);

// Clear the layers this view actually uses to 1.0 (fully lit).  Must be called
// outside a render pass, before VK_RT_DispatchShadowBatch.
void VK_RT_ShadowBatchClearLayers(VkCommandBuffer cmd);

// Trace + blur batch 0.  Must be called outside a render pass, after the TLAS
// rebuild.  Does nothing when no light casts shadows.
void VK_RT_DispatchShadowBatch(VkCommandBuffer cmd, const viewDef_t *viewDef);

// S3: batch index that traces this light, or -1 (no shadows).
int VK_RT_GetShadowBatchForLight(const viewLight_t *vLight);

// S3: batches this view (0 when nothing is batched).
int VK_RT_ShadowBatchCount(void);

// S3: clear, trace and blur batch `batch` (>= 1) from inside the interaction loop.
// Must be called with the render pass ended; depth and gbufAlbedo are attachment
// layouts on entry and restored on return.
void VK_RT_DispatchShadowBatchInLoop(VkCommandBuffer cmd, const viewDef_t *viewDef, int batch);

// ---------------------------------------------------------------------------
// Material table (Phase 5.4) — shared infrastructure for reflections, GI, shadow any-hit
// ---------------------------------------------------------------------------

// Create persistently-mapped SSBOs (MatTable, VtxAddrTable, IdxAddrTable) and the
// bindless sampler2D descriptor set.  Must be called once before creating any RT
// pipelines whose pipeline layouts include matDescLayout (for example reflections/shadows).
void VK_RT_InitMaterialTable(void);

// Destroy all material table GPU resources.  Device must be idle before calling.
void VK_RT_ShutdownMaterialTable(void);

// Release every bindless texture slot except the two fallbacks.  Called from
// VK_RT_BeginLevelLoad so slot assignments don't accumulate across map loads
// (VK_MAT_MAX_TEXTURES is exhausted after a few maps otherwise).
void VK_RT_MatTableLevelLoadReset(void);

// Returns true if `shader` has a resolvable emissive image — an SL_AMBIENT
// stage that is a strict additive overlay with explicit UVs, a cinematic/
// videomap stage, or (fallback) the diffuse image of a HasGui() material.
// Shared between VK_RT_MakeMaterialEntry (bindless emissive slot) and the
// auto-relight surface harvest (docs/plans/auto_relight.md AR1,
// vk_auto_relight.cpp) so the two "is this surface emissive" judgments can't
// drift apart. outEmissiveImage/outGuiEmissive/outIsCinematic may be NULL.
// outIsCinematic is auto_relight.md AR3's "videomap" signal (styleWeight x1.5,
// same bucket as outGuiEmissive — both are "screens are the money shot").
bool VK_RT_MaterialIsEmissive(const idMaterial *shader, idImage **outEmissiveImage, bool *outGuiEmissive,
                              bool *outIsCinematic = NULL);

// Public wrapper around the material table's bindless-slot assignment
// (vk_material_table.cpp's file-local GetOrAssignTexIndex), for callers
// outside that file that need to register an image into the same bindless
// array RT hit shaders sample by index — e.g. rt_projected_light_cookies.md's
// light-cookie image (vk_gi.cpp). img == NULL returns slot 0 (white fallback).
uint32_t VK_RT_GetOrAssignTexIndex(idImage *img);

// Build a VkMaterialEntry for one TLAS instance.
// shader:      the representative idMaterial for this instance (may be NULL — returns defaults).
// blas:        the BLAS built for this entity; all per-geometry addresses are written.
// baseGeomIdx: starting offset in the flat geomVtxAddrs/geomIdxAddrs arrays.
// outGeomVtxAddrs / outGeomIdxAddrs: flat arrays of VK_MAT_MAX_GEOMS entries; this call
//     writes blas->geomCount addresses starting at baseGeomIdx.
// NOTE: internally assigns bindless texture slots; call once per instance per frame.
VkMaterialEntry VK_RT_MakeMaterialEntry(const idMaterial *shader, const vkBLAS_t *blas, uint32_t baseGeomIdx,
                                        uint64_t *outGeomVtxAddrs, uint64_t *outGeomIdxAddrs);

// ---------------------------------------------------------------------------
// Global Illumination (Phase 6.1)
// ---------------------------------------------------------------------------

// Create the RGBA16F GI buffer, RT pipeline, and SBT.
// Called once from VK_InitVulkan after VK_RT_InitReflections.
void VK_RT_InitGI(void);

// Destroy all GI resources.  Device must be idle before calling.
void VK_RT_ShutdownGI(void);

// Resize GI buffer when render resolution changes.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeGI(uint32_t width, uint32_t height);

// Populate the per-frame GI light SSBO with all in-range world lights.
// Must be called once per frame BEFORE any RT dispatch that samples the light buffer
// (reflections, GI).  Safe to call multiple times per frame — the buffer is reset
// and refilled on each call.  Independent of r_rtGILightBounce so that reflection
// hit shaders always receive lighting data.
void VK_RT_UploadGILights(const viewDef_t *viewDef);

// G5b (20260906_froxel_probe_gi.md) — flicker factorization, owned by vk_gi.cpp's
// classifier and consumed by vk_gi_probe.cpp when it builds GIProbeParams.
//
// A fast light is uploaded at its PEAK colour Lhat with s = current/peak alongside,
// so the probe fast bucket caches its transport time-invariantly and the resolve
// multiplies the gain back in. Both are refreshed inside VK_RT_UploadGILights,
// which vk_backend.cpp calls before any probe dispatch — reading them earlier in
// the frame would hand the resolve the previous frame's flicker phase.
//
// Count is the clamped r_rtGIProbeFastBuckets (K). Gain is the Lhat-luminance-
// weighted mean of s over the bucket's uploaded lights, and is 0 — not 1 — when
// the bucket holds no light, so its decaying atlas history cannot republish.
int VK_RT_GIFastBucketCount(void);
float VK_RT_GIFastBucketGain(int bucket);
int VK_RT_GIFastLightCount(void);

// Dispatch GI rays for the current view (once per frame).
// Must be outside a render pass.  Depth must be in ATTACHMENT_OPTIMAL on entry;
// this function transitions to READ_ONLY_OPTIMAL and back.
// Output: giBuffer[currentFrame] is ready for FRAGMENT sampling when this returns.
void VK_RT_DispatchGI(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Composite the GI buffer onto the current framebuffer with additive blending.
// Must be called INSIDE the main render pass, before the per-light interaction draws.
// Reads from giReadView[currentFrame] — wherever the GI chain last left it.
// Does nothing when r_rtGI is off or the composite pipeline is not ready.
// GI is multiplied by AO here (r_rtAOIndirectStrength).
void VK_RT_CompositeGI(VkCommandBuffer cmd);

// r_rtAODebug view (replace blend).  Call inside the render pass after the shader
// passes, same frame as VK_RT_CompositeGI; no-op when that didn't run.
void VK_RT_CompositeGIDebug(VkCommandBuffer cmd);

// ---------------------------------------------------------------------------
// GI receiver-albedo modulation (docs/plans/gi_albedo_target.md)
// ---------------------------------------------------------------------------

// Create the gi_albedo_mod.comp compute pipeline and the giScratchA/B pair.
void VK_RT_InitGIAlbedoMod(void);

// Reallocate giScratchA/B at a new render resolution.
void VK_RT_ResizeGIAlbedoMod(uint32_t width, uint32_t height);

// Destroy the albedo-mod pipeline and scratch images.  Device must be idle.
void VK_RT_ShutdownGIAlbedoMod(void);

// Multiply the resolved GI (giReadView) by gbufAlbedo into whichever of
// giScratchA/B giReadView is NOT currently pointing at, then repoint giReadView
// at the result.  Must be called outside a render pass, after
// VK_RT_DispatchGIProbeResolve, while gbufAlbedo is in SHADER_READ_ONLY_OPTIMAL
// (the backend's Stage-3.5 barrier window).  Last in the GI chain: probe
// irradiance is albedo-free, so the resolve's output wants modulating.
// No-op when r_rtGIAlbedo is 0, a probe OVERLAY owns giBuffer (it is a
// measurement, not radiance), or the G-buffer/scratch images don't exist.
void VK_RT_DispatchGIAlbedoMod(VkCommandBuffer cmd, const viewDef_t *viewDef);

// ---------------------------------------------------------------------------
// Material table (Phase 5.4) — shared infrastructure for reflections, GI, shadow any-hit
// ---------------------------------------------------------------------------

// Upload the frame's material entries and address tables to the GPU SSBOs.
// Mirrors the static/dynamic split of VK_RT_RebuildTLAS.
// When rewriteStatic == false the static portion of the SSBO is left unchanged.
// staticGeomVtx/Idx: addresses for static geometries (local offsets [0..staticGeomCount-1]).
// dynGeomVtx/Idx:    addresses for dynamic geometries (local offsets [0..dynamicGeomCount-1]).
// GPU-side flat SSBO layout: static block at [0..sGC-1], dynamic block at [sGC..sGC+dGC-1].
// Also rebuilds the bindless texture descriptors if new images were encountered.
// Called from vk_accelstruct.cpp at the end of VK_RT_RebuildTLAS.
// staticMatCount/dynamicMatCount are the number of VkMaterialEntry entries in
// staticEntries/dynamicEntries (== staticGeomCount/dynamicGeomCount after the
// per-geometry material fix).
void VK_RT_UploadMatTableFrame(const VkMaterialEntry *staticEntries, uint32_t staticMatCount, bool rewriteStatic,
                               const VkMaterialEntry *dynamicEntries, uint32_t dynamicMatCount,
                               const uint64_t *staticGeomVtx, const uint64_t *staticGeomIdx, uint32_t staticGeomCount,
                               const uint64_t *dynGeomVtx, const uint64_t *dynGeomIdx, uint32_t dynamicGeomCount,
                               bool rewriteStaticGeoms);

// rt_projected_light_cookies.md: flush a pending bindless-descriptor rebuild
// (no-op if nothing is dirty). Callers that register a texture via
// VK_RT_GetOrAssignTexIndex AFTER this frame's VK_RT_UploadMatTableFrame call
// (vk_gi.cpp's light-cookie collection, which runs after VK_RT_RebuildTLAS)
// must call this afterward or the new slot stays unwritten until next frame.
void VK_RT_FlushBindlessTextures(void);

// Running bindless totals for r_vkLogTLASSlow (read as before/after deltas).
struct vkRTBindlessFrameStats_t
{
    uint32_t slotCount;    // slots assigned now (not cumulative)
    uint32_t slotsWritten; // descriptors written by rebuilds
    uint32_t bgLoads;      // slots written while their image had no Vulkan data yet
    double rebuildMs;      // CPU time inside RebuildBindlessDescriptors
};
void VK_RT_GetBindlessFrameStats(vkRTBindlessFrameStats_t *out);

// ---------------------------------------------------------------------------
// Volumetric Lighting (Phase 7.2)
// ---------------------------------------------------------------------------

// Create the RGBA16F vol buffer, the composite pipeline and the froxel grid.
// Called once from VK_InitVulkan after VK_RT_InitGI.
void VK_RT_InitVolumetrics(void);

// Destroy all volumetric resources.  Device must be idle before calling.
void VK_RT_ShutdownVolumetrics(void);

// Resize vol buffer when render resolution changes.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeVolumetrics(uint32_t width, uint32_t height);

// Composite the volumetric buffer onto the framebuffer with additive blending.
// Must be called INSIDE the main render pass, after VK_RT_CompositeGI.
// Does nothing when r_rtVol is off, the pipeline is not ready, or the froxel
// resolve stood down (nothing else writes volBuffer).
// Self-guards against compositing the same view twice in a frame (F6 gave it
// two possible call sites).
void VK_RT_CompositeVolumetrics(VkCommandBuffer cmd);

// F6 (20260906_froxel_probe_gi.md): true when r_rtVolAttenuateBackground is on
// and the attenuating pipeline exists.  The backend uses this to move the
// composite after the interaction/shader passes, so surface radiance — not just
// GI and ambient — is what gets multiplied by the path transmittance.  The late
// call site must additionally gate on a real camera; see the comment there.
bool VK_RT_VolCompositeAfterSurfaces(void);

// Medium coefficients (20260917_vol_transport_coefficients.md).  The froxel fill
// and the reflection in-scatter march read these rather than the cvars, so the
// clamping and the sigma_s = albedo * sigma_t fold live in one place.
enum
{
    VK_VOL_CLASS_POINT = 0,
    VK_VOL_CLASS_DIRECTED = 1,
    VK_VOL_CLASS_FLASHLIGHT = 2,
};

float VK_RT_VolExtinction(void);             // sigma_t, per world unit
float VK_RT_VolAlbedo(void);                 // sigma_s / sigma_t, [0,1]
float VK_RT_VolScatterScale(int lightClass); // sigma_s * per-class radiance gain

// ---------------------------------------------------------------------------
// Volumetric froxel grid (20260906_froxel_probe_gi.md Part A)
// ---------------------------------------------------------------------------

// Allocate the froxel grid images and create the fill pipeline.
// Called from VK_RT_InitVolumetrics, after the material table exists.
void VK_RT_InitVolFroxel(void);

// Destroy all froxel resources.  Device must be idle before calling.
void VK_RT_ShutdownVolFroxel(void);

// True when volumetrics are on and the froxel pipelines and grid images are all
// usable.  Every vol pass, the composite included, stands down when this is false.
bool VK_RT_VolFroxelActive(void);

// Fill the froxel grid: one invocation per cell.  Must be called outside a render
// pass, after the TLAS build.  Does not touch depth.
void VK_RT_DispatchVolFroxelFill(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Accumulate the filled grid front-to-back, one thread per grid column.
// Must be called outside a render pass, after VK_RT_DispatchVolFroxelFill.
void VK_RT_DispatchVolFroxelIntegrate(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Grid -> vkRT.volBuffer, and repoints volReadView at volBuffer so the composite
// reads it.  One trilinear fetch per pixel; also hosts the r_rtVolFroxelDebug
// overlays.
// Must be called outside a render pass; depth must be in ATTACHMENT_OPTIMAL.
void VK_RT_DispatchVolFroxelResolve(VkCommandBuffer cmd, const viewDef_t *viewDef);

// ---------------------------------------------------------------------------
// Sampling the scatter grid from outside the froxel passes
// (docs/plans/20261013_reflection_volumetrics.md).
//
// froxelScatter holds per-cell in-scatter and extinction with no camera-path
// integration baked in, so any ray can march it — which is how the reflection
// pass reaches the volumetrics. froxelIntegrated is the camera-prefix integral
// and is NOT reusable this way.
// ---------------------------------------------------------------------------

// Mirrors VolFroxelSlices in vol_froxel_sample.glsl, plus the projection the
// world -> grid direction needs.  viewProj is the GL-convention matrix the
// grid's own invViewProj inverts, built identically so the two cannot drift.
struct vkFroxelSampleParams_t
{
    float viewProj[16];
    float camPos[3];
    float camFwd[3];
    float dNear;
    float logRange;
    float invLogRange;
    int nz;
};

// This frame's grid description. False when the froxel path is standing down,
// in which case the caller should bind VK_RT_GetFroxelScatterView()'s null grid
// and take no samples.
bool VK_RT_GetFroxelSampleParams(const viewDef_t *viewDef, vkFroxelSampleParams_t *out);

// The live scatter grid, or a 1x1x1 black stand-in when there is none. Always a
// valid 3D view in VK_IMAGE_LAYOUT_GENERAL — the grids never leave that layout.
VkImageView VK_RT_GetFroxelScatterView(void);

// Stage 5's companion mean-direction grid (rgba8_snorm), same contract. Must be
// bound alongside the scatter view: that grid is unphased, so a consumer reading
// one without the other is reading an incomplete field. The null stand-in is
// zeroed, which rt_PhaseFromMeanDir reads as isotropic.
VkImageView VK_RT_GetFroxelDirView(void);

// Trilinear + clamp, shared with the resolve pass.
VkSampler VK_RT_GetFroxelSampler(void);

int VK_RT_VolFroxelDebugMode(void);

// ---------------------------------------------------------------------------
// GI irradiance probes (20260906_froxel_probe_gi.md Part B)
// ---------------------------------------------------------------------------

// Create giProbeDescLayout and its descriptor sets ONLY.  Must run before
// VK_RT_InitGIPipeline, which bakes that layout into the GI pipeline layout as
// set 2 so gi_probe_trace.rgen can share the pipeline with gi_ray.rgen.
// Idempotent.
void VK_RT_InitGIProbeLayout(void);

// Allocate the probe atlases/scratch/state and create the blend, border and
// resolve pipelines.  Called from VK_RT_InitGI, after the GI RT pipeline exists.
void VK_RT_InitGIProbe(void);

// Destroy all probe resources.  Device must be idle before calling.
void VK_RT_ShutdownGIProbe(void);

// giProbeDescLayout / this frame's probe descriptor set — vk_gi.cpp needs both
// to build the GI pipeline layout and to keep set 2 bound for every launch of
// that pipeline, including the per-pixel one.
VkDescriptorSetLayout VK_RT_GIProbeDescLayout(void);
VkDescriptorSet VK_RT_GIProbeDescSet(int frameIdx);

bool VK_RT_GIProbeActive(void);

// r_rtGIProbeDebug, but 0 unless the probe resolve pipeline and atlases exist.
// Non-zero means an OVERLAY owns giBuffer, which is why albedo mod skips.
int VK_RT_GIProbeDebugMode(void);

// Fire r_rtGIProbeRays rays for each of r_rtGIProbeUpdatesPerFrame probes into
// the scratch image, using the second raygen group of the GI pipeline.  Must be
// called outside a render pass, AFTER VK_RT_DispatchGI — it reuses the GIParams
// dynamic-UBO binding that function establishes for gi_ray.rchit.
void VK_RT_DispatchGIProbeTrace(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Fold the scratch rays into the octahedral atlases (EMA) and fill their
// borders.  Must follow VK_RT_DispatchGIProbeTrace in the same frame.
void VK_RT_DispatchGIProbeBlend(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Probe atlases -> vkRT.giBuffer, and repoints giReadView at it.  Mode 0 is the
// shipping resolve; any other r_rtGIProbeDebug value is an overlay.  Runs BEFORE
// VK_RT_DispatchGIAlbedoMod, which is what modulates mode 0's output.
// Must be called outside a render pass; depth must be in ATTACHMENT_OPTIMAL.
void VK_RT_DispatchGIProbeResolve(VkCommandBuffer cmd, const viewDef_t *viewDef);

// ---------------------------------------------------------------------------
// Tonemapping (Phase 8.1)
// ---------------------------------------------------------------------------

// Allocate HDR scene images and create the tonemap compute pipeline.
// Called once after device creation (inside the VK_RT_Init block).
void VK_RT_InitTonemap(void);

// Destroy all tonemap resources.  Device must be idle before calling.
void VK_RT_ShutdownTonemap(void);

// Reallocate HDR scene images when render resolution changes.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeTonemap(uint32_t width, uint32_t height);

// Dispatch the Uchimura tonemap compute pass: hdrScene → swapchain.
// Must be called outside a render pass, after all composites have finished.
void VK_RT_DispatchTonemap(VkCommandBuffer cmd);

// ---------------------------------------------------------------------------
// G-buffer normal/F0 pass (see docs/plans/gbuffer_normal_pass.md)
// ---------------------------------------------------------------------------

// Destroy all G-buffer resources. Safe to call even if never initialized.
// Called from VK_RT_ShutdownTonemap.
void VK_RT_ShutdownGBuffer(void);

// (Re)allocate the G-buffer images. No-op if vk.gbufferSupported is false.
// Called from VK_RT_ResizeTonemap (including its first call from VK_RT_InitTonemap).
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeGBuffer(uint32_t width, uint32_t height);

// ---------------------------------------------------------------------------
// Auto-relight (docs/plans/auto_relight.md AR1-5)
//
// Load-time CPU pass: harvests emissive world surfaces (AR1), clusters them
// into physical fixtures (AR2), scores/budgets them (AR3), dedupes against
// existing map lights (AR4), and synthesizes one real idRenderLight per
// surviving cluster (AR5) via world->AddLightDef(). AR3-5 land after AR1/AR2
// are validated in-game; until then this only harvests, clusters, and logs.
//
// Called exactly once per world lifetime from the end of
// idRenderWorldLocal::GenerateAllInteractions (RenderWorld.cpp) — that point
// is after every real map lightDef exists (game entities have been spawned),
// which AR4's dedupe needs; calling any earlier (e.g. end of InitFromMap)
// would dedupe against nothing. Re-armed by FreeDefs()/the constructor via
// world->autoRelightGenerated. No-op unless r_rtAutoRelight or
// r_rtAutoRelightDebug is nonzero.
// ---------------------------------------------------------------------------
void VK_AutoRelight_Generate(class idRenderWorldLocal *world);

#endif // __VK_RAYTRACING_H__
