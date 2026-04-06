/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
dhewm3 GLSL adaptation.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

// Shadow volume vertex shader - replaces shadow.vp ARB vertex program.
// Use the original branchless w-based extrusion form to avoid edge-case
// instability from thresholding on in_Position.w.

#version 450

// Shadow vertices have a 4th component: w=0 means vertex at infinity (extruded),
// w=1 means regular vertex. This encodes the extrusion in the .w of position.
layout(location = 0) in vec4 in_Position;  // xyz = position, w = 0 (infinity) or 1 (cap)

layout(set=0, binding=0) uniform ShadowParams {
    vec4  u_LightOrigin;             // local-space light origin
    mat4  u_ModelViewProjection;
};

void main() {
    // ARB/vkDOOM3 equivalent:
    //   vPos = in_Position - lightOrigin
    //   vPos = vPos.w * lightOrigin + vPos
    // For w=1 => original position. For w=0 => extruded direction at infinity.
    vec4 vPos = in_Position - u_LightOrigin;
    vPos = (vPos.wwww * u_LightOrigin) + vPos;
    gl_Position = u_ModelViewProjection * vPos;
}
