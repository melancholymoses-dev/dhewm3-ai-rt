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
#include "renderer/Vulkan/vk_common.h"
#include <string.h>

// ---------------------------------------------------------------------------
// VK_CreateBuffer - allocate a VkBuffer + VkDeviceMemory pair
// ---------------------------------------------------------------------------

void VK_CreateBuffer( VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memProps,
                      VkBuffer *outBuffer,
                      VkDeviceMemory *outMemory ) {
	VkBufferCreateInfo bufInfo = {};
	bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size        = size;
	bufInfo.usage       = usage;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK( vkCreateBuffer( vk.device, &bufInfo, NULL, outBuffer ) );

	VkMemoryRequirements memReqs;
	vkGetBufferMemoryRequirements( vk.device, *outBuffer, &memReqs );

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize  = memReqs.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReqs.memoryTypeBits, memProps );

	VK_CHECK( vkAllocateMemory( vk.device, &allocInfo, NULL, outMemory ) );
	VK_CHECK( vkBindBufferMemory( vk.device, *outBuffer, *outMemory, 0 ) );
}

// ---------------------------------------------------------------------------
// VK_UploadBuffer - copy data into a device-local buffer via a staging buffer
// ---------------------------------------------------------------------------

void VK_UploadBuffer( VkBuffer dstBuffer, const void *data, VkDeviceSize size ) {
	// Create staging buffer (host visible)
	VkBuffer       staging;
	VkDeviceMemory stagingMem;
	VK_CreateBuffer( size,
	                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &staging, &stagingMem );

	// Map and copy
	void *mapped;
	VK_CHECK( vkMapMemory( vk.device, stagingMem, 0, size, 0, &mapped ) );
	memcpy( mapped, data, (size_t)size );
	vkUnmapMemory( vk.device, stagingMem );

	// Copy staging -> device-local
	VkCommandBuffer cmd = VK_BeginSingleTimeCommands();

	VkBufferCopy region = { 0, 0, size };
	vkCmdCopyBuffer( cmd, staging, dstBuffer, 1, &region );

	VK_EndSingleTimeCommands( cmd );

	vkDestroyBuffer( vk.device, staging, NULL );
	vkFreeMemory( vk.device, stagingMem, NULL );
}

// ---------------------------------------------------------------------------
// VK_MapBuffer / VK_UnmapBuffer - for host-visible buffers (uniform buffers)
// ---------------------------------------------------------------------------

void *VK_MapBuffer( VkDeviceMemory memory, VkDeviceSize size ) {
	void *ptr = NULL;
	VK_CHECK( vkMapMemory( vk.device, memory, 0, size, 0, &ptr ) );
	return ptr;
}

void VK_UnmapBuffer( VkDeviceMemory memory ) {
	vkUnmapMemory( vk.device, memory );
}
