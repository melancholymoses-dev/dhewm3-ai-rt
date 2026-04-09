/* reflect_payload.glsl — shared ray payload for the reflection ray pipeline.
 
  Included by reflect_ray.rgen, reflect_ray.rchit, reflect_ray.rahit,
  and reflect_ray.rmiss before the rayPayloadEXT/rayPayloadInEXT declaration.
 
  Layout (must match across all reflection shader stages):
 
    colour        — RGB colour contributed by this ray segment.
    transmittance — Weight for the continuation ray.  0.0 = stop (opaque/miss).
                    0.96 = glass (4 % reflected, 96 % transmitted straight-through).
    nextOrigin    — World-space origin for the continuation ray (past the glass surface).
    nextDir       — Direction for the continuation ray (straight-through for glass).
This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/


struct ReflPayload {
    vec3  colour;
    float transmittance;
    vec3  nextOrigin;
    vec3  nextDir;
  uint  debugFlags;
};
