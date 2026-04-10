/*
glass_refl_overlay.frag — Additive RT-reflection overlay for translucent surfaces.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

Drawn once per MC_TRANSLUCENT surface (e.g. glass) after the normal material stages.
Samples the RT reflection buffer at the fragment's screen position and outputs it
additively so reflections appear on top of the existing translucent colour.

Schlick Fresnel weighting for the primary glass interface is written to alpha by
reflect_ray.rgen, and consumed here for additive compositing.

Layout notes:
  binding 0 — GuiParams UBO (shared with gui.vert.spv).
               Screen dimensions are passed via texGenS.xy (otherwise unused when
               texMatrixS.z == 0).
  binding 1 — RT reflection buffer (RGBA16F, sampler2D, GENERAL layout).
*/

#version 450

layout(set=0, binding=0) uniform GuiParams {
    mat4  u_ModelViewProjection; // used by vertex shader
    vec4  u_ColorModulate;
    vec4  u_ColorAdd;
    vec4  u_TexMatrixS;          // z == texgen flag (should be 0 for this overlay)
    vec4  u_TexMatrixT;
    vec4  u_TexGenS;             // x = renderWidth, y = renderHeight (stashed by backend)
    vec4  u_TexGenT;
    vec4  u_TexGenQ;
};

layout(set=0, binding=1) uniform sampler2D u_ReflectionMap;

layout(location = 0) out vec4 fragColor;

void main()
{
    // Compute normalised screen UV from pixel coordinate + render resolution.
    vec2 screenUV = gl_FragCoord.xy / u_TexGenS.xy;

    // Sample the reflection buffer.
    // RGB stores reflected radiance; alpha stores Schlick glass weight from raygen.
    vec4 reflSample = texture(u_ReflectionMap, screenUV);
    vec3 refl = reflSample.rgb * reflSample.a;

    // Additive output.  The pipeline blend is ONE/ONE so alpha is effectively ignored,
    // but set it to 1.0 for robustness.
    fragColor = vec4(refl, 1.0);
}
