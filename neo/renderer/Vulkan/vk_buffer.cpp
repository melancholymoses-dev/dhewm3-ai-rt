/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan backend - buffer allocation and upload.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Model.h"
#include "renderer/Vulkan/vk_common.h"
#include <string.h>

// ---------------------------------------------------------------------------
// VK_CreateBuffer - allocate a VkBuffer + VkDeviceMemory pair
// ---------------------------------------------------------------------------

void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps, VkBuffer *outBuffer,
                     VkDeviceMemory *outMemory)
{
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateBuffer(vk.device, &bufInfo, NULL, outBuffer));

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, *outBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, memProps);

    VkMemoryAllocateFlagsInfo flagsInfo = {};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
    {
        flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocInfo.pNext = &flagsInfo;
    }

    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, outMemory));
    VK_CHECK(vkBindBufferMemory(vk.device, *outBuffer, *outMemory, 0));
}

// ---------------------------------------------------------------------------
// VK_CreateBufferPreferred - as above, with a fallback property set
// ---------------------------------------------------------------------------

bool VK_CreateBufferPreferred(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags preferredProps,
                              VkMemoryPropertyFlags requiredProps, VkBuffer *outBuffer, VkDeviceMemory *outMemory)
{
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateBuffer(vk.device, &bufInfo, NULL, outBuffer));

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, *outBuffer, &memReqs);

    uint32_t typeIdx = VK_TryFindMemoryType(memReqs.memoryTypeBits, preferredProps);
    const bool gotPreferred = (typeIdx != UINT32_MAX);
    if (!gotPreferred)
        typeIdx = VK_FindMemoryType(memReqs.memoryTypeBits, requiredProps);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = typeIdx;

    VkMemoryAllocateFlagsInfo flagsInfo = {};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
    {
        flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocInfo.pNext = &flagsInfo;
    }

    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, outMemory));
    VK_CHECK(vkBindBufferMemory(vk.device, *outBuffer, *outMemory, 0));
    return gotPreferred;
}

// ---------------------------------------------------------------------------
// VK_UploadBuffer - copy data into a device-local buffer via a staging buffer
// ---------------------------------------------------------------------------

// Staging ring: one persistently mapped host-visible buffer shared by every upload in the
// open batch, replacing a create/alloc/map/free staging buffer per call. Reset once the
// batch has been submitted and waited on (VK_FlushPendingUploads via VK_StagingRing_Reset).
static const VkDeviceSize VK_STAGING_RING_SIZE = 16u * 1024u * 1024u;
static const VkDeviceSize VK_STAGING_ALIGN = 16;
static VkBuffer s_stagingRingBuf = VK_NULL_HANDLE;
static VkDeviceMemory s_stagingRingMem = VK_NULL_HANDLE;
static byte *s_stagingRingPtr = NULL;
static VkDeviceSize s_stagingRingOffset = 0;

void VK_StagingRing_Reset(void)
{
    s_stagingRingOffset = 0;
}

void VK_StagingRing_Shutdown(void)
{
    if (s_stagingRingPtr)
        vkUnmapMemory(vk.device, s_stagingRingMem);
    if (s_stagingRingBuf != VK_NULL_HANDLE)
        vkDestroyBuffer(vk.device, s_stagingRingBuf, NULL);
    if (s_stagingRingMem != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, s_stagingRingMem, NULL);
    s_stagingRingBuf = VK_NULL_HANDLE;
    s_stagingRingMem = VK_NULL_HANDLE;
    s_stagingRingPtr = NULL;
    s_stagingRingOffset = 0;
}

// Returns false when the request can't use the ring (larger than the ring itself).
static bool VK_UploadBufferRing(VkBuffer dstBuffer, VkDeviceSize dstOffset, const void *data, VkDeviceSize size)
{
    if (size > VK_STAGING_RING_SIZE)
        return false;

    if (s_stagingRingBuf == VK_NULL_HANDLE)
    {
        VK_CreateBuffer(VK_STAGING_RING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s_stagingRingBuf,
                        &s_stagingRingMem);
        void *mapped = NULL;
        VK_CHECK(vkMapMemory(vk.device, s_stagingRingMem, 0, VK_STAGING_RING_SIZE, 0, &mapped));
        s_stagingRingPtr = (byte *)mapped;
        s_stagingRingOffset = 0;
    }

    VkDeviceSize start = (s_stagingRingOffset + VK_STAGING_ALIGN - 1) & ~(VK_STAGING_ALIGN - 1);
    if (start + size > VK_STAGING_RING_SIZE)
    {
        // Ring full: submit and wait on what is recorded so far, which also resets the ring.
        VK_FlushPendingUploads();
        start = 0;
    }

    memcpy(s_stagingRingPtr + start, data, (size_t)size);
    s_stagingRingOffset = start + size;

    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
    VkBufferCopy region = {start, dstOffset, size};
    vkCmdCopyBuffer(cmd, s_stagingRingBuf, dstBuffer, 1, &region);
    VK_EndSingleTimeCommands(cmd);
    return true;
}

