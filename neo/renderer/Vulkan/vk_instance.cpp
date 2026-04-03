/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - instance, physical device, and logical device creation.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <string.h>

// Global Vulkan state
vkState_t vk;

// ---------------------------------------------------------------------------
// Instance layers and extensions
// ---------------------------------------------------------------------------

static const char *validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
static const int numValidationLayers = 1;

static bool VK_CheckValidationLayerSupport(void)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);

    VkLayerProperties *avail = (VkLayerProperties *)alloca(sizeof(VkLayerProperties) * count);
    vkEnumerateInstanceLayerProperties(&count, avail);

    for (int i = 0; i < numValidationLayers; i++)
    {
        bool found = false;
        for (uint32_t j = 0; j < count; j++)
        {
            if (strcmp(validationLayers[i], avail[j].layerName) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Device extensions required
// ---------------------------------------------------------------------------

static const char *deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};
static const int numDeviceExtensions = 3;

static const char *rtDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
};
static const int numRTDeviceExtensions = 10;

// ---------------------------------------------------------------------------
// Queue family detection
// ---------------------------------------------------------------------------

static bool VK_FindQueueFamilies(VkPhysicalDevice physDev, uint32_t *outGraphics, uint32_t *outPresent)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &count, NULL);
    VkQueueFamilyProperties *props = (VkQueueFamilyProperties *)alloca(sizeof(VkQueueFamilyProperties) * count);
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &count, props);

    *outGraphics = UINT32_MAX;
    *outPresent = UINT32_MAX;

    for (uint32_t i = 0; i < count; i++)
    {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            *outGraphics = i;
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physDev, i, vk.surface, &presentSupport);
        if (presentSupport)
        {
            *outPresent = i;
        }
        if (*outGraphics != UINT32_MAX && *outPresent != UINT32_MAX)
            break;
    }
    return (*outGraphics != UINT32_MAX && *outPresent != UINT32_MAX);
}

static bool VK_CheckDeviceExtensionSupport(VkPhysicalDevice physDev, const char **required, int count)
{
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physDev, NULL, &extCount, NULL);
    VkExtensionProperties *avail = (VkExtensionProperties *)alloca(sizeof(VkExtensionProperties) * extCount);
    vkEnumerateDeviceExtensionProperties(physDev, NULL, &extCount, avail);

    for (int i = 0; i < count; i++)
    {
        bool found = false;
        for (uint32_t j = 0; j < extCount; j++)
        {
            if (strcmp(required[i], avail[j].extensionName) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Physical device selection
// ---------------------------------------------------------------------------

static int VK_ScoreDevice(VkPhysicalDevice physDev, bool *outRTSupport)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDev, &props);

    uint32_t gfx, pres;
    if (!VK_FindQueueFamilies(physDev, &gfx, &pres))
        return -1;
    if (!VK_CheckDeviceExtensionSupport(physDev, deviceExtensions, numDeviceExtensions))
        return -1;

    *outRTSupport = VK_CheckDeviceExtensionSupport(physDev, rtDeviceExtensions, numRTDeviceExtensions);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 1000;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        score += 100;
    if (*outRTSupport)
        score += 500;
    score += (int)(props.limits.maxImageDimension2D / 1024);

    return score;
}

// ---------------------------------------------------------------------------
// VKimp_CreateInstance
// ---------------------------------------------------------------------------

static void VKimp_CreateInstance(SDL_Window *window)
{
    // Get extensions required by SDL
    uint32_t sdlExtCount = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &sdlExtCount, NULL);
    const char **sdlExts = (const char **)alloca(sizeof(const char *) * (sdlExtCount + 1));
    SDL_Vulkan_GetInstanceExtensions(window, &sdlExtCount, sdlExts);

    // Add debug extension if validation is enabled
    bool enableValidation = r_glDebugContext.GetBool();
    if (enableValidation && !VK_CheckValidationLayerSupport())
    {
        common->Printf("VK: validation layers not available, disabling\n");
        enableValidation = false;
    }

    uint32_t totalExtCount = sdlExtCount;
    if (enableValidation)
    {
        sdlExts[totalExtCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dhewm3";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 5, 1);
    appInfo.pEngineName = "dhewm3";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 5, 1);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = totalExtCount;
    createInfo.ppEnabledExtensionNames = sdlExts;

    if (enableValidation)
    {
        createInfo.enabledLayerCount = numValidationLayers;
        createInfo.ppEnabledLayerNames = validationLayers;
    }

    VK_CHECK(vkCreateInstance(&createInfo, NULL, &vk.instance));
}

// ---------------------------------------------------------------------------
// VKimp_SelectPhysicalDevice
// ---------------------------------------------------------------------------

static void VKimp_SelectPhysicalDevice(void)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, NULL);
    if (count == 0)
    {
        common->FatalError("VK: No Vulkan-capable GPU found");
    }

    VkPhysicalDevice *devices = (VkPhysicalDevice *)alloca(sizeof(VkPhysicalDevice) * count);
    vkEnumeratePhysicalDevices(vk.instance, &count, devices);

    int bestScore = -1;
    for (uint32_t i = 0; i < count; i++)
    {
        bool rtSupport = false;
        int score = VK_ScoreDevice(devices[i], &rtSupport);
        if (score > bestScore)
        {
            bestScore = score;
            vk.physicalDevice = devices[i];
            vk.rayTracingSupported = rtSupport;
            vk.asSupported = rtSupport;
        }
    }

    if (bestScore < 0)
    {
        common->FatalError("VK: No suitable GPU found");
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.physicalDevice, &props);
    glConfig.maxTextureAnisotropy = props.limits.maxSamplerAnisotropy;
    common->Printf("VK: Selected GPU: %s (RT=%s)\n", props.deviceName, vk.rayTracingSupported ? "yes" : "no");

    vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice, &vk.memProperties);
    VK_FindQueueFamilies(vk.physicalDevice, &vk.graphicsFamily, &vk.presentFamily);
}

