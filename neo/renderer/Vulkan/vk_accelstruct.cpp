/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan ray tracing - acceleration structure (BLAS/TLAS) management.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <string.h>

// ---------------------------------------------------------------------------
// RT extension function pointer definitions (declared extern in vk_raytracing.h)
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
// Deferred BLAS deletion queue
//
// When entities are destroyed (e.g. cutscene transitions, map loads), their
// BLAS is freed.  But in-flight command buffers may still reference the BLAS
// via the TLAS.  We queue destroyed BLASes and only free them once enough
// frames have passed that no in-flight work can reference them.
// ---------------------------------------------------------------------------

static const int BLAS_GARBAGE_MAX = 1024;

struct blasGarbageEntry_t
{
    vkBLAS_t *blas;
    int retireFrameCount; // safe to free when tr.frameCount >= this
};

static blasGarbageEntry_t s_blasGarbage[BLAS_GARBAGE_MAX];
static int s_blasGarbageCount = 0;

static ID_INLINE bool VK_RT_IsBLASQueuedForDeferredFree(vkBLAS_t *blas)
{
    for (int i = 0; i < s_blasGarbageCount; i++)
    {
        if (s_blasGarbage[i].blas == blas)
            return true;
    }
    return false;
}

// Lightweight per-frame BLAS source stats for Optimization 1 validation.
// These counters help track zero-copy adoption (GPU cache buffers) vs CPU fallback.
struct vkRTBlasBuildStats_t
{
    int frameCount;
    int gpuGeomCount;
    int cpuGeomCount;
    int singleBuildCount;
    int modelBuildCount;
    int summaryLoggedFrameCount;
};

static vkRTBlasBuildStats_t s_blasBuildStats = {-1, 0, 0, 0, 0, -1};

static const int VK_RT_MAX_TLAS_INSTANCES = 4096;

struct vkRTStaticInstanceCache_t
{
    bool valid;
    uint64_t signature;
    uint32_t count;
    VkAccelerationStructureInstanceKHR instances[VK_RT_MAX_TLAS_INSTANCES];
};

static vkRTStaticInstanceCache_t s_staticInstanceCache[VK_MAX_FRAMES_IN_FLIGHT] = {};

static void VK_RT_FreeBLASImmediate(vkBLAS_t *blas); // forward decl for model cache

// ---------------------------------------------------------------------------
// Model-keyed BLAS cache for static (non-deforming) entities
//
// Entities with dynamicModel=NULL share the same mesh every frame. Keying by
// hModel* avoids rebuilding the BLAS when the entity pointer churns (e.g.
// doors destroyed and recreated each frame by the game) and keeps the device
// address stable so the static TLAS signature stops churning.
//
// The cache owns the BLASes; ent->blas is a borrowed pointer for static
// entities. VK_RT_DestroyBLAS is a no-op for cached entries. The cache is
// cleared (and all GPU resources freed) on shutdown and map transitions.
// ---------------------------------------------------------------------------

static const int VK_RT_MODEL_BLAS_CACHE_MAX = 512;

struct vkModelBLASCacheEntry_t
{
    idRenderModel *model;
    vkBLAS_t *blas;
};

static vkModelBLASCacheEntry_t s_modelBLASCache[VK_RT_MODEL_BLAS_CACHE_MAX];
static int s_modelBLASCacheCount = 0;

static vkBLAS_t *VK_RT_ModelBLASCacheLookup(const idRenderModel *model)
{
    for (int i = 0; i < s_modelBLASCacheCount; i++)
        if (s_modelBLASCache[i].model == model)
            return s_modelBLASCache[i].blas;
    return NULL;
}

static void VK_RT_ModelBLASCacheInsert(idRenderModel *model, vkBLAS_t *blas)
{
    // Guard against duplicate insertion (model may appear in multiple worlds).
    for (int i = 0; i < s_modelBLASCacheCount; i++)
        if (s_modelBLASCache[i].model == model)
            return;

    if (s_modelBLASCacheCount >= VK_RT_MODEL_BLAS_CACHE_MAX)
    {
        common->Warning("VK RT: model BLAS cache full (%d entries)\n", VK_RT_MODEL_BLAS_CACHE_MAX);
        return;
    }

    blas->cachedByModel = true;
    s_modelBLASCache[s_modelBLASCacheCount].model = model;
    s_modelBLASCache[s_modelBLASCacheCount].blas = blas;
    s_modelBLASCacheCount++;
}

// Destroy all cached BLASes and reset the cache.
// Must be called when no in-flight GPU work references the TLAS (e.g. after
// vkDeviceWaitIdle on shutdown, or after fence wait on map transition).
static void VK_RT_ModelBLASCacheClear(void)
{
    for (int i = 0; i < s_modelBLASCacheCount; i++)
    {
        if (s_modelBLASCache[i].blas)
        {
            s_modelBLASCache[i].blas->cachedByModel = false;
            VK_RT_FreeBLASImmediate(s_modelBLASCache[i].blas);
            s_modelBLASCache[i].blas = NULL;
            s_modelBLASCache[i].model = NULL;
        }
    }
    s_modelBLASCacheCount = 0;
}

