#include "renderer/RendererBackend.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Image.h"
#include "renderer/GL/GLBackend.h"

// Forward declarations of the existing GL free functions
// (the ones that already exist in draw_glsl.cpp, tr_backend.cpp, etc.)
extern void RB_DrawView(const void *);

// might be the function we're messing with?
void GLBackend::Init()
{
    common->Printf("Starting GLBackend");
    GLimp_Init();
}

void GLBackend::Shutdown()
{
    common->Printf("Shutting down GLBackend");
    GLimp_Shutdown();
}
void GLBackend::PostSwapBuffers()
{
    GLimp_SwapBuffers();
}

void GLBackend::Image_Upload(idImage *img, const byte *data, int w, int h, textureFilter_t filterParm, bool allowDownSizeParm,
                             textureRepeat_t repeatParm, textureDepth_t depthParm)
{
    img->GenerateImage(data, w, h, filterParm, allowDownSizeParm,
                       repeatParm, depthParm);
}

void GLBackend::Image_Purge(idImage *img)
{
    img->PurgeImage();
}
void GLBackend::VertexCache_Alloc(vertCache_t **vc, void *data, int size, bool indexBuffer)
{
    vertexCache.Alloc(data, size, vc, indexBuffer);
}
void GLBackend::VertexCache_Free(vertCache_t *vc)
{
    vertexCache.Free(vc);
}

// Frame dispatch

void GLBackend::DrawView(const viewDef_t *view)
{
    RB_DrawView(view);
}

void GLBackend::CopyRender(const copyRenderCommand_t &cmd)
{
    RB_CopyRender(&cmd);
}