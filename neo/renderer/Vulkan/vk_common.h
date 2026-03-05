/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - common types and helper macros.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#ifndef __VK_COMMON_H__
#define __VK_COMMON_H__

#include <vulkan/vulkan.h>

// ---------------------------------------------------------------------------
// Error checking macro
// Aborts with a message if the VkResult is not VK_SUCCESS.
// ---------------------------------------------------------------------------

#define VK_CHECK(call) \
	do { \
		VkResult _vkr = (call); \
		if (_vkr != VK_SUCCESS) { \
			common->FatalError("Vulkan error %d in %s at %s:%d", (int)_vkr, #call, __FILE__, __LINE__); \
		} \
	} while(0)

#define VK_CHECK_NONFATAL(call, out_result) \
	do { (out_result) = (call); } while(0)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const int VK_MAX_FRAMES_IN_FLIGHT = 2;   // double-buffering
static const int VK_MAX_SWAPCHAIN_IMAGES = 8;

// ---------------------------------------------------------------------------
// Vulkan instance/device state (global, set during VKimp_Init)
// ---------------------------------------------------------------------------

struct vkState_t {
	VkInstance              instance;
	VkPhysicalDevice        physicalDevice;
	VkDevice                device;

	VkQueue                 graphicsQueue;
	VkQueue                 presentQueue;
	uint32_t                graphicsFamily;
	uint32_t                presentFamily;

	VkSurfaceKHR            surface;

	// Swapchain
	VkSwapchainKHR          swapchain;
	VkFormat                swapchainFormat;
	VkExtent2D              swapchainExtent;
	uint32_t                swapchainImageCount;
	VkImage                 swapchainImages[VK_MAX_SWAPCHAIN_IMAGES];
	VkImageView             swapchainImageViews[VK_MAX_SWAPCHAIN_IMAGES];
	VkFramebuffer           swapchainFramebuffers[VK_MAX_SWAPCHAIN_IMAGES];

	// Depth buffer
	VkImage                 depthImage;
	VkDeviceMemory          depthMemory;
	VkImageView             depthView;
	VkFormat                depthFormat;

	// Render pass
	VkRenderPass            renderPass;

	// Command pool / buffers
	VkCommandPool           commandPool;
	VkCommandBuffer         commandBuffers[VK_MAX_FRAMES_IN_FLIGHT];

	// Sync objects
	VkSemaphore             imageAvailableSemaphores[VK_MAX_FRAMES_IN_FLIGHT];
	VkSemaphore             renderFinishedSemaphores[VK_MAX_FRAMES_IN_FLIGHT];
	VkFence                 inFlightFences[VK_MAX_FRAMES_IN_FLIGHT];

	// Memory properties
	VkPhysicalDeviceMemoryProperties memProperties;

	// Capabilities
	bool                    rayTracingSupported;    // VK_KHR_ray_tracing_pipeline
	bool                    asSupported;            // VK_KHR_acceleration_structure

	uint32_t                currentFrame;    // 0..VK_MAX_FRAMES_IN_FLIGHT-1
	uint32_t                currentImageIdx; // current swapchain image index

	bool                    isInitialized;
};

extern vkState_t vk;

// ---------------------------------------------------------------------------
// Memory helper: find a memory type satisfying requirements
// ---------------------------------------------------------------------------

static inline uint32_t VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags props ) {
	for ( uint32_t i = 0; i < vk.memProperties.memoryTypeCount; i++ ) {
		if ( (typeBits & (1u << i)) &&
		     (vk.memProperties.memoryTypes[i].propertyFlags & props) == props ) {
			return i;
		}
	}
	common->FatalError( "VK_FindMemoryType: no suitable memory type found" );
	return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Single-shot command buffer helper
// ---------------------------------------------------------------------------

VkCommandBuffer VK_BeginSingleTimeCommands( void );
void            VK_EndSingleTimeCommands( VkCommandBuffer cmd );

// ---------------------------------------------------------------------------
// Image layout transition helper
// ---------------------------------------------------------------------------

void VK_TransitionImageLayout( VkCommandBuffer cmd,
                                VkImage image,
                                VkFormat format,
                                VkImageLayout oldLayout,
                                VkImageLayout newLayout );

#endif // __VK_COMMON_H__
