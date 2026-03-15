/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - buffer allocation and upload.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

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

    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, stagingMem, NULL);
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
//
// Called from VertexCache.cpp under #ifdef DHEWM3_VULKAN.
// vertCache_t stores handles as uint64_t to avoid <vulkan/vulkan.h> in the
// header; we cast to the real Vulkan types here.
// ---------------------------------------------------------------------------

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

#ifdef DHEWM3_RAYTRACING
    // Allow use as geometry input for BLAS builds
    usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
#endif

    VkBuffer buf;
    VkDeviceMemory mem;
    VK_CreateBuffer((VkDeviceSize)size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buf, &mem);
    VK_UploadBuffer(buf, data, (VkDeviceSize)size);

    // Store as uint64_t — matches VkBuffer / VkDeviceMemory underlying type on all platforms
    block->vkBuffer = (uint64_t)buf;
    block->vkMemory = (uint64_t)mem;
}

void VK_VertexCache_Free(vertCache_t *block)
{
    if (!vk.isInitialized)
        return;

    if (block->vkBuffer)
    {
        vkDestroyBuffer(vk.device, (VkBuffer)block->vkBuffer, NULL);
        block->vkBuffer = 0;
    }
    if (block->vkMemory)
    {
        vkFreeMemory(vk.device, (VkDeviceMemory)block->vkMemory, NULL);
        block->vkMemory = 0;
    }
}

// Returns true and fills buf/offset when the block has a valid Vulkan buffer.
bool VK_VertexCache_GetBuffer(vertCache_t *block, VkBuffer *outBuf, VkDeviceSize *outOffset)
{
    if (!block || !block->vkBuffer)
        return false;
    *outBuf = (VkBuffer)block->vkBuffer;
    *outOffset = (VkDeviceSize)block->offset; // always 0 for static blocks
    return true;
}
