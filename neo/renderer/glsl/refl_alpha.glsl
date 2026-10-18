/* refl_alpha.glsl — the reflection buffer's alpha channel convention.

  Stage 2b (docs/plans/20261017_reflection_pipeline_refactor.md): rgb holds
  incident light only and alpha holds the weight that turns it into radiance, so
  the denoisers filter a signal with no F0 pattern in it.

  | alpha              | Meaning                                                  |
  |--------------------|----------------------------------------------------------|
  | < 0                | glass — glass_refl_overlay.frag composites it, using abs |
  | <= REFL_ALPHA_VALID_MIN | never traced (sky, culled, no G-buffer)             |
  | > REFL_ALPHA_VALID_MIN  | opaque, traced; the Fresnel weight to multiply by  |

  The floor matters: a traced pixel whose per-pixel F0 is ~0 (a scuff inside a
  reflective panel) must still read as traced, or the filters treat it as a hole
  and the demodulation buys nothing.  ReflAlpha_Opaque keeps it clear of the
  threshold by 2x, which costs the composite at most 2/255 of a pixel's incident
  light where the real weight was zero.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef REFL_ALPHA_GLSL
#define REFL_ALPHA_GLSL

const float REFL_ALPHA_VALID_MIN   = 1.0 / 255.0;
const float REFL_ALPHA_TRACED_MIN  = 2.0 / 255.0;

bool ReflAlpha_IsGlass(float a) { return a < 0.0; }

// True for an opaque pixel that traced a ray.  Glass fails it too (negative).
bool ReflAlpha_Traced(float a) { return a > REFL_ALPHA_VALID_MIN; }

// What reflect_ray.rgen stores for a traced opaque pixel.
float ReflAlpha_Opaque(float fresnel) { return max(fresnel, REFL_ALPHA_TRACED_MIN); }

#endif // REFL_ALPHA_GLSL
