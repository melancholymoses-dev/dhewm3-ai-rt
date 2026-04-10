/* gi_payload.glsl — shared ray payload for the GI ray pipeline.

  Included by gi_ray.rgen, gi_ray.rchit, gi_ray.rahit, and gi_ray.rmiss
  before the rayPayloadEXT/rayPayloadInEXT declaration.

  Layout (must match across all GI shader stages):

    colour — RGB albedo at the secondary hit, or ambient sky colour on miss.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

struct GIPayload {
    vec3 colour; // albedo at hit (or ambient on miss)
};
