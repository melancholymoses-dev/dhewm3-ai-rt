/*
vk_gui_texture.h — live in-world GUI textures for RT reflections
(docs/plans/20261015_gui_reflections.md Stage 1).

The frontend copies each in-world GUI it draws into a small offscreen view; the backend
renders those views into a pool of slot idImages at the start of the frame, before any
RT dispatch can sample them.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef __VK_GUI_TEXTURE_H__
#define __VK_GUI_TEXTURE_H__

#include "renderer/Vulkan/vk_common.h"

class idUserInterface;
class idImage;

#define VK_GUITEX_SLOTS 16
#define VK_GUITEX_WIDTH 512
#define VK_GUITEX_HEIGHT 384

// Frontend, from R_RenderGuiSurf after gui->Redraw: queue tr.guiModel's contents for
// this GUI's slot. No-op when disabled, in subviews, or already queued this frame.
void VK_GuiTex_CaptureFromGuiModel(const idUserInterface *gui);

// Slot currently holding this GUI, or -1. For Stage 2's material binding.
int VK_GuiTex_FindSlot(const idUserInterface *gui);
idImage *VK_GuiTex_SlotImage(int slot);

// Backend: views queued this frame. Pending() returns NULL for a stale or empty entry.
int VK_GuiTex_PendingCount(void);
struct viewDef_s *VK_GuiTex_Pending(int index, int *outSlot);
void VK_GuiTex_ClearPending(void);

// Backend, outside any render pass: BeginPass sizes the slot image, then opens the
// offscreen pass (compatible with vk.hdrRenderPass, so every GUI pipeline draws into it).
// EndPassAndCopy closes it and blits into the slot image's mip 0 (all the RT stages read).
bool VK_GuiTex_BeginPass(VkCommandBuffer cmd, int slot);
void VK_GuiTex_EndPassAndCopy(VkCommandBuffer cmd, int slot);

void VK_GuiTex_BeginLevelLoad(void); // forget GUI -> slot mapping
void VK_GuiTex_Shutdown(void);       // device must be idle

#endif // __VK_GUI_TEXTURE_H__
