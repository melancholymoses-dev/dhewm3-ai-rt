/* gi_shadow_payload.glsl — shadow ray payload for GI Option B.

  Used by gi_ray.rchit (traces shadow rays toward lights at the secondary hit)
  and gi_shadow.rmiss (clears the flag when no geometry is hit).

  Payload layout (location = 1):
    occluded — true if a blocker was found, false if the ray reached the light.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

struct GIShadowPayload {
    bool occluded; // true = blocked; false = light visible
};
