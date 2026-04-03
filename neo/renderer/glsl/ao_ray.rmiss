#version 460
#extension GL_EXT_ray_tracing : require

// ao_ray.rmiss — AO miss shader
// No geometry was hit within aoRadius: the direction is unoccluded.
layout(location = 0) rayPayloadInEXT float aoPayload;

void main()
{
    aoPayload = 1.0; // unoccluded
}
