/*
===========================================================================

dhewm3-rt Vulkan — vk_bloom.h — screen-space emissive bloom.

Bloom's source is not a luminance threshold on the finished frame.  r_lightScale 2
puts lit walls near HDR luminance 2 while the blend-add stages that draw fixtures
and flames sit near 1, so a threshold blooms the walls first.  Instead the frame is
snapshotted after the interactions and diffed after the blend stages, which isolates
what those stages emitted.

  interactions -> [VK_RT_BloomPrepass] -> shader passes -> [VK_RT_BloomExtract] -> vol -> fog
  ... -> upscale -> [blur + composite, B2] -> GUI/HUD -> tonemap

Both dispatch points must be called OUTSIDE the HDR render pass.  Implemented:
B0 (the tonemap's luminance bands, see vk_tonemap.cpp) and B1.  B2's blur chain and
composite are not built yet, so nothing reaches the screen outside r_rtBloomDebug.

docs/plans/20260906_bloom_plan.md

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#pragma once

// tr_local.h for idCVar: the externs below are the reason this header cannot be
// standalone, the same way vk_upscale.h is not.
#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

extern idCVar r_rtBloom;
extern idCVar r_rtBloomThreshold;
extern idCVar r_rtBloomEmissiveThreshold;
extern idCVar r_rtBloomKnee;
extern idCVar r_rtBloomLitWeight;
extern idCVar r_rtBloomStrength;
extern idCVar r_rtBloomMips;
extern idCVar r_rtBloomDebug;

// Highest bloom mip count r_rtBloomMips can ask for.  bloomMip[0] is half display
// res, so mip 5 is 1/64 — a radius well past anything the art wants.
#define VK_BLOOM_MAX_MIPS 6

void VK_RT_InitBloom(void);
void VK_RT_ResizeBloom(uint32_t width, uint32_t height);
void VK_RT_ShutdownBloom(void);

// Does bloom want its passes run this frame?  False when disabled, when the shaders
// failed to load, or before the images exist.  Checked by both dispatch sites and by
// the backend's end/resume decision, so the render pass is never split for nothing.
bool VK_RT_BloomActive(void);

// B1a: half-res snapshot of hdrScene, taken after the interactions and before the
// blend stages.  Must be OUTSIDE the render pass.  Call once per frame from the
// primary, non-subview, real-camera path — the same gate as the FSR mask capture.
void VK_RT_BloomPrepass(VkCommandBuffer cmd);

// B1b: diff the current hdrScene against that snapshot into bloomMip[0].  Must be
// OUTSIDE the render pass, after VK_RB_DrawShaderPasses and before the late vol
// composite, which is a separate medium that must not bloom.  No-op unless the
// matching prepass ran this frame in this slot.
void VK_RT_BloomExtract(VkCommandBuffer cmd);

// B2: does the blur chain and composite want to run this frame?  Gated on r_rtBloom
// alone, unlike VK_RT_BloomActive — r_rtBloomDebug 2 pulls the capture up by itself
// and must not start compositing glow the user switched off.
bool VK_RT_BloomCompositeActive(void);

// B2: the down/up blur chain followed by the additive composite into hdrScene.  Must
// be OUTSIDE the render pass, at the 3D->GUI boundary: after VK_RT_DispatchUpscale so
// the glow is display-resolution and scale-independent, and before the UI draws, since
// the tonemap (and therefore the HUD's arrival in hdrScene) comes later.
void VK_RT_DispatchBloom(VkCommandBuffer cmd);

// r_rtBloomDebug 2: bloomMip[0] magnified over the display, replacing the frame.
// Same slot as the FSR debug overlays — outside the render pass, after the resolve
// and before the tonemap.
bool VK_RT_BloomDebugActive(void);
void VK_RT_DispatchBloomDebug(VkCommandBuffer cmd);
