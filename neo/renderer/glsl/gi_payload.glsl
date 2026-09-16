/* gi_payload.glsl — shared ray payload for the GI ray pipeline.

  Included by gi_ray.rgen, gi_ray.rchit, gi_ray.rahit, gi_ray.rmiss and
  gi_probe_trace.rgen before the rayPayloadEXT/rayPayloadInEXT declaration.

  Layout (must match across all GI shader stages):

    colour   — RGB albedo at the secondary hit, or ambient sky colour on miss.
    hitDist  — gl_HitTEXT at a hit, gl_RayTmaxEXT on a miss.  Probe GI needs it
               for the visibility (Chebyshev) moments; the per-pixel rgen ignores
               it.  20260906_froxel_probe_gi.md B.2.
    backface — 1.0 when the ray hit a back face, else 0.0.  Probe classification
               (G4) counts these to decide a probe is buried in geometry.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

struct GIPayload {
    vec3  colour;   // albedo at hit (or ambient on miss)
    float hitDist;  // gl_HitTEXT at hit, gl_RayTmaxEXT on miss
    float backface; // 1.0 = back-face hit
};
