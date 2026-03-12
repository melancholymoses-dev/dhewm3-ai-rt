#include "framework/Common.h"
#include "renderer/Model.h"
#include "renderer/VertexCache.h"


struct IBackend {
    // Lifecycle
    virtual void Init() = 0;
    virtual void Shutdown() = 0;
    virtual void PostSwapBuffers() = 0;

    // Resource management
    virtual void Image_Upload(idImage*, const byte*, int w, int h) = 0;
    virtual void Image_Purge(idImage*) = 0;
    virtual void VertexCache_Alloc(vertCache_t*, const void* data, int size, bool indexBuffer) = 0;
    virtual void VertexCache_Free(vertCache_t*) = 0;

    // Frame dispatch
    virtual void DrawView(const viewDef_t*) = 0;
    virtual void CopyRender(const copyRenderCommand_t&) = 0;
};

extern IBackend* activeBackend;  // set at init time