static void VK_UploadBufferAt(VkBuffer dstBuffer, VkDeviceSize dstOffset, const void *data, VkDeviceSize size)
{
    if (VK_UploadBufferRing(dstBuffer, dstOffset, data, size))
        return;

    // Oversize request: dedicated staging buffer, freed at the next flush.
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    VK_CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging, &stagingMem);

    // Map and copy
    void *mapped;
    VK_CHECK(vkMapMemory(vk.device, stagingMem, 0, size, 0, &mapped));
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(vk.device, stagingMem);

    // Copy staging -> device-local
    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();

    VkBufferCopy region = {0, dstOffset, size};
    vkCmdCopyBuffer(cmd, staging, dstBuffer, 1, &region);

    VK_EndSingleTimeCommands(cmd);

    // Staging buffer must stay alive until the GPU copy completes.
    // VK_FlushPendingUploads() will wait on the fence then free it.
    VK_DeferStagingFree(staging, stagingMem);
}

// ---------------------------------------------------------------------------
// VK_MapBuffer / VK_UnmapBuffer - for host-visible buffers (uniform buffers)
// ---------------------------------------------------------------------------

void *VK_MapBuffer(VkDeviceMemory memory, VkDeviceSize size)
{
    void *ptr = NULL;
    VK_CHECK(vkMapMemory(vk.device, memory, 0, size, 0, &ptr));
    return ptr;
}

void VK_UnmapBuffer(VkDeviceMemory memory)
{
    vkUnmapMemory(vk.device, memory);
}

// ---------------------------------------------------------------------------
// Vertex cache Vulkan buffer management
// ---------------------------------------------------------------------------

// Device-local chunk sub-allocator. A block is a range inside a shared chunk buffer, so
// Alloc/Free cost a free-list edit instead of vkCreateBuffer + vkAllocateMemory.
// Ranges return to the free list only from the garbage drain (after the slot fence).
static const VkDeviceSize VK_VC_CHUNK_SIZE = 32u * 1024u * 1024u;
static const VkDeviceSize VK_VC_ALIGN = 64;

struct vkVcRange_t
{
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct vkVcChunk_t
{
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    idList<vkVcRange_t> freeRanges; // sorted by offset, coalesced
};

static idList<vkVcChunk_t *> s_vcChunks;
static int s_vcGeneration = 0; // bumped at shutdown so blocks from a dead device are not returned to new chunks

struct vkBufferData_t
{
    VkBuffer buf; // chunk buffer
    int chunk;
    VkDeviceSize offset;
    VkDeviceSize size;       // requested bytes
    VkDeviceSize allocSize; // aligned range length
    int generation;
};

static idCVar r_vkVcCheck("r_vkVcCheck", "0", CVAR_RENDERER | CVAR_BOOL,
                          "validate vertex-cache block range/alignment in VK_VertexCache_GetBuffer");

// Nonfatal: returns NULL when the buffer or its memory cannot be created, so the caller can
// retry with a smaller chunk instead of aborting.
static vkVcChunk_t *VK_VcCreateChunk(VkDeviceSize size)
{
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk.device, &bufInfo, NULL, &buf) != VK_SUCCESS)
        return NULL;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, buf, &memReqs);

    VkMemoryAllocateFlagsInfo flagsInfo = {};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &flagsInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(vk.device, buf, mem, 0) != VK_SUCCESS)
    {
        if (mem != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, mem, NULL);
        vkDestroyBuffer(vk.device, buf, NULL);
        return NULL;
    }

    // Reuse a trimmed slot so chunk indices held by live blocks stay valid.
    vkVcChunk_t *c = NULL;
    for (int i = 0; i < s_vcChunks.Num(); i++)
    {
        if (s_vcChunks[i]->buf == VK_NULL_HANDLE)
        {
            c = s_vcChunks[i];
            break;
        }
    }
    const bool reused = (c != NULL);
    if (!c)
        c = new vkVcChunk_t;
    c->size = size;
    c->buf = buf;
    c->mem = mem;
    vkVcRange_t all = {0, size};
    c->freeRanges.Clear();
    c->freeRanges.Append(all);
    if (!reused)
        s_vcChunks.Append(c);
    return c;
}

