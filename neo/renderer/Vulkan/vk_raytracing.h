/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan ray tracing - types and declarations.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#ifndef __VK_RAYTRACING_H__
#define __VK_RAYTRACING_H__

#include "renderer/Vulkan/vk_common.h"

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
// BLAS (Bottom-Level Acceleration Structure) - one per unique mesh
// ---------------------------------------------------------------------------

struct vkBLAS_t
{
    VkAccelerationStructureKHR handle;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress deviceAddress;
    // Per-surface geometry buffers for the AS build input.
    // Arrays of length geomCount — kept alive until VK_RT_DestroyBLAS because they
    // must outlive the command buffer submission in which the BLAS build was recorded.
    uint32_t geomCount;
    VkBuffer *geomVertBufs;
    VkDeviceMemory *geomVertMems;
    VkBuffer *geomIdxBufs;
    VkDeviceMemory *geomIdxMems;
    // Per-surface geometry sizes — used to validate reuse for BLAS update.
    // NULL for GPU-cache-backed geometry (zero-copy path, no owned buffers).
    VkDeviceSize *geomVertSizes;
    VkDeviceSize *geomIdxSizes;
    // Per-surface primitive counts — must match exactly for MODE_UPDATE to be valid.
    uint32_t *geomPrimCounts;
    // Scratch buffer used during build (freed at destroy time).
    VkBuffer scratchBuf;
    VkDeviceMemory scratchMem;
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

// Shadow mask buffer (R8 UNORM, one pixel per screen pixel)
struct vkShadowMask_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    // Temp image for blur ping-pong (same format/size as shadow mask)
    VkImage blurTempImage;
    VkDeviceMemory blurTempMemory;
    VkImageView blurTempView;
    uint32_t width;
    uint32_t height;
};

// AO mask buffer (R8 UNORM, one pixel per screen pixel)
struct vkAOMask_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width;
    uint32_t height;
};

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
    vkAOMask_t aoMask[VK_MAX_FRAMES_IN_FLIGHT];
    VkSampler  aoMaskSampler; // nearest-clamp, used when sampling AO in the lighting pass

    VkPipeline           aoPipeline;
    VkPipelineLayout     aoPipelineLayout;
    VkDescriptorSetLayout aoDescLayout;
    VkDescriptorPool     aoDescPool;
    VkDescriptorSet      aoDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int                  aoDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

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

    // --------------------------------------------------------------------------
    // Temporal EMA resolve (Step 5.2)
    //
    // One history image per frame-in-flight slot — safe because the per-slot fence
    // guarantees the previous use of the same slot is complete before we start the
    // next frame using that slot.  Never share history images between slots.
    // --------------------------------------------------------------------------
    vkAOMask_t aoHistory[VK_MAX_FRAMES_IN_FLIGHT]; // accumulated EMA history, R8_UNORM

    // Temporal compute pipeline (temporal_resolve.comp)
    VkPipeline           temporalPipeline;
    VkPipelineLayout     temporalPipelineLayout;
    VkDescriptorSetLayout temporalDescLayout;
    VkDescriptorPool     temporalDescPool;
    // One descriptor set per frame slot: binding 0=aoMask (readonly), binding 1=aoHistory (rw)
    VkDescriptorSet      temporalDescSets[VK_MAX_FRAMES_IN_FLIGHT];
    int                  temporalDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // Per-slot history validity and camera-state cache for cut detection.
    // Reset to false when the slot's images are recreated or the pipeline is (re)initialised.
    bool  aoHistoryValid[VK_MAX_FRAMES_IN_FLIGHT];
    float aoPrevInvViewProj[VK_MAX_FRAMES_IN_FLIGHT][16]; // column-major, GL convention

    // --------------------------------------------------------------------------
    // Atrous spatial filter (Step 5.2b)
    //
    // Ping-pong between aoHistory and aoScratch.  Descriptor set index alternates
    // per pass: [frameIdx][0] reads history → writes scratch (odd pass outputs);
    //           [frameIdx][1] reads scratch → writes history (even pass outputs).
    // aoReadView[i] is updated after each Atrous dispatch to point at whichever
    // image holds the final result.  The backend always samples aoReadView[i].
    //
    // Requires r_rtTemporal 1 (EMA must run first to supply a reasonable  input).
    // --------------------------------------------------------------------------
    vkAOMask_t            aoScratch[VK_MAX_FRAMES_IN_FLIGHT]; // ping-pong scratch, R8_UNORM, GENERAL

    VkPipeline            atrousPipeline;
    VkPipelineLayout      atrousPipelineLayout;
    VkDescriptorSetLayout atrousDescLayout;
    VkDescriptorPool      atrousDescPool;
    // [frameIdx][0]: binding0=aoHistory(in), binding1=aoScratch(out), binding2=depth
    // [frameIdx][1]: binding0=aoScratch(in), binding1=aoHistory(out), binding2=depth
    VkDescriptorSet       atrousDescSets[VK_MAX_FRAMES_IN_FLIGHT][2];
    int                   atrousDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

    // View sampled by the interaction pass.  Initialised to aoHistory[i].view at
    // CreateHistoryImages; updated at end of each DispatchAtrousAO to whichever
    // image holds the final Atrous output (history if even pass count, scratch if odd).
    // Falls back to aoHistory[i].view when Atrous is disabled (r_rtAtrousIterations 0).
    VkImageView           aoReadView[VK_MAX_FRAMES_IN_FLIGHT];

    // SBT buffers
    VkBuffer sbtBuffer;
    VkDeviceMemory sbtMemory;
    VkStridedDeviceAddressRegionKHR rgenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callRegion;

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

