#pragma once
#include <renderer/Image.h>

void VK_Image_Upload(idImage *img, const byte *pic, int width, int height);
void VK_Image_UploadCubemap(idImage *img, const byte *const pic[6], int size);
void VK_Image_Purge(idImage *img);

// Cinematic (video) image — updated each frame before the render pass.
// cmd must be a recording command buffer outside any render pass.
bool VK_Image_UpdateCinematic(VkCommandBuffer cmd, const byte *rgba, int w, int h);
void VK_Image_GetCinematicDescriptorInfo(VkDescriptorImageInfo *out);