// ---------------------------------------------------------------------------
// VKimp_CreateDevice
// ---------------------------------------------------------------------------

static void VKimp_CreateDevice(void)
{
    float queuePriority = 1.0f;

    // Build queue create infos (may be same family for graphics+present)
    VkDeviceQueueCreateInfo queueInfos[2] = {};
    uint32_t numQueues = 0;

    queueInfos[numQueues].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfos[numQueues].queueFamilyIndex = vk.graphicsFamily;
    queueInfos[numQueues].queueCount = 1;
    queueInfos[numQueues].pQueuePriorities = &queuePriority;
    numQueues++;

    if (vk.presentFamily != vk.graphicsFamily)
    {
        queueInfos[numQueues].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[numQueues].queueFamilyIndex = vk.presentFamily;
        queueInfos[numQueues].queueCount = 1;
        queueInfos[numQueues].pQueuePriorities = &queuePriority;
        numQueues++;
    }

    // Query supported features before enabling them.
    VkPhysicalDeviceFeatures supportedFeatures = {};
    vkGetPhysicalDeviceFeatures(vk.physicalDevice, &supportedFeatures);
    if (!supportedFeatures.depthClamp)
        common->Warning("Vulkan: depthClamp not supported by this GPU (not currently required).");
    if (!supportedFeatures.samplerAnisotropy)
        common->Error("Vulkan: samplerAnisotropy is required but is not supported by this GPU.");

    // Device features
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.samplerAnisotropy = VK_TRUE;
    features2.features.depthClamp       = supportedFeatures.depthClamp; // enable if available, not currently required

    // Extension list: base + optional RT extensions
    const char **exts;
    int numExts;
    if (vk.rayTracingSupported)
    {
        exts = rtDeviceExtensions;
        numExts = numRTDeviceExtensions;
    }
    else
    {
        exts = deviceExtensions;
        numExts = numDeviceExtensions;
    }

    // Chain RT feature structs if RT is supported
    VkPhysicalDeviceBufferDeviceAddressFeatures bufDevAddrFeatures = {};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures = {};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = {};
    VkPhysicalDeviceDescriptorIndexingFeatures diFeatures = {};

    // Extended dynamic state (required for vkCmdSetCullMode)
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsFeatures = {};
    edsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    edsFeatures.extendedDynamicState = VK_TRUE;

    void **nextChain = (void **)&features2.pNext;
    *nextChain = &edsFeatures;
    nextChain = (void **)&edsFeatures.pNext;

    if (vk.rayTracingSupported)
    {
        bufDevAddrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        bufDevAddrFeatures.bufferDeviceAddress = VK_TRUE;
        *nextChain = &bufDevAddrFeatures;
        nextChain = (void **)&bufDevAddrFeatures.pNext;

        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = VK_TRUE;
        *nextChain = &asFeatures;
        nextChain = (void **)&asFeatures.pNext;

        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
        *nextChain = &rtPipelineFeatures;
        nextChain = (void **)&rtPipelineFeatures.pNext;
    }

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = numQueues;
    deviceInfo.pQueueCreateInfos = queueInfos;
    deviceInfo.enabledExtensionCount = (uint32_t)numExts;
    deviceInfo.ppEnabledExtensionNames = exts;

    VK_CHECK(vkCreateDevice(vk.physicalDevice, &deviceInfo, NULL, &vk.device));

    vkGetDeviceQueue(vk.device, vk.graphicsFamily, 0, &vk.graphicsQueue);
    vkGetDeviceQueue(vk.device, vk.presentFamily, 0, &vk.presentQueue);
}

