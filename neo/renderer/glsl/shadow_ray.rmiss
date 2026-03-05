#version 460
#extension GL_EXT_ray_tracing : require

// ---------------------------------------------------------------------------
// shadow_ray.rmiss  —  Miss shader
//
// Fired when the shadow ray reaches the light without hitting any geometry.
// Write 1.0 into the payload: this pixel is lit.
// ---------------------------------------------------------------------------

layout(location = 0) rayPayloadInEXT float shadowFactor;

void main() {
    shadowFactor = 1.0;
}