static ID_INLINE uint64_t VK_RT_HashFnv1a64_Bytes(uint64_t h, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < bytes; i++)
    {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void VK_RT_ResetBLASBuildStatsIfNewFrame()
{
    if (s_blasBuildStats.frameCount != tr.frameCount)
    {
        s_blasBuildStats.frameCount = tr.frameCount;
        s_blasBuildStats.gpuGeomCount = 0;
        s_blasBuildStats.cpuGeomCount = 0;
        s_blasBuildStats.singleBuildCount = 0;
        s_blasBuildStats.modelBuildCount = 0;
        s_blasBuildStats.summaryLoggedFrameCount = -1;
    }
}

static void VK_RT_AccumulateBLASBuildStats(int gpuGeoms, int cpuGeoms, bool modelBuild)
{
    VK_RT_ResetBLASBuildStatsIfNewFrame();
    s_blasBuildStats.gpuGeomCount += gpuGeoms;
    s_blasBuildStats.cpuGeomCount += cpuGeoms;
    if (modelBuild)
        s_blasBuildStats.modelBuildCount++;
    else
        s_blasBuildStats.singleBuildCount++;
}

// Actually free a BLAS's GPU resources and memory.
static void VK_RT_FreeBLASImmediate(vkBLAS_t *blas)
{
    if (!blas)
        return;
    if (blas->handle != VK_NULL_HANDLE)
        vkDestroyAccelerationStructureKHR(vk.device, blas->handle, NULL);
    if (blas->buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, blas->buffer, NULL);
    }
    if (blas->memory != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, blas->memory, NULL);
    if (blas->scratchBuf != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, blas->scratchBuf, NULL);
    }
    if (blas->scratchMem != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, blas->scratchMem, NULL);
    for (uint32_t i = 0; i < blas->geomCount; i++)
    {
        // Geometry buffers can be either:
        // 1) Owned staging/upload buffers allocated by BLAS build (mem != NULL), or
        // 2) External vertex-cache buffers (mem == NULL) owned elsewhere.
        // Only destroy/free owned buffers here.
        if (blas->geomVertBufs && blas->geomVertMems && blas->geomVertMems[i] != VK_NULL_HANDLE &&
            blas->geomVertBufs[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, blas->geomVertBufs[i], NULL);
        }
        if (blas->geomVertMems && blas->geomVertMems[i] != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, blas->geomVertMems[i], NULL);
        if (blas->geomIdxBufs && blas->geomIdxMems && blas->geomIdxMems[i] != VK_NULL_HANDLE &&
            blas->geomIdxBufs[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, blas->geomIdxBufs[i], NULL);
        }
        if (blas->geomIdxMems && blas->geomIdxMems[i] != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, blas->geomIdxMems[i], NULL);
    }
    delete[] blas->geomVertBufs;
    delete[] blas->geomVertMems;
    delete[] blas->geomIdxBufs;
    delete[] blas->geomIdxMems;
    delete[] blas->geomVertSizes;
    delete[] blas->geomIdxSizes;
    delete[] blas->geomPrimCounts;
    delete blas;
}

// Clear per-entity BLAS pointers across all render worlds.
// If destroyGpuResources is true, BLAS not already in deferred queue are destroyed immediately.
static void VK_RT_ClearWorldEntityBLASPointers(bool destroyGpuResources)
{
    for (int w = 0; w < tr.worlds.Num(); w++)
    {
        idRenderWorldLocal *world = tr.worlds[w];
        if (!world)
            continue;

        for (int e = 0; e < world->entityDefs.Num(); e++)
        {
            idRenderEntityLocal *ent = world->entityDefs[e];
            if (!ent || !ent->blas)
                continue;

            // Cached BLASes are owned by the model cache — don't destroy them
            // here; VK_RT_ModelBLASCacheClear() handles that below.
            if (destroyGpuResources && !ent->blas->cachedByModel && !VK_RT_IsBLASQueuedForDeferredFree(ent->blas))
            {
                VK_RT_FreeBLASImmediate(ent->blas);
            }

            ent->blas = NULL;
            ent->blasFrameCount = 0;
        }
    }

    if (destroyGpuResources)
        VK_RT_ModelBLASCacheClear();
}

// Drain BLAS garbage entries that are safe to free (called after fence wait).
void VK_RT_DrainBLASGarbage(void)
{
    int kept = 0;
    for (int i = 0; i < s_blasGarbageCount; i++)
    {
        if (tr.frameCount >= s_blasGarbage[i].retireFrameCount)
        {
            VK_RT_FreeBLASImmediate(s_blasGarbage[i].blas);
        }
        else
        {
            s_blasGarbage[kept++] = s_blasGarbage[i];
        }
    }
    s_blasGarbageCount = kept;
}

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

    // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT required for device-address-capable memory
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
// Geometry is read from the ambient vertex cache (already on GPU as a GL/Vk buffer).
// ---------------------------------------------------------------------------