static int VK_VcChunkIndex(const vkVcChunk_t *c)
{
    for (int i = 0; i < s_vcChunks.Num(); i++)
        if (s_vcChunks[i] == c)
            return i;
    return -1;
}

// Best fit across all chunks; creates a chunk when nothing fits.
static void VK_VcAllocRange(VkDeviceSize allocSize, int *outChunk, VkDeviceSize *outOffset)
{
    int bestChunk = -1, bestRange = -1;
    VkDeviceSize bestSize = ~(VkDeviceSize)0;
    for (int ci = 0; ci < s_vcChunks.Num(); ci++)
    {
        const idList<vkVcRange_t> &fr = s_vcChunks[ci]->freeRanges;
        for (int ri = 0; ri < fr.Num(); ri++)
        {
            if (fr[ri].size >= allocSize && fr[ri].size < bestSize)
            {
                bestSize = fr[ri].size;
                bestChunk = ci;
                bestRange = ri;
            }
        }
    }

    if (bestChunk < 0)
    {
        VkDeviceSize chunkSize = allocSize > VK_VC_CHUNK_SIZE ? allocSize : VK_VC_CHUNK_SIZE;
        vkVcChunk_t *newChunk = VK_VcCreateChunk(chunkSize);
        if (!newChunk && chunkSize > allocSize)
        {
            // Device memory is tight: fall back to a chunk sized for just this block.
            common->Warning("VK VC: %d MB chunk failed, retrying with %d KB", (int)(chunkSize >> 20),
                            (int)(allocSize >> 10));
            chunkSize = allocSize;
            newChunk = VK_VcCreateChunk(chunkSize);
        }
        if (!newChunk)
            common->FatalError("VK VC: out of device-local memory allocating %d KB vertex cache block",
                               (int)(allocSize >> 10));
        bestChunk = VK_VcChunkIndex(newChunk);
        bestRange = 0;
        common->Printf("VK VC: new chunk %d (%d MB)\n", bestChunk, (int)(chunkSize >> 20));
    }

    vkVcRange_t &r = s_vcChunks[bestChunk]->freeRanges[bestRange];
    *outChunk = bestChunk;
    *outOffset = r.offset;
    r.offset += allocSize;
    r.size -= allocSize;
    if (r.size == 0)
        s_vcChunks[bestChunk]->freeRanges.RemoveIndex(bestRange);
}

static void VK_VcFreeRange(int chunk, VkDeviceSize offset, VkDeviceSize size)
{
    idList<vkVcRange_t> &fr = s_vcChunks[chunk]->freeRanges;
    int i = 0;
    while (i < fr.Num() && fr[i].offset < offset)
        i++;

    const bool mergePrev = (i > 0) && (fr[i - 1].offset + fr[i - 1].size == offset);
    const bool mergeNext = (i < fr.Num()) && (offset + size == fr[i].offset);
    if (mergePrev && mergeNext)
    {
        fr[i - 1].size += size + fr[i].size;
        fr.RemoveIndex(i);
    }
    else if (mergePrev)
        fr[i - 1].size += size;
    else if (mergeNext)
    {
        fr[i].offset = offset;
        fr[i].size += size;
    }
    else
    {
        vkVcRange_t r = {offset, size};
        fr.Insert(r, i);
    }
}

static int s_vcTrimDrains = 0; // drains left in which to trim after a level load

// Request a trim after a level load; the old level's blocks only return to the free
// list once their garbage slots drain, so the trim runs over the next few drains.
void VK_VertexCache_RequestTrim(void)
{
    s_vcTrimDrains = VK_MAX_FRAMES_IN_FLIGHT * 2;
}

static void VK_VcTrimEmptyChunks(void)
{
    for (int i = 0; i < s_vcChunks.Num(); i++)
    {
        vkVcChunk_t *c = s_vcChunks[i];
        if (c->buf == VK_NULL_HANDLE || c->freeRanges.Num() != 1 || c->freeRanges[0].size != c->size)
            continue;
        common->Printf("VK VC: trimmed chunk %d (%d MB)\n", i, (int)(c->size >> 20));
        vkDestroyBuffer(vk.device, c->buf, NULL);
        vkFreeMemory(vk.device, c->mem, NULL);
        c->buf = VK_NULL_HANDLE;
        c->mem = VK_NULL_HANDLE;
        c->size = 0;
        c->freeRanges.Clear();
    }
}

