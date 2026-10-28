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
#include "ui/UserInterface.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_gui_texture.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_upscale.h"
#include "renderer/Vulkan/vk_bloom.h"

#include <string.h>
#include <SDL.h>

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

// BLAS memory pools. Each vkAllocateMemory costs ~0.3 ms on Windows; one per surface plus
// two per BLAS made a 35-BLAS first-sight burst cost ~130 ms. Ranges are released only by
// VK_RT_FreeBLASImmediate, i.e. after the garbage clock retires the BLAS.
static vkBufferPool_t s_blasStoragePool("blasStorage",
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 16u * 1024u * 1024u, 256, false);
// Alignment is raised to minAccelerationStructureScratchOffsetAlignment in VK_RT_Init.
static vkBufferPool_t s_blasScratchPool("blasScratch",
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 8u * 1024u * 1024u, 256, false);
// Host-mapped: dynamic refits memcpy straight into their ranges every frame.
static vkBufferPool_t s_blasGeomPool("blasGeom",
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     8u * 1024u * 1024u, 64, true);

static idCVar r_vkBLASPoolCheck("r_vkBLASPoolCheck", "0", CVAR_RENDERER | CVAR_BOOL,
                                "Validate every BLAS pool range (alignment, bounds) when it is written or built");

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
    // r_vkRTProfile: per-frame BLAS work, read back via VK_RT_GetBLASFrameStats.
    int updateCount;
    int rebuildCount;
    int triCount;
    uint64_t cpuTicks;
};

static vkRTBlasBuildStats_t s_blasBuildStats = {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0};
static int s_tlasDynamicInstances = 0;

static idCVar r_vkLogTLASSlow("r_vkLogTLASSlow", "0", CVAR_RENDERER | CVAR_FLOAT,
                              "Print a section breakdown of VK_RT_RebuildTLAS CPU time on frames where it takes "
                              "longer than this many ms (0 = off)");

// Adds the ticks since *mark to *acc and advances the mark.
static ID_INLINE void VK_RT_LapTicks(uint64_t *mark, uint64_t *acc)
{
    const uint64_t now = SDL_GetPerformanceCounter();
    *acc += now - *mark;
    *mark = now;
}

extern idCVar r_rtVolMaxDist; // vk_vol.cpp — vol shadow-ray reach, reported by r_rtTlasDump

static idCVar r_rtTlasFallbackRadius("r_rtTlasFallbackRadius", "0", CVAR_RENDERER | CVAR_FLOAT,
                                     "Pass-2 radius for adding off-screen static geometry to the TLAS. 0 = auto: "
                                     "max(2 x r_rtGIRadius, 2 x r_rtVolMaxDist, r_rtReflectionDistance).");

extern float g_vkVolMaxLightReach; // vk_gi.cpp — largest reach among vol-admitted lights (previous frame)

// Pass-2 cull radius: the longest ray reach of any TLAS consumer, so that occlusion
// does not change with camera yaw.  The vol term is the farthest admitted light from
// the farthest march sample (volMaxDist + light reach), capped to keep the TLAS bounded.
static float VK_RT_Pass2CullRadius()
{
    const float manual = r_rtTlasFallbackRadius.GetFloat();
    if (manual > 0.0f)
        return manual;
    const float volLightReach = Min(r_rtVolMaxDist.GetFloat() + g_vkVolMaxLightReach, 6000.0f);
    return Max(Max(Max(2.0f * r_rtGIRadius.GetFloat(), 2.0f * r_rtVolMaxDist.GetFloat()), volLightReach),
               cvarSystem->GetCVarFloat("r_rtReflectionDistance"));
}

static const int VK_RT_MAX_TLAS_INSTANCES = 4096;

// ---------------------------------------------------------------------------
// TLAS build instrumentation.
//
// The fallback passes exist to re-add occluders that portal/frustum culling
// dropped, so a zero count there is a silent correctness bug: rays simply pass
// through geometry that is not in the structure.  Counting admits alone can't
// tell "nothing to add" from "everything rejected", hence the reject tally.
// ---------------------------------------------------------------------------
struct vkRTTlasPassStats_t
{
    uint32_t pass1, pass2, pass3;    // instances admitted per pass
    uint32_t pass1World, pass2World; // of those, world BSP areas (_area<N>)
    uint32_t worldAreasInMap;        // total _area<N> entityDefs
    // Pass-2 rejects, by the check that dropped the entity.
    uint32_t rejDist, rejAlreadyAdded, rejDynamic, rejViewLocal, rejNoModel, rejNoCachedBlas, rejGeomFull;
    uint32_t rejDistWorld;       // world areas lost to the distance cull alone
    float nearestRejWorldOrigin; // origin-based distance of the closest such area
    float nearestRejWorldBounds; // same area measured against its bounds
};

// Distance from a point to the entity's world-space bounds, 0 when inside.
// referenceBounds is LOCAL (tr_local.h:288) and there is no cached world-space
// copy, so the corners go through modelMatrix here.
static float VK_RT_EntityBoundsDistance(const idRenderEntityLocal *ent, const idVec3 &p)
{
    if (ent->referenceBounds.IsCleared())
        return idMath::INFINITY;

    idBounds world;
    world.Clear();
    for (int i = 0; i < 8; i++)
    {
        idVec3 v, g;
        v[0] = ent->referenceBounds[i & 1][0];
        v[1] = ent->referenceBounds[(i >> 1) & 1][1];
        v[2] = ent->referenceBounds[(i >> 2) & 1][2];
        R_LocalPointToGlobal(ent->modelMatrix, v, g);
        world.AddPoint(g);
    }

    idVec3 d(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 3; i++)
    {
        if (p[i] < world[0][i])
            d[i] = world[0][i] - p[i];
        else if (p[i] > world[1][i])
            d[i] = p[i] - world[1][i];
    }
    return d.Length();
}

static bool VK_RT_IsWorldAreaEntity(const idRenderEntityLocal *ent)
{
    return ent->parms.hModel != NULL && ent->parms.hModel->IsStaticWorldModel();
}

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

static const int VK_RT_MODEL_BLAS_CACHE_MAX = 2048;

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
        s_blasBuildStats.updateCount = 0;
        s_blasBuildStats.rebuildCount = 0;
        s_blasBuildStats.triCount = 0;
        s_blasBuildStats.cpuTicks = 0;
    }
}

// Accumulates the CPU time of one VK_RT_BuildBLASForModel call across its return paths.
struct vkBLASCpuTimer_t
{
    uint64_t start;
    vkBLASCpuTimer_t()
    {
        VK_RT_ResetBLASBuildStatsIfNewFrame();
        start = SDL_GetPerformanceCounter();
    }
    ~vkBLASCpuTimer_t()
    {
        s_blasBuildStats.cpuTicks += SDL_GetPerformanceCounter() - start;
    }
};

