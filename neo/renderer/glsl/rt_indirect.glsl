/*
This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.
*/

// ---------------------------------------------------------------------------
// rt_indirect.glsl  —  Shared helpers for indirect lighting shaders
//
// Included by ao_ray.rgen and gi_ray.rgen (Phase 6).
// Do NOT add pipeline-specific bindings here; each rgen shader declares
// its own layout block and passes depthSampler + invViewProj explicitly.
//
// Required by includer:
//   uniform sampler2D depthSampler        (binding 2, same slot as shadow rgen)
//   mat4              rt_invViewProj       (from UBO params)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Wang hash — fast integer pseudo-random
// ---------------------------------------------------------------------------
uint wang_hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed  = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed  = seed ^ (seed >> 15u);
    return seed;
}

float randFloat(uint seed)
{
    return float(wang_hash(seed)) / 4294967296.0;
}

// ---------------------------------------------------------------------------
// World-space position from depth buffer
//
// coord      — integer screen pixel coordinate
// size       — full framebuffer dimensions (ivec2)
// invViewProj — inverse view-projection (GL convention, Z in [-1,1])
//
// Depth buffer stores Vulkan Z in [0,1].  The projection matrix was built
// with a GL-convention Z remap, so we un-remap here: ndcZ = 2*depth - 1.
// Viewport uses negative height (Y-flip), so ndcY = 1 - 2*uv.y.
// ---------------------------------------------------------------------------
vec3 rt_ReconstructWorldPos(ivec2 coord, ivec2 size, mat4 invViewProj)
{
    vec2 uv = (vec2(coord) + 0.5) / vec2(size);
    float depth = texelFetch(depthSampler, coord, 0).r;

    vec4 clipPos = vec4(uv.x * 2.0 - 1.0,
                        1.0 - 2.0 * uv.y,
                        2.0 * depth - 1.0,
                        1.0);
    vec4 worldPos = invViewProj * clipPos;
    return worldPos.xyz / worldPos.w;
}

// ---------------------------------------------------------------------------
// Depth-derived world-space normal
//
// Uses central differences (or one-sided fallback) on the four screen
// neighbours.  Returns the camera-toward-surface direction as a fallback
// when the reconstruction is degenerate (silhouette edges, sky).
//
// orient  — reference direction used to orient the normal (e.g. toward
//            a light, or toward the camera for AO).  The returned normal
//            is flipped so that dot(normal, orient) > 0.
// ---------------------------------------------------------------------------
vec3 rt_ReconstructNormal(ivec2 coord, ivec2 size, mat4 invViewProj, vec3 worldPos, vec3 orient)
{
    const float kEdge = 0.01; // depth-delta threshold for edge detection

    float depthC = texelFetch(depthSampler, coord,                  0).r;
    float depthR = texelFetch(depthSampler, coord + ivec2( 1,  0),  0).r;
    float depthL = texelFetch(depthSampler, coord + ivec2(-1,  0),  0).r;
    float depthD = texelFetch(depthSampler, coord + ivec2( 0,  1),  0).r;
    float depthU = texelFetch(depthSampler, coord + ivec2( 0, -1),  0).r;

    bool okR = abs(depthR - depthC) < kEdge;
    bool okL = abs(depthL - depthC) < kEdge;
    bool okD = abs(depthD - depthC) < kEdge;
    bool okU = abs(depthU - depthC) < kEdge;

    vec3 posR = rt_ReconstructWorldPos(coord + ivec2( 1,  0), size, invViewProj);
    vec3 posL = rt_ReconstructWorldPos(coord + ivec2(-1,  0), size, invViewProj);
    vec3 posD = rt_ReconstructWorldPos(coord + ivec2( 0,  1), size, invViewProj);
    vec3 posU = rt_ReconstructWorldPos(coord + ivec2( 0, -1), size, invViewProj);

    vec3 dX, dY;
    if      (okR && okL) dX = posR - posL;
    else if (okR)        dX = posR - worldPos;
    else if (okL)        dX = worldPos - posL;
    else                 dX = vec3(0.0);

    if      (okD && okU) dY = posD - posU;
    else if (okD)        dY = posD - worldPos;
    else if (okU)        dY = worldPos - posU;
    else                 dY = vec3(0.0);

    vec3  crossed  = cross(dX, dY);
    float crossLen = length(crossed);

    vec3 n;
    if (crossLen > 1e-4)
    {
        n = crossed / crossLen;
        if (dot(n, orient) < 0.0) n = -n;
    }
    else
    {
        // Degenerate: fall back to orient direction (safe for AO hemisphere)
        n = normalize(orient);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Cosine-weighted hemisphere sample around normal n
//
// seed   — two consecutive Wang-hash values are consumed (seed, seed+1)
// Returns a unit vector in the hemisphere of n.
// ---------------------------------------------------------------------------
vec3 rt_CosineSampleHemisphere(vec3 n, uint seed)
{
    float r1 = randFloat(seed);
    float r2 = randFloat(seed + 1u);

    // Malley's method: uniform disk sample lifted to hemisphere
    float r     = sqrt(r1);
    float theta = 2.0 * 3.14159265358979 * r2;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - r1)); // r1 = x^2 + y^2

    // Build local frame around n
    vec3 up    = abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, n));
    vec3 fwd   = cross(n, right);

    return normalize(right * x + fwd * y + n * z);
}

// ---------------------------------------------------------------------------
// World-anchored seed + per-pixel blue-noise phi for temporal stability
//
// worldPos   — surface point (used for world-space seed cell)
// coord      — screen pixel (used for per-pixel phi offset)
// frameIndex — current frame (used to advance phi by golden ratio)
//
// Returns seed suitable for use with rt_CosineSampleHemisphere.
// phiSeed is a separate value in [0,1) for azimuthal rotation; blend it
// into your sample generation as appropriate.
// ---------------------------------------------------------------------------
uint rt_WorldAnchoredSeed(vec3 worldPos, uint frameIndex)
{
    ivec3 wCell = ivec3(floor(worldPos * 8.0));
    uint  wSeed = wang_hash(uint(wCell.x) * 2053u
                ^ wang_hash(uint(wCell.y) * 4099u
                ^ wang_hash(uint(wCell.z) * 8191u)));
    return wang_hash(wSeed + frameIndex * 7919u);
}

float rt_PixelPhiOffset(ivec2 coord, uint frameIndex)
{
    float pixHash = float(wang_hash(uint(coord.x) ^ wang_hash(uint(coord.y)))) / 4294967296.0;
    return fract(pixHash + float(frameIndex) * 0.6180339887);
}
