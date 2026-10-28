/*
===========================================================================

Doom 3 GPL Source Code
dhewm3-rt Vulkan backend - instance, physical device, and logical device creation.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_profile.h"

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
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME, // the froxel fill uses rayQueryEXT
};
static const int numRTDeviceExtensions = 10;

// Optional, profiling only.  A missing one disables its metric, never the device.
static const char *optionalDeviceExtensions[] = {
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
};
static const int numOptionalDeviceExtensions = 1;

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

static bool VK_CheckInstanceExtensionSupport(const char *name)
{
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &extCount, NULL);
    VkExtensionProperties *avail = (VkExtensionProperties *)alloca(sizeof(VkExtensionProperties) * extCount);
    vkEnumerateInstanceExtensionProperties(NULL, &extCount, avail);
    for (uint32_t i = 0; i < extCount; i++)
        if (strcmp(name, avail[i].extensionName) == 0)
            return true;
    return false;
}

static void VKimp_CreateInstance(SDL_Window *window)
{
    // Get extensions required by SDL
    uint32_t sdlExtCount = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &sdlExtCount, NULL);
    const char **sdlExts = (const char **)alloca(sizeof(const char *) * (sdlExtCount + 2));
    SDL_Vulkan_GetInstanceExtensions(window, &sdlExtCount, sdlExts);

    // Add debug extension if validation is enabled
    bool enableValidation = r_glDebugContext.GetBool();
    if (enableValidation && !VK_CheckValidationLayerSupport())
    {
        common->Printf("VK: validation layers not available, disabling\n");
        enableValidation = false;
    }

    // debug_utils also names passes/objects for GPU tools, so enable it whenever present.
    // The NULL-layer query misses layer-provided extensions, so validation appends it
    // regardless; the flag is set only once vkCreateInstance accepts the list.
    uint32_t totalExtCount = sdlExtCount;
    const bool wantDebugUtils = enableValidation || VK_CheckInstanceExtensionSupport(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (wantDebugUtils)
        sdlExts[totalExtCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    vk.debugUtilsSupported = false;

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dhewm3";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 5, 1);
    appInfo.pEngineName = "dhewm3";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 5, 1);
    appInfo.apiVersion = VK_API_VERSION_1_4;

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

    // Past the VK_CHECK the extension list was accepted, so this is the only point
    // where claiming debug_utils support is actually true.
    vk.debugUtilsSupported = wantDebugUtils;
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
    VkPhysicalDeviceFeatures2 supportedFeatures2 = {};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceVulkan12Features supportedVk12 = {};
    supportedVk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT supportedEds = {};
    supportedEds.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAs = {};
    supportedAs.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supportedRtPipeline = {};
    supportedRtPipeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

    VkPhysicalDeviceRayQueryFeaturesKHR supportedRayQuery = {};
    supportedRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

    // subgroupSizeControl (core in 1.3): FSR2 forces wave64 whenever the extension is
    // available, so the feature must be enabled or its pipelines are invalid on RDNA.
    VkPhysicalDeviceProperties physProps = {};
    vkGetPhysicalDeviceProperties(vk.physicalDevice, &physProps);
    const bool deviceHasVk13 = physProps.apiVersion >= VK_API_VERSION_1_3;

    VkPhysicalDeviceVulkan13Features supportedVk13 = {};
    supportedVk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    supportedFeatures2.pNext = &supportedVk12;
    supportedVk12.pNext = &supportedEds;
    void **supportedTail = (void **)&supportedEds.pNext;
    if (deviceHasVk13)
    {
        *supportedTail = &supportedVk13;
        supportedTail = (void **)&supportedVk13.pNext;
    }
    if (vk.rayTracingSupported)
    {
        *supportedTail = &supportedAs;
        supportedAs.pNext = &supportedRtPipeline;
        supportedRtPipeline.pNext = &supportedRayQuery;
    }

    vkGetPhysicalDeviceFeatures2(vk.physicalDevice, &supportedFeatures2);

    if (!supportedFeatures2.features.depthClamp)
        common->Warning("Vulkan: depthClamp not supported by this GPU (not currently required).");
    if (!supportedFeatures2.features.samplerAnisotropy)
        common->Error("Vulkan: samplerAnisotropy is required but is not supported by this GPU.");
    if (vk.rayTracingSupported && !supportedFeatures2.features.shaderInt64)
        common->Warning("Vulkan RT: shaderInt64 not supported; RT shader modules may fail.");
    if (!supportedFeatures2.features.independentBlend)
        common->Warning(
            "Vulkan: independentBlend not supported; the G-buffer normal/F0 pass will be disabled.");
    if (vk.rayTracingSupported && !supportedVk12.shaderSampledImageArrayNonUniformIndexing)
        common->Warning(
            "Vulkan RT: shaderSampledImageArrayNonUniformIndexing not supported; bindless indexing may fail.");
    if (vk.rayTracingSupported && !supportedVk12.descriptorIndexing)
        common->Warning("Vulkan RT: descriptorIndexing not supported; bindless descriptors may fail.");
    if (vk.rayTracingSupported && !supportedRayQuery.rayQuery)
        common->Warning("Vulkan RT: rayQuery not supported; volumetrics will fail.");

    // Device features
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.samplerAnisotropy = VK_TRUE;
    features2.features.depthClamp =
        supportedFeatures2.features.depthClamp; // enable if available, not currently required
    features2.features.shaderInt64 = vk.rayTracingSupported && supportedFeatures2.features.shaderInt64;
    features2.features.independentBlend = supportedFeatures2.features.independentBlend;
    // Gates the G-buffer prepass (color attachments with differing blend/write masks).
    vk.gbufferSupported = supportedFeatures2.features.independentBlend == VK_TRUE;

    VkPhysicalDeviceVulkan12Features vk12Features = {};
    vk12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12Features.bufferDeviceAddress = vk.rayTracingSupported && supportedVk12.bufferDeviceAddress;
    vk12Features.descriptorIndexing = vk.rayTracingSupported && supportedVk12.descriptorIndexing;
    vk12Features.shaderSampledImageArrayNonUniformIndexing =
        vk.rayTracingSupported && supportedVk12.shaderSampledImageArrayNonUniformIndexing;
    // Required to back UPDATE_AFTER_BIND and PARTIALLY_BOUND flags on the material descriptor set layout.
    vk12Features.descriptorBindingStorageBufferUpdateAfterBind =
        vk.rayTracingSupported && supportedVk12.descriptorBindingStorageBufferUpdateAfterBind;
    vk12Features.descriptorBindingSampledImageUpdateAfterBind =
        vk.rayTracingSupported && supportedVk12.descriptorBindingSampledImageUpdateAfterBind;
    vk12Features.descriptorBindingPartiallyBound =
        vk.rayTracingSupported && supportedVk12.descriptorBindingPartiallyBound;
    // FSR 2.2.1 uses fp16 and depth-only-aspect barriers (illegal on our D32S8 buffer
    // without separateDepthStencilLayouts) without requesting them. Enable when present;
    // r_fsr 2 is gated on them below.
    vk12Features.shaderFloat16 = supportedVk12.shaderFloat16;
    vk12Features.separateDepthStencilLayouts = supportedVk12.separateDepthStencilLayouts;

    VkPhysicalDeviceVulkan13Features vk13Features = {};
    vk13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13Features.subgroupSizeControl = deviceHasVk13 && supportedVk13.subgroupSizeControl;

    // deviceHasVk13 is part of the gate rather than a separate fallback: on a 1.2-only
    // device we cannot enable subgroupSizeControl through this struct, and FSR2 would
    // still chain the required-subgroup-size struct if the EXT extension is advertised.
    vk.fsr2Supported = (supportedVk12.separateDepthStencilLayouts == VK_TRUE) && deviceHasVk13;
    if (!vk.fsr2Supported)
        common->Warning("Vulkan: r_fsr 2 (FSR 2) unavailable — %s",
                        !deviceHasVk13 ? "device reports less than Vulkan 1.3"
                                       : "separateDepthStencilLayouts not supported");

    // Extension list: base + optional RT extensions, then any supported profiling ones
    const char **baseExts;
    int numBaseExts;
    if (vk.rayTracingSupported)
    {
        baseExts = rtDeviceExtensions;
        numBaseExts = numRTDeviceExtensions;
    }
    else
    {
        baseExts = deviceExtensions;
        numBaseExts = numDeviceExtensions;
    }

    const char **exts =
        (const char **)alloca(sizeof(const char *) * (numBaseExts + numOptionalDeviceExtensions));
    int numExts = 0;
    for (int i = 0; i < numBaseExts; i++)
        exts[numExts++] = baseExts[i];

    vk.memoryBudgetSupported = false;
    for (int i = 0; i < numOptionalDeviceExtensions; i++)
    {
        if (!VK_CheckDeviceExtensionSupport(vk.physicalDevice, &optionalDeviceExtensions[i], 1))
            continue;
        exts[numExts++] = optionalDeviceExtensions[i];
        if (idStr::Cmp(optionalDeviceExtensions[i], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0)
            vk.memoryBudgetSupported = true;
    }

    // Chain RT feature structs if RT is supported
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures = {};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = {};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {};

    // Extended dynamic state (required for vkCmdSetCullMode)
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT edsFeatures = {};
    edsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    edsFeatures.extendedDynamicState = supportedEds.extendedDynamicState;

    void **nextChain = (void **)&features2.pNext;
    *nextChain = &vk12Features;
    nextChain = (void **)&vk12Features.pNext;

    *nextChain = &edsFeatures;
    nextChain = (void **)&edsFeatures.pNext;

    if (deviceHasVk13)
    {
        *nextChain = &vk13Features;
        nextChain = (void **)&vk13Features.pNext;
    }

    if (vk.rayTracingSupported)
    {
        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = supportedAs.accelerationStructure;
        *nextChain = &asFeatures;
        nextChain = (void **)&asFeatures.pNext;

        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = supportedRtPipeline.rayTracingPipeline;
        *nextChain = &rtPipelineFeatures;
        nextChain = (void **)&rtPipelineFeatures.pNext;

        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rayQueryFeatures.rayQuery = supportedRayQuery.rayQuery;
        *nextChain = &rayQueryFeatures;
        nextChain = (void **)&rayQueryFeatures.pNext;
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
        VK_CHECK(vkCreateFence(vk.device, &fenceInfo, NULL, &vk.inFlightFences[i]));
    }

    for (int i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; i++)
    {
        VK_CHECK(vkCreateSemaphore(vk.device, &semInfo, NULL, &vk.renderFinishedSemaphores[i]));
    }
}

// ---------------------------------------------------------------------------
// Batched upload command buffers
//
// All VK_BeginSingleTimeCommands / VK_EndSingleTimeCommands calls between submits
// accumulate into ONE open batch.  Batches rotate through VK_UPLOAD_BATCHES slots; each
// owns a staging-ring segment and its oversize staging buffers, released when its fence
// signals.
//
// VK_SubmitPendingUploads (frame start) submits without a CPU wait.  The frame command
// buffer opens with a global barrier, which covers the batch through queue submission
// order.  VK_FlushPendingUploads submits and waits for every batch, for callers that
// need the result on the CPU or before the next frame starts.
// ---------------------------------------------------------------------------

struct vkStagingEntry_t
{
    VkBuffer buf;
    VkDeviceMemory mem;
};
static const int VK_MAX_PENDING_STAGING = 512;

struct vkUploadBatch_t
{
    VkCommandBuffer cmd;
    VkFence fence;
    bool inFlight;
    vkStagingEntry_t staging[VK_MAX_PENDING_STAGING];
    int stagingCount;
};

static vkUploadBatch_t s_uploadBatches[VK_UPLOAD_BATCHES];
static int s_openBatch = -1; // batch being recorded, -1 = none
static int s_nextBatch = 0;
static bool s_uploadBatchPoisoned = false;
static uint64_t s_uploadBatchSerial = 0;

static void VK_ReleaseUploadBatch(vkUploadBatch_t &b)
{
    for (int i = 0; i < b.stagingCount; i++)
    {
        if (b.staging[i].buf != VK_NULL_HANDLE)
            vkDestroyBuffer(vk.device, b.staging[i].buf, NULL);
        if (b.staging[i].mem != VK_NULL_HANDLE)
            vkFreeMemory(vk.device, b.staging[i].mem, NULL);
    }
    b.stagingCount = 0;
    if (b.cmd != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &b.cmd);
        b.cmd = VK_NULL_HANDLE;
    }
    b.inFlight = false;
}

// Marks the batch state unusable and says why — every later submit only logs "skipped".
static void VK_PoisonUploadBatch(const char *stage, VkResult result, uint64_t batchId, uint32_t pendingCount)
{
    s_uploadBatchPoisoned = true;
    common->Warning("VK: upload batch #%llu poisoned at %s: %d (%s), %u pending staging buffers",
                    (unsigned long long)batchId, stage, (int)result, VK_ResultToString(result), pendingCount);
    fflush(NULL);
}

// Waits for one submitted batch (normally already signaled) and frees what it held.
static void VK_RetireUploadBatch(int idx)
{
    vkUploadBatch_t &b = s_uploadBatches[idx];
    if (!b.inFlight)
        return;
    VkResult waitResult = VK_Prof_WaitForFences(VKPROF_WAIT_UPLOAD_FLUSH, 1, &b.fence, VK_TRUE, UINT64_MAX);
    if (waitResult != VK_SUCCESS)
        VK_PoisonUploadBatch("vkWaitForFences", waitResult, s_uploadBatchSerial, (uint32_t)b.stagingCount);
    VK_ReleaseUploadBatch(b);
}

static void VK_RetireAllUploadBatches(void)
{
    for (int i = 0; i < VK_UPLOAD_BATCHES; i++)
        VK_RetireUploadBatch(i);
}

VkCommandBuffer VK_BeginSingleTimeCommands(void)
{
    if (s_openBatch >= 0)
        return s_uploadBatches[s_openBatch].cmd; // accumulate into open batch

    const int idx = s_nextBatch;
    VK_RetireUploadBatch(idx); // its staging segment is about to be overwritten
    vkUploadBatch_t &b = s_uploadBatches[idx];

    if (b.fence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &b.fence));
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = vk.commandPool;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &allocInfo, &b.cmd));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(b.cmd, &beginInfo));

    // The previous batch may still be copying; order its writes before this batch's.
    VkMemoryBarrier waw = {};
    waw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    waw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    waw.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(b.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &waw, 0, NULL, 0,
                         NULL);

    VK_StagingRing_BeginSegment(idx);
    s_openBatch = idx;
    s_nextBatch = (idx + 1) % VK_UPLOAD_BATCHES;
    return b.cmd;
}

void VK_EndSingleTimeCommands(VkCommandBuffer /*cmd*/)
{
    // No-op: the batch stays open until the next submit or flush.
}

