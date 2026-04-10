/*
===========================================================================

dhewm3-rt Vulkan — gi_composite.vert — fullscreen triangle for GI composite.

Generates a single oversized triangle from the vertex index.  No vertex
buffer is required; the caller issues vkCmdDraw(3, ...).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#version 450

void main()
{
    // Standard full-screen triangle from vertex index:
    //   ID=0 → (-1,-1)   ID=1 → (3,-1)   ID=2 → (-1,3)
    vec2 uv  = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
