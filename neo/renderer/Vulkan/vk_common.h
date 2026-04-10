/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan backend - common types and helper macros.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.


===========================================================================
*/

#ifndef __VK_COMMON_H__
#define __VK_COMMON_H__

#include <vulkan/vulkan.h>

// ---------------------------------------------------------------------------
// Error checking macro
// Aborts with a message if the VkResult is not VK_SUCCESS.
// ---------------------------------------------------------------------------

#define VK_CHECK(call)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        VkResult _vkr = (call);                                                                                        \
        if (_vkr != VK_SUCCESS)                                                                                        \
        {                                                                                                              \
            common->FatalError("Vulkan error %d in %s at %s:%d", (int)_vkr, #call, __FILE__, __LINE__);                \
        }                                                                                                              \
    } while (0)

#define VK_CHECK_NONFATAL(call, out_result)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        (out_result) = (call);                                                                                         \
    } while (0)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const int VK_MAX_FRAMES_IN_FLIGHT = 2; // double-buffering
static const int VK_MAX_SWAPCHAIN_IMAGES = 8;

// ---------------------------------------------------------------------------
// Vulkan instance/device state (global, set during VKimp_Init)
// ---------------------------------------------------------------------------

struct vkState_t
{
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;

    VkQueue graphicsQueue;
    VkQueue presentQueue;
    uint32_t graphicsFamily;
    uint32_t presentFamily;

    VkSurfaceKHR surface;

    // Swapchain
    VkSwapchainKHR swapchain;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    uint32_t swapchainImageCount;
    VkImage swapchainImages[VK_MAX_SWAPCHAIN_IMAGES];
    VkImageView swapchainImageViews[VK_MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer swapchainFramebuffers[VK_MAX_SWAPCHAIN_IMAGES];

    // Depth buffer
    VkImage depthImage;
    VkDeviceMemory depthMemory;
    VkImageView depthView;        // combined depth+stencil view (framebuffer attachment)
    VkImageView depthSampledView; // depth-only view (for shader sampling — conformant)
    VkFormat depthFormat;

    // Render pass
    VkRenderPass renderPass;       // CLEAR load op — used on first begin each frame
    VkRenderPass renderPassResume; // LOAD load op  — used when reopening after RT dispatch

    // Command pool / buffers
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffers[VK_MAX_FRAMES_IN_FLIGHT];

    // Sync objects
    VkSemaphore imageAvailableSemaphores[VK_MAX_FRAMES_IN_FLIGHT];
    // Present completion is tied to swapchain image lifetime; index these by acquired image.
    VkSemaphore renderFinishedSemaphores[VK_MAX_SWAPCHAIN_IMAGES];
    VkFence inFlightFences[VK_MAX_FRAMES_IN_FLIGHT];

    // Memory properties
    VkPhysicalDeviceMemoryProperties memProperties;

    // Capabilities
    bool rayTracingSupported; // VK_KHR_ray_tracing_pipeline
    bool asSupported;         // VK_KHR_acceleration_structure

    uint32_t currentFrame;    // 0..VK_MAX_FRAMES_IN_FLIGHT-1
    uint32_t currentImageIdx; // current swapchain image index
    bool deviceLost;          // latched after first VK_ERROR_DEVICE_LOST

    bool isInitialized;
};

extern vkState_t vk;

// Latches vk.deviceLost and logs the first call site that observed it.

const char *VK_ResultToString(VkResult r);

// ---------------------------------------------------------------------------
// Graphics pipeline objects — defined in vk_pipeline.cpp
// ---------------------------------------------------------------------------

struct vkPipelines_t
{
    VkDescriptorSetLayout interactionDescLayout;
    VkPipelineLayout interactionLayout;
    VkPipeline interactionPipeline;              // stencil EQUAL 128 (opaque/normal interactions)
    VkPipeline interactionPipelineStencilLEqual; // stencil GEQUAL 128 + depth LEQUAL (weapon depth-hack interactions)
    VkPipeline interactionPipelineStencilAlways; // stencil GEQUAL 128 + depth ALWAYS (weapon depth-hack fallback)
    VkPipeline interactionPipelineNoStencil;     // stencil disabled (translucent interactions)

    VkDescriptorSetLayout shadowDescLayout;
    VkPipelineLayout shadowLayout;
    VkPipeline shadowPipelineZFail;       // Z-fail (Carmack's Reverse) — camera inside shadow volume
    VkPipeline shadowPipelineZFailMirror; // Z-fail for mirrored views (front/back ops swapped)
    VkPipeline shadowPipelineZPass;       // Z-pass — camera outside shadow volume (no caps needed)
    VkPipeline shadowPipelineZPassMirror; // Z-pass for mirrored views (front/back ops swapped)

