/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan ray tracing - acceleration structure (BLAS/TLAS) management.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#ifdef DHEWM3_RAYTRACING

#include "renderer/VertexCache.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/tr_local.h"
#include "sys/platform.h"

#include <string.h>

// ---------------------------------------------------------------------------
// RT extension function pointer definitions (declared extern in
// vk_raytracing.h)
// ---------------------------------------------------------------------------

PFN_vkCreateAccelerationStructureKHR pfn_vkCreateAccelerationStructureKHR = NULL;
PFN_vkDestroyAccelerationStructureKHR pfn_vkDestroyAccelerationStructureKHR = NULL;
PFN_vkGetAccelerationStructureBuildSizesKHR pfn_vkGetAccelerationStructureBuildSizesKHR = NULL;
PFN_vkCmdBuildAccelerationStructuresKHR pfn_vkCmdBuildAccelerationStructuresKHR = NULL;
PFN_vkGetAccelerationStructureDeviceAddressKHR pfn_vkGetAccelerationStructureDeviceAddressKHR = NULL;
PFN_vkCreateRayTracingPipelinesKHR pfn_vkCreateRayTracingPipelinesKHR = NULL;
PFN_vkGetRayTracingShaderGroupHandlesKHR pfn_vkGetRayTracingShaderGroupHandlesKHR = NULL;
PFN_vkCmdTraceRaysKHR pfn_vkCmdTraceRaysKHR = NULL;
PFN_vkGetBufferDeviceAddressKHR pfn_vkGetBufferDeviceAddressKHR = NULL;

// Global RT state
vkRTState_t vkRT;

// ---------------------------------------------------------------------------
// VK_RT_LoadFunctionPointers
// Must be called after logical device creation.
// ---------------------------------------------------------------------------

void VK_RT_LoadFunctionPointers(void)
{
#define LOAD_RT_FN(name)                                                                                               \
    pfn_##name = (PFN_##name)vkGetDeviceProcAddr(vk.device, #name);                                                    \
    if (!pfn_##name)                                                                                                   \
        common->Warning("VK RT: could not load " #name);

    LOAD_RT_FN(vkCreateAccelerationStructureKHR)
    LOAD_RT_FN(vkDestroyAccelerationStructureKHR)
    LOAD_RT_FN(vkGetAccelerationStructureBuildSizesKHR)
    LOAD_RT_FN(vkCmdBuildAccelerationStructuresKHR)
    LOAD_RT_FN(vkGetAccelerationStructureDeviceAddressKHR)
    LOAD_RT_FN(vkCreateRayTracingPipelinesKHR)
    LOAD_RT_FN(vkGetRayTracingShaderGroupHandlesKHR)
    LOAD_RT_FN(vkCmdTraceRaysKHR)
    LOAD_RT_FN(vkGetBufferDeviceAddressKHR)

#undef LOAD_RT_FN
}

// ---------------------------------------------------------------------------
// Helper: get device address of a buffer
// ---------------------------------------------------------------------------

static VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer)
{
    VkBufferDeviceAddressInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddressKHR(vk.device, &info);
}

// ---------------------------------------------------------------------------
// Helper: allocate a buffer suitable for use as an AS storage / scratch buffer.
// The buffer needs VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR and
// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
// ---------------------------------------------------------------------------

static void AllocASBuffer(VkDeviceSize size, VkBufferUsageFlags extraUsage, VkBuffer *outBuf, VkDeviceMemory *outMem)
{
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extraUsage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.device, &bufInfo, NULL, outBuf));

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, *outBuf, &memReqs);

    // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT required for device-address-capable
    // memory
    VkMemoryAllocateFlagsInfo flagsInfo = {};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &flagsInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, outMem));
    VK_CHECK(vkBindBufferMemory(vk.device, *outBuf, *outMem, 0));
}

// ---------------------------------------------------------------------------
// VK_RT_BuildBLAS
// Build a bottom-level acceleration structure from a srfTriangles_t mesh.
// Geometry is read from the ambient vertex cache (already on GPU as a GL/Vk
// buffer).
// ---------------------------------------------------------------------------

