/*
GLBackend

These are the headers for the OpenGL Backend renderer class that implements most of the
methods used in the rendering loop in gl_backend.cpp.
This base definition is defined in ../RenderBackend.h,
and this implementation parallels the one in ../Vulkan/VKBackend.cpp,

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

*/

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