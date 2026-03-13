#pragma once

#include "framework/Common.h"
#include "renderer/Image.h"
#include "renderer/Model.h"
#include "renderer/VertexCache.h"
#include "renderer/tr_local.h"

struct IBackend
{
    // Lifecycle
    virtual void Init() = 0;
    virtual void Shutdown() = 0;
    virtual void PostSwapBuffers() = 0;

    // Resource management
    virtual void Image_Upload(idImage *img, const byte *data, int w, int h, textureFilter_t filterParm,
                              bool allowDownSizeParm, textureRepeat_t repeatParm, textureDepth_t depthParm) = 0;
    virtual void Image_Purge(idImage *img) = 0;
    virtual void VertexCache_Alloc(vertCache_t **vc, void *data, int size, bool indexBuffer) = 0;
    virtual void VertexCache_Free(vertCache_t *vc) = 0;

    // Frame dispatch
    virtual void DrawView(const viewDef_t *view) = 0;
    virtual void CopyRender(const copyRenderCommand_t &cmd) = 0;
};

class GLBackend;
class VKBackend;
extern IBackend *activeBackend; // set at init time
