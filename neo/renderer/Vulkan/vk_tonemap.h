/*
===========================================================================

dhewm3-rt Vulkan — vk_tonemap.h — HDR scene buffer and Uchimura tonemap resolve.

Phase 8.1: allocates a per-frame RGBA16F HDR accumulation image that replaces
the swapchain as the colour attachment for scene + composite passes.  A final
compute dispatch reads the HDR image, applies the Uchimura filmic curve, and
writes the tonemapped result to the swapchain storage image.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

// Allocate HDR scene images and create the tonemap compute pipeline.
// Called once after device creation (inside VK_RT_Init block).
void VK_RT_InitTonemap(void);

// Destroy all tonemap resources.  Device must be idle before calling.
void VK_RT_ShutdownTonemap(void);

// Reallocate HDR scene images at the new swapchain dimensions.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeTonemap(uint32_t width, uint32_t height);

// Dispatch the Uchimura tonemap compute pass: hdrScene → swapchain.
// Must be called outside a render pass, after all composites have finished.
void VK_RT_DispatchTonemap(VkCommandBuffer cmd);
