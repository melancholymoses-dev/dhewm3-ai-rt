#include "renderer/RendererBackend.h"

class VKBackend : public IBackend
{
public:
    void Init() override;
    void Shutdown() override;
    void PostSwapBuffers() override;
    void Image_Upload(idImage *, const byte *, int w, int h) override;
    void Image_Purge(idImage *) override;
    void VertexCache_Alloc(vertCache_t **, const void *, int size, bool indexBuffer) override;
    void VertexCache_Free(vertCache_t *) override;
    void DrawView(const viewDef_t *) override;
    void CopyRender(const copyRenderCommand_t &) override;
};