// ---------------------------------------------------------------------------
// VKimp_CreateCommandPool
// ---------------------------------------------------------------------------

static void VKimp_CreateCommandPool(void)
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = vk.graphicsFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VK_CHECK(vkCreateCommandPool(vk.device, &poolInfo, NULL, &vk.commandPool));

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = vk.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = VK_MAX_FRAMES_IN_FLIGHT;

    VK_CHECK(vkAllocateCommandBuffers(vk.device, &allocInfo, vk.commandBuffers));
}

// ---------------------------------------------------------------------------
// VKimp_CreateSyncObjects
// ---------------------------------------------------------------------------

static void VKimp_CreateSyncObjects(void)
{
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_CHECK(vkCreateSemaphore(vk.device, &semInfo, NULL, &vk.imageAvailableSemaphores[i]));
        VK_CHECK(vkCreateSemaphore(vk.device, &semInfo, NULL, &vk.renderFinishedSemaphores[i]));
        VK_CHECK(vkCreateFence(vk.device, &fenceInfo, NULL, &vk.inFlightFences[i]));
    }
}

// ---------------------------------------------------------------------------
// Batched upload command buffer
//
// All VK_BeginSingleTimeCommands / VK_EndSingleTimeCommands calls within a
// frame accumulate into ONE command buffer.  VK_EndSingleTimeCommands is a
// no-op; staging memory is registered via VK_DeferStagingFree and kept alive
// until VK_FlushPendingUploads() submits the batch, waits on a fence, then
// frees everything at once.  This replaces N serial vkQueueWaitIdle calls
// (one per texture/buffer upload) with a single wait at frame start.
// ---------------------------------------------------------------------------

static VkCommandBuffer s_uploadCmdBuf  = VK_NULL_HANDLE;
static VkFence         s_uploadFence   = VK_NULL_HANDLE;

struct vkStagingEntry_t { VkBuffer buf; VkDeviceMemory mem; };
static const int VK_MAX_PENDING_STAGING = 512;
static vkStagingEntry_t s_pendingStaging[VK_MAX_PENDING_STAGING];
static int              s_pendingStagingCount = 0;

VkCommandBuffer VK_BeginSingleTimeCommands(void)
{
    if (s_uploadCmdBuf != VK_NULL_HANDLE)
        return s_uploadCmdBuf; // accumulate into open batch

    if (s_uploadFence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &s_uploadFence));
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = vk.commandPool;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &allocInfo, &s_uploadCmdBuf));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(s_uploadCmdBuf, &beginInfo));
    return s_uploadCmdBuf;
}

