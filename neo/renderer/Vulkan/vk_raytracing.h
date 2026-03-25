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
    uint32_t        geomCount;
    VkBuffer       *geomVertBufs;
    VkDeviceMemory *geomVertMems;
    VkBuffer       *geomIdxBufs;
    VkDeviceMemory *geomIdxMems;
    // Scratch buffer used during build (freed at destroy time).
    VkBuffer scratchBuf;
    VkDeviceMemory scratchMem;
    bool isValid;
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

    VkBuffer instanceBuffer; // VkAccelerationStructureInstanceKHR array
    VkDeviceMemory instanceMemory;
    uint32_t instanceCount;

    VkBuffer scratchBuffer; // build scratch
    VkDeviceMemory scratchMemory;

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

    // Shadow mask blur (compute pipeline)
    VkPipeline blurPipeline;
    VkPipelineLayout blurPipelineLayout;
    VkDescriptorSetLayout blurDescLayout;
    VkDescriptorPool blurDescPool;
    VkDescriptorSet blurDescSetH[VK_MAX_FRAMES_IN_FLIGHT]; // horizontal pass: shadowMask→blurTemp
    VkDescriptorSet blurDescSetV[VK_MAX_FRAMES_IN_FLIGHT]; // vertical pass: blurTemp→shadowMask
    int blurDescSetLastUpdatedFrameCount[VK_MAX_FRAMES_IN_FLIGHT];

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

// Build/update BLAS for a single mesh (single-surface, kept for external use).
// cmd must be a command buffer currently recording outside a render pass.
vkBLAS_t *VK_RT_BuildBLAS(const srfTriangles_t *tri, VkCommandBuffer cmd, bool isPerforated = false);
void VK_RT_DestroyBLAS(vkBLAS_t *blas);

// Build a multi-geometry BLAS covering all non-translucent surfaces of a model.
// Produces one TLAS instance per entity so shadow rays intersect every surface,
// not just Surface(0).  Used by VK_RT_RebuildTLAS.
vkBLAS_t *VK_RT_BuildBLASForModel(idRenderModel *model, VkCommandBuffer cmd);

// Drain deferred BLAS deletions (call after fence wait when frame slot is safe)
void VK_RT_DrainBLASGarbage(void);

// Rebuild TLAS from all visible entities this frame
void VK_RT_RebuildTLAS(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Dispatch shadow rays for a single light.
// Must be called outside a render pass.  Depth must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL on entry;
// this function transitions depth to READ_ONLY_OPTIMAL for the dispatch then back before returning.
// The shadow mask is kept in VK_IMAGE_LAYOUT_GENERAL throughout (no layout transition).
void VK_RT_DispatchShadowRaysForLight(VkCommandBuffer cmd, const viewDef_t *viewDef, const viewLight_t *vLight);

// Resize shadow mask when resolution changes
void VK_RT_ResizeShadowMask(uint32_t width, uint32_t height);

#endif // __VK_RAYTRACING_H__
