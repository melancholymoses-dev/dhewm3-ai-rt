/*
===========================================================================

dhewm3-rt Vulkan backend - chunked buffer sub-allocator.

A pool hands out aligned ranges inside large shared VkBuffers, so an allocation is a
free-list edit instead of vkCreateBuffer + vkAllocateMemory (~0.3 ms each on Windows).
Used by the vertex cache and by BLAS storage, scratch and geometry.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#ifndef __VK_BUFFER_POOL_H__
#define __VK_BUFFER_POOL_H__

#include "renderer/Vulkan/vk_common.h"

struct vkPoolRange_t
{
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct vkPoolChunk_t
{
    VkBuffer buf; // VK_NULL_HANDLE for a trimmed slot awaiting reuse
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint8_t *mapped; // persistent mapping, NULL unless the pool is host-mapped
    idList<vkPoolRange_t> freeRanges; // sorted by offset, coalesced
};

struct vkBufferPool_t
{
    const char *name;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags memProps;
    VkDeviceSize chunkSize;
    VkDeviceSize align; // power of two; every range offset and length is a multiple
    bool hostMapped;
    idList<vkPoolChunk_t *> chunks; // indices stay stable: trimmed slots are reused
    int generation;                 // bumped by Destroy so stale allocations are ignored

    vkBufferPool_t(const char *name_, VkBufferUsageFlags usage_, VkMemoryPropertyFlags memProps_,
                   VkDeviceSize chunkSize_, VkDeviceSize align_, bool hostMapped_)
        : name(name_), usage(usage_), memProps(memProps_), chunkSize(chunkSize_), align(align_),
          hostMapped(hostMapped_), generation(0)
    {
    }
};

// One range. buf == VK_NULL_HANDLE means "no allocation".
struct vkPoolAlloc_t
{
    VkBuffer buf; // the chunk's buffer
    int chunk;
    VkDeviceSize offset;
    VkDeviceSize allocSize; // aligned length
    int generation;
};

// Fatal error when device memory is exhausted, matching the vertex cache's old behaviour.
void VK_Pool_Alloc(vkBufferPool_t *pool, VkDeviceSize size, vkPoolAlloc_t *out);
// Returns the range to the free list (no-op for an empty or stale allocation) and clears *a.
// Callers defer this until no in-flight command buffer can reference the range.
void VK_Pool_Free(vkBufferPool_t *pool, vkPoolAlloc_t *a);
bool VK_Pool_IsLive(const vkBufferPool_t *pool, const vkPoolAlloc_t &a);
// CPU pointer to the range; NULL unless the pool is host-mapped.
void *VK_Pool_Mapped(const vkBufferPool_t *pool, const vkPoolAlloc_t &a);
// Range/alignment validation for debug cvars; warns (rate-limited) and returns false on a bad range.
bool VK_Pool_Check(const vkBufferPool_t *pool, const vkPoolAlloc_t &a, VkDeviceSize usedSize);
// Destroys chunks that are entirely free.
void VK_Pool_TrimEmpty(vkBufferPool_t *pool);
// Destroys every chunk; outstanding allocations become stale.
void VK_Pool_Destroy(vkBufferPool_t *pool);
void VK_Pool_Stats(const vkBufferPool_t *pool, int *chunks, VkDeviceSize *totalBytes, VkDeviceSize *freeBytes,
                   VkDeviceSize *largestFree);

#endif // __VK_BUFFER_POOL_H__