void VK_EndSingleTimeCommands(VkCommandBuffer /*cmd*/)
{
    // No-op: the batch stays open until VK_FlushPendingUploads().
}

void VK_DeferStagingFree(VkBuffer buf, VkDeviceMemory mem)
{
    if (s_pendingStagingCount < VK_MAX_PENDING_STAGING)
    {
        s_pendingStaging[s_pendingStagingCount++] = {buf, mem};
    }
    else
    {
        // Overflow: flush now to make room (rare, only if > 512 uploads in one batch).
        VK_FlushPendingUploads();
        s_pendingStaging[s_pendingStagingCount++] = {buf, mem};
    }
}

void VK_FlushPendingUploads(void)
{
    if (s_uploadCmdBuf == VK_NULL_HANDLE)
        return;

    VK_CHECK(vkEndCommandBuffer(s_uploadCmdBuf));

    VK_CHECK(vkResetFences(vk.device, 1, &s_uploadFence));

    VkSubmitInfo si = {};
    si.sType               = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount  = 1;
    si.pCommandBuffers     = &s_uploadCmdBuf;
    VK_CHECK(vkQueueSubmit(vk.graphicsQueue, 1, &si, s_uploadFence));
    VK_CHECK(vkWaitForFences(vk.device, 1, &s_uploadFence, VK_TRUE, UINT64_MAX));

    for (int i = 0; i < s_pendingStagingCount; i++)
    {
        vkDestroyBuffer(vk.device, s_pendingStaging[i].buf, NULL);
        vkFreeMemory(vk.device, s_pendingStaging[i].mem, NULL);
    }
    s_pendingStagingCount = 0;

    vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &s_uploadCmdBuf);
    s_uploadCmdBuf = VK_NULL_HANDLE;
}

void VK_ShutdownUploadBatch(void)
{
    VK_FlushPendingUploads();
    if (s_uploadFence != VK_NULL_HANDLE)
    {
        vkDestroyFence(vk.device, s_uploadFence, NULL);
        s_uploadFence = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Image layout transition
// ---------------------------------------------------------------------------

void VK_TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }
    else
    {
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

// ---------------------------------------------------------------------------
// VKimp_Init - entry point called from glimp.cpp when r_backend == "vulkan"
// ---------------------------------------------------------------------------

void VKimp_Init(SDL_Window *window)
{
    memset(&vk, 0, sizeof(vk));

    // Create Vulkan surface via SDL
    VKimp_CreateInstance(window);

    if (!SDL_Vulkan_CreateSurface(window, vk.instance, &vk.surface))
    {
        common->FatalError("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
    }

    VKimp_SelectPhysicalDevice();
    VKimp_CreateDevice();
    VKimp_CreateCommandPool();
    VKimp_CreateSyncObjects();

    vk.currentFrame = 0;
    vk.isInitialized = true;
    common->Printf("VK: Instance initialized\n");
}

// ---------------------------------------------------------------------------
// VKimp_Shutdown
// ---------------------------------------------------------------------------

void VKimp_Shutdown(void)
{
    if (!vk.isInitialized)
        return;

    vkDeviceWaitIdle(vk.device);

    // Drain any deferred image/buffer deletions and free the readback buffer.
    extern void VK_Image_DrainAllGarbage(void);
    VK_Image_DrainAllGarbage();
    extern void VK_Buffer_DrainAllGarbage(void);
    VK_Buffer_DrainAllGarbage();
    extern void VK_CleanupReadback(void);
    VK_CleanupReadback();

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkDestroySemaphore(vk.device, vk.imageAvailableSemaphores[i], NULL);
        vkDestroySemaphore(vk.device, vk.renderFinishedSemaphores[i], NULL);
        vkDestroyFence(vk.device, vk.inFlightFences[i], NULL);
    }

    vkDestroyCommandPool(vk.device, vk.commandPool, NULL);
    vkDestroyDevice(vk.device, NULL);
    vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vkDestroyInstance(vk.instance, NULL);

    memset(&vk, 0, sizeof(vk));
}
