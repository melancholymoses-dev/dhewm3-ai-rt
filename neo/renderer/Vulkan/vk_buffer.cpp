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
// VK_UploadBuffer - copy data into a device-local buffer via a staging buffer
// ---------------------------------------------------------------------------

void VK_UploadBuffer(VkBuffer dstBuffer, const void *data, VkDeviceSize size)
{
    // Create staging buffer (host visible)
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

    VkBufferCopy region = {0, 0, size};
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

struct vkBufferData_t
{
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
};

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
    if (bd->buf != VK_NULL_HANDLE)
        vkDestroyBuffer(vk.device, bd->buf, NULL);
    if (bd->mem != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, bd->mem, NULL);
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

    if (r_vkLogBufferGarbage.GetBool() && (ringDrained || overflowDrained))
    {
        common->Printf("VK buf drain slot %u: ring %d, overflow %d -> live %d bufs / %d KB\n", frameIdx, ringDrained,
                       overflowDrained, s_bufferLiveCount, (int)(s_bufferLiveBytes >> 10));
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

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (indexBuffer)
    {
        usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    else
    {
        usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }

    // Allow use as geometry input for BLAS builds
    usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    vkBufferData_t *bd = new vkBufferData_t;
    bd->size = (VkDeviceSize)size;
    VK_CreateBuffer((VkDeviceSize)size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &bd->buf, &bd->mem);
    VK_UploadBuffer(bd->buf, data, (VkDeviceSize)size);
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
    if (!bd || bd->buf == VK_NULL_HANDLE)
        return false;
    *outBuf = bd->buf;
    *outOffset = (VkDeviceSize)block->offset; // always 0 for static blocks
    return true;
}