    VkDescriptorSetLayout depthDescLayout;
    VkPipelineLayout depthLayout;
    VkPipeline depthPipeline;     // opaque surfaces — no texture sample
    VkPipeline depthClipPipeline; // MC_PERFORATED — samples diffuse, discards on alpha

    // GUI / unlit shader-pass pipeline (menu, HUD, console)
    VkDescriptorSetLayout guiDescLayout;
    VkPipelineLayout guiLayout;
    VkPipeline guiOpaquePipeline; // blend disabled (opaque stages)
    VkPipeline guiAlphaPipeline;  // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    VkPipeline skyboxPipeline;    // samplerCube skybox path (TG_SKYBOX_CUBE)

    // Glass RT-reflection overlay pipeline.
    // Drawn additively over MC_TRANSLUCENT surfaces to add ray-traced reflections.
    // Descriptor: binding0=GuiParams UBO (screen dims in texGenS.xy), binding1=reflBuffer sampler.
    VkDescriptorSetLayout glassReflDescLayout;
    VkPipelineLayout      glassReflLayout;
    VkPipeline            glassReflPipeline;

    // Fog light pipeline (FogAllLights pass)
    // Shared descriptor layout: binding0=UBO, binding1=samp0, binding2=samp1
    VkDescriptorSetLayout fogDescLayout;
    VkPipelineLayout fogLayout;
    VkPipeline fogPipeline;        // depth EQUAL, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    VkPipeline fogFrustumPipeline; // depth LESS,  SRC_ALPHA/ONE_MINUS_SRC_ALPHA, back-cull (fog cap)
    VkPipeline blendlightPipeline; // depth EQUAL, DST_COLOR/ZERO (modulate) — most common blend light

    bool isValid;
};

extern vkPipelines_t vkPipes;

// ---------------------------------------------------------------------------
// Push descriptor function pointer
// Loaded at device creation time in VK_InitPipelines.
// Used instead of vkAllocateDescriptorSets+vkUpdateDescriptorSets per draw.
// ---------------------------------------------------------------------------

extern PFN_vkCmdPushDescriptorSetKHR pfn_vkCmdPushDescriptorSetKHR;
#define vkCmdPushDescriptorSetKHR pfn_vkCmdPushDescriptorSetKHR

// ---------------------------------------------------------------------------
// Buffer helper — defined in vk_buffer.cpp
// ---------------------------------------------------------------------------

void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps, VkBuffer *outBuffer,
                     VkDeviceMemory *outMemory);

// ---------------------------------------------------------------------------
// Memory helper: find a memory type satisfying requirements
// ---------------------------------------------------------------------------

static inline uint32_t VK_FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < vk.memProperties.memoryTypeCount; i++)
    {
        if ((typeBits & (1u << i)) && (vk.memProperties.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    common->FatalError("VK_FindMemoryType: no suitable memory type found");
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Batched upload helpers
//
// VK_BeginSingleTimeCommands / VK_EndSingleTimeCommands accumulate all upload
// commands into one command buffer per batch.  VK_EndSingleTimeCommands is a
// no-op; call VK_FlushPendingUploads() once at frame start to submit the
// batch with a single fence wait.  Register staging buffers that must stay
// alive until the flush via VK_DeferStagingFree().
// ---------------------------------------------------------------------------

VkCommandBuffer VK_BeginSingleTimeCommands(void);
void VK_EndSingleTimeCommands(VkCommandBuffer cmd); // no-op; kept for call-site compatibility
void VK_DeferStagingFree(VkBuffer buf, VkDeviceMemory mem);
void VK_FlushPendingUploads(void);
void VK_ShutdownUploadBatch(void);

// ---------------------------------------------------------------------------
// Image layout transition helper
// ---------------------------------------------------------------------------

void VK_TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

// ---------------------------------------------------------------------------
// Screenshot readback — defined in vk_backend.cpp
// ---------------------------------------------------------------------------

// Call before rendering the screenshot frame.  Allocates the staging buffer
// on first use.
void VK_RequestReadback();

// Call after the screenshot frame returns.  Copies out packed RGB pixels.
void VK_ReadPixels(int x, int y, int w, int h, unsigned char *out_rgb);

#endif // __VK_COMMON_H__
