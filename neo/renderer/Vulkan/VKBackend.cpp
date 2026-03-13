#include "renderer/VertexCache.h"
//#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/VKBackend.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_buffer.h"


void VKBackend::Init()
{
	common->Printf("Starting Vulkan Backend");
	VKimp_Init();
	VKimp_PostInit();
}

void VKBackend::Shutdown()
{
	common->Printf("Shutting down Vulkan Backend");
	VKimp_PreShutdown();
}
void VKBackend::PostSwapBuffers()
{
	PostSwapBuffers();
}

void VKBackend::Image_Upload(idImage *img, const byte *data, int w, int h, textureFilter_t, bool, textureRepeat_t, textureDepth_t)
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
	VK_VertexCache_Alloc(vc, data, size, indexBuffer)
	//	VK_VertexCache_Alloc(data, size, vc, indexBuffer);
}
void VKBackend::VertexCache_Free(vertCache_t *vc)
{
	VK_VertexCache_Free(vc);
}

// Frame dispatch

void VKBackend::DrawView(const viewDef_t *view)
{
	VK_RB_DrawView(view); // just delegate to the existing function
}

void VKBackend::CopyRender(const copyRenderCommand_t &cmd)
{
}