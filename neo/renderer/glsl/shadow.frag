/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 GLSL adaptation.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Shadow volume fragment shader.
// Color output is disabled for the shadow pass (stencil-only writes).
// This shader exists only to satisfy the pipeline creation requirement.

#version 450

void main() {
    // Intentionally empty — only stencil buffer is written during shadow volumes.
}
