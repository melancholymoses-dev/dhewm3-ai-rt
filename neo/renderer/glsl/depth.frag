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

#version 330 core

in vec2 vary_TexCoord;

uniform sampler2D u_DiffuseMap;  // used only for alpha-test surfaces
uniform bool      u_AlphaTest;
uniform float     u_AlphaTestThreshold;

out vec4 fragColor;

void main() {
    if (u_AlphaTest) {
        float alpha = texture(u_DiffuseMap, vary_TexCoord).a;
        if (alpha < u_AlphaTestThreshold) {
            discard;
        }
    }
    // No color output needed — depth is written automatically.
    fragColor = vec4(0.0);
}
