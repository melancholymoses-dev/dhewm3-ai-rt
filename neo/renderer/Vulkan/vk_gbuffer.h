/*
===========================================================================

dhewm3-rt Vulkan — vk_gbuffer.h — G-buffer normal/F0 image lifetime.

Allocates the per-frame R8G8B8A8_UNORM image written by the depth prepass and
sampled by the RT reflection (and, in a later commit, AO/GI) passes:
rgb = world-space bump-mapped normal (× 0.5 + 0.5), a = normalized F0.
See docs/plans/gbuffer_normal_pass.md for the full design.

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

// Allocate the G-buffer normal/F0 images at the current swapchain extent.
// No-op (leaves vkRT.gbufNormal all-NULL) if vk.gbufferSupported is false.
// Called once after device creation (inside VK_RT_Init block), after tonemap init.
void VK_RT_InitGBuffer(void);

// Destroy all G-buffer resources.  Safe to call even if never initialized.
void VK_RT_ShutdownGBuffer(void);

// Reallocate the G-buffer images at the new swapchain dimensions.
// Calls vkDeviceWaitIdle internally; do not call from a hot path.
void VK_RT_ResizeGBuffer(uint32_t width, uint32_t height);
