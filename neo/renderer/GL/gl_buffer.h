/*
The vast majority of this code came from the original Dhewm3 OpenGL implementation and is refactored here.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

#pragma once
#include "renderer/VertexCache.h"

// GL-specific vertex buffer resource management.
// Called through GLBackend::VertexCache_Free -> activeBackend->VertexCache_Free.
void GL_VertexCache_Free(vertCache_t *block);
