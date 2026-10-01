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
extern idCVar r_fsrAutoReactive;
extern idCVar r_fsrReactiveCutoff;
extern idCVar r_fsrReactiveScale;
extern idCVar r_fsrTcMask;
extern idCVar r_fsrQuality;
extern idCVar r_fsrMipBias;
extern idCVar r_fsrMipBiasOffset;

// U3: latch the 3D view's projection parameters for the FSR 2 dispatch, which runs
// after the frame's last RC_DRAW_VIEW and cannot trust backEnd.viewDef by then (the
// 2D overlay view has a zeroed viewaxis and a meaningless fov).  Call once per frame
// from the primary, non-subview, real-camera path.
void VK_RT_CaptureFsrViewParams(const viewDef_t *viewDef);

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

// U4: does the FSR 2 path want a reactive mask this frame?  True means the backend owes it
// a pre-alpha snapshot of hdrScene, taken between the interactions and the blend stages.
bool VK_RT_UpscaleNeedsReactiveMask(void);

// U4: and the transparency-and-composition mask, which the G-buffer prepass encodes into
// gbufAlbedo's alpha channel.
bool VK_RT_UpscaleNeedsTcMask(void);

// U4: build both mask inputs.  Must be called OUTSIDE a render pass, after the interactions
// and before VK_RB_DrawShaderPasses — a vkCmdCopyImage cannot be recorded inside a render
// pass, and the snapshot is worthless once anything translucent has drawn.  No-op unless
// one of the two demands above is true.
void VK_RT_CaptureReactiveInputs(VkCommandBuffer cmd);

// U4: r_fsrDebug 3 mask overlay.  Same slot as the motion overlay — outside a render pass,
// after the resolve and before the tonemap; overwrites hdrScene.
bool VK_RT_MaskDebugActive(void);
void VK_RT_DispatchMaskDebug(VkCommandBuffer cmd);

void VK_RT_DispatchUpscale(VkCommandBuffer cmd);