void VK_RT_GetBLASFrameStats(vkRTBlasFrameStats_t *out)
{
    VK_RT_ResetBLASBuildStatsIfNewFrame();
    const uint64_t freq = SDL_GetPerformanceFrequency();
    out->updates = s_blasBuildStats.updateCount;
    out->rebuilds = s_blasBuildStats.rebuildCount;
    out->tris = s_blasBuildStats.triCount;
    out->dynamicInstances = s_tlasDynamicInstances;
    out->cpuMs = freq ? (double)s_blasBuildStats.cpuTicks * 1000.0 / (double)freq : 0.0;
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
    VK_Pool_Free(&s_blasStoragePool, &blas->storage);
    VK_Pool_Free(&s_blasScratchPool, &blas->scratch);
    // Zero-copy geometry has empty ranges (vertex cache owns it); Free ignores those.
    for (uint32_t i = 0; i < blas->geomCount; i++)
    {
        if (blas->geomVertRanges)
            VK_Pool_Free(&s_blasGeomPool, &blas->geomVertRanges[i]);
        if (blas->geomIdxRanges)
            VK_Pool_Free(&s_blasGeomPool, &blas->geomIdxRanges[i]);
    }
    delete[] blas->geomVertRanges;
    delete[] blas->geomIdxRanges;
    delete[] blas->geomVertSizes;
    delete[] blas->geomIdxSizes;
    delete[] blas->geomPrimCounts;
    delete[] blas->geomVertAddrs;
    delete[] blas->geomIdxAddrs;
    delete[] blas->geomShaders;
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

// Scratch ranges of static BLASes, released once their build command has retired.
// Same frame-count clock as the BLAS garbage queue.
struct vkScratchGarbage_t
{
    vkPoolAlloc_t range;
    int retireFrameCount;
};
static idList<vkScratchGarbage_t> s_scratchGarbage;

static void VK_RT_DeferScratchFree(vkPoolAlloc_t *range)
{
    vkScratchGarbage_t g;
    g.range = *range;
    g.retireFrameCount = tr.frameCount + VK_MAX_FRAMES_IN_FLIGHT;
    s_scratchGarbage.Append(g);
    memset(range, 0, sizeof(*range));
}

// force = true frees everything (device idle: level load, shutdown).
static void VK_RT_DrainScratchGarbage(bool force)
{
    int kept = 0;
    for (int i = 0; i < s_scratchGarbage.Num(); i++)
    {
        if (force || tr.frameCount >= s_scratchGarbage[i].retireFrameCount)
            VK_Pool_Free(&s_blasScratchPool, &s_scratchGarbage[i].range);
        else
            s_scratchGarbage[kept++] = s_scratchGarbage[i];
    }
    s_scratchGarbage.SetNum(kept, false);
}

// Drain BLAS garbage entries that are safe to free (called after fence wait).
void VK_RT_DrainBLASGarbage(void)
{
    VK_RT_DrainScratchGarbage(false);

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

// Log each GUI surface
// the BLAS filter sees, once per model/surface. Dynamic models rebuild often, so dedupe.
static void VK_RT_LogGuiBlasSurf(const idRenderModel *model, int surfIdx, const idMaterial *shader, bool skipped)
{
    static idStrList logged;
    const idStr key = va("%s#%d", model->Name(), surfIdx);
    if (logged.FindIndex(key) >= 0 || logged.Num() >= 4096)
        return;
    logged.Append(key);

    const idUserInterface *global = shader->GlobalGui();
    common->Printf("VK RT GUI SURF: %s model='%s' surf=%d shader='%s' gui=%s stages=%d coverage=%d\n",
                   skipped ? "SKIPPED" : "admitted", model->Name(), surfIdx, shader->GetName(),
                   global ? global->Name() : va("entity%d", shader->GetEntityGui()), shader->GetNumStages(),
                   (int)shader->Coverage());
}

// The MF_NOSHADOWS exclusion. Shared with the dynamic-instance address refresh, which must
// walk the same surfaces to keep BLAS geometry slots aligned. Real window glass stays for
// reflection rays; so do GUI surfaces (stage-less entityGui is translucent, hence noshadows),
// which reflections show via their live texture.
static bool VK_RT_NoShadowsExcluded(const idMaterial *shader)
{
    if (!shader->TestMaterialFlag(MF_NOSHADOWS))
        return false;
    if (shader->Coverage() != MC_TRANSLUCENT)
        return true;
    return !VK_RT_IsGlassPane(shader) && !shader->HasGui();
}

// Point a GUI surface's material entry at its live texture slot, resolving the GUI per
// instance as tr_light.cpp does.  Without a slot the entry keeps GUI_SURF only and any-hit
// ignores it.  Returns a value for the static signature so slot changes rewrite entries.
static uint32_t VK_RT_BindGuiMaterial(VkMaterialEntry &e, const idMaterial *shader, const idRenderEntityLocal *ent)
{
    if (!shader || (e.flags & VK_MAT_FLAG_GUI_SURF) == 0)
        return 0;

    // A screen is opaque emission, never a glass interface.
    e.flags &= ~(VK_MAT_FLAG_GLASS | VK_MAT_FLAG_REAL_GLASS);

    const idUserInterface *gui = NULL;
    const int guiNum = shader->GetEntityGui() - 1;
    if (ent && guiNum >= 0 && guiNum < MAX_RENDERENTITY_GUI)
        gui = ent->parms.gui[guiNum];
    if (!gui)
        gui = shader->GlobalGui();

    const int slot = VK_GuiTex_FindSlot(gui);
    idImage *img = VK_GuiTex_SlotImage(slot);
    int w = 0, h = 0;
    if (!img || !VK_Image_GetExtent(img, &w, &h) || w != VK_GUITEX_WIDTH || h != VK_GUITEX_HEIGHT)
        return 0xFFFFu;

    e.emissiveTexIndex = VK_RT_GetOrAssignTexIndex(img);
    e.flags = (e.flags | VK_MAT_FLAG_EMISSIVE | VK_MAT_FLAG_GUI_LIVE) & ~VK_MAT_FLAG_GUI_EMISSIVE;
    return ((uint32_t)slot + 1u) | (e.emissiveTexIndex << 16);
}

// ---------------------------------------------------------------------------
// VK_RT_BuildBLASForModel
// Build a multi-geometry BLAS covering all non-translucent surfaces of a model.
// One geometry entry per surface — shadow rays intersect every surface material
// rather than only Surface(0).  Used by VK_RT_RebuildTLAS.
// ---------------------------------------------------------------------------

vkBLAS_t *VK_RT_BuildBLASForModel(idRenderModel *model, VkCommandBuffer cmd, vkBLAS_t *prevBlas, bool allowUpdate)
{
    extern bool VK_VertexCache_GetBuffer(vertCache_t * block, VkBuffer * outBuf, VkDeviceSize * outOffset);

    if (!model || model->NumSurfaces() == 0)
        return NULL;

    vkBLASCpuTimer_t cpuTimer;
    VK_RTProfile_BLASGpuBegin(cmd);

    // --- Gather valid (shadow-casting) surfaces ---
    struct SurfEntry
    {
        const srfTriangles_t *geo;
        const idMaterial *shader;
        bool perforated;
        bool translucent;
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

        // Not TLAS occluders: MF_POLYGONOFFSET decals (coplanar, self-hit by nearby rays),
        // MF_NOSHADOWS opt-outs, and particle/sprite/flare billboards (no thickness).
        if (surf->shader)
        {
            if (surf->shader->TestMaterialFlag(MF_POLYGONOFFSET))
                continue;
            // MF_NOSHADOWS normally skips a surface; window glass and GUI surfaces are
            // kept (VK_RT_NoShadowsExcluded).  Translucent smoke, particles and flares
            // stay out — the RT pipeline would render them as opaque white boxes.
            const bool noShadowsExcluded = VK_RT_NoShadowsExcluded(surf->shader);
            if (surf->shader->HasGui() && r_vkLogRT.GetInteger() >= 1)
                VK_RT_LogGuiBlasSurf(model, s, surf->shader, noShadowsExcluded);
            if (noShadowsExcluded)
                continue;
            const deform_t def = surf->shader->Deform();
            if (def == DFRM_PARTICLE || def == DFRM_PARTICLE2 || def == DFRM_SPRITE || def == DFRM_FLARE)
                continue;
            // Sprites and beams carry tr.defaultMaterial and only look right via the entity's
            // customShader at raster time; in RT they would be opaque white boxes.
            if (surf->shader == tr.defaultMaterial)
                continue;
        }
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
        {
            // Diagnostic: log translucent surfaces that fail the geometry check so we
            // can determine whether the problem is verts/indexes being null, or
            // ambientCache having no GPU buffer.
            if (surf->shader && surf->shader->Coverage() == MC_TRANSLUCENT && r_vkLogRT.GetInteger() >= 1)
            {
                common->Printf("VK RT GLASS SKIP: model='%s' surf=%d shader='%s'"
                               " verts=%s idx=%s ambCache=%s gpuV=%d gpuI=%d\n",
                               model->Name(), s, surf->shader->GetName(), geo->verts ? "yes" : "NO",
                               geo->indexes ? "yes" : "NO", geo->ambientCache ? "yes" : "NO", haveGpuVerts, haveGpuIdx);
                fflush(NULL);
            }
            continue;
        }
        validSurfs[validCount].geo = geo;
        validSurfs[validCount].shader = surf->shader;
        validSurfs[validCount].perforated = surf->shader && surf->shader->Coverage() == MC_PERFORATED;
        // Translucent (glass) surfaces: include in BLAS as non-opaque so rahit is invoked.
        // shadow_ray.rahit passes through glass; reflect_ray.rchit applies the Fresnel split.
        // Stage-less GUI surfaces are translucent too, so the any-hit shaders decide per ray
        // type whether a screen counts (MAT_FLAG_GUI_SURF / GUI_LIVE).
        validSurfs[validCount].translucent = surf->shader && surf->shader->Coverage() == MC_TRANSLUCENT;
        if (validSurfs[validCount].translucent && !surf->shader->HasGui() && r_vkLogRT.GetInteger() >= 1)
        {
            common->Printf("VK RT GLASS: model='%s' surf=%d shader='%s' gpu=%s verts=%d idx=%d\n", model->Name(), s,
                           surf->shader ? surf->shader->GetName() : "<null>", haveGpuGeom ? "yes" : "no", geo->numVerts,
                           geo->numIndexes);
            fflush(NULL);
        }
        validSurfUseGpu[validCount] = haveGpuGeom;
        if (r_vkLogRT.GetInteger() >= 2)
        {
            common->Printf("VK RT BLAS SURF: model='%s' surf=%d shader='%s' cov=%d noshadow=%d\n", model->Name(), s,
                           surf->shader ? surf->shader->GetName() : "<null>",
                           surf->shader ? (int)surf->shader->Coverage() : -1,
                           surf->shader ? (int)surf->shader->TestMaterialFlag(MF_NOSHADOWS) : -1);
            fflush(NULL);
        }
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
    // BLAS update path: reuse buffers and AS when geometry is compatible, avoiding per-frame
    // alloc/free and keeping the device address stable.  Owned (CPU-path) buffers only.
    // ---------------------------------------------------------------------------
    if (prevBlas && prevBlas->isValid && prevBlas->allowUpdate && prevBlas->geomCount == (uint32_t)validCount &&
        prevBlas->geomPrimCounts && prevBlas->geomVertSizes && prevBlas->geomVertRanges)
    {
        bool compatible = true;
        for (int i = 0; i < validCount && compatible; i++)
        {
            const srfTriangles_t *geo = validSurfs[i].geo;
            if (prevBlas->geomPrimCounts[i] != (uint32_t)(geo->numIndexes / 3) ||
                prevBlas->geomVertSizes[i] != (VkDeviceSize)geo->numVerts * sizeof(idDrawVert) ||
                !VK_Pool_Mapped(&s_blasGeomPool, prevBlas->geomVertRanges[i]) ||
                !VK_Pool_Mapped(&s_blasGeomPool, prevBlas->geomIdxRanges[i]) ||
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
                if (r_vkBLASPoolCheck.GetBool())
                {
                    VK_Pool_Check(&s_blasGeomPool, prevBlas->geomVertRanges[i], prevBlas->geomVertSizes[i]);
                    VK_Pool_Check(&s_blasGeomPool, prevBlas->geomIdxRanges[i], prevBlas->geomIdxSizes[i]);
                }
                memcpy(VK_Pool_Mapped(&s_blasGeomPool, prevBlas->geomVertRanges[i]), cpuVerts,
                       (size_t)prevBlas->geomVertSizes[i]);
                memcpy(VK_Pool_Mapped(&s_blasGeomPool, prevBlas->geomIdxRanges[i]), cpuIdx,
                       (size_t)prevBlas->geomIdxSizes[i]);

                VkAccelerationStructureGeometryTrianglesDataKHR triData = {};
                triData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                triData.vertexData.deviceAddress = prevBlas->geomVertAddrs[i];
                triData.vertexStride = sizeof(idDrawVert);
                triData.maxVertex = (uint32_t)geo->numVerts - 1;
                triData.indexType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                triData.indexData.deviceAddress = prevBlas->geomIdxAddrs[i];
                triData.transformData.deviceAddress = 0;

                updateGeoms[i] = {};
                updateGeoms[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                updateGeoms[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                updateGeoms[i].geometry.triangles = triData;
                updateGeoms[i].flags =
                    (validSurfs[i].perforated || validSurfs[i].translucent) ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

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
            // Guard: if the scratch range is gone (shouldn't happen, but e.g. after a partial
            // shutdown or device-lost recovery) fall through to a full rebuild below
            // rather than passing address 0 to the GPU (which would cause a page fault).
            if (!VK_Pool_IsLive(&s_blasScratchPool, prevBlas->scratch))
                goto fullRebuild;
            updateInfo.scratchData.deviceAddress =
                GetBufferDeviceAddress(prevBlas->scratch.buf) + prevBlas->scratch.offset;

            const VkAccelerationStructureBuildRangeInfoKHR *pRanges = updateRanges;
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &updateInfo, &pRanges);

            // Geometry counts were already accumulated above; adding them again here
            // double-counted every update in the BLAS SRC log line.
            s_blasBuildStats.updateCount++;
            for (int i = 0; i < validCount; i++)
                s_blasBuildStats.triCount += (int)prevBlas->geomPrimCounts[i];
            return prevBlas; // device address unchanged — TLAS signature stable
        }
    }

fullRebuild:
    vkBLAS_t *blas = new vkBLAS_t(); // value-initialized: empty pool ranges, null handles

    blas->geomCount = (uint32_t)validCount;
    blas->geomVertRanges = new vkPoolAlloc_t[validCount]();
    blas->geomIdxRanges = new vkPoolAlloc_t[validCount]();
    blas->geomVertSizes = new VkDeviceSize[validCount]();
    blas->geomIdxSizes = new VkDeviceSize[validCount]();
    blas->geomPrimCounts = new uint32_t[validCount]();
    blas->geomVertAddrs = new VkDeviceAddress[validCount]();
    blas->geomIdxAddrs = new VkDeviceAddress[validCount]();
    blas->geomShaders = new const idMaterial *[validCount]();
    for (int i = 0; i < validCount; i++)
        blas->geomShaders[i] = validSurfs[i].shader;

    // Build geometry descriptors — one per valid surface
    static VkAccelerationStructureGeometryKHR asGeoms[MAX_BLAS_SURFACES];
    static uint32_t primCounts[MAX_BLAS_SURFACES];
    static VkBuffer vertBufs[MAX_BLAS_SURFACES];
    static VkBuffer idxBufs[MAX_BLAS_SURFACES];
    static VkDeviceSize vertOffsets[MAX_BLAS_SURFACES];
    static VkDeviceSize idxOffsets[MAX_BLAS_SURFACES];

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
            vertBufs[i] = gpuVertBuf;
            idxBufs[i] = gpuIdxBuf;
            vertOffsets[i] = gpuVertOff;
            idxOffsets[i] = gpuIdxOff;
        }
        else
        {
            // Fallback: copy AS input geometry from CPU-visible pointers into the geometry pool.
            VK_Pool_Alloc(&s_blasGeomPool, vertSize, &blas->geomVertRanges[i]);
            VK_Pool_Alloc(&s_blasGeomPool, idxSize, &blas->geomIdxRanges[i]);
            if (r_vkBLASPoolCheck.GetBool())
            {
                VK_Pool_Check(&s_blasGeomPool, blas->geomVertRanges[i], vertSize);
                VK_Pool_Check(&s_blasGeomPool, blas->geomIdxRanges[i], idxSize);
            }
            memcpy(VK_Pool_Mapped(&s_blasGeomPool, blas->geomVertRanges[i]), cpuVerts, (size_t)vertSize);
            memcpy(VK_Pool_Mapped(&s_blasGeomPool, blas->geomIdxRanges[i]), cpuIdx, (size_t)idxSize);

            vertBufs[i] = blas->geomVertRanges[i].buf;
            idxBufs[i] = blas->geomIdxRanges[i].buf;
            vertOffsets[i] = blas->geomVertRanges[i].offset;
            idxOffsets[i] = blas->geomIdxRanges[i].offset;
        }

        VkAccelerationStructureGeometryTrianglesDataKHR triData = {};
        triData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // xyz at offset 0 of idDrawVert
        triData.vertexData.deviceAddress = GetBufferDeviceAddress(vertBufs[i]) + vertOffsets[i];
        triData.vertexStride = sizeof(idDrawVert);
        triData.maxVertex = (uint32_t)geo->numVerts - 1;
        triData.indexType = (sizeof(glIndex_t) == 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        triData.indexData.deviceAddress = GetBufferDeviceAddress(idxBufs[i]) + idxOffsets[i];
        triData.transformData.deviceAddress = 0; // identity — world transform goes in TLAS instance

        // Store per-geometry addresses for the material table.
        blas->geomVertAddrs[i] = triData.vertexData.deviceAddress;
        blas->geomIdxAddrs[i] = triData.indexData.deviceAddress;

        asGeoms[i] = {};
        asGeoms[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        asGeoms[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        asGeoms[i].geometry.triangles = triData;
        asGeoms[i].flags = (validSurfs[i].perforated || validSurfs[i].translucent) ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

        primCounts[i] = (uint32_t)geo->numIndexes / 3;
        blas->geomVertSizes[i] = vertSize;
        blas->geomIdxSizes[i] = idxSize;
        blas->geomPrimCounts[i] = primCounts[i];
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Static BLASes never refit: no ALLOW_UPDATE gives the driver a smaller, faster-to-trace AS.
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      (allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = (uint32_t)validCount;
    buildInfo.pGeometries = asGeoms;
    blas->allowUpdate = allowUpdate;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                            primCounts, &sizeInfo);
    VkDeviceSize scratchSize = sizeInfo.buildScratchSize;

    if (allowUpdate)
    {
        // Query update scratch too — updateScratchSize can exceed buildScratchSize on some drivers.
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        VkAccelerationStructureBuildSizesInfoKHR updateSizeInfo = {};
        updateSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                                primCounts, &updateSizeInfo);
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        if (updateSizeInfo.updateScratchSize > scratchSize)
            scratchSize = updateSizeInfo.updateScratchSize;
    }

    VK_Pool_Alloc(&s_blasStoragePool, sizeInfo.accelerationStructureSize, &blas->storage);

    VkAccelerationStructureCreateInfoKHR asInfo = {};
    asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = blas->storage.buf;
    asInfo.offset = blas->storage.offset; // pool align 256 meets the spec's AS offset rule
    asInfo.size = sizeInfo.accelerationStructureSize;
    asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &asInfo, NULL, &blas->handle));

    VK_Pool_Alloc(&s_blasScratchPool, scratchSize, &blas->scratch);
    if (r_vkBLASPoolCheck.GetBool())
    {
        VK_Pool_Check(&s_blasStoragePool, blas->storage, sizeInfo.accelerationStructureSize);
        VK_Pool_Check(&s_blasScratchPool, blas->scratch, scratchSize);
    }

    buildInfo.dstAccelerationStructure = blas->handle;
    buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(blas->scratch.buf) + blas->scratch.offset;

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

    // A BLAS that never refits needs its scratch only until this build has executed.
    if (!allowUpdate)
        VK_RT_DeferScratchFree(&blas->scratch);

    s_blasBuildStats.rebuildCount++;
    for (int i = 0; i < validCount; i++)
        s_blasBuildStats.triCount += (int)primCounts[i];

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
// VK_RT_TLASActive
// Single source of truth for "the TLAS is live this frame".  Also called from
// the frontend to decide whether RT-only per-frame geometry work is needed.
// ---------------------------------------------------------------------------

bool VK_RT_TLASActive(void)
{
    if (!vk.rayTracingSupported || !vkRT.isInitialized || !r_useRayTracing.GetBool())
        return false;
    return r_rtShadows.GetBool() || r_rtAO.GetBool() || r_rtReflections.GetBool() || r_rtGI.GetBool();
}

// ---------------------------------------------------------------------------
// Player shadow suppression per light (renderLight_t::lightId).
//
// The game sets suppressShadowInLightID on the player body, head and the weapon's
// world model to the first-person muzzle flash's lightId — which is also the
// flashlight beam.  Interaction.cpp honours it for stencil shadows; the RT path
// cannot, because one TLAS serves every light, so the shadow pass turns it into a
// per-light cull mask instead.  Collected here because the player body is never a
// viewEntity in its own view (suppressSurfaceInViewID), so this is the only pass
// that actually enumerates it.
// ---------------------------------------------------------------------------

#define VK_RT_PLAYER_SUPPRESS_MAX 4

static int s_playerSuppressIds[VK_RT_PLAYER_SUPPRESS_MAX];
static int s_playerSuppressCount = 0;
static int s_playerSuppressFrame = -1;

static void VK_RT_NotePlayerSuppress(const idRenderEntityLocal *ent)
{
    const int id = ent->parms.suppressShadowInLightID;
    if (!ent->parms.rtPlayer || id == 0)
        return;

    for (int i = 0; i < s_playerSuppressCount; i++)
    {
        if (s_playerSuppressIds[i] == id)
            return;
    }
    if (s_playerSuppressCount < VK_RT_PLAYER_SUPPRESS_MAX)
        s_playerSuppressIds[s_playerSuppressCount++] = id;
}

bool VK_RT_PlayerSuppressesLight(int lightId)
{
    // Stale list (no TLAS rebuild this frame) reports no suppression, which is the
    // pre-existing behaviour rather than a wrong cull.
    if (lightId == 0 || s_playerSuppressFrame != tr.frameCount)
        return false;

    for (int i = 0; i < s_playerSuppressCount; i++)
    {
        if (s_playerSuppressIds[i] == lightId)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// VK_RT_RebuildTLAS
// Rebuild the Top-Level AS each frame from visible entities in viewDef.
// Each entity contributes one instance pointing to its BLAS.
// ---------------------------------------------------------------------------

void VK_RT_RebuildTLAS(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    extern bool VK_VertexCache_GetBuffer(vertCache_t * block, VkBuffer * outBuf, VkDeviceSize * outOffset);
    if (!viewDef)
        return;

    // Build once per frameCount: several DrawViews share the command buffer, and rebuilding
    // would free a TLAS that earlier recorded traces reference (device lost).
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

    // r_vkLogTLASSlow: section laps. BLAS ticks are read as deltas of the build timer, which
    // resets on the first build of a frame, hence the explicit reset first.
    const float slowLogMs = r_vkLogTLASSlow.GetFloat();
    const bool slowLog = slowLogMs > 0.0f;
    VK_RT_ResetBLASBuildStatsIfNewFrame();
    const uint64_t slowStart = SDL_GetPerformanceCounter();
    uint64_t slowLap = slowStart;
    uint64_t tPass1 = 0, tPass2 = 0, tPass3 = 0, tDynModel = 0, tMat1 = 0, tInst = 0, tMatTable = 0, tBuild = 0;
    const uint64_t blasTicks0 = s_blasBuildStats.cpuTicks;
    uint64_t blasTicks1 = blasTicks0, blasTicks3 = blasTicks0;
    bool tlasStorageRegrown = false, tlasScratchRegrown = false;
    vkRTBindlessFrameStats_t bindless0 = {};
    VK_RT_GetBindlessFrameStats(&bindless0);

    // Count and collect instances in static/dynamic buckets.
    // Static instances can be cached and skipped when unchanged.
    static VkAccelerationStructureInstanceKHR staticInstances[VK_RT_MAX_TLAS_INSTANCES];
    static VkAccelerationStructureInstanceKHR dynamicInstances[VK_RT_MAX_TLAS_INSTANCES];
    // Material entries, one per geometry; sized for every geometry slot filled.
    static VkMaterialEntry staticMatEntries[VK_MAT_MAX_GEOMS];
    static VkMaterialEntry dynamicMatEntries[VK_MAT_MAX_GEOMS];
    // Per-instance base geom index — needed to patch instanceCustomIndex after
    // the static/dynamic merge (dynamic entries shift by staticGeomCount).
    static uint32_t staticInstBaseGeom[VK_RT_MAX_TLAS_INSTANCES];
    static uint32_t dynamicInstBaseGeom[VK_RT_MAX_TLAS_INSTANCES];
    // Separate static/dynamic geometry address arrays.  After the loop, dynamic material entries' baseGeomIdx
    // values are patched to add the final staticGeomCount so that the GPU-side
    // flat SSBO has static [0..sGC-1] followed by dynamic [sGC..sGC+dGC-1].
    static uint64_t s_staticGeomVtxAddrs[VK_MAT_MAX_GEOMS];
    static uint64_t s_staticGeomIdxAddrs[VK_MAT_MAX_GEOMS];
    static uint64_t s_dynGeomVtxAddrs[VK_MAT_MAX_GEOMS];
    static uint64_t s_dynGeomIdxAddrs[VK_MAT_MAX_GEOMS];
    uint32_t staticGeomCount = 0;
    uint32_t dynamicGeomCount = 0;
    uint32_t staticCount = 0;
    uint32_t dynamicCount = 0;
    uint64_t staticSignature = 1469598103934665603ull; // FNV-1a 64 offset basis
    bool anyBLASBuilt = false;

    // Rebuilt with the instance list below, and only here — the early-out above keeps
    // the previous DrawView's list intact for the rest of this frame.
    s_playerSuppressCount = 0;
    s_playerSuppressFrame = tr.frameCount;

    vkRTTlasPassStats_t passStats = {};
    passStats.nearestRejWorldOrigin = idMath::INFINITY;
    passStats.nearestRejWorldBounds = idMath::INFINITY;

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
        // First-person weapon (view model): exclude from the TLAS entirely so it
        // doesn't appear in RT shadows or reflections. Mark blasFrameCount so the
        // nearby-dynamic pass (pass 3) also skips it via the dedup check.
        if (ent->parms.weaponDepthHack)
        {
            ent->blasFrameCount = tr.frameCount;
            continue;
        }
        // View-local entities (first-person body) duplicate world geometry already in the TLAS
        // and would cast a second shadow.  Stamped like above so pass 3 skips them too.
        if (ent->parms.allowSurfaceInViewID)
        {
            ent->blasFrameCount = tr.frameCount;
            continue;
        }

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

        // Static entities reuse a cached BLAS per hModel, so entity-pointer churn (doors) doesn't
        // rebuild it and the device address stays stable.
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
                // Static models go to the cache and never refit, so they build without ALLOW_UPDATE.
                ent->blas = VK_RT_BuildBLASForModel(model, cmd, NULL, ent->dynamicModel != NULL);
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
        if (VK_RT_IsWorldAreaEntity(ent))
            passStats.pass1World++;
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
        // Player geometry (renderEntity_t::rtPlayer) gets its own mask bit so rays can
        // exclude it with 0xFD. Everything else is bit 0, so a cullMask of 0x02 isolates
        // the player.
        inst.mask = ent->parms.rtPlayer ? VK_RT_MASK_PLAYER : VK_RT_MASK_WORLD;
        // Player body uses SBT offset 2 → player_reflect.rchit (main) and placeholder (probe, never fires).
        // World geometry uses offset 0 → reflect_ray.rchit (main) and glass_probe.rchit (probe).
        inst.instanceShaderBindingTableRecordOffset = ent->parms.rtPlayer ? 2 : 0;
        VK_RT_NotePlayerSuppress(ent);
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        // Per-surface opaque flags are encoded in the BLAS geometry entries; no instance-level override needed.
        inst.accelerationStructureReference = ent->blas->deviceAddress;

        // Per-surface material entries.  instanceCustomIndex = baseGeomIdx, so shaders use
        //   matIdx = gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT
        const uint64_t matStart = slowLog ? SDL_GetPerformanceCounter() : 0;
        {
            const uint32_t geomCount = ent->blas ? ent->blas->geomCount : 0;
            if (isDynamicInstance)
            {
                if (dynamicGeomCount + geomCount > VK_MAT_MAX_GEOMS)
                {
                    static int s_dynOverflowWarn = 0;
                    if (s_dynOverflowWarn++ < 4)
                        common->Warning("VK RT: dynamic geom address table overflow (count=%u geoms=%u limit=%u) — "
                                        "skipping instance",
                                        dynamicGeomCount, geomCount, VK_MAT_MAX_GEOMS);
                }
                else
                {
                    const uint32_t base = dynamicGeomCount;
                    dynamicInstBaseGeom[dynamicCount - 1] = base;

                    // Dynamic instances re-query addresses each frame: the vertex cache ring
                    // advances, so the offsets captured at BLAS build are stale.
                    idRenderModel *dynModel = ent->dynamicModel ? ent->dynamicModel : ent->parms.hModel;
                    uint32_t validSurfIdx = 0; // tracks which BLAS geometry slot each surface maps to

                    for (uint32_t g = 0; g < geomCount; g++)
                    {
                        const idMaterial *surfShader =
                            (ent->blas->geomShaders && ent->blas->geomShaders[g]) ? ent->blas->geomShaders[g] : NULL;
                        VkMaterialEntry matEntry = VK_RT_MakeMaterialEntry(surfShader, ent->blas, base + g,
                                                                           s_dynGeomVtxAddrs, s_dynGeomIdxAddrs);
                        matEntry.baseGeomIdx = base + g;
                        VK_RT_BindGuiMaterial(matEntry, surfShader, ent);
                        if (ent->parms.rtPlayer)
                            matEntry.flags |= VK_MAT_FLAG_PLAYER_BODY;
                        // Bounds check: max valid vertex index for rt_InterpolateUV.
                        if (ent->blas->geomVertSizes && ent->blas->geomVertSizes[g] >= sizeof(idDrawVert))
                            matEntry.maxVertex = (uint32_t)(ent->blas->geomVertSizes[g] / sizeof(idDrawVert)) - 1u;
                        // Bounds check: max valid triangle index — must be checked BEFORE the
                        // index-buffer read, not just the vertex indices it returns.
                        if (ent->blas->geomIdxSizes && ent->blas->geomIdxSizes[g] >= 3u * sizeof(uint32_t))
                            matEntry.maxPrimId = (uint32_t)(ent->blas->geomIdxSizes[g] / (3u * sizeof(uint32_t))) - 1u;
                        dynamicMatEntries[base + g] = matEntry;

                        // Refresh addresses from the current surface cache entry.
                        // Walk model surfaces to find the g-th valid BLAS surface and
                        // re-query its current buffer + offset.
                        bool addrRefreshed = false;
                        if (dynModel)
                        {
                            const int numSurfaces = dynModel->NumSurfaces();
                            for (int s = 0; s < numSurfaces && !addrRefreshed; s++)
                            {
                                const modelSurface_t *surf = dynModel->Surface(s);
                                if (!surf || !surf->geometry || !surf->shader)
                                    continue;
                                // Skip surfaces that the BLAS builder would have excluded.
                                if (surf->shader->TestMaterialFlag(MF_POLYGONOFFSET))
                                    continue;
                                if (VK_RT_NoShadowsExcluded(surf->shader))
                                    continue;
                                const deform_t def = surf->shader->Deform();
                                if (def == DFRM_PARTICLE || def == DFRM_PARTICLE2 || def == DFRM_SPRITE ||
                                    def == DFRM_FLARE)
                                    continue;

                                if (validSurfIdx == g)
                                {
                                    // This is the surface for BLAS geometry slot g.
                                    const srfTriangles_t *geo = surf->geometry;
                                    VkBuffer vtxBuf = VK_NULL_HANDLE, idxBuf = VK_NULL_HANDLE;
                                    VkDeviceSize vtxOff = 0, idxOff = 0;
                                    const bool freshVtx = geo->ambientCache &&
                                                          VK_VertexCache_GetBuffer(geo->ambientCache, &vtxBuf, &vtxOff);
                                    const bool freshIdx =
                                        geo->indexCache && VK_VertexCache_GetBuffer(geo->indexCache, &idxBuf, &idxOff);
                                    if (freshVtx && freshIdx)
                                    {
                                        s_dynGeomVtxAddrs[base + g] =
                                            (uint64_t)(GetBufferDeviceAddress(vtxBuf) + vtxOff);
                                        s_dynGeomIdxAddrs[base + g] =
                                            (uint64_t)(GetBufferDeviceAddress(idxBuf) + idxOff);
                                        addrRefreshed = true;
                                    }
                                }
                                validSurfIdx++;
                            }
                        }

                        // Fallback: use cached address (static geometry, or refresh failed).
                        if (!addrRefreshed && ent->blas->geomVertAddrs && ent->blas->geomIdxAddrs)
                        {
                            s_dynGeomVtxAddrs[base + g] = (uint64_t)ent->blas->geomVertAddrs[g];
                            s_dynGeomIdxAddrs[base + g] = (uint64_t)ent->blas->geomIdxAddrs[g];
                        }
                    }
                    dynamicGeomCount += geomCount;
                }
            }
            else
            {
                if (staticGeomCount + geomCount > VK_MAT_MAX_GEOMS)
                {
                    static int s_staticOverflowWarn = 0;
                    if (s_staticOverflowWarn++ < 4)
                        common->Warning("VK RT: static geom address table overflow (count=%u geoms=%u limit=%u) — "
                                        "skipping instance",
                                        staticGeomCount, geomCount, VK_MAT_MAX_GEOMS);
                }
                else
                {
                    const uint32_t base = staticGeomCount;
                    staticInstBaseGeom[staticCount - 1] = base;
                    for (uint32_t g = 0; g < geomCount; g++)
                    {
                        const idMaterial *surfShader =
                            (ent->blas->geomShaders && ent->blas->geomShaders[g]) ? ent->blas->geomShaders[g] : NULL;
                        VkMaterialEntry matEntry = VK_RT_MakeMaterialEntry(surfShader, ent->blas, base + g,
                                                                           s_staticGeomVtxAddrs, s_staticGeomIdxAddrs);
                        // baseGeomIdx in each entry is the absolute slot for THIS geometry only.
                        matEntry.baseGeomIdx = base + g;
                        // Slot changes must rewrite the cached static entries.
                        if (const uint32_t guiSig = VK_RT_BindGuiMaterial(matEntry, surfShader, ent))
                            staticSignature = VK_RT_HashFnv1a64_Bytes(staticSignature, &guiSig, sizeof(guiSig));
                        // Tag player/weapon geometry so glass_probe.rahit can skip it.
                        if (ent->parms.rtPlayer)
                            matEntry.flags |= VK_MAT_FLAG_PLAYER_BODY;
                        // Bounds check: max valid vertex index for rt_InterpolateUV.
                        if (ent->blas->geomVertSizes && ent->blas->geomVertSizes[g] >= sizeof(idDrawVert))
                            matEntry.maxVertex = (uint32_t)(ent->blas->geomVertSizes[g] / sizeof(idDrawVert)) - 1u;
                        // Bounds check: max valid triangle index — must be checked BEFORE the
                        // index-buffer read, not just the vertex indices it returns.
                        if (ent->blas->geomIdxSizes && ent->blas->geomIdxSizes[g] >= 3u * sizeof(uint32_t))
                            matEntry.maxPrimId = (uint32_t)(ent->blas->geomIdxSizes[g] / (3u * sizeof(uint32_t))) - 1u;
                        staticMatEntries[base + g] = matEntry;
                        // Write the per-geometry vertex/index address directly.
                        if (ent->blas->geomVertAddrs && ent->blas->geomIdxAddrs)
                        {
                            s_staticGeomVtxAddrs[base + g] = (uint64_t)ent->blas->geomVertAddrs[g];
                            s_staticGeomIdxAddrs[base + g] = (uint64_t)ent->blas->geomIdxAddrs[g];
                            // Fold into staticSignature so a reallocated vertex-cache buffer
                            // (address changes, BLAS address doesn't) forces a rewrite.
                            staticSignature = VK_RT_HashFnv1a64_Bytes(staticSignature, &s_staticGeomVtxAddrs[base + g],
                                                                      sizeof(uint64_t));
                            staticSignature = VK_RT_HashFnv1a64_Bytes(staticSignature, &s_staticGeomIdxAddrs[base + g],
                                                                      sizeof(uint64_t));
                        }
                    }
                    staticGeomCount += geomCount;
                }
            }
        }
        if (slowLog)
            tMat1 += SDL_GetPerformanceCounter() - matStart;

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

    passStats.pass1 = staticCount + dynamicCount;
    VK_RT_LapTicks(&slowLap, &tPass1);
    blasTicks1 = s_blasBuildStats.cpuTicks;

    // Second pass: culled static entities whose BLASes are already cached (no new builds), so
    // GI rays still hit adjacent-area geometry.  Distance cull at 2x r_rtGIRadius: pixel
    // distance plus ray length.
    if (tr.primaryWorld != NULL)
    {
        const float pass2Rad = VK_RT_Pass2CullRadius();
        const float distCullSq = pass2Rad * pass2Rad;
        const idVec3 camPos = viewDef->renderView.vieworg;

        const int numEntityDefs = tr.primaryWorld->entityDefs.Num();
        for (int ei = 0; ei < numEntityDefs && (staticCount + dynamicCount) < VK_RT_MAX_TLAS_INSTANCES; ei++)
        {
            idRenderEntityLocal *ent = tr.primaryWorld->entityDefs[ei];
            if (!ent)
                continue;

            // Read before the culls so the dump can attribute rejects to world BSP
            // areas specifically — they are the ones the vol shadow rays need.
            const bool isWorldArea = VK_RT_IsWorldAreaEntity(ent);
            if (isWorldArea)
                passStats.worldAreasInMap++;

            // World areas all sit at origin (0,0,0), so they use bounds distance only.  Other
            // entities take the cheap origin accept first; the 8-corner bounds test runs only
            // on origin rejects, so large or offset meshes whose origin is far still count.
            const bool outOfRange = isWorldArea ? (VK_RT_EntityBoundsDistance(ent, camPos) > pass2Rad)
                                                : ((ent->parms.origin - camPos).LengthSqr() > distCullSq &&
                                                   VK_RT_EntityBoundsDistance(ent, camPos) > pass2Rad);
            if (outOfRange)
            {
                passStats.rejDist++;
                if (isWorldArea && ent->blasFrameCount != tr.frameCount)
                {
                    passStats.rejDistWorld++;
                }
                continue;
            }

            // Already included by the viewEntitys pass above.
            if (ent->blasFrameCount == tr.frameCount)
            {
                passStats.rejAlreadyAdded++;
                continue;
            }

            // Only static entities — dynamic ones need a BLAS rebuild each frame
            // which we can't do here safely outside the viewEntitys loop.
            if (ent->dynamicModel)
            {
                passStats.rejDynamic++;
                continue;
            }

            // View-local-only entities are excluded from every pass, so the invariant
            // holds uniformly rather than depending on the dynamicModel check above.
            if (ent->parms.allowSurfaceInViewID)
            {
                passStats.rejViewLocal++;
                continue;
            }

            // Must have a valid cached BLAS — no building allowed in this pass.
            idRenderModel *model = ent->parms.hModel;
            if (!model || model->NumSurfaces() == 0)
            {
                passStats.rejNoModel++;
                continue;
            }

            // Check model BLAS cache.
            vkBLAS_t *cached = VK_RT_ModelBLASCacheLookup(model);
            if (!cached || !cached->isValid)
            {
                passStats.rejNoCachedBlas++;
                continue;
            }

            // Use the cached BLAS; mark as seen so it won't be double-added.
            ent->blas = cached;
            ent->blasFrameCount = tr.frameCount;

            if (staticGeomCount + cached->geomCount > VK_MAT_MAX_GEOMS)
            {
                passStats.rejGeomFull++;
                continue; // no room in geom table
            }

            if (isWorldArea)
                passStats.pass2World++;

            VkAccelerationStructureInstanceKHR &inst = staticInstances[staticCount];
            memset(&inst, 0, sizeof(inst));

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

            inst.instanceCustomIndex = 0; // patched below
            inst.mask = VK_RT_MASK_WORLD;
            inst.instanceShaderBindingTableRecordOffset = 0;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst.accelerationStructureReference = cached->deviceAddress;

            const uint32_t base = staticGeomCount;
            staticInstBaseGeom[staticCount] = base;
            for (uint32_t g = 0; g < cached->geomCount; g++)
            {
                const idMaterial *surfShader =
                    (cached->geomShaders && cached->geomShaders[g]) ? cached->geomShaders[g] : NULL;
                VkMaterialEntry matEntry =
                    VK_RT_MakeMaterialEntry(surfShader, cached, base + g, s_staticGeomVtxAddrs, s_staticGeomIdxAddrs);
                matEntry.baseGeomIdx = base + g;
                if (const uint32_t guiSig = VK_RT_BindGuiMaterial(matEntry, surfShader, ent))
                    staticSignature = VK_RT_HashFnv1a64_Bytes(staticSignature, &guiSig, sizeof(guiSig));
                if (cached->geomVertSizes && cached->geomVertSizes[g] >= sizeof(idDrawVert))
                    matEntry.maxVertex = (uint32_t)(cached->geomVertSizes[g] / sizeof(idDrawVert)) - 1u;
                if (cached->geomIdxSizes && cached->geomIdxSizes[g] >= 3u * sizeof(uint32_t))
                    matEntry.maxPrimId = (uint32_t)(cached->geomIdxSizes[g] / (3u * sizeof(uint32_t))) - 1u;
                staticMatEntries[base + g] = matEntry;
                if (cached->geomVertAddrs && cached->geomIdxAddrs)
                {
                    s_staticGeomVtxAddrs[base + g] = (uint64_t)cached->geomVertAddrs[g];
                    s_staticGeomIdxAddrs[base + g] = (uint64_t)cached->geomIdxAddrs[g];
                    // Fold into staticSignature — see the other construction site.
                    staticSignature =
                        VK_RT_HashFnv1a64_Bytes(staticSignature, &s_staticGeomVtxAddrs[base + g], sizeof(uint64_t));
                    staticSignature =
                        VK_RT_HashFnv1a64_Bytes(staticSignature, &s_staticGeomIdxAddrs[base + g], sizeof(uint64_t));
                }
            }
            staticGeomCount += cached->geomCount;

            // Update static signature so TLAS rebuild is triggered when new
            // off-screen entities enter/leave the cache-hit set.
            const uintptr_t entTag = (uintptr_t)ent;
            uint64_t sigAfterEnt = VK_RT_HashFnv1a64_Bytes(staticSignature, &entTag, sizeof(entTag));
            uint64_t sigAfterAddr = VK_RT_HashFnv1a64_Bytes(sigAfterEnt, &inst.accelerationStructureReference,
                                                            sizeof(inst.accelerationStructureReference));
            staticSignature =
                VK_RT_HashFnv1a64_Bytes(sigAfterAddr, &inst.transform.matrix[0][0], sizeof(inst.transform.matrix));
            staticCount++;
        }
    }

    passStats.pass2 = staticCount + dynamicCount - passStats.pass1;
    VK_RT_LapTicks(&slowLap, &tPass2);

    // Third pass: off-screen dynamic entities within r_rtNearDynRadius, so they still reflect
    // and cast RT shadows.  Each costs one CPU skin of the current pose plus a BLAS update.
    if (tr.primaryWorld != NULL && r_rtNearDynRadius.GetFloat() > 0.0f)
    {
        const float nearRad = r_rtNearDynRadius.GetFloat();
        const float distCullSq = nearRad * nearRad;
        const idVec3 camPos = viewDef->renderView.vieworg;

        const int numEntityDefs = tr.primaryWorld->entityDefs.Num();
        for (int ei = 0; ei < numEntityDefs && (staticCount + dynamicCount) < VK_RT_MAX_TLAS_INSTANCES; ei++)
        {
            idRenderEntityLocal *ent = tr.primaryWorld->entityDefs[ei];
            if (!ent)
                continue;

            // Already added by a previous pass this frame.
            if (ent->blasFrameCount == tr.frameCount)
                continue;

            // Must be within the near-dynamic radius.
            if ((ent->parms.origin - camPos).LengthSqr() > distCullSq)
                continue;

            // Only dynamic model types — static entities are covered by the second pass.
            idRenderModel *hModel = ent->parms.hModel;
            if (!hModel || hModel->IsDynamicModel() == DM_STATIC)
                continue;

            // Exclude first-person weapon (view model). weaponDepthHack identifies
            // the view-space gun mesh — it must not appear in RT reflections.
            if (ent->parms.weaponDepthHack)
                continue;

            // Same for view-local-only entities (first-person body): a duplicate of
            // world geometry already in the TLAS. Pass 1 stamps blasFrameCount, but this
            // pass also runs when the entity was never in viewEntitys, so check here too.
            if (ent->parms.allowSurfaceInViewID)
                continue;

            // parms.joints is updated every frame regardless of visibility, so this is the
            // current pose.
            const uint64_t dynModelStart = slowLog ? SDL_GetPerformanceCounter() : 0;
            idRenderModel *model = R_EntityDefDynamicModel(ent);
            if (slowLog)
                tDynModel += SDL_GetPerformanceCounter() - dynModelStart;
            if (!model || model->NumSurfaces() == 0)
                continue;

            // Rebuild the BLAS in-place so vertex positions match the current pose.
            // In-place update reuses the existing GPU buffers and device address,
            // preventing TLAS signature churn.
            if (ent->blas)
            {
                vkBLAS_t *updated = VK_RT_BuildBLASForModel(model, cmd, ent->blas);
                if (updated != ent->blas)
                    VK_RT_DestroyBLAS(ent->blas);
                ent->blas = updated;
            }
            else
            {
                ent->blas = VK_RT_BuildBLASForModel(model, cmd);
            }
            if (!ent->blas || !ent->blas->isValid)
                continue;
            anyBLASBuilt = true;

            if (dynamicGeomCount + ent->blas->geomCount > VK_MAT_MAX_GEOMS)
                continue;

            ent->blasFrameCount = tr.frameCount;

            VkAccelerationStructureInstanceKHR &inst = dynamicInstances[dynamicCount];
            memset(&inst, 0, sizeof(inst));

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

            inst.instanceCustomIndex = 0; // patched below
            inst.mask = ent->parms.rtPlayer ? VK_RT_MASK_PLAYER : VK_RT_MASK_WORLD;
            inst.instanceShaderBindingTableRecordOffset = ent->parms.rtPlayer ? 2 : 0;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst.accelerationStructureReference = ent->blas->deviceAddress;
            // This pass is where the player body actually arrives: it is never a
            // viewEntity in its own view, so pass 1 above never sees it.
            VK_RT_NotePlayerSuppress(ent);

            const uint32_t base = dynamicGeomCount;
            dynamicInstBaseGeom[dynamicCount] = base;
            dynamicCount++;

            const uint32_t geomCount = ent->blas->geomCount;
            for (uint32_t g = 0; g < geomCount; g++)
            {
                const idMaterial *surfShader =
                    (ent->blas->geomShaders && ent->blas->geomShaders[g]) ? ent->blas->geomShaders[g] : NULL;
                VkMaterialEntry matEntry =
                    VK_RT_MakeMaterialEntry(surfShader, ent->blas, base + g, s_dynGeomVtxAddrs, s_dynGeomIdxAddrs);
                matEntry.baseGeomIdx = base + g;
                VK_RT_BindGuiMaterial(matEntry, surfShader, ent);
                if (ent->parms.rtPlayer)
                    matEntry.flags |= VK_MAT_FLAG_PLAYER_BODY;
                if (ent->blas->geomVertSizes && ent->blas->geomVertSizes[g] >= sizeof(idDrawVert))
                    matEntry.maxVertex = (uint32_t)(ent->blas->geomVertSizes[g] / sizeof(idDrawVert)) - 1u;
                if (ent->blas->geomIdxSizes && ent->blas->geomIdxSizes[g] >= 3u * sizeof(uint32_t))
                    matEntry.maxPrimId = (uint32_t)(ent->blas->geomIdxSizes[g] / (3u * sizeof(uint32_t))) - 1u;
                dynamicMatEntries[base + g] = matEntry;
                // Addresses captured during the BLAS build above — current for this frame.
                if (ent->blas->geomVertAddrs && ent->blas->geomIdxAddrs)
                {
                    s_dynGeomVtxAddrs[base + g] = (uint64_t)ent->blas->geomVertAddrs[g];
                    s_dynGeomIdxAddrs[base + g] = (uint64_t)ent->blas->geomIdxAddrs[g];
                }
            }
            dynamicGeomCount += geomCount;
        }
    }

    VK_RT_LapTicks(&slowLap, &tPass3);
    blasTicks3 = s_blasBuildStats.cpuTicks;

    // All BLAS builds are recorded by now; end the GPU span before the TLAS build.
    VK_RTProfile_BLASGpuEnd(cmd);
    s_tlasDynamicInstances = (int)dynamicCount;

    const uint32_t instanceCount = staticCount + dynamicCount;
    passStats.pass3 = instanceCount - passStats.pass1 - passStats.pass2;

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

    // Patch dynamic material entries: their baseGeomIdx was a local offset within
    // s_dynGeom*; now that staticGeomCount is final, shift to absolute GPU-side index.
    for (uint32_t i = 0; i < dynamicGeomCount; i++)
        dynamicMatEntries[i].baseGeomIdx += staticGeomCount;

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
        // instanceCustomIndex = baseGeomIdx for this instance so that shaders can
        // index the per-geometry material table as:
        //   matIdx = gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT
        for (uint32_t i = 0; i < staticCount; i++)
            staticInstances[i].instanceCustomIndex = staticInstBaseGeom[i];

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
        // Dynamic instanceCustomIndex = staticGeomCount (the start of the dynamic block)
        // plus the per-instance local base geom offset.
        for (uint32_t i = 0; i < dynamicCount; i++)
            dynamicInstances[i].instanceCustomIndex = staticGeomCount + dynamicInstBaseGeom[i];
        memcpy(dstBytes + staticBytes, dynamicInstances, dynamicBytes);
    }

    vkUnmapMemory(vk.device, tlas.instanceMemory);

    // Guard against combined static+dynamic overflow: each bucket is independently bounded to
    // VK_MAT_MAX_GEOMS, but the GPU SSBO is only VK_MAT_MAX_GEOMS entries total.
    if (staticGeomCount + dynamicGeomCount > VK_MAT_MAX_GEOMS)
    {
        common->Warning("VK RT: combined geom count overflow (static=%u dynamic=%u limit=%u) — clamping dynamic",
                        staticGeomCount, dynamicGeomCount, VK_MAT_MAX_GEOMS);
        dynamicGeomCount = VK_MAT_MAX_GEOMS - staticGeomCount;
    }

    // Any glass in this TLAS?  If not, the reflection rgen skips its full-screen glass probe.
    // REAL_GLASS, not GLASS: GLASS is every MC_TRANSLUCENT surface.
    {
        bool hasGlass = false;
        for (uint32_t i = 0; i < staticGeomCount && !hasGlass; i++)
            hasGlass = (staticMatEntries[i].flags & VK_MAT_FLAG_REAL_GLASS) != 0;
        for (uint32_t i = 0; i < dynamicGeomCount && !hasGlass; i++)
            hasGlass = (dynamicMatEntries[i].flags & VK_MAT_FLAG_REAL_GLASS) != 0;
        if (hasGlass != vkRT.sceneHasGlass && r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT TLAS: sceneHasGlass %d -> %d (frame=%d)\n", vkRT.sceneHasGlass ? 1 : 0,
                           hasGlass ? 1 : 0, tr.frameCount);
        vkRT.sceneHasGlass = hasGlass;
    }

    VK_RT_LapTicks(&slowLap, &tInst);

    // Upload material table entries: one entry per geometry slot (not per instance).
    VK_RT_UploadMatTableFrame(staticMatEntries, staticGeomCount, rewriteStatic, dynamicMatEntries, dynamicGeomCount,
                              s_staticGeomVtxAddrs, s_staticGeomIdxAddrs, staticGeomCount, s_dynGeomVtxAddrs,
                              s_dynGeomIdxAddrs, dynamicGeomCount, rewriteStatic);
    VK_RT_LapTicks(&slowLap, &tMatTable);

    tlas.instanceCount = instanceCount;

    if (r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK RT TLAS: building — instances=%u (static=%u dynamic=%u) pass1=%u pass2=%u pass3=%u "
                       "worldAreas=%u/%u blasBuiltThisFrame=%s\n",
                       instanceCount, staticCount, dynamicCount, passStats.pass1, passStats.pass2, passStats.pass3,
                       passStats.pass1World + passStats.pass2World, passStats.worldAreasInMap,
                       anyBLASBuilt ? "yes" : "no");
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
    // FAST_BUILD: the TLAS is rebuilt every frame and trace quality barely differs at TLAS
    // level.  BLASes stay FAST_TRACE.
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
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
        tlasStorageRegrown = true;

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
        tlasScratchRegrown = true;
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

    VK_RT_LapTicks(&slowLap, &tBuild);
    const double toMs = 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (slowLog && (slowLap - slowStart) * toMs > slowLogMs)
    {
        vkRTBindlessFrameStats_t bindless1 = {};
        VK_RT_GetBindlessFrameStats(&bindless1);
        const uint64_t blasPass1 = blasTicks1 - blasTicks0, blasPass3 = blasTicks3 - blasTicks1;
        common->Printf("VK RT TLAS SLOW: frame=%d total=%.2f | pass1=%.2f (blas=%.2f mat=%.2f) pass2=%.2f "
                       "pass3=%.2f (blas=%.2f dynModel=%.2f) inst=%.2f (instRealloc=%d rewriteStatic=%d) "
                       "matTable=%.2f (bindlessRebuild=%.2f newSlots=%u slotsWritten=%u bgLoads=%u) "
                       "build=%.2f (storageRegrow=%d scratchRegrow=%d) | inst=%u/%u/%u geoms=%u/%u\n",
                       tr.frameCount, (slowLap - slowStart) * toMs, tPass1 * toMs, blasPass1 * toMs, tMat1 * toMs,
                       tPass2 * toMs, tPass3 * toMs, blasPass3 * toMs, tDynModel * toMs, tInst * toMs,
                       instanceBufferRecreated ? 1 : 0, rewriteStatic ? 1 : 0, tMatTable * toMs,
                       bindless1.rebuildMs - bindless0.rebuildMs, bindless1.slotCount - bindless0.slotCount,
                       bindless1.slotsWritten - bindless0.slotsWritten, bindless1.bgLoads - bindless0.bgLoads,
                       tBuild * toMs, tlasStorageRegrown ? 1 : 0, tlasScratchRegrown ? 1 : 0, passStats.pass1,
                       passStats.pass2, passStats.pass3, staticGeomCount, dynamicGeomCount);
    }
}

// ---------------------------------------------------------------------------
// VK_RT_Init / VK_RT_Shutdown
// Called once after device creation (when rayTracingSupported is true).
// The shadow pipeline and shadow mask are set up in vk_shadows.cpp.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VK_RT_BeginLevelLoad (public)
// Called from idRenderSystemLocal::BeginLevelLoad() before the render model
// manager purges assets.  Worlds are still valid at this point.
// Performs a full RT clean-slate for the new level:
//   (a) frees all per-entity and cached-model BLASes so model pointers don't dangle, and
//   (b) destroys all per-frame TLAS resources so RebuildTLAS never calls
//       vkDestroyAccelerationStructureKHR inline while a command buffer is recording.
//   (c) resets all descriptor-set frame counters so every RT pipeline refreshes its
//       TLAS binding on the first frame of the new level.
//   (d) invalidates the static-instance cache so the first TLAS build is unconditional.
// vkDeviceWaitIdle is safe here — the game's main loop stops rendering during
// level loads.
// ---------------------------------------------------------------------------

void VK_RT_BeginLevelLoad(void)
{
    if (!vkRT.isInitialized)
        return;

    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_ACCELSTRUCT);

    // Walk all worlds (still valid here) and free per-entity non-cached BLASes.
    // Also clears the model BLAS cache.
    VK_RT_ClearWorldEntityBLASPointers(true);

    // Drain any remaining deferred garbage — all safe since device is now idle.
    for (int i = 0; i < s_blasGarbageCount; i++)
        VK_RT_FreeBLASImmediate(s_blasGarbage[i].blas);
    s_blasGarbageCount = 0;

    // Every BLAS was freed above; hand the old level's chunks back to the driver.
    VK_RT_DrainScratchGarbage(true);
    VK_Pool_TrimEmpty(&s_blasStoragePool);
    VK_Pool_TrimEmpty(&s_blasScratchPool);
    VK_Pool_TrimEmpty(&s_blasGeomPool);

    VK_GuiTex_BeginLevelLoad();

    // Destroy all per-frame TLAS resources.
    // The new level's first RebuildTLAS call will allocate fresh buffers.
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
        memset(&tlas, 0, sizeof(tlas));
        tlas.lastBuiltFrameCount = -1;
    }

    // Invalidate static-instance cache so the first TLAS build is unconditional.
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        s_staticInstanceCache[i].valid = false;

    // Release bindless texture slots held by the old map's images. Must follow the
    // static-instance invalidation above: that is what guarantees every material
    // entry (and so every tex index) is rewritten on the new map's first frame.
    VK_RT_MatTableLevelLoadReset();

    // Reset all descriptor-set frame counters so every RT pipeline re-writes its
    // TLAS binding on the first rendered frame of the new level.
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRT.shadowDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.aoDescSetLastUpdatedFrameCount[i] = -1;
        for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
            vkRT.reflChain[c].traceDescSetLastUpdated[i] = -1;
        vkRT.blurDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.temporalDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.atrousDescSetLastUpdatedFrameCount[i] = -1;
    }
    // Temporal history from the old level is meaningless for the new one.
    vkRT.aoHistoryValid = false;
    // reflChain[].historyValid is left alone: a level load always trips the camera-cut test
    // or !prevFrameValid, so the reflection EMA resets itself.
    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT: BeginLevelLoad — cleared %d model BLAS cache entries, freed TLAS\n",
                       s_modelBLASCacheCount);
    // s_modelBLASCacheCount was already reset to 0 by VK_RT_ModelBLASCacheClear() above.
}

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

    // Scratch ranges must start on the device's scratch alignment (a power of two).
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps = {};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(vk.physicalDevice, &props2);
    if (asProps.minAccelerationStructureScratchOffsetAlignment > s_blasScratchPool.align)
        s_blasScratchPool.align = asProps.minAccelerationStructureScratchOffsetAlignment;
    common->Printf("VK RT: BLAS scratch alignment %u\n", (unsigned)s_blasScratchPool.align);
    common->Printf("VK RT: acceleration structure support initialized\n");
}

void VK_RT_Shutdown(void)
{
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_ACCELSTRUCT);

    // Destroy live per-entity BLAS objects that are not in the deferred queue,
    // then clear all entity pointers so the next device lifetime starts clean.
    VK_RT_ClearWorldEntityBLASPointers(true);

    // Flush deferred BLAS deletions (device is idle, all are safe to free)
    for (int i = 0; i < s_blasGarbageCount; i++)
        VK_RT_FreeBLASImmediate(s_blasGarbage[i].blas);
    memset(s_blasGarbage, 0, sizeof(s_blasGarbage));
    s_blasGarbageCount = 0;

    // Every BLAS is gone, so the pools hold no live ranges.
    s_scratchGarbage.Clear();
    VK_Pool_Destroy(&s_blasStoragePool);
    VK_Pool_Destroy(&s_blasScratchPool);
    VK_Pool_Destroy(&s_blasGeomPool);

    VK_GuiTex_Shutdown();

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

    // AO mask images
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &ao = vkRT.aoMask[i];
        if (ao.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, ao.view, NULL);
            ao.view = VK_NULL_HANDLE;
        }
        if (ao.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, ao.image, NULL);
            ao.image = VK_NULL_HANDLE;
        }
        if (ao.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, ao.memory, NULL);
            ao.memory = VK_NULL_HANDLE;
        }
    }

    // AO pipeline + SBT
    if (vkRT.aoPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(vk.device, vkRT.aoPipeline, NULL);
    if (vkRT.aoPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vk.device, vkRT.aoPipelineLayout, NULL);
    if (vkRT.aoDescLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vk.device, vkRT.aoDescLayout, NULL);
    if (vkRT.aoDescPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(vk.device, vkRT.aoDescPool, NULL);
    if (vkRT.sbtAOBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.sbtAOBuffer, NULL);
        vkFreeMemory(vk.device, vkRT.sbtAOMemory, NULL);
    }
    if (vkRT.aoMaskSampler != VK_NULL_HANDLE)
        vkDestroySampler(vk.device, vkRT.aoMaskSampler, NULL);

    // Temporal resolve pipeline + history images
    VK_RT_ShutdownTemporal();

    // Reflection pipeline, SBT, and buffers
    VK_RT_ShutdownReflections();

    // GI pipeline, SBT, and buffers
    VK_RT_ShutdownGI();

    // Volumetric lighting pipeline and buffers
    VK_RT_ShutdownVolumetrics();

    // Tonemap (with the HDR scene buffer and the G-buffer images) and upscale are shut
    // down by VKimp_PreShutdown instead: they are created whether or not the device has
    // RT, so their teardown cannot live behind this function's RT gate.

    // Bloom images and pipelines
    VK_RT_ShutdownBloom();

    // Material table SSBOs and bindless descriptor set
    VK_RT_ShutdownMaterialTable();

    memset(&vkRT, 0, sizeof(vkRT));
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeShadowMask
// Called when the render resolution changes.
// ---------------------------------------------------------------------------

void VK_RT_ResizeShadowMask(uint32_t width, uint32_t height)
{
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_ACCELSTRUCT);

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

        // R8 UNORM storage image array — one layer per shadow-casting light.
        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8_UNORM;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = VK_RT_SHADOW_LAYERS;
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
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = VK_RT_SHADOW_LAYERS;
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

        // Transition both images to GENERAL layout for storage image use.
        // VK_TransitionImageLayout() hardcodes layerCount=1, which would leave layers
        // 1..N-1 in UNDEFINED — emit the barrier directly so every layer is covered.
        {
            VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
            VkImageMemoryBarrier toGeneral[2] = {};
            for (int b = 0; b < 2; b++)
            {
                toGeneral[b].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toGeneral[b].srcAccessMask = 0;
                toGeneral[b].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                toGeneral[b].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toGeneral[b].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                toGeneral[b].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral[b].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral[b].image = (b == 0) ? sm.image : sm.blurTempImage;
                toGeneral[b].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_RT_SHADOW_LAYERS};
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
                                 0, NULL, 2, toGeneral);
            VK_EndSingleTimeCommands(cmd);
        }

        sm.width = width;
        sm.height = height;
        sm.layers = VK_RT_SHADOW_LAYERS;
        vkRT.shadowDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.blurDescSetLastUpdatedFrameCount[i] = -1;
    }
}
