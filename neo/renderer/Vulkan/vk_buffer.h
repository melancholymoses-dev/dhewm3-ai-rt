/*
Vulkan Buffer Headers

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/
#pragma once
#include <renderer/VertexCache.h>

void VK_VertexCache_Alloc(vertCache_t *block, const void *data, int size, bool indexBuffer);
void VK_VertexCache_Free(vertCache_t *block);