vkBLAS_t *VK_RT_BuildBLAS(const srfTriangles_t *tri)
{
    if (!tri || tri->numVerts == 0 || tri->numIndexes == 0)
    {
        return NULL;
    }

    // The vertex cache provides a GPU buffer for this surface.
    // In the Vulkan path the vertex cache buffer has SHADER_DEVICE_ADDRESS_BIT.
    // If the data isn't cached yet, bail — the caller must ensure ambient cache
    // is populated.
    if (!tri->ambientCache)
    {
        return NULL;
    }

    // Vertex / index buffer addresses from the vertex cache
    // vertexCache gives us a vertCacheHandle_t; the VK vertex cache backend
    // stores the VkBuffer. For now we read the CPU-side data and build the BLAS
    // from a host-visible staging buffer. A later optimisation can reuse the GPU
    // vertex buffer directly.
    const idDrawVert *verts = tri->verts;
    const glIndex_t *indexes = tri->indexes;

    // Upload vertices into a temporary device-local buffer via staging
    VkDeviceSize vertexDataSize = (VkDeviceSize)tri->numVerts * sizeof(idDrawVert);
    VkDeviceSize indexDataSize = (VkDeviceSize)tri->numIndexes * sizeof(glIndex_t);

    VkBuffer vertBuf, idxBuf;
    VkDeviceMemory vertMem, idxMem;

    {
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        bi.size = vertexDataSize;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &vertBuf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, vertBuf, &mr);
        VkMemoryAllocateFlagsInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &fi;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &vertMem));
        VK_CHECK(vkBindBufferMemory(vk.device, vertBuf, vertMem, 0));

        bi.size = indexDataSize;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &idxBuf));
        vkGetBufferMemoryRequirements(vk.device, idxBuf, &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &idxMem));
        VK_CHECK(vkBindBufferMemory(vk.device, idxBuf, idxMem, 0));
    }

    // Upload vertex/index data using staging
    {
        // Vertex upload
        VkBuffer stagingV;
        VkDeviceMemory stagingVMem;
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = vertexDataSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &stagingV));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, stagingV, &mr);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &stagingVMem));
        VK_CHECK(vkBindBufferMemory(vk.device, stagingV, stagingVMem, 0));
        void *ptr;
        VK_CHECK(vkMapMemory(vk.device, stagingVMem, 0, vertexDataSize, 0, &ptr));
        memcpy(ptr, verts, (size_t)vertexDataSize);
        vkUnmapMemory(vk.device, stagingVMem);
        VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
        VkBufferCopy region = {0, 0, vertexDataSize};
        vkCmdCopyBuffer(cmd, stagingV, vertBuf, 1, &region);
        VK_EndSingleTimeCommands(cmd);
        vkDestroyBuffer(vk.device, stagingV, NULL);
        vkFreeMemory(vk.device, stagingVMem, NULL);

        // Index upload
        VkBuffer stagingI;
        VkDeviceMemory stagingIMem;
        bi.size = indexDataSize;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &stagingI));
        vkGetBufferMemoryRequirements(vk.device, stagingI, &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &stagingIMem));
        VK_CHECK(vkBindBufferMemory(vk.device, stagingI, stagingIMem, 0));
        VK_CHECK(vkMapMemory(vk.device, stagingIMem, 0, indexDataSize, 0, &ptr));
        memcpy(ptr, indexes, (size_t)indexDataSize);
        vkUnmapMemory(vk.device, stagingIMem);
        cmd = VK_BeginSingleTimeCommands();
        region = {0, 0, indexDataSize};
        vkCmdCopyBuffer(cmd, stagingI, idxBuf, 1, &region);
        VK_EndSingleTimeCommands(cmd);
        vkDestroyBuffer(vk.device, stagingI, NULL);
        vkFreeMemory(vk.device, stagingIMem, NULL);
    }

    // Describe geometry
    VkAccelerationStructureGeometryTrianglesDataKHR triData = {};
    triData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // xyz at offset 0 of idDrawVert
    triData.vertexData.deviceAddress = GetBufferDeviceAddress(vertBuf);
    triData.vertexStride = sizeof(idDrawVert);
    triData.maxVertex = (uint32_t)tri->numVerts - 1;
    triData.indexType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    triData.indexData.deviceAddress = GetBufferDeviceAddress(idxBuf);
    triData.transformData.deviceAddress = 0; // identity — world transform goes in TLAS instance

    VkAccelerationStructureGeometryKHR asGeom = {};
    asGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    asGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeom.geometry.triangles = triData;
    asGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &asGeom;

    uint32_t primitiveCount = (uint32_t)tri->numIndexes / 3;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            &primitiveCount, &sizeInfo);

    // Allocate BLAS storage buffer
    vkBLAS_t *blas = new vkBLAS_t();
    memset(blas, 0, sizeof(*blas));

    AllocASBuffer(sizeInfo.accelerationStructureSize, 0, &blas->buffer, &blas->memory);

    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR asInfo = {};
    asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = blas->buffer;
    asInfo.size = sizeInfo.accelerationStructureSize;
    asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &asInfo, NULL, &blas->handle));

    // Scratch buffer
    VkBuffer scratchBuf;
    VkDeviceMemory scratchMem;
    AllocASBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratchBuf, &scratchMem);

    buildInfo.dstAccelerationStructure = blas->handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(scratchBuf);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
    rangeInfo.primitiveCount = primitiveCount;
    rangeInfo.primitiveOffset = 0;
    rangeInfo.firstVertex = 0;
    rangeInfo.transformOffset = 0;

    const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfo = &rangeInfo;

    // Build on device
    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    // Memory barrier: wait for BLAS build before any ray tracing reads it
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);

    VK_EndSingleTimeCommands(cmd);

    // Device address for use in TLAS instances
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = blas->handle;
    blas->deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &addrInfo);

    // Free scratch and geometry buffers (no longer needed after build)
    vkDestroyBuffer(vk.device, scratchBuf, NULL);
    vkFreeMemory(vk.device, scratchMem, NULL);
    vkDestroyBuffer(vk.device, vertBuf, NULL);
    vkFreeMemory(vk.device, vertMem, NULL);
    vkDestroyBuffer(vk.device, idxBuf, NULL);
    vkFreeMemory(vk.device, idxMem, NULL);

    blas->isValid = true;
    return blas;
}

