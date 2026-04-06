/*
Vulkan Image Headers
This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

#pragma once
#include <renderer/Image.h>

void VK_Image_Upload(idImage *img, const byte *pic, int width, int height);
void VK_Image_UploadCubemap(idImage *img, const byte *const pic[6], int size);
void VK_Image_Purge(idImage *img);

// Cinematic (video) image — updated each frame before the render pass.
// cmd must be a recording command buffer outside any render pass.
bool VK_Image_UpdateCinematic(VkCommandBuffer cmd, const byte *rgba, int w, int h);
void VK_Image_GetCinematicDescriptorInfo(VkDescriptorImageInfo *out);
bool VK_Image_GetDescriptorInfoCube(idImage *img, VkDescriptorImageInfo *out);

// Returns the underlying VkImage for a backend-uploaded idImage.
// Returns false when img is null or has no Vulkan backend data yet.
bool VK_Image_GetHandle(idImage *img, VkImage *out);

// Returns the actual Vulkan image extent for a backend-uploaded idImage.
// Returns false when img is null or has no Vulkan backend data yet.
bool VK_Image_GetExtent(idImage *img, int *outW, int *outH);