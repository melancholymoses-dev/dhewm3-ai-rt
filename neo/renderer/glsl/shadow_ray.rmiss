/*
RT Shadow Shader
This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

#version 460
#extension GL_EXT_ray_tracing : require

// ---------------------------------------------------------------------------
// shadow_ray.rmiss  —  Miss shader
//
// Fired when the shadow ray reaches the light without hitting any geometry.
// Write 1.0 into the payload: this pixel is lit.
// ---------------------------------------------------------------------------

layout(location = 0) rayPayloadInEXT float shadowFactor;

void main() {
    shadowFactor = 1.0;
}