// ---------------------------------------------------------------------------
// VK_RT_DestroyBLAS
// ---------------------------------------------------------------------------

void VK_RT_DestroyBLAS(vkBLAS_t *blas)
{
    if (!blas)
        return;
    if (blas->handle != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(vk.device, blas->handle, NULL);
    }
    if (blas->buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, blas->buffer, NULL);
    }
    if (blas->memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, blas->memory, NULL);
    }
    delete blas;
}

// ---------------------------------------------------------------------------
// VK_RT_RebuildTLAS
// Rebuild the Top-Level AS each frame from visible entities in viewDef.
// Each entity contributes one instance pointing to its BLAS.
// ---------------------------------------------------------------------------

void VK_RT_RebuildTLAS(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!viewDef)
        return;

    // Count and collect instances
    static VkAccelerationStructureInstanceKHR instances[4096];
    uint32_t instanceCount = 0;

    for (const viewEntity_t *vEntity = viewDef->viewEntitys; vEntity != NULL && instanceCount < 4096;
         vEntity = vEntity->next)
    {
        idRenderEntityLocal *ent = vEntity->entityDef;
        if (!ent)
            continue;

        // Build or rebuild BLAS for this entity if needed
        idRenderModel *model = ent->dynamicModel ? ent->dynamicModel : ent->parms.hModel;
        if (!model)
            continue;

        // Use the first surface's triangles for the BLAS.
        // A more complete implementation would build one BLAS per model surface.
        const modelSurface_t *surf = model->Surface(0);
        if (!surf || !surf->geometry || surf->geometry->numVerts == 0)
            continue;

        const srfTriangles_t *geo = surf->geometry;
        bool needRebuild = !ent->blas || !ent->blas->isValid;

        // Dynamic/animated models must be rebuilt every frame
        if (ent->dynamicModel)
            needRebuild = true;

        if (needRebuild)
        {
            if (ent->blas)
            {
                VK_RT_DestroyBLAS(ent->blas);
            }
            ent->blas = VK_RT_BuildBLAS(geo);
            ent->blasFrameCount = tr.frameCount;
        }

        if (!ent->blas || !ent->blas->isValid)
            continue;

        // Fill VkAccelerationStructureInstanceKHR
        VkAccelerationStructureInstanceKHR &inst = instances[instanceCount++];
        memset(&inst, 0, sizeof(inst));

        // 3x4 row-major transform from modelMatrix (column-major in GL)
        // idDrawVert layout: GL/Vulkan column-major 4x4 → row-major 3x4
        const float *m = ent->modelMatrix;
        inst.transform.matrix[0][0] = m[0];
        inst.transform.matrix[0][1] = m[4];
        inst.transform.matrix[0][2] = m[8];
        inst.transform.matrix[0][3] = m[12];
        inst.transform.matrix[1][0] = m[1];
        inst.transform.matrix[1][1] = m[5];
        inst.transform.matrix[1][2] = m[9];
        inst.transform.matrix[1][3] = m[13];
        inst.transform.matrix[2][0] = m[2];
        inst.transform.matrix[2][1] = m[6];
        inst.transform.matrix[2][2] = m[10];
        inst.transform.matrix[2][3] = m[14];

        inst.instanceCustomIndex = instanceCount - 1;
        inst.mask = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = ent->blas->deviceAddress;
    }

    if (instanceCount == 0)
        return;

    vkTLAS_t &tlas = vkRT.tlas;

    // (Re)create instance buffer if size changed
    VkDeviceSize instBufSize = instanceCount * sizeof(VkAccelerationStructureInstanceKHR);

    if (tlas.instanceBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, tlas.instanceBuffer, NULL);
        vkFreeMemory(vk.device, tlas.instanceMemory, NULL);
        tlas.instanceBuffer = VK_NULL_HANDLE;
        tlas.instanceMemory = VK_NULL_HANDLE;
    }

    // Instance buffer must be host-visible so we can fill it each frame
    {
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = instBufSize;
        bi.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &tlas.instanceBuffer));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, tlas.instanceBuffer, &mr);
        VkMemoryAllocateFlagsInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &fi;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &tlas.instanceMemory));
        VK_CHECK(vkBindBufferMemory(vk.device, tlas.instanceBuffer, tlas.instanceMemory, 0));
    }

    // Copy instance data
    void *ptr;
    VK_CHECK(vkMapMemory(vk.device, tlas.instanceMemory, 0, instBufSize, 0, &ptr));
    memcpy(ptr, instances, (size_t)instBufSize);
    vkUnmapMemory(vk.device, tlas.instanceMemory);

    tlas.instanceCount = instanceCount;

    // Barrier: ensure instance writes are visible to AS build
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0,
                         1, &barrier, 0, NULL, 0, NULL);

    // Describe TLAS geometry
    VkAccelerationStructureGeometryInstancesDataKHR instData = {};
    instData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instData.arrayOfPointers = VK_FALSE;
    instData.data.deviceAddress = GetBufferDeviceAddress(tlas.instanceBuffer);

    VkAccelerationStructureGeometryKHR tlasGeom = {};
    tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.geometry.instances = instData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &tlasGeom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            &instanceCount, &sizeInfo);

    // Recreate TLAS storage if needed
    bool needNewAS = (tlas.handle == VK_NULL_HANDLE);
    // Simple policy: destroy and recreate if size grows
    if (tlas.handle != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(vk.device, tlas.handle, NULL);
        vkDestroyBuffer(vk.device, tlas.buffer, NULL);
        vkFreeMemory(vk.device, tlas.memory, NULL);
        tlas.handle = VK_NULL_HANDLE;
        needNewAS = true;
    }
    if (tlas.scratchBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, tlas.scratchBuffer, NULL);
        vkFreeMemory(vk.device, tlas.scratchMemory, NULL);
        tlas.scratchBuffer = VK_NULL_HANDLE;
    }

    AllocASBuffer(sizeInfo.accelerationStructureSize, 0, &tlas.buffer, &tlas.memory);
    AllocASBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &tlas.scratchBuffer,
                  &tlas.scratchMemory);

    VkAccelerationStructureCreateInfoKHR asInfo = {};
    asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = tlas.buffer;
    asInfo.size = sizeInfo.accelerationStructureSize;
    asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &asInfo, NULL, &tlas.handle));

    buildInfo.dstAccelerationStructure = tlas.handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(tlas.scratchBuffer);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
    rangeInfo.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfo = &rangeInfo;

    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    // Barrier: TLAS build must complete before ray tracing reads it
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);

    // Store device address for use in RT pipeline descriptor
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = tlas.handle;
    tlas.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &addrInfo);
    tlas.isValid = true;
}

