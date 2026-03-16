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
