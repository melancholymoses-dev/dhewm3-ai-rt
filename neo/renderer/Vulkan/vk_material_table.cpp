/*
===========================================================================

dhewm3-rt Vulkan ray tracing — material table.

Builds and maintains three persistently-mapped SSBOs and one bindless
combined-image-sampler array that are bound at set=2 in all RT shaders
that need to look up hit-surface material properties.

  set=2, binding=0  — VkMaterialEntry[]   (material SSBO)
  set=2, binding=1  — uint64_t[]          (vertex buffer device addresses)
  set=2, binding=2  — uint64_t[]          (index  buffer device addresses)
  set=2, binding=3  — sampler2D textures[] (bindless, VK_MAT_MAX_TEXTURES slots)

The material SSBO has one entry per TLAS instance, indexed by
gl_InstanceCustomIndexEXT.  It is rebuilt every frame alongside the TLAS:
  - static entries are written only when the static instance set changes
    (mirrors the rewriteStatic flag in VK_RT_RebuildTLAS)
  - dynamic entries are written every frame

The bindless texture array is initialised with the white/flat-normal fallback
images.  When VK_RT_MakeMaterialEntry() encounters a diffuse or normal image
it has not seen before, the slot is assigned and the dirty flag is set.
A rebuild of the bindless descriptors (vkUpdateDescriptorSets) is issued at
the end of VK_RT_UploadMatTableFrame when the dirty flag is set.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Image.h"
#include "renderer/Material.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_image.h"
#include "framework/DeclManager.h"

#include <string.h>
#include <SDL.h>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern idCVar r_vkLogRT;

// ---------------------------------------------------------------------------
// Local constants
// ---------------------------------------------------------------------------

// Matches VK_RT_MAX_TLAS_INSTANCES in vk_accelstruct.cpp
static const uint32_t MAT_MAX_INSTANCES = 4096;

// ---------------------------------------------------------------------------
// Bindless texture tracking
//
// s_bindlessImages[i] holds the idImage* assigned to slot i.
// Index 0 is always the white fallback (diffuse) or flat-normal (normal).
// When a new image is first encountered, it gets the next free slot.
// s_bindlessDirty[slot] is set for every frame slot whenever a slot is assigned
// or an image changes; each slot's descriptor set is rewritten on its own frame.
// ---------------------------------------------------------------------------

static idImage *s_bindlessImages[VK_MAT_MAX_TEXTURES];
static uint32_t s_bindlessCount = 0;
static bool s_bindlessDirty[VK_MAX_FRAMES_IN_FLIGHT] = {};

static void MarkBindlessDirty(bool dirty)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        s_bindlessDirty[i] = dirty;
}

// The idImage::backendData (vkImageData_t*) each slot's descriptor was written
// against.  idImage objects survive a level-load purge but their Vulkan image +
// view are destroyed and recreated, so an unchanged idImage* is NOT proof the
// descriptor is still valid — comparing backendData is.
static void *s_bindlessBackendData[VK_MAT_MAX_TEXTURES];
// Slots [0, s_bindlessWritten) have been through RebuildBindlessDescriptors at
// least once; slots at or above it are freshly assigned and may lazy-load.
static uint32_t s_bindlessWritten = 0;

// VK_Image_ChangeCounter() at the time the descriptors were last written.  The
// backendData pointer compare below cannot stand on its own: VK_DestroyImageData
// deletes the vkImageData_t and a later upload can be handed the same address, so
// a purge/reupload pair that lands on the old pointer (ABA) reads as unchanged and
// leaves the descriptor on a destroyed VkImageView.  The counter cannot miss that
// transition, so it is the authority; the pointer scan survives only to name the
// affected slots in the log.
static uint32_t s_bindlessImageGeneration = 0;

// Cheap per-frame check: mark dirty if any image was purged or reuploaded since
// the descriptors were written.  No lazy loading.
static void ValidateBindlessSlots(void)
{
    const uint32_t gen = VK_Image_ChangeCounter();
    if (gen == s_bindlessImageGeneration)
        return;

    MarkBindlessDirty(true);

    if (r_vkLogRT.GetInteger() >= 1)
    {
        uint32_t stale = 0;
        for (uint32_t i = 0; i < s_bindlessWritten; i++)
        {
            idImage *img = s_bindlessImages[i];
            if (img && img->backendData != s_bindlessBackendData[i])
                stale++;
        }
        common->Printf("VK RT MatTable: image generation %u -> %u (%u/%u slots differ by pointer) — refreshing\n",
                       s_bindlessImageGeneration, gen, stale, s_bindlessWritten);
    }
}

// Returns the bindless slot index for img, assigning a new one if needed.
// img == NULL returns slot 0 (white/flat-normal fallback, slot 0 is reserved).
static uint32_t GetOrAssignTexIndex(idImage *img)
{
    if (!img)
        return 0;

    for (uint32_t i = 0; i < s_bindlessCount; i++)
    {
        if (s_bindlessImages[i] == img)
            return i;
    }

    if (s_bindlessCount >= VK_MAT_MAX_TEXTURES)
    {
        static int s_overflowWarned = 0;
        if (!s_overflowWarned)
        {
            common->Warning("VK RT MatTable: bindless texture array full (%u slots). "
                            "Extra images will use slot 0 (white fallback).",
                            VK_MAT_MAX_TEXTURES);
            s_overflowWarned = 1;
        }
        return 0;
    }

    uint32_t idx = s_bindlessCount++;
    s_bindlessImages[idx] = img;
    MarkBindlessDirty(true);
    return idx;
}

// See declaration in vk_raytracing.h.
uint32_t VK_RT_GetOrAssignTexIndex(idImage *img)
{
    return GetOrAssignTexIndex(img);
}

// ---------------------------------------------------------------------------
// RebuildBindlessDescriptors
//
// Writes all assigned bindless slots into the current frame slot's
// vkRT.matDescSet binding=3.  Only that slot's set is touched: its fence has been
// waited on, whereas the other slot's set may still be read by the GPU.
// Uses the image's own VkImageView with vkRT.matSampler.
// Unfilled slots (beyond s_bindlessCount) keep their initial fallback value.
// Called when s_bindlessDirty[vk.currentFrame] is set.
// ---------------------------------------------------------------------------

// Running totals for r_vkLogTLASSlow, which reads them as before/after deltas.
static vkRTBindlessFrameStats_t s_bindlessStats = {};

void VK_RT_GetBindlessFrameStats(vkRTBindlessFrameStats_t *out)
{
    *out = s_bindlessStats;
    out->slotCount = s_bindlessCount;
}

static void RebuildBindlessDescriptors(void)
{
    if (!vkRT.matTableInitialized || s_bindlessCount == 0)
        return;
    const uint32_t frame = vk.currentFrame;
    const uint64_t rebuildStart = SDL_GetPerformanceCounter();

    // Build one VkDescriptorImageInfo per assigned slot.
    // Stack-allocate for the expected common case; heap for overflow.
    static VkDescriptorImageInfo infos[VK_MAT_MAX_TEXTURES];

    // White fallback info — used for any slot whose image has no Vulkan data yet.
    VkDescriptorImageInfo fallbackInfo = {};
    VK_Image_GetDescriptorInfo(globalImages->whiteImage, &fallbackInfo);
    fallbackInfo.sampler = vkRT.matSampler;
    fallbackInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    for (uint32_t i = 0; i < s_bindlessCount; i++)
    {
        VkDescriptorImageInfo info = fallbackInfo;
        idImage *img = s_bindlessImages[i];
        // Only fresh slots may lazy-load.  A written slot whose image is purged takes the white
        // fallback: after a level load, loading thousands here stalls and resurrects unused
        // textures.  ValidateBindlessSlots picks it up once the raster path reuploads it.
        if (img && (img->backendData || i >= s_bindlessWritten))
        {
            if (!img->backendData)
                s_bindlessStats.bgLoads++; // takes VK_Image_GetDescriptorInfo's load path
            VkDescriptorImageInfo tmp = {};
            if (VK_Image_GetDescriptorInfo(img, &tmp))
            {
                info.imageView = tmp.imageView;
                info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        info.sampler = vkRT.matSampler; // always use our bilinear sampler
        infos[i] = info;
        s_bindlessBackendData[i] = img ? img->backendData : NULL;
    }

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vkRT.matDescSet[frame];
    write.dstBinding = 3;
    write.dstArrayElement = 0;
    write.descriptorCount = s_bindlessCount;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = infos;

    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    s_bindlessStats.slotsWritten += s_bindlessCount;
    s_bindlessStats.rebuildMs +=
        (double)(SDL_GetPerformanceCounter() - rebuildStart) * 1000.0 / (double)SDL_GetPerformanceFrequency();

    if (r_vkLogRT.GetInteger() >= 2)
    {
        common->Printf("VK RT MatTable: rebuilt bindless descriptors — %u textures (slot %u)\n", s_bindlessCount,
                       frame);
        fflush(NULL);
    }

    s_bindlessWritten = s_bindlessCount;
    s_bindlessDirty[frame] = false;
    // Read AFTER the descriptors are written: anything that purges or uploads
    // between here and the next validate must still be caught.
    s_bindlessImageGeneration = VK_Image_ChangeCounter();
}

// ---------------------------------------------------------------------------
// VK_RT_FlushBindlessTextures (public)
//
// VK_RT_UploadGILights runs after this frame's VK_RT_UploadMatTableFrame and can register
// a new light-cookie image; flush it now rather than next frame.  Binding 3's
// UPDATE_AFTER_BIND + PARTIALLY_BOUND make a second update in the same frame valid.
// ---------------------------------------------------------------------------

void VK_RT_FlushBindlessTextures(void)
{
    if (s_bindlessDirty[vk.currentFrame])
        RebuildBindlessDescriptors();
}

// ---------------------------------------------------------------------------
// VK_RT_InitMaterialTable
// ---------------------------------------------------------------------------

void VK_RT_InitMaterialTable(void)
{
    if (vkRT.matTableInitialized)
        return;

    // --- Persistent SSBOs ---

    const VkDeviceSize matSSBOSize = (VkDeviceSize)VK_MAT_MAX_GEOMS * sizeof(VkMaterialEntry);
    const VkDeviceSize addrSSBOSize = (VkDeviceSize)VK_MAT_MAX_GEOMS * sizeof(uint64_t);

    // System RAM is fine for these: BAR placement measured no gain.
    const VkBufferUsageFlags ssboUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
    {
        VK_CreateBuffer(matSSBOSize, ssboUsage, hostFlags, &vkRT.matTableSSBO[f], &vkRT.matTableSSBOMemory[f]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.matTableSSBOMemory[f], 0, matSSBOSize, 0, &vkRT.matTableMapped[f]));

        VK_CreateBuffer(addrSSBOSize, ssboUsage, hostFlags, &vkRT.vtxAddrSSBO[f], &vkRT.vtxAddrSSBOMemory[f]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.vtxAddrSSBOMemory[f], 0, addrSSBOSize, 0, &vkRT.vtxAddrMapped[f]));

        VK_CreateBuffer(addrSSBOSize, ssboUsage, hostFlags, &vkRT.idxAddrSSBO[f], &vkRT.idxAddrSSBOMemory[f]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.idxAddrSSBOMemory[f], 0, addrSSBOSize, 0, &vkRT.idxAddrMapped[f]));

        // Zero-initialise so GPU reads zeros for uninitialised instances.
        memset(vkRT.matTableMapped[f], 0, (size_t)matSSBOSize);
        memset(vkRT.vtxAddrMapped[f], 0, (size_t)addrSSBOSize);
        memset(vkRT.idxAddrMapped[f], 0, (size_t)addrSSBOSize);
    }

    // --- Bilinear sampler for material textures ---

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.minLod = 0.0f;
    sampInfo.maxLod = VK_LOD_CLAMP_NONE;
    sampInfo.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(vk.device, &sampInfo, NULL, &vkRT.matSampler));

    // --- Descriptor set layout: set=2 ---
    // binding=0  STORAGE_BUFFER   — MaterialTable
    // binding=1  STORAGE_BUFFER   — VtxAddrTable
    // binding=2  STORAGE_BUFFER   — IdxAddrTable
    // binding=3  COMBINED_IMAGE_SAMPLER [VK_MAT_MAX_TEXTURES] — bindless textures

    VkDescriptorSetLayoutBinding bindings[4] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags =
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = bindings[0].stageFlags;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = bindings[0].stageFlags;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = VK_MAT_MAX_TEXTURES;
    // The froxel fill (compute) samples light cookies from the bindless array; the SSBO
    // bindings 0-2 stay RT-only.
    bindings[3].stageFlags = bindings[0].stageFlags | VK_SHADER_STAGE_COMPUTE_BIT;

    // UPDATE_AFTER_BIND so the set can be updated while referenced by in-flight or recording
    // command buffers; binding 3 is also PARTIALLY_BOUND since not every slot is populated.
    VkDescriptorBindingFlags bindingFlags[4] = {
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindFlagsInfo = {};
    bindFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindFlagsInfo.bindingCount = 4;
    bindFlagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext = &bindFlagsInfo;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.matDescLayout));

    // --- Descriptor pool (one set per frame slot) ---

    VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * VK_MAX_FRAMES_IN_FLIGHT}, // 3 SSBOs per set
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MAT_MAX_TEXTURES * VK_MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.matDescPool));

    // --- Allocate descriptor sets ---

    VkDescriptorSetLayout setLayouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
        setLayouts[f] = vkRT.matDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.matDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = setLayouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.matDescSet));

    // --- Write SSBO descriptors (static — each set points at its own slot's buffers) ---

    for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
    {
        VkDescriptorBufferInfo matBufInfo = {};
        matBufInfo.buffer = vkRT.matTableSSBO[f];
        matBufInfo.offset = 0;
        matBufInfo.range = matSSBOSize;

        VkDescriptorBufferInfo vtxBufInfo = {};
        vtxBufInfo.buffer = vkRT.vtxAddrSSBO[f];
        vtxBufInfo.offset = 0;
        vtxBufInfo.range = addrSSBOSize;

        VkDescriptorBufferInfo idxBufInfo = {};
        idxBufInfo.buffer = vkRT.idxAddrSSBO[f];
        idxBufInfo.offset = 0;
        idxBufInfo.range = addrSSBOSize;

        const VkDescriptorBufferInfo *bufInfos[3] = {&matBufInfo, &vtxBufInfo, &idxBufInfo};
        VkWriteDescriptorSet ssboWrites[3] = {};
        for (int b = 0; b < 3; b++)
        {
            ssboWrites[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ssboWrites[b].dstSet = vkRT.matDescSet[f];
            ssboWrites[b].dstBinding = (uint32_t)b;
            ssboWrites[b].descriptorCount = 1;
            ssboWrites[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ssboWrites[b].pBufferInfo = bufInfos[b];
        }
        vkUpdateDescriptorSets(vk.device, 3, ssboWrites, 0, NULL);
    }

    // --- Seed bindless slot 0 with fallback images ---
    // Slot 0 = white (diffuse fallback), slot 1 = flat normal fallback.

    s_bindlessCount = 0;
    s_bindlessWritten = 0;
    MarkBindlessDirty(false);
    s_bindlessImageGeneration = VK_Image_ChangeCounter();
    memset(s_bindlessImages, 0, sizeof(s_bindlessImages));
    memset(s_bindlessBackendData, 0, sizeof(s_bindlessBackendData));

    GetOrAssignTexIndex(globalImages->whiteImage);    // slot 0 — diffuse fallback
    GetOrAssignTexIndex(globalImages->flatNormalMap); // slot 1 — normal fallback
    // s_bindlessDirty is now true; will be flushed on first UploadMatTableFrame.

    vkRT.matTableInitialized = true;

    common->Printf("VK RT MatTable: initialised — %d slots x (matSSBO=%u KB, addrSSBO=%u KB), "
                   "bindlessSlots=%u\n",
                   VK_MAX_FRAMES_IN_FLIGHT, (unsigned)(matSSBOSize / 1024), (unsigned)(addrSSBOSize / 1024),
                   VK_MAT_MAX_TEXTURES);
}

// ---------------------------------------------------------------------------
// VK_RT_ShutdownMaterialTable
// ---------------------------------------------------------------------------

void VK_RT_ShutdownMaterialTable(void)
{
    if (!vkRT.matTableInitialized)
        return;

    for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
    {
        VkBuffer *bufs[3] = {&vkRT.matTableSSBO[f], &vkRT.vtxAddrSSBO[f], &vkRT.idxAddrSSBO[f]};
        VkDeviceMemory *mems[3] = {&vkRT.matTableSSBOMemory[f], &vkRT.vtxAddrSSBOMemory[f],
                                   &vkRT.idxAddrSSBOMemory[f]};
        void **maps[3] = {&vkRT.matTableMapped[f], &vkRT.vtxAddrMapped[f], &vkRT.idxAddrMapped[f]};
        for (int b = 0; b < 3; b++)
        {
            if (*maps[b])
            {
                vkUnmapMemory(vk.device, *mems[b]);
                *maps[b] = NULL;
            }
            if (*bufs[b] != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(vk.device, *bufs[b], NULL);
                *bufs[b] = VK_NULL_HANDLE;
            }
            if (*mems[b] != VK_NULL_HANDLE)
            {
                vkFreeMemory(vk.device, *mems[b], NULL);
                *mems[b] = VK_NULL_HANDLE;
            }
        }
    }

    if (vkRT.matSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.matSampler, NULL);
        vkRT.matSampler = VK_NULL_HANDLE;
    }

    if (vkRT.matDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.matDescPool, NULL);
        vkRT.matDescPool = VK_NULL_HANDLE;
        for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
            vkRT.matDescSet[f] = VK_NULL_HANDLE;
    }

    if (vkRT.matDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.matDescLayout, NULL);
        vkRT.matDescLayout = VK_NULL_HANDLE;
    }

    s_bindlessCount = 0;
    s_bindlessWritten = 0;
    MarkBindlessDirty(false);
    s_bindlessImageGeneration = VK_Image_ChangeCounter();
    memset(s_bindlessImages, 0, sizeof(s_bindlessImages));
    memset(s_bindlessBackendData, 0, sizeof(s_bindlessBackendData));

    vkRT.matTableInitialized = false;
}

// ---------------------------------------------------------------------------
// VK_RT_MatTableLevelLoadReset (public)
//
// Slot assignment is append-only, so without this the table keeps every image
// from every map visited this session and runs out of its VK_MAT_MAX_TEXTURES
// slots after a few transitions. Safe here because VK_RT_BeginLevelLoad also
// invalidates the static instance cache, forcing a full material-entry rewrite
// on the new map's first frame — no stale tex index survives the reset.
// ---------------------------------------------------------------------------

void VK_RT_MatTableLevelLoadReset(void)
{
    if (!vkRT.matTableInitialized)
        return;

    const uint32_t released = s_bindlessCount;

    s_bindlessCount = 0;
    s_bindlessWritten = 0;
    MarkBindlessDirty(false);
    s_bindlessImageGeneration = VK_Image_ChangeCounter();
    memset(s_bindlessImages, 0, sizeof(s_bindlessImages));
    memset(s_bindlessBackendData, 0, sizeof(s_bindlessBackendData));

    GetOrAssignTexIndex(globalImages->whiteImage);    // slot 0 — diffuse fallback
    GetOrAssignTexIndex(globalImages->flatNormalMap); // slot 1 — normal fallback

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT MatTable: level-load reset — released %u bindless slots\n", released);
}

// ---------------------------------------------------------------------------
// VK_RT_MaterialIsEmissive
//
// Shared by VK_RT_MakeMaterialEntry and the auto-relight harvest so they agree.  An
// SL_AMBIENT stage is emissive only if it is an additive overlay (blend one,one) with
// explicit UVs, or a cinematic/videomap; cubemap-reflect stages (visor rim-light) are
// excluded.  HasGui() shaders with no emissive stage fall back to the diffuse image.
// ---------------------------------------------------------------------------

bool VK_RT_MaterialIsEmissive(const idMaterial *shader, idImage **outEmissiveImage, bool *outGuiEmissive,
                              bool *outIsCinematic)
{
    if (outEmissiveImage)
        *outEmissiveImage = NULL;
    if (outGuiEmissive)
        *outGuiEmissive = false;
    if (outIsCinematic)
        *outIsCinematic = false;

    if (!shader)
        return false;

    idImage *emissiveImg = NULL;
    idImage *diffuseImg = NULL;
    bool emissiveIsCinematic = false;

    for (int i = 0; i < shader->GetNumStages(); i++)
    {
        const shaderStage_t *stage = shader->GetStage(i);
        if (!stage)
            continue;

        idImage *img = stage->texture.image;

        if (stage->lighting == SL_AMBIENT && !emissiveImg)
        {
            const int blendBits = stage->drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS);
            const int srcBlend = blendBits & GLS_SRCBLEND_BITS;
            const int dstBlend = blendBits & GLS_DSTBLEND_BITS;
            const bool isAdditive = (srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ONE);
            const bool isCinematic = (stage->texture.cinematic != NULL);
            const bool isExplicitUV = (stage->texture.texgen == TG_EXPLICIT);
            const bool isPerforated = (shader->Coverage() == MC_PERFORATED);

            if ((isCinematic || (isExplicitUV && isAdditive && !isPerforated)) && img != NULL)
            {
                emissiveImg = img;
                emissiveIsCinematic = isCinematic;
            }
        }

        if (!img)
            continue;

        if (stage->lighting == SL_DIFFUSE && !diffuseImg)
            diffuseImg = img;
    }

    bool guiEmissive = false;
    if (!emissiveImg && shader->HasGui() && diffuseImg)
    {
        emissiveImg = diffuseImg;
        guiEmissive = true;
    }

    if (outEmissiveImage)
        *outEmissiveImage = emissiveImg;
    if (outGuiEmissive)
        *outGuiEmissive = guiEmissive;
    if (outIsCinematic)
        *outIsCinematic = emissiveIsCinematic;

    return emissiveImg != NULL;
}

// ---------------------------------------------------------------------------
// VK_RT_MakeMaterialEntry
//
// Converts an idMaterial + vkBLAS_t into a VkMaterialEntry ready for upload
// to the material SSBO.  Also writes all per-geometry device addresses into
// outGeomVtxAddrs/outGeomIdxAddrs starting at baseGeomIdx.
//
// shader may be NULL — all fields get safe defaults.
// blas   may be NULL — geometry addresses are set to 0.
// ---------------------------------------------------------------------------

VkMaterialEntry VK_RT_MakeMaterialEntry(const idMaterial *shader, const vkBLAS_t *blas, uint32_t baseGeomIdx,
                                        uint64_t *outGeomVtxAddrs, uint64_t *outGeomIdxAddrs)
{
    VkMaterialEntry entry = {};
    entry.diffuseTexIndex = 0; // white fallback
    entry.normalTexIndex = 1;  // flat normal fallback
    entry.roughness = 1.0f;
    entry.flags = 0;
    entry.baseGeomIdx = baseGeomIdx;
    entry.alphaThreshold = 0.5f;
    entry.maxVertex = 0xFFFFFFFFu; // sentinel: no bounds check (overridden in TLAS loop)
    entry.emissiveTexIndex = 0;
    entry.maxPrimId = 0xFFFFFFFFu; // sentinel: no bounds check (overridden in TLAS loop)

    // --- Geometry addresses ---
    // Normally written per geometry in the TLAS loop (vk_accelstruct.cpp).  Only the
    // single-surface VK_RT_BuildBLAS path (geomCount == 1) still fills them here.
    if (blas && blas->geomCount == 1 && blas->geomVertAddrs && blas->geomIdxAddrs && outGeomVtxAddrs && outGeomIdxAddrs)
    {
        outGeomVtxAddrs[baseGeomIdx] = (uint64_t)blas->geomVertAddrs[0];
        outGeomIdxAddrs[baseGeomIdx] = (uint64_t)blas->geomIdxAddrs[0];
    }

    if (!shader)
        return entry;

    // --- Coverage flags ---

    if (shader->Coverage() == MC_PERFORATED)
        entry.flags |= VK_MAT_FLAG_ALPHA_TESTED;

    if (shader->Coverage() == MC_TRANSLUCENT)
    {
        entry.flags |= VK_MAT_FLAG_GLASS;
        // Same SURFTYPE_GLASS test as the BLAS filter and compositing overlay; only these
        // may act as a reflection plane.
        if (shader->GetSurfaceType() == SURFTYPE_GLASS)
            entry.flags |= VK_MAT_FLAG_REAL_GLASS;
    }

    if (shader->GetCullType() == CT_TWO_SIDED)
        entry.flags |= VK_MAT_FLAG_TWO_SIDED;

    // Stage-less (translucent) GUI surfaces only: those are the non-opaque screens the
    // any-hit shaders special-case. GUI materials with real stages keep their own path.
    if (shader->HasGui() && shader->Coverage() == MC_TRANSLUCENT)
        entry.flags |= VK_MAT_FLAG_GUI_SURF;

    // --- Walk stages for diffuse and normal images ---

    for (int i = 0; i < shader->GetNumStages(); i++)
    {
        const shaderStage_t *stage = shader->GetStage(i);
        if (!stage)
            continue;

        idImage *img = stage->texture.image;

        // Skip stages with no image (e.g. vertex-program-only stages) for diffuse/normal.
        if (!img)
            continue;

        if (stage->lighting == SL_DIFFUSE)
            entry.diffuseTexIndex = GetOrAssignTexIndex(img);
        else if (stage->lighting == SL_BUMP)
            entry.normalTexIndex = GetOrAssignTexIndex(img);
        // SL_SPECULAR: the Doom 3 material API has no scalar exponent to take roughness from.
    }

    // Emissive only when a static image resolved; otherwise slot 0's white fallback would
    // be sampled as bright emission.
    {
        idImage *emissiveImg = NULL;
        bool guiEmissive = false;
        if (VK_RT_MaterialIsEmissive(shader, &emissiveImg, &guiEmissive) && emissiveImg)
        {
            entry.emissiveTexIndex = GetOrAssignTexIndex(emissiveImg);
            entry.flags |= VK_MAT_FLAG_EMISSIVE;
            if (guiEmissive)
                entry.flags |= VK_MAT_FLAG_GUI_EMISSIVE;
        }
    }

    // Alpha threshold: default 0.5 for all MC_PERFORATED materials.
    // Doom3 materials don't expose the evaluated register value at this level.
    if (entry.flags & VK_MAT_FLAG_ALPHA_TESTED)
        entry.alphaThreshold = 0.5f;

    return entry;
}

// ---------------------------------------------------------------------------
// VK_RT_UploadMatTableFrame
//
// Merges static+dynamic material entries into the SSBO (same split pattern
// as the TLAS instance buffer in VK_RT_RebuildTLAS).  Also uploads the
// parallel VtxAddrTable and IdxAddrTable arrays.
//
// If the bindless texture set changed (s_bindlessDirty), rebuilds the
// bindless descriptor writes via vkUpdateDescriptorSets before returning.
// ---------------------------------------------------------------------------

void VK_RT_UploadMatTableFrame(const VkMaterialEntry *staticEntries, uint32_t staticMatCount, bool rewriteStatic,
                               const VkMaterialEntry *dynamicEntries, uint32_t dynamicMatCount,
                               const uint64_t *staticGeomVtx, const uint64_t *staticGeomIdx, uint32_t staticGeomCount,
                               const uint64_t *dynGeomVtx, const uint64_t *dynGeomIdx, uint32_t dynamicGeomCount,
                               bool rewriteStaticGeoms)
{
    if (!vkRT.matTableInitialized)
        return;

    const size_t matEntrySize = sizeof(VkMaterialEntry);
    const size_t addrSize = sizeof(uint64_t);

    // Per-slot tables: the previous frame may still be reading the other copy.
    uint8_t *matDst = (uint8_t *)vkRT.matTableMapped[vk.currentFrame];
    uint8_t *vtxDst = (uint8_t *)vkRT.vtxAddrMapped[vk.currentFrame];
    uint8_t *idxDst = (uint8_t *)vkRT.idxAddrMapped[vk.currentFrame];

    // Static block — material entries (one per geometry slot)
    if (rewriteStatic && staticMatCount > 0)
        memcpy(matDst, staticEntries, staticMatCount * matEntrySize);

    // Dynamic block — material entries (always written, immediately after static)
    if (dynamicMatCount > 0)
        memcpy(matDst + staticMatCount * matEntrySize, dynamicEntries, dynamicMatCount * matEntrySize);

    // Geometry address tables.
    // GPU-side SSBO layout: static [0..staticGeomCount-1], dynamic [staticGeomCount..sGC+dGC-1].
    // Each set is a separate source array (accumulated independently during the interleaved loop).
    if (rewriteStaticGeoms && staticGeomCount > 0)
    {
        memcpy(vtxDst, staticGeomVtx, staticGeomCount * addrSize);
        memcpy(idxDst, staticGeomIdx, staticGeomCount * addrSize);
    }
    if (dynamicGeomCount > 0)
    {
        memcpy(vtxDst + staticGeomCount * addrSize, dynGeomVtx, dynamicGeomCount * addrSize);
        memcpy(idxDst + staticGeomCount * addrSize, dynGeomIdx, dynamicGeomCount * addrSize);
    }

    // Rebuild for new images, or ones purged/reuploaded behind our back (level load, LRU,
    // vid_restart): sampling a destroyed VkImageView is a device lost.
    ValidateBindlessSlots();
    if (s_bindlessDirty[vk.currentFrame])
        RebuildBindlessDescriptors();

    // Debug summary of emissive tagging coverage.
    // Log on static rewrites at normal verbosity, and every frame only at very high verbosity.
    if ((r_vkLogRT.GetInteger() >= 1 && rewriteStatic) || r_vkLogRT.GetInteger() >= 3)
    {
        uint32_t emissiveCount = 0;
        uint32_t guiEmissiveCount = 0;
        uint32_t guiSurfCount = 0;
        uint32_t guiLiveCount = 0;
        const VkMaterialEntry *lists[2] = {staticEntries, dynamicEntries};
        const uint32_t counts[2] = {staticMatCount, dynamicMatCount};

        for (int l = 0; l < 2; l++)
        {
            for (uint32_t i = 0; i < counts[l]; i++)
            {
                const uint32_t flags = lists[l][i].flags;
                if (flags & VK_MAT_FLAG_EMISSIVE)
                    emissiveCount++;
                if (flags & VK_MAT_FLAG_GUI_EMISSIVE)
                    guiEmissiveCount++;
                if (flags & VK_MAT_FLAG_GUI_SURF)
                    guiSurfCount++;
                if (flags & VK_MAT_FLAG_GUI_LIVE)
                    guiLiveCount++;
            }
        }

        common->Printf("VK RT MatTable: emissive-tagged=%u gui-emissive=%u gui-surf=%u gui-live=%u total=%u\n",
                       emissiveCount, guiEmissiveCount, guiSurfCount, guiLiveCount, staticMatCount + dynamicMatCount);
    }

    if (r_vkLogRT.GetInteger() >= 2)
    {
        common->Printf("VK RT MatTable: uploaded — staticMat=%u%s dynamicMat=%u bindless=%u\n", staticMatCount,
                       rewriteStatic ? "(rewritten)" : "(cached)", dynamicMatCount, s_bindlessCount);
        fflush(NULL);
    }
}
