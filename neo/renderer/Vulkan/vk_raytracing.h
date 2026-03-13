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
// RT extension function pointers (always loaded at runtime via
// vkGetDeviceProcAddr)
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
};

// Shadow mask buffer (R8 UNORM, one pixel per screen pixel)
struct vkShadowMask_t
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
    vkTLAS_t tlas;
    vkShadowMask_t shadowMask[VK_MAX_FRAMES_IN_FLIGHT];
    VkSampler shadowMaskSampler; // nearest-clamp, used when sampling shadow mask
                                 // in lighting pass

    // RT pipeline for shadow rays
    VkPipeline shadowPipeline;
    VkPipelineLayout shadowPipelineLayout;
    VkDescriptorSetLayout shadowDescLayout;
    VkDescriptorPool shadowDescPool;
    VkDescriptorSet shadowDescSets[VK_MAX_FRAMES_IN_FLIGHT];

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

// Initialize the RT pipeline (called once after device creation when RT is
// available)
void VK_RT_Init(void);
void VK_RT_Shutdown(void);

// Initialize the shadow ray pipeline and shadow mask images (called after
// swapchain creation)
void VK_RT_InitShadows(void);

// Build/update BLAS for a mesh (called when geometry is added to the scene)
vkBLAS_t *VK_RT_BuildBLAS(const srfTriangles_t *tri);
void VK_RT_DestroyBLAS(vkBLAS_t *blas);

// Rebuild TLAS from all visible entities this frame
void VK_RT_RebuildTLAS(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Dispatch shadow ray pass - writes shadow mask for current frame
void VK_RT_DispatchShadowRays(VkCommandBuffer cmd, const viewDef_t *viewDef);

// Resize shadow mask when resolution changes
void VK_RT_ResizeShadowMask(uint32_t width, uint32_t height);

#endif // __VK_RAYTRACING_H__
