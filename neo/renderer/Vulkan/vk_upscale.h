#pragma once

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include <string.h>

extern idCVar r_fsr;
extern idCVar r_fsrRenderScale;
extern idCVar r_fsrDebug;
extern idCVar r_fsrSharpness;

void VK_RT_InitUpscale();
void VK_RT_ResizeUpscale(uint32_t w, uint32_t h);
void VK_RT_ShutdownUpscale(void);
void VK_RT_UpdateRenderExtent(void);
float VK_RT_RenderScaleX(void);
float VK_RT_RenderScaleY(void);
idScreenRect VK_RT_ScaleDisplayRect(const idScreenRect &s);
bool VK_RT_UpscaleActive(void);

void VK_RT_DispatchUpscale(VkCommandBuffer cmd);