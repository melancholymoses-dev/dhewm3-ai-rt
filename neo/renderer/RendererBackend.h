#pragma once

#include "framework/Common.h"
#include "renderer/Model.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Image.h"

struct IBackend
{
    // Lifecycle
    virtual void Init() = 0;
    virtual void Shutdown() = 0;
    virtual void PostSwapBuffers() = 0;

    // Resource management
    virtual void Image_Upload(idImage *img, const byte *data, int w, int h, textureFilter_t filterParm,
                              bool allowDownSizeParm, textureRepeat_t repeatParm, textureDepth_t depthParm) = 0;
    virtual void CubeImage_Upload(idImage *img, const byte *const pic[6], int size) {}
    virtual void Image_Purge(idImage *img) = 0;
    virtual void VertexCache_Alloc(vertCache_t **vc, void *data, int size, bool indexBuffer) = 0;
    virtual void VertexCache_Free(vertCache_t *vc) = 0;

    // Command batch lifecycle — called once per RB_ExecuteBackEndCommands invocation
    virtual void BeginCommandBatch() = 0; // pre-loop: set default render state
    virtual void EndCommandBatch() = 0;   // post-loop: restore default texture bindings

    // Per-command handlers — dispatched from RB_ExecuteBackEndCommands
    virtual void SetBuffer(const void *data) = 0;    // RC_SET_BUFFER
    virtual void SwapBuffers(const void *data) = 0;  // RC_SWAP_BUFFERS

    // Frame dispatch
    virtual void DrawView(const drawSurfsCommand_t *cmd) = 0;
    virtual void CopyRender(const copyRenderCommand_t &cmd) = 0;
};

class GLBackend;
class VKBackend;
extern IBackend *activeBackend; // set at init time
