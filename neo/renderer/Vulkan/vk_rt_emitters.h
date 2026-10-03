/*
===========================================================================

dhewm3-rt Vulkan ray tracing - reflection emitter list.

Projectiles tagged renderEntity_t::rtGlow are collected each frame into a small
SSBO of glowing points. reflect_ray.rgen intersects reflection rays against them
analytically (ray-facing discs), so projectiles appear in glass without entering
the TLAS. See docs/plans/20261002_reflection_emitters.md.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#ifndef __VK_RT_EMITTERS_H__
#define __VK_RT_EMITTERS_H__

#include "renderer/Vulkan/vk_common.h"

// Must match RT_MAX_EMITTERS in rt_emitter.glsl.
#define VK_RT_MAX_EMITTERS 64

// std430 mirror of RtEmitter in rt_emitter.glsl — 32 bytes.
struct vkRTEmitter_t
{
    float pos[3];
    float radius;
    float rgb[3];
    uint32_t texIndex; // bindless matTextures slot; 0 = procedural falloff
};

void VK_RT_InitReflEmitters(void);
void VK_RT_ShutdownReflEmitters(void);

// Fills the current frame slot's emitter buffer. Call after VK_RT_RebuildTLAS and
// before VK_RT_FlushBindlessTextures so newly registered textures are written this
// frame. Only the primary 3D view writes; subviews, mirrors and the GUI view leave the
// slot untouched (the dispatch already captured the count in its UBO).
void VK_RT_BuildReflEmitters(const struct viewDef_s *viewDef);

VkBuffer VK_RT_GetReflEmitterBuffer(int frameIdx);
uint32_t VK_RT_GetReflEmitterCount(int frameIdx);

#endif // __VK_RT_EMITTERS_H__
