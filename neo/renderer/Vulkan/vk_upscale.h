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
extern idCVar r_fsrJitter;
extern idCVar r_fsrMotionScale;

// U2: sub-pixel jitter for this frame, in render pixels over [-0.5,+0.5], and the
// render extent to measure it against.  False = not jittering; outputs untouched.
bool VK_RT_GetFsrJitter(float *jx, float *jy, int *renderW, int *renderH);

// U2: r_fsrDebug 7 motion-vector overlay.  Must be called outside a render pass,
// after the resolve and before the tonemap; overwrites hdrScene entirely.
bool VK_RT_MotionDebugActive(void);
void VK_RT_DispatchMotionDebug(VkCommandBuffer cmd);

void VK_RT_InitUpscale();
void VK_RT_ResizeUpscale(uint32_t w, uint32_t h);
void VK_RT_ShutdownUpscale(void);
void VK_RT_UpdateRenderExtent(void);
float VK_RT_RenderScaleX(void);
float VK_RT_RenderScaleY(void);
idScreenRect VK_RT_ScaleDisplayRect(const idScreenRect &s);
bool VK_RT_UpscaleActive(void);

void VK_RT_DispatchUpscale(VkCommandBuffer cmd);