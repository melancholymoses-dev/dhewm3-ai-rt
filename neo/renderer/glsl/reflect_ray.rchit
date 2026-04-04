/*
===========================================================================
Doom 3 GPL Source Code — dhewm3 Vulkan
reflect_ray.rchit — closest-hit shader for reflection rays.

Cheap approximation: returns a direction-based environment tint using the
reflected ray direction rather than sampling any material texture.  The
tint distinguishes "ceiling / open space above" from "wall / floor" hits
while requiring no vertex-buffer or material-table access.

Once Phase 5.4 (TLAS instance metadata) is in place, this shader can be
upgraded to sample the actual hit surface's diffuse colour.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 reflPayload;

void main()
{
    // Use the reflected ray's travel direction as an IBL-probe approximation.
    // The ray direction is world-space; derive a simple sky/floor gradient.
    vec3 dir = normalize(gl_WorldRayDirectionEXT);

    // In Doom 3 world space the camera looks along -Y with Z up (game space),
    // but the shaders use GL convention where Y is up after the camera transform.
    // Use the Y component as the "up" axis for the gradient.
    float up = dir.y * 0.5 + 0.5;   // remap [-1,1] → [0,1]

    // Environment tint: dark reddish-brown for downward hits (floors/lower walls),
    // warm dusty rust for upward hits (Martian ceiling/upper walls).
    // Mars surface is iron-oxide red; no blue-sky light source exists indoors.
    // Geometry hits are 60% as bright as open sky (miss shader) to distinguish.
    vec3 floorTint  = vec3(0.10, 0.06, 0.04);  // dark red-brown dust
    vec3 ceilTint   = vec3(0.18, 0.11, 0.07);  // warmer rust for upward-facing hits
    vec3 color = mix(floorTint, ceilTint, up);

    reflPayload = vec4(color, 1.0);
}
