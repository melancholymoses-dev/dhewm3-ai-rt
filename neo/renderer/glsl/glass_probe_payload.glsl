/* glass_probe_payload.glsl — payload for the glass-surface probe ray.
 
  Fired from reflect_ray.rgen before the main reflection bounce, travelling
  from the camera toward the depth-buffer surface.  If a glass surface is
  found in between, its world-space position and outward normal are written
  here so the rgen can use the correct origin for the reflection.

  hitT == 0  →  no glass surface was found; use depth-reconstructed origin.
  hitT >  0  →  glass found at that t; hitPos/hitNormal are valid.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

struct GlassProbePayload {
    vec3  hitPos;     // world-space position on glass surface
    float hitT;       // ray t value; 0 = no glass found
    vec3  hitNormal;  // world-space outward-facing normal of glass surface
    float pad;
};
