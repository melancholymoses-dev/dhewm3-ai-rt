#pragma once
#include <renderer/Image.h>

void VK_Image_Upload(idImage *img, const byte *pic, int width, int height);
void VK_Image_Purge(idImage *img);