void VK_DeferStagingFree(VkBuffer buf, VkDeviceMemory mem)
{
    VK_BeginSingleTimeCommands(); // the buffer belongs to the open batch
    if (s_uploadBatches[s_openBatch].stagingCount >= VK_MAX_PENDING_STAGING)
    {
        // Overflow: flush now to make room (rare, only if > 512 uploads in one batch).
        VK_FlushPendingUploads();
        VK_BeginSingleTimeCommands();
    }
    vkUploadBatch_t &b = s_uploadBatches[s_openBatch];
    b.staging[b.stagingCount++] = {buf, mem};
}

// Submits the open batch with its fence and no CPU wait.
static void VK_SubmitOpenBatch(void)
{
    if (s_openBatch < 0)
        return;
    vkUploadBatch_t &b = s_uploadBatches[s_openBatch];
    s_openBatch = -1;

    const uint64_t batchId = ++s_uploadBatchSerial;
    const uint32_t pendingCount = (uint32_t)b.stagingCount;

    if (s_uploadBatchPoisoned)
    {
        // A previous submit failed (typically device lost). Avoid replaying
        // a one-time command buffer during shutdown/fatal cleanup.
        common->Warning("VK: upload batch #%llu skipped because batch state is poisoned", (unsigned long long)batchId);
        fflush(NULL);
        VK_ReleaseUploadBatch(b);
        return;
    }

    VkResult result = vkEndCommandBuffer(b.cmd);
    if (result == VK_SUCCESS)
        result = vkResetFences(vk.device, 1, &b.fence);
    if (result != VK_SUCCESS)
    {
        VK_PoisonUploadBatch("vkEndCommandBuffer/vkResetFences", result, batchId, pendingCount);
        VK_ReleaseUploadBatch(b);
        return;
    }

    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &b.cmd;
    result = vkQueueSubmit(vk.graphicsQueue, 1, &si, b.fence);
    if (result != VK_SUCCESS)
    {
        VK_PoisonUploadBatch("vkQueueSubmit", result, batchId, pendingCount);
        VK_ReleaseUploadBatch(b);
        return;
    }

    VK_Prof_Add(VKPROF_UPLOAD_FLUSHES, 1.0);
    b.inFlight = true;
}

