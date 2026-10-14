/*
===========================================================================

dhewm3-rt Vulkan backend - chunked buffer sub-allocator (see vk_buffer_pool.h).

Lifted from the vertex-cache chunk allocator in vk_buffer.cpp: best fit across chunks,
coalescing free list, a dedicated chunk for any request larger than the chunk size.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_buffer_pool.h"
#include <string.h>

// Nonfatal: returns NULL when the buffer or its memory cannot be created, so the caller can
// retry with a smaller chunk instead of aborting.
static vkPoolChunk_t *VK_Pool_CreateChunk(vkBufferPool_t *pool, VkDeviceSize size)
{
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = pool->usage;
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
    if (pool->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        allocInfo.pNext = &flagsInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, pool->memProps);

    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *mapped = NULL;
    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(vk.device, buf, mem, 0) != VK_SUCCESS ||
        (pool->hostMapped && vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS))
    {
        if (mem != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, mem, NULL);
        vkDestroyBuffer(vk.device, buf, NULL);
        return NULL;
    }

    // Pool chunks are among the largest allocations, so naming them keeps an RMV capture legible.
    VK_Prof_NameObject((uint64_t)buf, VK_OBJECT_TYPE_BUFFER, va("%sChunk(%dKB)", pool->name, (int)(size >> 10)));
    VK_Prof_NameObject((uint64_t)mem, VK_OBJECT_TYPE_DEVICE_MEMORY,
                       va("%sChunkMem(%dKB)", pool->name, (int)(size >> 10)));

    // Reuse a trimmed slot so chunk indices held by live allocations stay valid.
    vkPoolChunk_t *c = NULL;
    for (int i = 0; i < pool->chunks.Num(); i++)
    {
        if (pool->chunks[i]->buf == VK_NULL_HANDLE)
        {
            c = pool->chunks[i];
            break;
        }
    }
    const bool reused = (c != NULL);
    if (!c)
        c = new vkPoolChunk_t;
    c->size = size;
    c->buf = buf;
    c->mem = mem;
    c->mapped = (uint8_t *)mapped;
    vkPoolRange_t all = {0, size};
    c->freeRanges.Clear();
    c->freeRanges.Append(all);
    if (!reused)
        pool->chunks.Append(c);
    return c;
}

static int VK_Pool_ChunkIndex(const vkBufferPool_t *pool, const vkPoolChunk_t *c)
{
    for (int i = 0; i < pool->chunks.Num(); i++)
        if (pool->chunks[i] == c)
            return i;
    return -1;
}

static void VK_Pool_DestroyChunk(vkPoolChunk_t *c)
{
    if (c->mapped)
        vkUnmapMemory(vk.device, c->mem);
    if (c->buf != VK_NULL_HANDLE)
        vkDestroyBuffer(vk.device, c->buf, NULL);
    if (c->mem != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, c->mem, NULL);
    c->buf = VK_NULL_HANDLE;
    c->mem = VK_NULL_HANDLE;
    c->mapped = NULL;
    c->size = 0;
    c->freeRanges.Clear();
}

void VK_Pool_Alloc(vkBufferPool_t *pool, VkDeviceSize size, vkPoolAlloc_t *out)
{
    const VkDeviceSize allocSize = (size + pool->align - 1) & ~(pool->align - 1);

    // Best fit across all chunks; create a chunk when nothing fits.
    int bestChunk = -1, bestRange = -1;
    VkDeviceSize bestSize = ~(VkDeviceSize)0;
    for (int ci = 0; ci < pool->chunks.Num(); ci++)
    {
        const idList<vkPoolRange_t> &fr = pool->chunks[ci]->freeRanges;
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
        VkDeviceSize chunkSize = allocSize > pool->chunkSize ? allocSize : pool->chunkSize;
        vkPoolChunk_t *newChunk = VK_Pool_CreateChunk(pool, chunkSize);
        if (!newChunk && chunkSize > allocSize)
        {
            // Device memory is tight: fall back to a chunk sized for just this request.
            common->Warning("VK POOL %s: %d KB chunk failed, retrying with %d KB", pool->name, (int)(chunkSize >> 10),
                            (int)(allocSize >> 10));
            chunkSize = allocSize;
            newChunk = VK_Pool_CreateChunk(pool, chunkSize);
        }
        if (!newChunk)
            common->FatalError("VK POOL %s: out of memory allocating %d KB", pool->name, (int)(allocSize >> 10));
        bestChunk = VK_Pool_ChunkIndex(pool, newChunk);
        bestRange = 0;
        common->Printf("VK POOL %s: new chunk %d (%d KB)\n", pool->name, bestChunk, (int)(chunkSize >> 10));
    }

    vkPoolRange_t &r = pool->chunks[bestChunk]->freeRanges[bestRange];
    out->buf = pool->chunks[bestChunk]->buf;
    out->chunk = bestChunk;
    out->offset = r.offset;
    out->allocSize = allocSize;
    out->generation = pool->generation;
    r.offset += allocSize;
    r.size -= allocSize;
    if (r.size == 0)
        pool->chunks[bestChunk]->freeRanges.RemoveIndex(bestRange);
}

bool VK_Pool_IsLive(const vkBufferPool_t *pool, const vkPoolAlloc_t &a)
{
    return a.buf != VK_NULL_HANDLE && a.generation == pool->generation;
}

void VK_Pool_Free(vkBufferPool_t *pool, vkPoolAlloc_t *a)
{
    if (VK_Pool_IsLive(pool, *a))
    {
        idList<vkPoolRange_t> &fr = pool->chunks[a->chunk]->freeRanges;
        const VkDeviceSize offset = a->offset, size = a->allocSize;
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
            vkPoolRange_t r = {offset, size};
            fr.Insert(r, i);
        }
    }
    memset(a, 0, sizeof(*a));
}

void *VK_Pool_Mapped(const vkBufferPool_t *pool, const vkPoolAlloc_t &a)
{
    if (!VK_Pool_IsLive(pool, a) || !pool->chunks[a.chunk]->mapped)
        return NULL;
    return pool->chunks[a.chunk]->mapped + a.offset;
}

bool VK_Pool_Check(const vkBufferPool_t *pool, const vkPoolAlloc_t &a, VkDeviceSize usedSize)
{
    if (!VK_Pool_IsLive(pool, a))
        return true;
    const vkPoolChunk_t *c = pool->chunks[a.chunk];
    if (c->buf == a.buf && a.offset % pool->align == 0 && usedSize <= a.allocSize && a.offset + a.allocSize <= c->size)
        return true;
    static int s_warned = 0;
    if (s_warned++ < 8)
        common->Warning("VK POOL %s: bad range chunk=%d off=%d size=%d used=%d", pool->name, a.chunk, (int)a.offset,
                        (int)a.allocSize, (int)usedSize);
    return false;
}

void VK_Pool_TrimEmpty(vkBufferPool_t *pool)
{
    for (int i = 0; i < pool->chunks.Num(); i++)
    {
        vkPoolChunk_t *c = pool->chunks[i];
        if (c->buf == VK_NULL_HANDLE || c->freeRanges.Num() != 1 || c->freeRanges[0].size != c->size)
            continue;
        common->Printf("VK POOL %s: trimmed chunk %d (%d KB)\n", pool->name, i, (int)(c->size >> 10));
        VK_Pool_DestroyChunk(c);
    }
}

void VK_Pool_Destroy(vkBufferPool_t *pool)
{
    for (int i = 0; i < pool->chunks.Num(); i++)
    {
        VK_Pool_DestroyChunk(pool->chunks[i]);
        delete pool->chunks[i];
    }
    pool->chunks.Clear();
    pool->generation++;
}

void VK_Pool_Stats(const vkBufferPool_t *pool, int *chunks, VkDeviceSize *totalBytes, VkDeviceSize *freeBytes,
                   VkDeviceSize *largestFree)
{
    *chunks = 0;
    *totalBytes = *freeBytes = *largestFree = 0;
    for (int i = 0; i < pool->chunks.Num(); i++)
    {
        const vkPoolChunk_t *c = pool->chunks[i];
        if (c->buf == VK_NULL_HANDLE)
            continue;
        (*chunks)++;
        *totalBytes += c->size;
        for (int r = 0; r < c->freeRanges.Num(); r++)
        {
            *freeBytes += c->freeRanges[r].size;
            if (c->freeRanges[r].size > *largestFree)
                *largestFree = c->freeRanges[r].size;
        }
    }
}