// ---------------------------------------------------------------------------
// VK_RT_Init / VK_RT_Shutdown
// Called once after device creation (when rayTracingSupported is true).
// The shadow pipeline and shadow mask are set up in vk_shadows.cpp.
// ---------------------------------------------------------------------------

void VK_RT_Init(void)
{
    memset(&vkRT, 0, sizeof(vkRT));
    VK_RT_LoadFunctionPointers();
    common->Printf("VK RT: acceleration structure support initialized\n");
}

void VK_RT_Shutdown(void)
{
    vkDeviceWaitIdle(vk.device);

    // Destroy TLAS
    if (vkRT.tlas.handle != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(vk.device, vkRT.tlas.handle, NULL);
        vkDestroyBuffer(vk.device, vkRT.tlas.buffer, NULL);
        vkFreeMemory(vk.device, vkRT.tlas.memory, NULL);
    }
    if (vkRT.tlas.instanceBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.tlas.instanceBuffer, NULL);
        vkFreeMemory(vk.device, vkRT.tlas.instanceMemory, NULL);
    }
    if (vkRT.tlas.scratchBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.tlas.scratchBuffer, NULL);
        vkFreeMemory(vk.device, vkRT.tlas.scratchMemory, NULL);
    }

    // Shadow mask images
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkShadowMask_t &sm = vkRT.shadowMask[i];
        if (sm.view != VK_NULL_HANDLE)
            vkDestroyImageView(vk.device, sm.view, NULL);
        if (sm.image != VK_NULL_HANDLE)
            vkDestroyImage(vk.device, sm.image, NULL);
        if (sm.memory != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, sm.memory, NULL);
    }

    // Shadow pipeline (destroyed by vk_shadows.cpp if initialized)
    if (vkRT.shadowPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.shadowPipeline, NULL);
    }
    if (vkRT.shadowPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.shadowPipelineLayout, NULL);
    }
    if (vkRT.shadowDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.shadowDescLayout, NULL);
    }
    if (vkRT.shadowDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.shadowDescPool, NULL);
    }
    if (vkRT.sbtBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.sbtBuffer, NULL);
        vkFreeMemory(vk.device, vkRT.sbtMemory, NULL);
    }
    if (vkRT.shadowMaskSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.shadowMaskSampler, NULL);
    }

    memset(&vkRT, 0, sizeof(vkRT));
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeShadowMask
// Called when the render resolution changes.
// ---------------------------------------------------------------------------

void VK_RT_ResizeShadowMask(uint32_t width, uint32_t height)
{
    vkDeviceWaitIdle(vk.device);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkShadowMask_t &sm = vkRT.shadowMask[i];
        if (sm.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, sm.view, NULL);
            sm.view = VK_NULL_HANDLE;
        }
        if (sm.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, sm.image, NULL);
            sm.image = VK_NULL_HANDLE;
        }
        if (sm.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, sm.memory, NULL);
            sm.memory = VK_NULL_HANDLE;
        }

        // R8 UNORM storage image
        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8_UNORM;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &sm.image));

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk.device, sm.image, &mr);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &sm.memory));
        VK_CHECK(vkBindImageMemory(vk.device, sm.image, sm.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = sm.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &sm.view));

        // Transition to GENERAL layout for storage image use
        VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
        VK_TransitionImageLayout(cmd, sm.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        VK_EndSingleTimeCommands(cmd);

        sm.width = width;
        sm.height = height;
    }
}

#endif // DHEWM3_RAYTRACING