// Initialize the shadow ray pipeline and shadow mask images (called after swapchain creation)
void VK_RT_InitShadows(void);

// Initialize the AO ray pipeline and AO mask images (called after swapchain creation)
void VK_RT_InitAO(void);

// Dispatch AO rays for the current view (once per frame, after TLAS rebuild, outside render pass).
// Depth must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL on entry.
// Transitions depth to READ_ONLY_OPTIMAL for the dispatch, then restores to ATTACHMENT_OPTIMAL.
void VK_RT_DispatchAO(VkCommandBuffer cmd, const viewDef_t *viewDef);

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
// On exit aoHistory[currentFrame] contains the blended result and a
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

// Build/update BLAS for a single mesh (single-surface, kept for external use).
// cmd must be a command buffer currently recording outside a render pass.
vkBLAS_t *VK_RT_BuildBLAS(const srfTriangles_t *tri, VkCommandBuffer cmd, bool isPerforated = false);
void VK_RT_DestroyBLAS(vkBLAS_t *blas);

// Build a multi-geometry BLAS covering all non-translucent surfaces of a model.
// Produces one TLAS instance per entity so shadow rays intersect every surface,
// not just Surface(0).  Used by VK_RT_RebuildTLAS.
// prevBlas: if non-NULL and geometry is compatible, performs an in-place update
// (reuses geometry buffers and uses MODE_UPDATE) instead of a full rebuild.
// Returns prevBlas on successful update, or a new vkBLAS_t on full rebuild.
vkBLAS_t *VK_RT_BuildBLASForModel(idRenderModel *model, VkCommandBuffer cmd, vkBLAS_t *prevBlas = NULL);

// Drain deferred BLAS deletions (call after fence wait when frame slot is safe)
void VK_RT_DrainBLASGarbage(void);

// Rebuild TLAS from all visible entities this frame
void VK_RT_RebuildTLAS(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Dispatch shadow rays for a single light.
// Must be called outside a render pass.  Depth must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL on entry;
// this function transitions depth to READ_ONLY_OPTIMAL for the dispatch then back before returning.
// The shadow mask is kept in VK_IMAGE_LAYOUT_GENERAL throughout (no layout transition).
void VK_RT_DispatchShadowRaysForLight(VkCommandBuffer cmd, const viewDef_t *viewDef, const viewLight_t *vLight, VkRect2D dispatchRect);

// Resize shadow mask when resolution changes
void VK_RT_ResizeShadowMask(uint32_t width, uint32_t height);

#endif // __VK_RAYTRACING_H__
