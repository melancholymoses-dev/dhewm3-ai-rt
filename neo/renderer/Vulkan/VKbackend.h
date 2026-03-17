#pragma once
#include "renderer/RendererBackend.h"

// Screenshot readback helpers (vk_backend.cpp).
// Call VK_RequestReadback() just before UpdateScreen/EndFrame for the screenshot
// frame, then VK_ReadPixels() after it returns to retrieve packed RGB data.
void VK_RequestReadback();
void VK_ReadPixels(int x, int y, int w, int h, unsigned char *out_rgb);

class VKBackend : public IBackend
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
    void DrawView(const drawSurfsCommand_t *) override;
    void CopyRender(const copyRenderCommand_t &) override;
};