void VK_VertexCache_Shutdown(void)
{
    for (int i = 0; i < s_vcChunks.Num(); i++)
    {
        vkVcChunk_t *c = s_vcChunks[i];
        if (c->buf != VK_NULL_HANDLE)
            vkDestroyBuffer(vk.device, c->buf, NULL);
        if (c->mem != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, c->mem, NULL);
        delete c;
    }
    s_vcChunks.Clear();
    s_vcGeneration++;
}

// Live vertex-cache buffer accounting. A steadily climbing live count/bytes is a
// leak; a high drain volume with a flat live count is just churn.
static int s_bufferLiveCount = 0;
static VkDeviceSize s_bufferLiveBytes = 0;

static idCVar r_vkLogBufferGarbage("r_vkLogBufferGarbage", "0", CVAR_RENDERER | CVAR_BOOL,
                                   "log vertex-cache buffer alloc/free accounting every drain");

// ---------------------------------------------------------------------------
// Buffer garbage list — deferred destruction after the per-frame fence fires,
// mirroring the image garbage pattern in vk_image.cpp.
// ---------------------------------------------------------------------------

static const int VK_BUFFER_GARBAGE_MAX = 512;
static vkBufferData_t *s_bufferGarbage[VK_MAX_FRAMES_IN_FLIGHT][VK_BUFFER_GARBAGE_MAX];
static int s_bufferGarbageCount[VK_MAX_FRAMES_IN_FLIGHT] = {};
static uint32_t s_bufferGarbageOverflowCount[VK_MAX_FRAMES_IN_FLIGHT] = {};
static idList<vkBufferData_t *> s_bufferGarbageOverflow[VK_MAX_FRAMES_IN_FLIGHT];

static void VK_DestroyBufferData(vkBufferData_t *bd)
{
    if (bd->generation == s_vcGeneration)
        VK_VcFreeRange(bd->chunk, bd->offset, bd->allocSize);
    s_bufferLiveCount--;
    s_bufferLiveBytes -= bd->size;
    delete bd;
}

void VK_Buffer_DrainGarbage(uint32_t frameIdx)
{
    const int ringDrained = s_bufferGarbageCount[frameIdx];
    for (int i = 0; i < ringDrained; i++)
    {
        VK_DestroyBufferData(s_bufferGarbage[frameIdx][i]);
    }
    s_bufferGarbageCount[frameIdx] = 0;

    const int overflowDrained = s_bufferGarbageOverflow[frameIdx].Num();
    for (int i = 0; i < overflowDrained; i++)
    {
        VK_DestroyBufferData(s_bufferGarbageOverflow[frameIdx][i]);
    }
    s_bufferGarbageOverflow[frameIdx].Clear();

    if (s_bufferGarbageOverflowCount[frameIdx] > 0)
    {
        common->Warning("VK: buffer garbage ring overflowed slot %u by %u (ring max %d); live %d bufs / %d KB",
                        frameIdx, s_bufferGarbageOverflowCount[frameIdx], VK_BUFFER_GARBAGE_MAX, s_bufferLiveCount,
                        (int)(s_bufferLiveBytes >> 10));
        s_bufferGarbageOverflowCount[frameIdx] = 0;
    }

    if (s_vcTrimDrains > 0)
    {
        s_vcTrimDrains--;
        VK_VcTrimEmptyChunks();
    }

    // Accumulate and log once per 30 drains instead of every frame.
    static int s_logDrains = 0, s_logRing = 0, s_logOverflow = 0;
    if (r_vkLogBufferGarbage.GetBool())
    {
        s_logDrains++;
        s_logRing += ringDrained;
        s_logOverflow += overflowDrained;
    }
    else
    {
        s_logDrains = s_logRing = s_logOverflow = 0;
    }

    if (r_vkLogBufferGarbage.GetBool() && s_logDrains >= 30)
    {
        int chunks = 0;
        VkDeviceSize totalBytes = 0, freeBytes = 0, largestFree = 0;
        for (int i = 0; i < s_vcChunks.Num(); i++)
        {
            const vkVcChunk_t *c = s_vcChunks[i];
            if (c->buf == VK_NULL_HANDLE)
                continue;
            chunks++;
            totalBytes += c->size;
            for (int r = 0; r < c->freeRanges.Num(); r++)
            {
                freeBytes += c->freeRanges[r].size;
                if (c->freeRanges[r].size > largestFree)
                    largestFree = c->freeRanges[r].size;
            }
        }
        common->Printf("VK buf drain (last %d): freed %d (overflow %d) -> live %d bufs / %d KB | chunks %d / %d MB, "
                       "free %d MB (largest %d KB)\n",
                       s_logDrains, s_logRing + s_logOverflow, s_logOverflow, s_bufferLiveCount,
                       (int)(s_bufferLiveBytes >> 10), chunks, (int)(totalBytes >> 20), (int)(freeBytes >> 20),
                       (int)(largestFree >> 10));
        s_logDrains = s_logRing = s_logOverflow = 0;
    }
}

