/*
rt_ggx.glsl — GGX VNDF importance sampling (Stage 3, docs/plans/20261013_reflection_roughness.md).

Isotropic GGX, sampling the distribution of VISIBLE normals rather than the full
NDF (Heitz 2018, "Sampling the GGX Distribution of Visible Normals") — unbiased,
and alpha == 0 collapses the sampled half-vector to the macro normal exactly, so
a roughness-0 surface reduces to today's mirror ray with no special-casing at
the call site.

Single-sample per pixel: this file has no sample-count loop. The resulting
variance is resolved over frames by the temporal denoiser (Stage 1,
refl_temporal_resolve.comp), not by sampling more here.

No bindings, no ray payload, no traceRayEXT — pipeline-agnostic (docs/rt_engine_rules.md).

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_GGX_GLSL
#define RT_GGX_GLSL

// Arbitrary orthonormal basis around n. Same construction as
// rt_CosineSampleHemisphere (rt_indirect.glsl); duplicated here since this file
// stays standalone/pipeline-agnostic rather than depending on that include.
void rt_BuildBasis(vec3 n, out vec3 t1, out vec3 t2)
{
    vec3 up = abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    t1 = normalize(cross(up, n));
    t2 = cross(n, t1);
}

// Samples a reflected direction around macro normal n, given the incident
// direction (pointing INTO the surface, same convention as GLSL's reflect())
// and isotropic roughness alpha (= perceptual roughness^2, the standard
// remap). u1/u2 are independent uniform randoms in [0,1).
//
// alpha == 0 (or a grazing/degenerate view angle) falls back to the exact
// mirror reflection of incidentDir around n.
vec3 rt_SampleGGXReflection(vec3 n, vec3 incidentDir, float alpha, float u1, float u2)
{
    vec3 v = -incidentDir; // points from the surface toward the viewer (Heitz's Ve)

    vec3 t1, t2;
    rt_BuildBasis(n, t1, t2);

    vec3 Ve = vec3(dot(v, t1), dot(v, t2), dot(v, n));
    if (Ve.z <= 1e-4 || alpha <= 1e-4)
        return reflect(incidentDir, n);

    vec3 Vh = normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3  Th1   = (lensq > 0.0) ? vec3(-Vh.y, Vh.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3  Th2   = cross(Vh, Th1);

    float r   = sqrt(u1);
    float phi = 2.0 * 3.14159265358979 * u2;
    float p1  = r * cos(phi);
    float p2t = r * sin(phi);
    float s   = 0.5 * (1.0 + Vh.z);
    float p2  = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2t;

    vec3 Nh = p1 * Th1 + p2 * Th2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * Vh;
    vec3 Ne = normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));

    vec3 h = Ne.x * t1 + Ne.y * t2 + Ne.z * n;
    return reflect(incidentDir, h);
}

#endif // RT_GGX_GLSL
