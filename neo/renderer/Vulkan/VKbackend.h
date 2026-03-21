#pragma once
#include "renderer/RendererBackend.h"

// Screenshot readback helpers (vk_backend.cpp).
// Call VK_RequestReadback() just before UpdateScreen/EndFrame for the screenshot
// frame, then VK_ReadPixels() after it returns to retrieve packed RGB data.
void VK_RequestReadback();
void VK_ReadPixels(int x, int y, int w, int h, unsigned char *out_rgb);

// Called from SDL window event handlers (events.cpp) when the window is
// minimized or restored.  Prevents vkAcquireNextImageKHR from blocking
// forever while the presentation engine holds all swapchain images.
void VK_SetWindowMinimized(bool minimized);

// Called from GLimp_SetScreenParms (glimp.cpp) when a fullscreen/windowed
// toggle succeeds.  On some drivers the SDL window-mode change invalidates
// the Vulkan surface so the already-acquired swapchain image is unusable;
// VK_RB_SwapBuffers will skip the submit and recreate the swapchain instead.
void VK_NotifyWindowModeChanged();

class VKBackend : public IBackend
{
  public:
    void Init() override;
    void Shutdown() override;
    void PostSwapBuffers() override;
    void Image_Upload(idImage *, const byte *, int w, int h, textureFilter_t, bool, textureRepeat_t,
                      textureDepth_t) override;
    void CubeImage_Upload(idImage *, const byte *const pic[6], int size) override;
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