vkBLAS_t *VK_RT_BuildBLAS(const srfTriangles_t *tri, VkCommandBuffer cmd, bool isPerforated)
{
    extern bool VK_VertexCache_GetBuffer(vertCache_t * block, VkBuffer * outBuf, VkDeviceSize * outOffset);

    if (!tri || tri->numVerts == 0 || tri->numIndexes == 0)
        return NULL;

    // Preferred path: use GPU cache buffers directly as AS build inputs.
    // Fallback path: build from CPU-visible pointers when cache buffers are unavailable.
    VkBuffer vertSrcBuf = VK_NULL_HANDLE;
    VkBuffer idxSrcBuf = VK_NULL_HANDLE;
    VkDeviceSize vertSrcOffset = 0;
    VkDeviceSize idxSrcOffset = 0;
    const bool haveGpuVerts =
        tri->ambientCache && VK_VertexCache_GetBuffer(tri->ambientCache, &vertSrcBuf, &vertSrcOffset);
    const bool haveGpuIdx = tri->indexCache && VK_VertexCache_GetBuffer(tri->indexCache, &idxSrcBuf, &idxSrcOffset);
    const bool useGpuBuffers = haveGpuVerts && haveGpuIdx;

    VK_RT_AccumulateBLASBuildStats(useGpuBuffers ? 1 : 0, useGpuBuffers ? 0 : 1, false);

    if (!useGpuBuffers && (!tri->verts || !tri->indexes))
        return NULL;

    VkDeviceSize vertexDataSize = (VkDeviceSize)tri->numVerts * sizeof(idDrawVert);
    VkDeviceSize indexDataSize = (VkDeviceSize)tri->numIndexes * sizeof(glIndex_t);

    vkBLAS_t *blas = new vkBLAS_t();
    blas->handle = VK_NULL_HANDLE;
    blas->buffer = VK_NULL_HANDLE;
    blas->memory = VK_NULL_HANDLE;
    blas->deviceAddress = 0;
    blas->scratchBuf = VK_NULL_HANDLE;
    blas->scratchMem = VK_NULL_HANDLE;
    blas->isValid = false;

    blas->geomCount = 1;
    blas->geomVertBufs = new VkBuffer[1]();
    blas->geomVertMems = new VkDeviceMemory[1]();
    blas->geomIdxBufs = new VkBuffer[1]();
    blas->geomIdxMems = new VkDeviceMemory[1]();

    if (useGpuBuffers)
    {
        // Zero-copy path: reference persistent vertex-cache/index-cache buffers.
        // Do not own/destroy these buffers from BLAS lifetime management.
        blas->geomVertBufs[0] = vertSrcBuf;
        blas->geomVertMems[0] = VK_NULL_HANDLE;
        blas->geomIdxBufs[0] = idxSrcBuf;
        blas->geomIdxMems[0] = VK_NULL_HANDLE;
    }
    else
    {
        // Fallback path: allocate host-visible geometry buffers for AS build input.
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkMemoryAllocateFlagsInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &fi;

        VkMemoryRequirements mr;
        void *ptr;

        // Vertex buffer
        bi.size = vertexDataSize;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &blas->geomVertBufs[0]));
        vkGetBufferMemoryRequirements(vk.device, blas->geomVertBufs[0], &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &blas->geomVertMems[0]));
        VK_CHECK(vkBindBufferMemory(vk.device, blas->geomVertBufs[0], blas->geomVertMems[0], 0));
        VK_CHECK(vkMapMemory(vk.device, blas->geomVertMems[0], 0, vertexDataSize, 0, &ptr));
        memcpy(ptr, tri->verts, (size_t)vertexDataSize);
        vkUnmapMemory(vk.device, blas->geomVertMems[0]);

        // Index buffer
        bi.size = indexDataSize;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &blas->geomIdxBufs[0]));
        vkGetBufferMemoryRequirements(vk.device, blas->geomIdxBufs[0], &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &blas->geomIdxMems[0]));
        VK_CHECK(vkBindBufferMemory(vk.device, blas->geomIdxBufs[0], blas->geomIdxMems[0], 0));
        VK_CHECK(vkMapMemory(vk.device, blas->geomIdxMems[0], 0, indexDataSize, 0, &ptr));
        memcpy(ptr, tri->indexes, (size_t)indexDataSize);
        vkUnmapMemory(vk.device, blas->geomIdxMems[0]);
    }

    // Describe geometry
    VkAccelerationStructureGeometryTrianglesDataKHR triData = {};
    triData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // xyz at offset 0 of idDrawVert
    triData.vertexData.deviceAddress = GetBufferDeviceAddress(blas->geomVertBufs[0]) + vertSrcOffset;
    triData.vertexStride = sizeof(idDrawVert);
    triData.maxVertex = (uint32_t)tri->numVerts - 1;
    triData.indexType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    triData.indexData.deviceAddress = GetBufferDeviceAddress(blas->geomIdxBufs[0]) + idxSrcOffset;
    triData.transformData.deviceAddress = 0; // identity — world transform goes in TLAS instance

    VkAccelerationStructureGeometryKHR asGeom = {};
    asGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    asGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeom.geometry.triangles = triData;
    asGeom.flags = isPerforated ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

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

    AllocASBuffer(sizeInfo.accelerationStructureSize, 0, &blas->buffer, &blas->memory);

    VkAccelerationStructureCreateInfoKHR asInfo = {};
    asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = blas->buffer;
    asInfo.size = sizeInfo.accelerationStructureSize;
    asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &asInfo, NULL, &blas->handle));

    // Scratch buffer kept in blas struct — freed at VK_RT_DestroyBLAS.
    AllocASBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &blas->scratchBuf, &blas->scratchMem);

    buildInfo.dstAccelerationStructure = blas->handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(blas->scratchBuf);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
    rangeInfo.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfo = &rangeInfo;

    // Record build into the caller's command buffer.
    // No per-BLAS barrier here — VK_RT_RebuildTLAS emits one barrier after all builds.
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    // Device address is a property of the AS object; valid immediately after creation.
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = blas->handle;
    blas->deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &addrInfo);

    blas->isValid = true;
    return blas;
}

// ---------------------------------------------------------------------------
// VK_RT_BuildBLASForModel
// Build a multi-geometry BLAS covering all non-translucent surfaces of a model.
// One geometry entry per surface — shadow rays intersect every surface material
// rather than only Surface(0).  Used by VK_RT_RebuildTLAS.
// ---------------------------------------------------------------------------

