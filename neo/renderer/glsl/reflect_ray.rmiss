/*
===========================================================================
Doom 3 GPL Source Code — dhewm3 Vulkan
reflect_ray.rmiss — miss shader for reflection rays.

Ray missed all geometry: the reflected direction points toward open space
(sky, void, or far distance).  Return a sky gradient colour and set
transmittance = 0 so the rgen bounce loop stops here.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

#include "reflect_payload.glsl"
layout(location = 0) rayPayloadInEXT ReflPayload reflPayload;

void main()
{
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    float up = dir.y * 0.5 + 0.5;   // remap [-1,1] → [0,1]

    // Sky colours: Martian atmosphere — fine iron-oxide dust scatters red/orange,
    // producing a salmon-pink sky rather than earth-blue.
    // Horizon/ground: deeper dusty red-brown.
    // Zenith: slightly lighter salmon (still warm, not blue).
    vec3 groundColor = vec3(0.26, 0.12, 0.07);  // dark Martian dust at horizon
    vec3 skyColor    = vec3(0.38, 0.22, 0.14);  // hazy salmon-pink Mars sky
    vec3 color = mix(groundColor, skyColor, up);

    reflPayload.colour        = color;
    reflPayload.transmittance = 0.0;  // stop — sky has no continuation
}
