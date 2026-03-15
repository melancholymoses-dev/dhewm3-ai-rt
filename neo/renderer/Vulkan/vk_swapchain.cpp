/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - swapchain, render pass, and framebuffers.

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

// ---------------------------------------------------------------------------
// Surface format / present mode selection
// ---------------------------------------------------------------------------

static VkSurfaceFormatKHR VK_ChooseSurfaceFormat(VkPhysicalDevice physDev)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, vk.surface, &count, NULL);
    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)alloca(sizeof(VkSurfaceFormatKHR) * count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, vk.surface, &count, formats);

    for (uint32_t i = 0; i < count; i++)
    {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return formats[i];
        }
    }
    return formats[0]; // fallback
}

static VkPresentModeKHR VK_ChoosePresentMode(VkPhysicalDevice physDev)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDev, vk.surface, &count, NULL);
    VkPresentModeKHR *modes = (VkPresentModeKHR *)alloca(sizeof(VkPresentModeKHR) * count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDev, vk.surface, &count, modes);

    // Prefer mailbox (triple-buffer), fall back to FIFO (vsync)
    for (uint32_t i = 0; i < count; i++)
    {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            return modes[i];
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// ---------------------------------------------------------------------------
// Depth format selection
// ---------------------------------------------------------------------------

static VkFormat VK_FindDepthFormat(void)
{
    VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT};
    for (int i = 0; i < 3; i++)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(vk.physicalDevice, candidates[i], &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            return candidates[i];
        }
    }
    common->FatalError("VK: No suitable depth format found");
    return VK_FORMAT_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Create image and image view helper (used for depth buffer)
// ---------------------------------------------------------------------------

static void VK_CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                           VkMemoryPropertyFlags memProps, VkImage *outImage, VkDeviceMemory *outMemory)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VK_CHECK(vkCreateImage(vk.device, &imageInfo, NULL, outImage));

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vk.device, *outImage, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, memProps);

    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, outMemory));
    VK_CHECK(vkBindImageMemory(vk.device, *outImage, *outMemory, 0));
}

static VkImageView VK_CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectMask)
{
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view;
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &view));
    return view;
}

// ---------------------------------------------------------------------------
// VK_CreateRenderPass
// ---------------------------------------------------------------------------

void VK_CreateRenderPass(void)
{
    // Color attachment (swapchain image)
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = vk.swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth+stencil attachment
    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = vk.depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[2] = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(vk.device, &rpInfo, NULL, &vk.renderPass));
}

// ---------------------------------------------------------------------------
// VK_CreateSwapchain
// ---------------------------------------------------------------------------

void VK_CreateSwapchain(int width, int height)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physicalDevice, vk.surface, &caps);

    VkSurfaceFormatKHR surfaceFormat = VK_ChooseSurfaceFormat(vk.physicalDevice);
    VkPresentModeKHR presentMode = VK_ChoosePresentMode(vk.physicalDevice);

    vk.swapchainFormat = surfaceFormat.format;
    vk.swapchainExtent = {(uint32_t)width, (uint32_t)height};
    vk.depthFormat = VK_FindDepthFormat();

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
    {
        imageCount = caps.maxImageCount;
    }
    if (imageCount > VK_MAX_SWAPCHAIN_IMAGES)
        imageCount = VK_MAX_SWAPCHAIN_IMAGES;

    VkSwapchainCreateInfoKHR swapInfo = {};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = vk.surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = surfaceFormat.format;
    swapInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapInfo.imageExtent = vk.swapchainExtent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = presentMode;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = VK_NULL_HANDLE;

    uint32_t queueFamilies[2] = {vk.graphicsFamily, vk.presentFamily};
    if (vk.graphicsFamily != vk.presentFamily)
    {
        swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapInfo.queueFamilyIndexCount = 2;
        swapInfo.pQueueFamilyIndices = queueFamilies;
    }
    else
    {
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VK_CHECK(vkCreateSwapchainKHR(vk.device, &swapInfo, NULL, &vk.swapchain));

    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swapchainImageCount, NULL);
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swapchainImageCount, vk.swapchainImages);

    for (uint32_t i = 0; i < vk.swapchainImageCount; i++)
    {
        vk.swapchainImageViews[i] =
            VK_CreateImageView(vk.swapchainImages[i], vk.swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Create depth buffer
    VK_CreateImage(vk.swapchainExtent.width, vk.swapchainExtent.height, vk.depthFormat,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vk.depthImage,
                   &vk.depthMemory);
    vk.depthView = VK_CreateImageView(vk.depthImage, vk.depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Create render pass
    VK_CreateRenderPass();

    // Create framebuffers
    for (uint32_t i = 0; i < vk.swapchainImageCount; i++)
    {
        VkImageView attachments[2] = {vk.swapchainImageViews[i], vk.depthView};

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = vk.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = vk.swapchainExtent.width;
        fbInfo.height = vk.swapchainExtent.height;
        fbInfo.layers = 1;

        VK_CHECK(vkCreateFramebuffer(vk.device, &fbInfo, NULL, &vk.swapchainFramebuffers[i]));
    }

    common->Printf("VK: Swapchain created (%dx%d, %u images)\n", vk.swapchainExtent.width, vk.swapchainExtent.height,
                   vk.swapchainImageCount);
}

// ---------------------------------------------------------------------------
// VK_DestroySwapchain
// ---------------------------------------------------------------------------

void VK_DestroySwapchain(void)
{
    for (uint32_t i = 0; i < vk.swapchainImageCount; i++)
    {
        vkDestroyFramebuffer(vk.device, vk.swapchainFramebuffers[i], NULL);
        vkDestroyImageView(vk.device, vk.swapchainImageViews[i], NULL);
    }
    vkDestroyImageView(vk.device, vk.depthView, NULL);
    vkDestroyImage(vk.device, vk.depthImage, NULL);
    vkFreeMemory(vk.device, vk.depthMemory, NULL);
    vkDestroyRenderPass(vk.device, vk.renderPass, NULL);
    vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
}
