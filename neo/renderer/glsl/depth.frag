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

// Depth pre-pass fragment shader.
// Outputs nothing (depth only), with optional alpha test for masked surfaces.

#version 450

layout(location = 0) in vec2 vary_TexCoord;

// Shared UBO with depth.vert — binding 0, both stages.
layout(set=0, binding=0) uniform DepthParams {
    mat4  u_ModelViewProjection;
    vec4  u_TextureMatrixS;
    vec4  u_TextureMatrixT;
    int   u_AlphaTest;
    float u_AlphaTestThreshold;
    vec2  _pad;
};
layout(set=0, binding=1) uniform sampler2D u_DiffuseMap;  // alpha-test surfaces only

layout(location = 0) out vec4 fragColor;

void main() {
    if (u_AlphaTest != 0) {
        float alpha = texture(u_DiffuseMap, vary_TexCoord).a;
        if (alpha < u_AlphaTestThreshold) {
            discard;
        }
    }
    // No color output needed — depth is written automatically.
    fragColor = vec4(0.0);
}
