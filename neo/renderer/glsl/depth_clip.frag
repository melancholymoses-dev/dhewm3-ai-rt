/*
===========================================================================

dhewm3-rt - depth-prepass alpha-clip fragment shader.

Used for MC_PERFORATED (alpha-tested) surfaces during the depth prepass.
Samples the diffuse texture and discards pixels whose alpha falls below
the threshold stored in u_ColorAdd.w.  Colour writes are disabled in the
pipeline, so the output colour is irrelevant.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/
#version 450

layout(location = 0) in vec4 vary_TexCoord;
layout(location = 1) in vec4 vary_Color;

layout(set=0, binding=0) uniform GuiParams {
    mat4 u_ModelViewProjection;
    vec4 u_ColorModulate;
    vec4 u_ColorAdd;    // w = alpha-test threshold
};

layout(set=0, binding=1) uniform sampler2D u_Texture;

layout(location = 0) out vec4 fragColor;

void main() {
    // Match GL perforated depth fill: stage alpha scale comes from current color.
    float alpha = texture(u_Texture, vary_TexCoord.xy).a * u_ColorModulate.w;
    if (alpha <= u_ColorAdd.w)
        discard;
    fragColor = vec4(0.0); // colour writes disabled in pipeline
}
