/*
Vulkan Backend Implementation

This is the full implementation of the Vulkan Backend renderer class that implements most of the
methods used in the rendering loop in vk_backend.cpp.
This base definition is defined in ../RenderBackend.h,
and this implementation parallels the one in ../GL/GLBackend.cpp,

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

#include "renderer/Vulkan/VKBackend.h"
#include "renderer/VertexCache.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_buffer.h"
#include "renderer/Vulkan/vk_backend.h"

// Forward declarations - defined in vk_backend.cpp
extern void VKimp_InitFromGlimp(int width, int height);
extern void VKimp_ShutdownFromGlimp(void);

// Populate glConfig string fields from the Vulkan physical device.
// Called once from VKBackend::Init() after VKimp_InitFromGlimp succeeds.
static void VK_SetGlConfigStrings(void)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.physicalDevice, &props);

    // Re-use the glConfig string fields so existing GfxInfo_f output works.
    static char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    static char apiVer[32];
    idStr::snPrintf(deviceName, sizeof(deviceName), "%s", props.deviceName);
    idStr::snPrintf(apiVer, sizeof(apiVer), "Vulkan %u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    glConfig.vendor_string = deviceName;
    glConfig.renderer_string = deviceName;
    glConfig.version_string = apiVer;

    common->Printf("Vulkan device:  %s\n", props.deviceName);
    common->Printf("Vulkan API:     %u.%u.%u\n", VK_API_VERSION_MAJOR(props.apiVersion),
                   VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    common->Printf("Vulkan driver:  %u\n", props.driverVersion);
    common->Printf("Vendor ID:      0x%04X\n", props.vendorID);
    common->Printf("RT supported:   %s\n", vk.rayTracingSupported ? "yes" : "no");
}

void VKBackend::Init()
{
    // Reuses the SDL window already created by GLimp_Init.
    common->Printf("VK: calling VKimp_InitFromGlimp (%dx%d)\n", glConfig.vidWidth, glConfig.vidHeight);
    VKimp_InitFromGlimp(glConfig.vidWidth, glConfig.vidHeight);
    common->Printf("VK: VKimp_InitFromGlimp returned\n");

    glConfig.isInitialized = true;
    glConfig.isVulkan = true;
    glConfig.maxTextureSize = 8192;
    glConfig.textureCompressionAvailable = false;
    VK_SetGlConfigStrings();

    common->Printf("VK: render system fully initialized\n");
    fflush(NULL);
    Sys_Sleep(100);
}

void VKBackend::Shutdown()
{
    VKimp_ShutdownFromGlimp();
    // GLimp_Shutdown() is called by the outer shutdown path after this returns.
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

void VKBackend::CubeImage_Upload(idImage *img, const byte *const pic[6], int size)
{
    VK_Image_UploadCubemap(img, pic, size);
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
    if (*vc && data && (*vc)->tag != TAG_TEMP)
    {
        VK_VertexCache_Alloc(*vc, data, size, indexBuffer);
    }
}
void VKBackend::VertexCache_Free(vertCache_t *vc)
{
    VK_VertexCache_Free(vc);
}

// Command batch lifecycle

void VKBackend::BeginCommandBatch()
{ // no-op: Vulkan has no fixed-function GL state to reset
}

void VKBackend::EndCommandBatch()
{ // no-op: no GL texture bindings to restore
}

void VKBackend::SetBuffer(const void *data)
{ // no-op: Vulkan has no draw-buffer concept
    (void)data;
}

void VKBackend::SwapBuffers(const void *data)
{
    (void)data;
    VK_RB_SwapBuffers();
}

// Frame dispatch

void VKBackend::DrawView(const drawSurfsCommand_t *cmd)
{
    VK_RB_DrawView(cmd);
}

void VKBackend::CopyRender(const copyRenderCommand_t &cmd)
{
    VK_RB_CopyRender((const void *)&cmd);
}