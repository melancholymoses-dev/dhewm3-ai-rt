/*
The vast majority of this code came from the original Dhewm3 OpenGL implementation and is refactored here.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"

/*
==============
GL_VertexCache_Free

Releases the GL-side resource held by a vertex cache block.
For VBO-backed blocks the handle is retained for reuse; for virtual-memory
blocks the CPU allocation is freed.  Called by GLBackend::VertexCache_Free,
which is invoked from idVertexCache::ActuallyFree via activeBackend.
==============
*/
void GL_VertexCache_Free(vertCache_t *block)
{
    if (block->vbo)
    {
#if 0 // not necessary — the VBO will be reused
        qglBindBufferARB(GL_ARRAY_BUFFER_ARB, block->vbo);
        qglBufferDataARB(GL_ARRAY_BUFFER_ARB, 0, 0, GL_DYNAMIC_DRAW_ARB);
#endif
    }
    else if (block->virtMem)
    {
        Mem_Free(block->virtMem);
        block->virtMem = NULL;
    }
}