void VK_SubmitPendingUploads(void)
{
    // Batches from earlier frames ran ahead of the frame whose fence was just waited on,
    // so this retire is expected to cost ~0.  It must precede the garbage drain: a block
    // freed since then may still be the target of one of their copies.
    VK_RetireAllUploadBatches();
    VK_SubmitOpenBatch();
}

void VK_FlushPendingUploads(void)
{
    VK_SubmitOpenBatch();
    VK_RetireAllUploadBatches();
}

void VK_ShutdownUploadBatch(void)
{
    VK_FlushPendingUploads();

    for (int i = 0; i < VK_UPLOAD_BATCHES; i++)
    {
        VK_ReleaseUploadBatch(s_uploadBatches[i]);
        if (s_uploadBatches[i].fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(vk.device, s_uploadBatches[i].fence, NULL);
            s_uploadBatches[i].fence = VK_NULL_HANDLE;
        }
    }
    s_openBatch = -1;
    s_nextBatch = 0;

    VK_StagingRing_Shutdown();
    s_uploadBatchPoisoned = false;
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
        // Not FRAGMENT alone: bindless textures are sampled from the RT stages, and the
        // preAlphaColor snapshot is read by FSR 2's compute passes.  A fragment-only
        // destination leaves both of those reading memory the barrier never made visible.
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (vk.rayTracingSupported)
            dstStage |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
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

    VK_Prof_InitDebugUtils();

    vk.currentFrame = 0;
    vk.isInitialized = true;
    common->Printf("VK: Instance initialized\n");
    VK_Prof_LogDeviceLimits();
}

