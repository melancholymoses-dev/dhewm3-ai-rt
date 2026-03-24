#pragma once
#include <renderer/Image.h>

void VK_Image_Upload(idImage *img, const byte *pic, int width, int height);
void VK_Image_UploadCubemap(idImage *img, const byte *const pic[6], int size);
void VK_Image_Purge(idImage *img);

// Cinematic (video) image — updated each frame before the render pass.
// cmd must be a recording command buffer outside any render pass.
bool VK_Image_UpdateCinematic(VkCommandBuffer cmd, const byte *rgba, int w, int h);
void VK_Image_GetCinematicDescriptorInfo(VkDescriptorImageInfo *out);

// Returns the underlying VkImage for a backend-uploaded idImage.
// Returns false when img is null or has no Vulkan backend data yet.
bool VK_Image_GetHandle(idImage *img, VkImage *out);