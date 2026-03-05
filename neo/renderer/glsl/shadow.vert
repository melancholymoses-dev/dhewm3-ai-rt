/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
dhewm3 GLSL adaptation.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Shadow volume vertex shader - replaces shadow.vp ARB vertex program.
// Extrudes shadow volume geometry toward the light (for stencil shadow volumes).

#version 330 core

// Shadow vertices have a 4th component: w=0 means vertex at infinity (extruded),
// w=1 means regular vertex. This encodes the extrusion in the .w of position.
layout(location = 0) in vec4 in_Position;  // xyz = position, w = 0 (infinity) or 1 (cap)

uniform vec4 u_LightOrigin;             // local-space light origin
uniform mat4 u_ModelViewProjection;

void main() {
    // If w == 0, this vertex is at "infinity" in the direction away from the light.
    // Extrude: mix between the vertex and the light->vertex direction at infinity.
    // When w=1: regular cap vertex at in_Position.xyz
    // When w=0: vertex extruded to infinity in direction (vertex - light), rendered
    //           in homogeneous clip space as a point at infinity.

    // Doom 3 shadow vertex format: w component encodes extrusion flag.
    // The ARB program multiplied the vertex position by the w component to collapse
    // w=0 vertices to the light origin, then moved them to infinity in projection.

    // In homogeneous coordinates: for w=0, gl_Position = P*MVP where the xyz are
    // the direction (vertex - light) expressed at infinity.
    vec4 pos = in_Position;

    if (pos.w < 0.5) {
        // Extruded vertex: direction from light to surface vertex, at infinity
        vec3 dir = pos.xyz - u_LightOrigin.xyz;
        // Set w=0 in clip space so the point is at infinity (no perspective divide)
        gl_Position = u_ModelViewProjection * vec4(dir, 0.0);
    } else {
        // Cap vertex: regular position
        gl_Position = u_ModelViewProjection * vec4(pos.xyz, 1.0);
    }
}
