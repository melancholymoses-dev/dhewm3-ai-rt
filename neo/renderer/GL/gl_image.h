/*
The vast majority of this code came from the original Dhewm3 OpenGL implementation and is refactored here.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

#pragma once

#include "renderer/Image.h"

// GL-specific image helpers called directly by GLBackend.
// These are the implementations behind idImage::GenerateImage / PurgeImage
// for the GL backend.  They must NOT be called when Vulkan is active.
void GL_GenerateTexture(idImage *img, const byte *pic, int width, int height, textureFilter_t filterParm,
                        bool allowDownSizeParm, textureRepeat_t repeatParm, textureDepth_t depthParm);
void GL_PurgeTexture(idImage *img);