vkBLAS_t *VK_RT_BuildBLASForModel(idRenderModel *model, VkCommandBuffer cmd, vkBLAS_t *prevBlas)
{
    extern bool VK_VertexCache_GetBuffer(vertCache_t * block, VkBuffer * outBuf, VkDeviceSize * outOffset);

    if (!model || model->NumSurfaces() == 0)
        return NULL;

    // --- Gather valid (shadow-casting) surfaces ---
    struct SurfEntry
    {
        const srfTriangles_t *geo;
        bool perforated;
    };
    static const int MAX_BLAS_SURFACES = 512;
    static SurfEntry validSurfs[MAX_BLAS_SURFACES];
    static bool validSurfUseGpu[MAX_BLAS_SURFACES];
    int validCount = 0;
    int droppedByCap = 0;
    const int numSurfaces = model->NumSurfaces();

    for (int s = 0; s < numSurfaces; s++)
    {
        if (validCount >= MAX_BLAS_SURFACES)
        {
            droppedByCap++;
            continue;
        }

        const modelSurface_t *surf = model->Surface(s);
        if (!surf || !surf->geometry)
            continue;
        const srfTriangles_t *geo = surf->geometry;
        if (geo->numVerts == 0 || geo->numIndexes == 0)
            continue;
        VkBuffer gpuVertBuf = VK_NULL_HANDLE, gpuIdxBuf = VK_NULL_HANDLE;
        VkDeviceSize gpuVertOff = 0, gpuIdxOff = 0;
        const bool haveGpuVerts =
            geo->ambientCache && VK_VertexCache_GetBuffer(geo->ambientCache, &gpuVertBuf, &gpuVertOff);
        const bool haveGpuIdx = geo->indexCache && VK_VertexCache_GetBuffer(geo->indexCache, &gpuIdxBuf, &gpuIdxOff);
        const bool haveGpuGeom = haveGpuVerts && haveGpuIdx;

        const void *cpuVerts = geo->verts ? (const void *)geo->verts
                                          : (geo->ambientCache ? vertexCache.Position(geo->ambientCache) : NULL);
        const void *cpuIdx = geo->indexes ? (const void *)geo->indexes
                                          : (geo->indexCache ? vertexCache.Position(geo->indexCache) : NULL);
        const bool haveCpuGeom = (cpuVerts != NULL) && (cpuIdx != NULL);

        if (!haveGpuGeom && !haveCpuGeom)
            continue;
        // Translucent surfaces never cast shadows — same rule as GL stencil path.
        if (surf->shader && surf->shader->Coverage() == MC_TRANSLUCENT)
            continue;
        validSurfs[validCount].geo = geo;
        validSurfs[validCount].perforated = surf->shader && surf->shader->Coverage() == MC_PERFORATED;
        validSurfUseGpu[validCount] = haveGpuGeom;
        validCount++;
    }

    if (droppedByCap > 0)
    {
        common->Printf(
            "VK RT BLAS WARNING: model '%s' exceeded %d surfaces; truncated %d surface(s) (numSurfaces=%d).\n",
            model->Name(), MAX_BLAS_SURFACES, droppedByCap, numSurfaces);
    }

    if (validCount == 0)
        return NULL;

    {
        int gpuGeomCount = 0;
        int cpuGeomCount = 0;
        for (int i = 0; i < validCount; i++)
        {
            if (validSurfUseGpu[i])
                gpuGeomCount++;
            else
                cpuGeomCount++;
        }
        VK_RT_AccumulateBLASBuildStats(gpuGeomCount, cpuGeomCount, true);
    }

    // ---------------------------------------------------------------------------
    // BLAS update path: reuse existing buffers and AS when geometry is compatible.
    // Avoids per-frame alloc/free cycle and keeps device address stable so the
    // static TLAS signature does not churn on dynamic entity BLASes.
    // Only valid for CPU-path (owned) buffers; GPU-cache buffers are managed
    // externally and have stable addresses already.
    // ---------------------------------------------------------------------------
    if (prevBlas && prevBlas->isValid && prevBlas->geomCount == (uint32_t)validCount && prevBlas->geomPrimCounts &&
        prevBlas->geomVertSizes && prevBlas->geomVertMems)
    {
        bool compatible = true;
        for (int i = 0; i < validCount && compatible; i++)
        {
            const srfTriangles_t *geo = validSurfs[i].geo;
            if (prevBlas->geomPrimCounts[i] != (uint32_t)(geo->numIndexes / 3) ||
                prevBlas->geomVertSizes[i] != (VkDeviceSize)geo->numVerts * sizeof(idDrawVert) ||
                prevBlas->geomVertMems[i] == VK_NULL_HANDLE || prevBlas->geomIdxMems[i] == VK_NULL_HANDLE ||
                prevBlas->geomIdxSizes[i] != (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t))
            {
                compatible = false;
            }
        }

        if (compatible)
        {
            static VkAccelerationStructureGeometryKHR updateGeoms[MAX_BLAS_SURFACES];
            static VkAccelerationStructureBuildRangeInfoKHR updateRanges[MAX_BLAS_SURFACES];
            for (int i = 0; i < validCount; i++)
            {
                const srfTriangles_t *geo = validSurfs[i].geo;
                const void *cpuVerts = geo->verts
                                           ? (const void *)geo->verts
                                           : (geo->ambientCache ? vertexCache.Position(geo->ambientCache) : NULL);
                const void *cpuIdx = geo->indexes ? (const void *)geo->indexes
                                                  : (geo->indexCache ? vertexCache.Position(geo->indexCache) : NULL);
                void *ptr;
                VK_CHECK(vkMapMemory(vk.device, prevBlas->geomVertMems[i], 0, prevBlas->geomVertSizes[i], 0, &ptr));
                memcpy(ptr, cpuVerts, (size_t)prevBlas->geomVertSizes[i]);
                vkUnmapMemory(vk.device, prevBlas->geomVertMems[i]);

                VK_CHECK(vkMapMemory(vk.device, prevBlas->geomIdxMems[i], 0, prevBlas->geomIdxSizes[i], 0, &ptr));
                memcpy(ptr, cpuIdx, (size_t)prevBlas->geomIdxSizes[i]);
                vkUnmapMemory(vk.device, prevBlas->geomIdxMems[i]);

                VkAccelerationStructureGeometryTrianglesDataKHR triData = {};
                triData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                triData.vertexData.deviceAddress = GetBufferDeviceAddress(prevBlas->geomVertBufs[i]);
                triData.vertexStride = sizeof(idDrawVert);
                triData.maxVertex = (uint32_t)geo->numVerts - 1;
                triData.indexType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                triData.indexData.deviceAddress = GetBufferDeviceAddress(prevBlas->geomIdxBufs[i]);
                triData.transformData.deviceAddress = 0;

                updateGeoms[i] = {};
                updateGeoms[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                updateGeoms[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                updateGeoms[i].geometry.triangles = triData;
                updateGeoms[i].flags = validSurfs[i].perforated ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

                updateRanges[i] = {};
                updateRanges[i].primitiveCount = prevBlas->geomPrimCounts[i];
            }

            VkAccelerationStructureBuildGeometryInfoKHR updateInfo = {};
            updateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            updateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            updateInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                               VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            updateInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            updateInfo.srcAccelerationStructure = prevBlas->handle;
            updateInfo.dstAccelerationStructure = prevBlas->handle;
            updateInfo.geometryCount = (uint32_t)validCount;
            updateInfo.pGeometries = updateGeoms;
            updateInfo.scratchData.deviceAddress = GetBufferDeviceAddress(prevBlas->scratchBuf);

            const VkAccelerationStructureBuildRangeInfoKHR *pRanges = updateRanges;
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &updateInfo, &pRanges);

            VK_RT_AccumulateBLASBuildStats(0, validCount, true);
            return prevBlas; // device address unchanged — TLAS signature stable
        }
    }

    vkBLAS_t *blas = new vkBLAS_t();
    blas->handle = VK_NULL_HANDLE;
    blas->buffer = VK_NULL_HANDLE;
    blas->memory = VK_NULL_HANDLE;
    blas->deviceAddress = 0;
    blas->scratchBuf = VK_NULL_HANDLE;
    blas->scratchMem = VK_NULL_HANDLE;
    blas->isValid = false;

    blas->geomCount = (uint32_t)validCount;
    blas->geomVertBufs = new VkBuffer[validCount]();
    blas->geomVertMems = new VkDeviceMemory[validCount]();
    blas->geomIdxBufs = new VkBuffer[validCount]();
    blas->geomIdxMems = new VkDeviceMemory[validCount]();
    blas->geomVertSizes = new VkDeviceSize[validCount]();
    blas->geomIdxSizes = new VkDeviceSize[validCount]();
    blas->geomPrimCounts = new uint32_t[validCount]();

    // Build geometry descriptors — one per valid surface
    static VkAccelerationStructureGeometryKHR asGeoms[MAX_BLAS_SURFACES];
    static uint32_t primCounts[MAX_BLAS_SURFACES];
    static VkDeviceSize vertOffsets[MAX_BLAS_SURFACES];
    static VkDeviceSize idxOffsets[MAX_BLAS_SURFACES];

    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkMemoryAllocateFlagsInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &fi;

    for (int i = 0; i < validCount; i++)
    {
        const srfTriangles_t *geo = validSurfs[i].geo;
        VkBuffer gpuVertBuf = VK_NULL_HANDLE, gpuIdxBuf = VK_NULL_HANDLE;
        VkDeviceSize gpuVertOff = 0, gpuIdxOff = 0;
        const bool haveGpuVerts =
            geo->ambientCache && VK_VertexCache_GetBuffer(geo->ambientCache, &gpuVertBuf, &gpuVertOff);
        const bool haveGpuIdx = geo->indexCache && VK_VertexCache_GetBuffer(geo->indexCache, &gpuIdxBuf, &gpuIdxOff);
        const bool useGpuBuffers = haveGpuVerts && haveGpuIdx;

        const void *cpuVerts = geo->verts ? (const void *)geo->verts
                                          : (geo->ambientCache ? vertexCache.Position(geo->ambientCache) : NULL);
        const void *cpuIdx = geo->indexes ? (const void *)geo->indexes
                                          : (geo->indexCache ? vertexCache.Position(geo->indexCache) : NULL);
        // Source was validated in gather loop above.

        VkDeviceSize vertSize = (VkDeviceSize)geo->numVerts * sizeof(idDrawVert);
        VkDeviceSize idxSize = (VkDeviceSize)geo->numIndexes * sizeof(glIndex_t);
        if (useGpuBuffers)
        {
            // Zero-copy geometry path: use cache buffers directly.
            blas->geomVertBufs[i] = gpuVertBuf;
            blas->geomVertMems[i] = VK_NULL_HANDLE;
            blas->geomIdxBufs[i] = gpuIdxBuf;
            blas->geomIdxMems[i] = VK_NULL_HANDLE;
            vertOffsets[i] = gpuVertOff;
            idxOffsets[i] = gpuIdxOff;
        }
        else
        {
            VkMemoryRequirements mr;
            void *ptr;

            // Fallback: build AS input geometry from CPU-visible pointers.
            bi.size = vertSize;
            VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &blas->geomVertBufs[i]));
            vkGetBufferMemoryRequirements(vk.device, blas->geomVertBufs[i], &mr);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &blas->geomVertMems[i]));
            VK_CHECK(vkBindBufferMemory(vk.device, blas->geomVertBufs[i], blas->geomVertMems[i], 0));
            VK_CHECK(vkMapMemory(vk.device, blas->geomVertMems[i], 0, vertSize, 0, &ptr));
            memcpy(ptr, cpuVerts, (size_t)vertSize);
            vkUnmapMemory(vk.device, blas->geomVertMems[i]);

            bi.size = idxSize;
            VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &blas->geomIdxBufs[i]));
            vkGetBufferMemoryRequirements(vk.device, blas->geomIdxBufs[i], &mr);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &blas->geomIdxMems[i]));
            VK_CHECK(vkBindBufferMemory(vk.device, blas->geomIdxBufs[i], blas->geomIdxMems[i], 0));
            VK_CHECK(vkMapMemory(vk.device, blas->geomIdxMems[i], 0, idxSize, 0, &ptr));
            memcpy(ptr, cpuIdx, (size_t)idxSize);
            vkUnmapMemory(vk.device, blas->geomIdxMems[i]);

            vertOffsets[i] = 0;
            idxOffsets[i] = 0;
        }

        VkAccelerationStructureGeometryTrianglesDataKHR triData = {};
        triData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // xyz at offset 0 of idDrawVert
        triData.vertexData.deviceAddress = GetBufferDeviceAddress(blas->geomVertBufs[i]) + vertOffsets[i];
        triData.vertexStride = sizeof(idDrawVert);
        triData.maxVertex = (uint32_t)geo->numVerts - 1;
        triData.indexType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        triData.indexData.deviceAddress = GetBufferDeviceAddress(blas->geomIdxBufs[i]) + idxOffsets[i];
        triData.transformData.deviceAddress = 0; // identity — world transform goes in TLAS instance

        asGeoms[i] = {};
        asGeoms[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        asGeoms[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        asGeoms[i].geometry.triangles = triData;
        asGeoms[i].flags = validSurfs[i].perforated ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

        primCounts[i] = (uint32_t)geo->numIndexes / 3;
        blas->geomVertSizes[i] = vertSize;
        blas->geomIdxSizes[i] = idxSize;
        blas->geomPrimCounts[i] = primCounts[i];
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = (uint32_t)validCount;
    buildInfo.pGeometries = asGeoms;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            primCounts, &sizeInfo);

    // Query update scratch too — updateScratchSize can exceed buildScratchSize on some drivers.
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    VkAccelerationStructureBuildSizesInfoKHR updateSizeInfo = {};
    updateSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            primCounts, &updateSizeInfo);
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    VkDeviceSize scratchSize = sizeInfo.buildScratchSize > updateSizeInfo.updateScratchSize
                                   ? sizeInfo.buildScratchSize
                                   : updateSizeInfo.updateScratchSize;

    AllocASBuffer(sizeInfo.accelerationStructureSize, 0, &blas->buffer, &blas->memory);

    VkAccelerationStructureCreateInfoKHR asInfo = {};
    asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = blas->buffer;
    asInfo.size = sizeInfo.accelerationStructureSize;
    asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &asInfo, NULL, &blas->handle));

    AllocASBuffer(scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &blas->scratchBuf, &blas->scratchMem);

    buildInfo.dstAccelerationStructure = blas->handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(blas->scratchBuf);

    // vkCmdBuildAccelerationStructuresKHR: for 1 buildInfo with N geometries,
    // ppBuildRangeInfos[0] must point to an array of N rangeInfos (one per geometry).
    static VkAccelerationStructureBuildRangeInfoKHR ranges[MAX_BLAS_SURFACES];
    for (int i = 0; i < validCount; i++)
    {
        ranges[i] = {};
        ranges[i].primitiveCount = primCounts[i];
    }
    const VkAccelerationStructureBuildRangeInfoKHR *pRanges = ranges;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRanges);

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = blas->handle;
    blas->deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &addrInfo);

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

    // Cached BLASes are owned by the model cache, not by individual entities.
    // VK_RT_ModelBLASCacheClear handles their destruction.
    if (blas->cachedByModel)
        return;

    // Defer destruction: in-flight command buffers may still reference this
    // BLAS via the TLAS.  Wait VK_MAX_FRAMES_IN_FLIGHT frames so all slots
    // have completed their GPU work before freeing.
    if (s_blasGarbageCount < BLAS_GARBAGE_MAX)
    {
        blasGarbageEntry_t &entry = s_blasGarbage[s_blasGarbageCount++];
        entry.blas = blas;
        entry.retireFrameCount = tr.frameCount + VK_MAX_FRAMES_IN_FLIGHT;
    }
    else
    {
        // Queue full — must free immediately (risk of device lost, but
        // better than leaking unboundedly).  This shouldn't happen in
        // practice with a 1024-entry queue.
        common->Warning("VK RT: BLAS garbage queue full, forcing immediate free");
        VK_RT_FreeBLASImmediate(blas);
    }
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

    // Multiple DrawViews can occur within a single frame (game world, HUD, PDA, etc.).
    // Each records commands into the same command buffer.  If we destroy+recreate the
    // TLAS for a later DrawView, the earlier trace commands (already recorded) reference
    // a freed acceleration structure → VK_ERROR_DEVICE_LOST.
    // Fix: only build the TLAS once per frameCount.  Subsequent DrawViews reuse it.
    vkTLAS_t &tlasRef = vkRT.tlas[vk.currentFrame];
    if (tlasRef.isValid && tlasRef.lastBuiltFrameCount == tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
        {
            common->Printf("VK RT TLAS: reusing — already built this frame (frameCount=%d)\n", tr.frameCount);
            fflush(NULL);
        }
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK RT TLAS: begin rebuild — frame=%u frameCount=%d\n", vk.currentFrame, tr.frameCount);
        fflush(NULL);
    }

    // Count and collect instances in static/dynamic buckets.
    // Static instances can be cached and skipped when unchanged.
    static VkAccelerationStructureInstanceKHR staticInstances[VK_RT_MAX_TLAS_INSTANCES];
    static VkAccelerationStructureInstanceKHR dynamicInstances[VK_RT_MAX_TLAS_INSTANCES];
    uint32_t staticCount = 0;
    uint32_t dynamicCount = 0;
    uint64_t staticSignature = 1469598103934665603ull; // FNV-1a 64 offset basis
    bool anyBLASBuilt = false;

    for (const viewEntity_t *vEntity = viewDef->viewEntitys;
         vEntity != NULL && (staticCount + dynamicCount) < VK_RT_MAX_TLAS_INSTANCES; vEntity = vEntity->next)
    {
        idRenderEntityLocal *ent = vEntity->entityDef;
        if (!ent)
            continue;

        // Match classic shadow suppression semantics for RT shadows.
        // Without this, first-person world weapon/viewmodel entities can still
        // contribute to the TLAS and cast disembodied RT shadows.
        if (ent->parms.suppressShadowInViewID && ent->parms.suppressShadowInViewID == viewDef->renderView.viewID)
            continue;
        if (ent->parms.weaponDepthHack)
            continue;

        // Build or rebuild BLAS for this entity if needed
        idRenderModel *model = ent->dynamicModel ? ent->dynamicModel : ent->parms.hModel;
        if (!model)
            continue;

        if (model->NumSurfaces() == 0)
            continue;

        bool needRebuild = !ent->blas || !ent->blas->isValid;

        // Dynamic/animated models must be rebuilt every frame
        if (ent->dynamicModel)
            needRebuild = true;

        // For static entities, check the model BLAS cache before building.
        // If the same hModel was already built this session, reuse that BLAS:
        // avoids BLAS rebuilds when the entity pointer churns (e.g. doors
        // destroyed/recreated each frame) and keeps the device address stable
        // so the static TLAS signature stops churning.
        if (needRebuild && !ent->dynamicModel)
        {
            idRenderModel *staticModel = ent->parms.hModel;
            vkBLAS_t *cached = staticModel ? VK_RT_ModelBLASCacheLookup(staticModel) : NULL;
            if (cached && cached->isValid)
            {
                ent->blas = cached;
                ent->blasFrameCount = tr.frameCount;
                needRebuild = false;
            }
        }

        if (needRebuild)
        {
            if (ent->dynamicModel && ent->blas)
            {
                // Try in-place BLAS update: reuses buffers and keeps device address
                // stable, avoiding static TLAS signature churn each frame.
                vkBLAS_t *updated = VK_RT_BuildBLASForModel(model, cmd, ent->blas);
                if (updated != ent->blas)
                    VK_RT_DestroyBLAS(ent->blas);
                ent->blas = updated;
            }
            else
            {
                if (ent->blas)
                    VK_RT_DestroyBLAS(ent->blas); // no-op for cached; safe to call
                // Build multi-geometry BLAS covering all non-translucent surfaces.
                // A single TLAS instance per entity, one geometry per surface.
                ent->blas = VK_RT_BuildBLASForModel(model, cmd);
                // Cache newly built BLAS for static entities so future entity
                // pointer churn reuses it without a rebuild.
                if (!ent->dynamicModel && ent->blas)
                    VK_RT_ModelBLASCacheInsert(ent->parms.hModel, ent->blas);
            }
            ent->blasFrameCount = tr.frameCount;
            if (ent->blas)
                anyBLASBuilt = true;
        }

        if (!ent->blas || !ent->blas->isValid)
            continue;

        const bool isDynamicInstance = (ent->dynamicModel != NULL);
        VkAccelerationStructureInstanceKHR *dst =
            isDynamicInstance ? &dynamicInstances[dynamicCount++] : &staticInstances[staticCount++];

        // Fill VkAccelerationStructureInstanceKHR
        VkAccelerationStructureInstanceKHR &inst = *dst;
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

        // instanceCustomIndex is patched after static/dynamic merge.
        inst.instanceCustomIndex = 0;
        // Player body entities get mask bit 0x01 so shadow rays can optionally
        // exclude them (e.g. when the player is directly under a light).
        // World geometry keeps all bits set (0xFF) so it is always intersected.
        inst.mask = ent->parms.noSelfShadow ? 0x01 : 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        // Per-surface opaque flags are encoded in the BLAS geometry entries; no instance-level override needed.
        inst.accelerationStructureReference = ent->blas->deviceAddress;

        if (!isDynamicInstance)
        {
            const uintptr_t entTag = (uintptr_t)ent;
            const uint64_t prevSig = staticSignature;
            uint64_t sigAfterEnt = VK_RT_HashFnv1a64_Bytes(prevSig, &entTag, sizeof(entTag));
            uint64_t sigAfterAddr = VK_RT_HashFnv1a64_Bytes(sigAfterEnt, &inst.accelerationStructureReference,
                                                            sizeof(inst.accelerationStructureReference));
            uint64_t sigAfterXfm =
                VK_RT_HashFnv1a64_Bytes(sigAfterAddr, &inst.transform.matrix[0][0], sizeof(inst.transform.matrix));
            staticSignature = sigAfterXfm;
        }
    }

    const uint32_t instanceCount = staticCount + dynamicCount;

    if (instanceCount == 0)
    {
        if (r_vkLogRT.GetInteger() >= 1)
        {
            common->Printf("VK RT TLAS: no instances this frame, skipping build\n");
            fflush(NULL);
        }
        return;
    }

    // One barrier for all BLAS builds recorded above.
    // Ensures every BLAS write is visible to the TLAS build that follows.
    if (anyBLASBuilt)
    {
        VkMemoryBarrier blasBarrier = {};
        blasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &blasBarrier, 0, NULL, 0,
                             NULL);
    }

    vkTLAS_t &tlas = vkRT.tlas[vk.currentFrame];

    // (Re)create instance buffer only when capacity is insufficient.
    VkDeviceSize instBufSize = instanceCount * sizeof(VkAccelerationStructureInstanceKHR);

    bool instanceBufferRecreated = false;
    if (tlas.instanceBuffer == VK_NULL_HANDLE || tlas.instanceBufferSize < instBufSize)
    {
        if (tlas.instanceBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, tlas.instanceBuffer, NULL);
            vkFreeMemory(vk.device, tlas.instanceMemory, NULL);
            tlas.instanceBuffer = VK_NULL_HANDLE;
            tlas.instanceMemory = VK_NULL_HANDLE;
            tlas.instanceBufferSize = 0;
        }

        // Instance buffer must be host-visible so we can fill it each frame
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
        tlas.instanceBufferSize = instBufSize;
        instanceBufferRecreated = true;
    }

    vkRTStaticInstanceCache_t &staticCache = s_staticInstanceCache[vk.currentFrame];
    const bool staticBlockChanged =
        (!staticCache.valid || staticCache.count != staticCount || staticCache.signature != staticSignature);
    const bool rewriteStatic = staticBlockChanged || instanceBufferRecreated;

    // Copy instance data.
    // Static block is copied only when changed; dynamic block is copied every frame.
    void *ptr;
    VK_CHECK(vkMapMemory(vk.device, tlas.instanceMemory, 0, instBufSize, 0, &ptr));
    uint8_t *dstBytes = (uint8_t *)ptr;
    const size_t staticBytes = (size_t)staticCount * sizeof(VkAccelerationStructureInstanceKHR);
    const size_t dynamicBytes = (size_t)dynamicCount * sizeof(VkAccelerationStructureInstanceKHR);

    if (rewriteStatic && staticCount > 0)
    {
        for (uint32_t i = 0; i < staticCount; i++)
            staticInstances[i].instanceCustomIndex = i;

        memcpy(dstBytes, staticInstances, staticBytes);
        memcpy(staticCache.instances, staticInstances, staticBytes);
        staticCache.valid = true;
        staticCache.count = staticCount;
        staticCache.signature = staticSignature;
    }
    else if (rewriteStatic)
    {
        staticCache.valid = true;
        staticCache.count = 0;
        staticCache.signature = staticSignature;
    }

    if (dynamicCount > 0)
    {
        for (uint32_t i = 0; i < dynamicCount; i++)
            dynamicInstances[i].instanceCustomIndex = staticCount + i;
        memcpy(dstBytes + staticBytes, dynamicInstances, dynamicBytes);
    }

    vkUnmapMemory(vk.device, tlas.instanceMemory);

    tlas.instanceCount = instanceCount;

    if (r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK RT TLAS: building — instances=%u (static=%u dynamic=%u) blasBuiltThisFrame=%s\n",
                       instanceCount, staticCount, dynamicCount, anyBLASBuilt ? "yes" : "no");
        fflush(NULL);

        const size_t uploadedBytes = (rewriteStatic ? staticBytes : 0) + dynamicBytes;
        common->Printf("VK RT TLAS UPLOAD: frameCount=%d staticRewritten=%s uploadedBytes=%u\n", tr.frameCount,
                       rewriteStatic ? "yes" : "no", (unsigned int)uploadedBytes);
        fflush(NULL);

        // One lightweight summary line per frame: BLAS geometry source mix.
        // Useful for confirming Optimization 1 rollout across real maps.
        if (s_blasBuildStats.summaryLoggedFrameCount != tr.frameCount)
        {
            const int totalGeom = s_blasBuildStats.gpuGeomCount + s_blasBuildStats.cpuGeomCount;
            const float gpuPct =
                (totalGeom > 0) ? (100.0f * (float)s_blasBuildStats.gpuGeomCount / (float)totalGeom) : 0.0f;
            common->Printf(
                "VK RT BLAS SRC: frameCount=%d geoms(total=%d gpu=%d cpu=%d gpuPct=%.1f) builds(single=%d model=%d)\n",
                tr.frameCount, totalGeom, s_blasBuildStats.gpuGeomCount, s_blasBuildStats.cpuGeomCount, gpuPct,
                s_blasBuildStats.singleBuildCount, s_blasBuildStats.modelBuildCount);
            fflush(NULL);
            s_blasBuildStats.summaryLoggedFrameCount = tr.frameCount;
        }
    }

    // Barrier: ensure host-mapped instance writes are visible to AS build.
    // Instance data is written via vkMapMemory/memcpy (HOST writes), not transfer commands.
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
                         &barrier, 0, NULL, 0, NULL);

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

    // Recreate TLAS storage only if missing or too small for current build size.
    if (tlas.handle == VK_NULL_HANDLE || tlas.bufferSize < sizeInfo.accelerationStructureSize)
    {
        if (tlas.handle != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(vk.device, tlas.handle, NULL);
            vkDestroyBuffer(vk.device, tlas.buffer, NULL);
            vkFreeMemory(vk.device, tlas.memory, NULL);
            tlas.handle = VK_NULL_HANDLE;
            tlas.buffer = VK_NULL_HANDLE;
            tlas.memory = VK_NULL_HANDLE;
            tlas.bufferSize = 0;
        }

        AllocASBuffer(sizeInfo.accelerationStructureSize, 0, &tlas.buffer, &tlas.memory);

        VkAccelerationStructureCreateInfoKHR asInfo = {};
        asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        asInfo.buffer = tlas.buffer;
        asInfo.size = sizeInfo.accelerationStructureSize;
        asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &asInfo, NULL, &tlas.handle));

        tlas.bufferSize = sizeInfo.accelerationStructureSize;
    }

    // Recreate scratch buffer only if missing or too small.
    if (tlas.scratchBuffer == VK_NULL_HANDLE || tlas.scratchBufferSize < sizeInfo.buildScratchSize)
    {
        if (tlas.scratchBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, tlas.scratchBuffer, NULL);
            vkFreeMemory(vk.device, tlas.scratchMemory, NULL);
            tlas.scratchBuffer = VK_NULL_HANDLE;
            tlas.scratchMemory = VK_NULL_HANDLE;
            tlas.scratchBufferSize = 0;
        }
        AllocASBuffer(sizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &tlas.scratchBuffer,
                      &tlas.scratchMemory);
        tlas.scratchBufferSize = sizeInfo.buildScratchSize;
    }

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
    tlas.lastBuiltFrameCount = tr.frameCount;
}

