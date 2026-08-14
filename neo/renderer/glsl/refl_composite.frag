/*
===========================================================================

dhewm3-rt Vulkan — refl_composite.frag — reflection buffer composite into framebuffer.

Stage 3.5 / Step 8 (see docs/plans/gbuffer_normal_pass.md): additively blends
reflBuffer (written by reflect_ray.rgen) onto the framebuffer once per view,
replacing the disabled per-light reflection block in interaction.frag — that
block accumulated N times for N lights touching a pixel; this pass runs once.

Deliberately as simple as gi_composite.frag: reflect_ray.rgen already computes
the Schlick Fresnel term (using the G-buffer normal and depth, both legitimately
sampled there while depth is in a shader-readable layout outside any render
pass) and bakes it into reflBuffer.rgb, and also handles r_rtReflectionDebugMode
2-4 by writing the debug visualization directly into reflBuffer. This pass
cannot do that work itself: it runs INSIDE the resumed render pass (same slot
as VK_RT_CompositeGI), where depth is bound as that render pass's read-write
attachment (subview depth prepasses can still write it later in the same
render pass instance) and so cannot simultaneously be sampled here.

Negative alpha is a sentinel meaning "glass pixel — skip, composited
separately by glass_refl_overlay.frag": the depth/G-buffer at a glass-covered
pixel belongs to the unrelated opaque surface behind the glass, so this pass
must not also apply that surface's data to the glass ray's radiance.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#version 450

layout(set = 0, binding = 0) uniform sampler2D u_ReflMap;

layout(location = 0) out vec4 fragColor;

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4  s     = texelFetch(u_ReflMap, coord, 0);

    // Negative alpha: glass pixel, handled by glass_refl_overlay.frag instead.
    fragColor = (s.a < 0.0) ? vec4(0.0) : vec4(s.rgb, 1.0);
}