void VK_Buffer_DrainAllGarbage()
{
    for (int f = 0; f < VK_MAX_FRAMES_IN_FLIGHT; f++)
        VK_Buffer_DrainGarbage(f);
}

void VK_VertexCache_Alloc(vertCache_t *block, const void *data, int size, bool indexBuffer)
{
    if (!vk.isInitialized || !data || size <= 0)
        return;

    // indexBuffer is unused: chunks carry both vertex and index usage.
    (void)indexBuffer;

    vkBufferData_t *bd = new vkBufferData_t;
    bd->size = (VkDeviceSize)size;
    bd->allocSize = ((VkDeviceSize)size + VK_VC_ALIGN - 1) & ~(VK_VC_ALIGN - 1);
    VK_VcAllocRange(bd->allocSize, &bd->chunk, &bd->offset);
    bd->buf = s_vcChunks[bd->chunk]->buf;
    bd->generation = s_vcGeneration;
    VK_UploadBufferAt(bd->buf, bd->offset, data, (VkDeviceSize)size);
    s_bufferLiveCount++;
    s_bufferLiveBytes += bd->size;
    block->backendData = bd;
}

void VK_VertexCache_Free(vertCache_t *block)
{
    // Mirrors GL_VertexCache_Free: the CPU-side copy from idVertexCache::Alloc is
    // owned by the block whether or not a device buffer was ever created for it,
    // so release it ahead of the early-outs below.
    if (block->virtMem)
    {
        Mem_Free(block->virtMem);
        block->virtMem = NULL;
    }

    if (!vk.isInitialized)
        return;

    vkBufferData_t *bd = (vkBufferData_t *)block->backendData;
    if (!bd)
        return;

    // Defer destruction until the frame fence has fired for the slot that last
    // rendered with this buffer. vk.currentFrame is already incremented after
    // submit, so the slot that just rendered is (currentFrame - 1). Queuing
    // into that slot means the drain waits for its fence before destroying,
    // which covers the command buffer that referenced this buffer.
    uint32_t frameIdx = (vk.currentFrame + VK_MAX_FRAMES_IN_FLIGHT - 1) % VK_MAX_FRAMES_IN_FLIGHT;
    if (s_bufferGarbageCount[frameIdx] < VK_BUFFER_GARBAGE_MAX)
    {
        s_bufferGarbage[frameIdx][s_bufferGarbageCount[frameIdx]++] = bd;
    }
    else
    {
        // Garbage ring full: keep deferring in overflow list so we never destroy
        // resources that might still be referenced by in-flight command buffers.
        s_bufferGarbageOverflowCount[frameIdx]++;
        s_bufferGarbageOverflow[frameIdx].Append(bd);
    }
    block->backendData = NULL;
}

// Returns true and fills buf/offset when the block has a valid Vulkan buffer.
bool VK_VertexCache_GetBuffer(vertCache_t *block, VkBuffer *outBuf, VkDeviceSize *outOffset)
{
    if (!block)
        return false;
    vkBufferData_t *bd = (vkBufferData_t *)block->backendData;
    if (!bd || bd->buf == VK_NULL_HANDLE || bd->generation != s_vcGeneration)
        return false;
    if (r_vkVcCheck.GetBool() &&
        (bd->offset % VK_VC_ALIGN != 0 || bd->offset + bd->size > s_vcChunks[bd->chunk]->size))
    {
        static int s_warned = 0;
        if (s_warned++ < 8)
            common->Warning("VK VC: bad block chunk=%d off=%d size=%d", bd->chunk, (int)bd->offset, (int)bd->size);
    }
    *outBuf = bd->buf;
    *outOffset = bd->offset;
    return true;
}
