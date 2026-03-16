#pragma once

#include "renderer/Image.h"

// GL-specific image helpers called directly by GLBackend.
// These are the implementations behind idImage::GenerateImage / PurgeImage
// for the GL backend.  They must NOT be called when Vulkan is active.
void GL_GenerateTexture(idImage *img, const byte *pic, int width, int height,
                        textureFilter_t filterParm, bool allowDownSizeParm,
                        textureRepeat_t repeatParm, textureDepth_t depthParm);
void GL_PurgeTexture(idImage *img);
