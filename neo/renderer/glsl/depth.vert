/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
dhewm3 GLSL adaptation.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Depth pre-pass vertex shader.
// Used by RB_T_FillDepthBuffer to populate the depth buffer before lighting passes.

#version 450

layout(location = 0) in vec3 in_Position;
layout(location = 8) in vec2 in_TexCoord;   // for alpha-test

// Shared UBO with depth.frag — binding 0, both stages.
layout(set=0, binding=0) uniform DepthParams {
    mat4  u_ModelViewProjection;
    vec4  u_TextureMatrixS;       // diffuse matrix S (for alpha-tested surfaces)
    vec4  u_TextureMatrixT;       // diffuse matrix T
    int   u_AlphaTest;
    float u_AlphaTestThreshold;
    vec2  _pad;
};

layout(location = 0) out vec2 vary_TexCoord;

void main() {
    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);
    vary_TexCoord.x = dot(tc, u_TextureMatrixS);
    vary_TexCoord.y = dot(tc, u_TextureMatrixT);

    gl_Position = u_ModelViewProjection * vec4(in_Position, 1.0);
}
