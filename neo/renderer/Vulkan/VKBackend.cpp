#include "renderer/Vulkan/VKBackend.h"
#include "renderer/VertexCache.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_buffer.h"

// Forward declarations - defined in vk_backend.cpp
extern void VKimp_PreShutdown(void);
extern void VK_RB_DrawView(const void *data);

void VKBackend::Init()
{
    common->Printf("Starting Vulkan Backend");
    // Reuses SDL window created by GL init; handles VKimp_Init + VKimp_PostInit internally
    // TODO: decouple window creation from GLimp
    extern void VKimp_InitFromGlimp(int width, int height);
    VKimp_InitFromGlimp(glConfig.vidWidth, glConfig.vidHeight);
}

void VKBackend::Shutdown()
{
    common->Printf("Shutting down Vulkan Backend");
    VKimp_PreShutdown();
}
void VKBackend::PostSwapBuffers()
{
    // this space left as no-op
}

void VKBackend::Image_Upload(idImage *img, const byte *data, int w, int h, textureFilter_t, bool, textureRepeat_t,
                             textureDepth_t)
{
    // drops the texture filter/repeat arguments
    VK_Image_Upload(img, data, w, h);
}

void VKBackend::Image_Purge(idImage *img)
{
    VK_Image_Purge(img);
}
void VKBackend::VertexCache_Alloc(vertCache_t **vc, void *data, int size, bool indexBuffer)
{
    // Populate the shared linked-list header first, then create the device-local buffer.
    // TODO: skip GPU alloc for temp (stream) buffers once allocatingTempBuffer is exposed.
    vertexCache.Alloc(data, size, vc, indexBuffer);
    if (*vc && data)
    {
        VK_VertexCache_Alloc(*vc, data, size, indexBuffer);
    }
}
void VKBackend::VertexCache_Free(vertCache_t *vc)
{
    VK_VertexCache_Free(vc);
}

// Frame dispatch

void VKBackend::DrawView(const viewDef_t *view)
{
    VK_RB_DrawView((const void *)view);
}

void VKBackend::CopyRender(const copyRenderCommand_t &cmd)
{ // no op
}