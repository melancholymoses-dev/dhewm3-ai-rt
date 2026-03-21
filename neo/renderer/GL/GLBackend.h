#include "renderer/RendererBackend.h"

class GLBackend : public IBackend
{
  public:
    void Init() override;
    void Shutdown() override;
    void PostSwapBuffers() override;
    void Image_Upload(idImage *, const byte *, int w, int h, textureFilter_t, bool, textureRepeat_t,
                      textureDepth_t) override;
    void Image_Purge(idImage *) override;
    void VertexCache_Alloc(vertCache_t **, void *, int size, bool indexBuffer) override;
    void VertexCache_Free(vertCache_t *) override;
    void BeginCommandBatch() override;
    void EndCommandBatch() override;
    void SetBuffer(const void *data) override;
    void SwapBuffers(const void *data) override;
    void DrawView(const drawSurfsCommand_t *) override;
    void CopyRender(const copyRenderCommand_t &) override;
};