// ---------------------------------------------------------------------------
// VK_RT_Init / VK_RT_Shutdown
// Called once after device creation (when rayTracingSupported is true).
// The shadow pipeline and shadow mask are set up in vk_shadows.cpp.
// ---------------------------------------------------------------------------

void VK_RT_Init(void)
{
    memset(&vkRT, 0, sizeof(vkRT));
    // Defensive reset for restart/crash-recovery paths: old deferred queue entries
    // belong to a previous device lifetime and must never be reused.
    memset(s_blasGarbage, 0, sizeof(s_blasGarbage));
    s_blasGarbageCount = 0;
    // Reset model BLAS cache without freeing — old device handles are invalid.
    memset(s_modelBLASCache, 0, sizeof(s_modelBLASCache));
    s_modelBLASCacheCount = 0;
    // Clear any stale per-entity BLAS pointers that may have survived a failed restart.
    VK_RT_ClearWorldEntityBLASPointers(false);
    VK_RT_LoadFunctionPointers();
    common->Printf("VK RT: acceleration structure support initialized\n");
}

void VK_RT_Shutdown(void)
{
    vkDeviceWaitIdle(vk.device);

    // Destroy live per-entity BLAS objects that are not in the deferred queue,
    // then clear all entity pointers so the next device lifetime starts clean.
    VK_RT_ClearWorldEntityBLASPointers(true);

    // Flush deferred BLAS deletions (device is idle, all are safe to free)
    for (int i = 0; i < s_blasGarbageCount; i++)
        VK_RT_FreeBLASImmediate(s_blasGarbage[i].blas);
    memset(s_blasGarbage, 0, sizeof(s_blasGarbage));
    s_blasGarbageCount = 0;

    // Destroy TLAS (one per frame-in-flight slot)
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkTLAS_t &tlas = vkRT.tlas[i];
        if (tlas.handle != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(vk.device, tlas.handle, NULL);
            vkDestroyBuffer(vk.device, tlas.buffer, NULL);
            vkFreeMemory(vk.device, tlas.memory, NULL);
        }
        if (tlas.instanceBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, tlas.instanceBuffer, NULL);
            vkFreeMemory(vk.device, tlas.instanceMemory, NULL);
        }
        if (tlas.scratchBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, tlas.scratchBuffer, NULL);
            vkFreeMemory(vk.device, tlas.scratchMemory, NULL);
        }
    }

    // Shadow mask images (including blur temp images)
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkShadowMask_t &sm = vkRT.shadowMask[i];
        if (sm.view != VK_NULL_HANDLE)
            vkDestroyImageView(vk.device, sm.view, NULL);
        if (sm.image != VK_NULL_HANDLE)
            vkDestroyImage(vk.device, sm.image, NULL);
        if (sm.memory != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, sm.memory, NULL);
        if (sm.blurTempView != VK_NULL_HANDLE)
            vkDestroyImageView(vk.device, sm.blurTempView, NULL);
        if (sm.blurTempImage != VK_NULL_HANDLE)
            vkDestroyImage(vk.device, sm.blurTempImage, NULL);
        if (sm.blurTempMemory != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, sm.blurTempMemory, NULL);
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
    if (vkRT.depthSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.depthSampler, NULL);
    }

    // Blur pipeline
    if (vkRT.blurPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(vk.device, vkRT.blurPipeline, NULL);
    if (vkRT.blurPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vk.device, vkRT.blurPipelineLayout, NULL);
    if (vkRT.blurDescLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vk.device, vkRT.blurDescLayout, NULL);
    if (vkRT.blurDescPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk.device, vkRT.blurDescPool, NULL);

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
        // Destroy blur temp image
        if (sm.blurTempView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, sm.blurTempView, NULL);
            sm.blurTempView = VK_NULL_HANDLE;
        }
        if (sm.blurTempImage != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, sm.blurTempImage, NULL);
            sm.blurTempImage = VK_NULL_HANDLE;
        }
        if (sm.blurTempMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, sm.blurTempMemory, NULL);
            sm.blurTempMemory = VK_NULL_HANDLE;
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
        imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

        // Create blur temp image (same format/size as shadow mask)
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &sm.blurTempImage));
        vkGetImageMemoryRequirements(vk.device, sm.blurTempImage, &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = VK_FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &sm.blurTempMemory));
        VK_CHECK(vkBindImageMemory(vk.device, sm.blurTempImage, sm.blurTempMemory, 0));

        viewInfo.image = sm.blurTempImage;
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &sm.blurTempView));

        // Transition both images to GENERAL layout for storage image use
        VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
        VK_TransitionImageLayout(cmd, sm.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        VK_TransitionImageLayout(cmd, sm.blurTempImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        VK_EndSingleTimeCommands(cmd);

        sm.width = width;
        sm.height = height;
        vkRT.shadowDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.blurDescSetLastUpdatedFrameCount[i] = -1;
    }
}