// ---------------------------------------------------------------------------
// VKimp_Shutdown
// ---------------------------------------------------------------------------

void VKimp_Shutdown(void)
{
    if (!vk.isInitialized)
        return;

    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_SHUTDOWN);
    VK_Prof_Shutdown();

    // Drain any deferred image/buffer deletions and free the readback buffer.
    extern void VK_Image_DrainAllGarbage(void);
    VK_Image_DrainAllGarbage();
    extern void VK_Buffer_DrainAllGarbage(void);
    VK_Buffer_DrainAllGarbage();
    extern void VK_VertexCache_Shutdown(void);
    VK_VertexCache_Shutdown();
    extern void VK_CleanupReadback(void);
    VK_CleanupReadback();

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkDestroySemaphore(vk.device, vk.imageAvailableSemaphores[i], NULL);
        vkDestroyFence(vk.device, vk.inFlightFences[i], NULL);
    }

    for (int i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; i++)
    {
        vkDestroySemaphore(vk.device, vk.renderFinishedSemaphores[i], NULL);
    }

    vkDestroyCommandPool(vk.device, vk.commandPool, NULL);
    vkDestroyDevice(vk.device, NULL);
    vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vkDestroyInstance(vk.instance, NULL);

    memset(&vk, 0, sizeof(vk));
}
