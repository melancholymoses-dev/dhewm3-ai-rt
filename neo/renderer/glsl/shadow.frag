/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 GLSL adaptation.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
===========================================================================
*/

// Shadow volume fragment shader.
// Color output is disabled for the shadow pass (stencil-only writes).
// This shader exists only to satisfy the pipeline creation requirement.

#version 450

void main() {
    // Intentionally empty — only stencil buffer is written during shadow volumes.
}
