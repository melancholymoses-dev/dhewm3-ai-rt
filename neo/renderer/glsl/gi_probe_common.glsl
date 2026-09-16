/* gi_probe_common.glsl — shared geometry/addressing for the irradiance probe
  passes (20260906_froxel_probe_gi.md, Part B).

  THE LATTICE IS ABSOLUTE.  A probe's world position is cell * spacing, where
  cell is an absolute integer lattice coordinate — never an offset from a
  camera-derived origin.  baseCell only says which 32x32x16 window of that
  infinite lattice is currently resident, so probes cannot swim when the camera
  moves; only which probes exist changes.

  STORAGE IS TOROIDAL.  A probe's slot in the atlas is (absCell mod gridDim),
  not its position within the window.  Scrolling the window by one cell then
  leaves every surviving probe on its own slot and invalidates only the slab
  that wrapped around — which is why the grid can follow the camera without
  rewriting the atlas.  The CPU (vk_gi_probe.cpp) clears GIPROBE_FLAG_TRACED on
  exactly those probes, and the blend pass treats an untraced probe as having no
  history rather than blending against the previous occupant's irradiance.

  Octahedral maps carry each probe's directional data: an interiorSide^2 block
  plus a one-texel border, so a bilinear fetch at the seam reads the wrapped
  neighbour instead of clamping.  gi_probe_border.comp fills that border.

  Includer contract:
    #define GIPROBE_SET <n>   before #include — the RT pipeline binds these at
    set 2, the compute pipelines at set 0.  The file declares the params UBO at
    binding 2 and the probe-state SSBO at binding 3 of that set, so every probe
    pass must leave those two slots free.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef GI_PROBE_COMMON_GLSL
#define GI_PROBE_COMMON_GLSL

#ifndef GIPROBE_SET
#define GIPROBE_SET 0
#endif

// std140 — offsets must match GIProbeParamsUBO in vk_gi_probe.cpp exactly.
// vec4-packed for the same reason VolFroxelParams is: every member is 16-byte
// aligned by construction, so std140 has nothing left to surprise us with.
layout(set = GIPROBE_SET, binding = 2, std140) uniform GIProbeParams {
    mat4  invViewProj; //   0  GL-convention clip Z, see gip_RayDirForUV
    vec4  cameraPosW;  //  64  xyz = eye world position
    vec4  gridOrigin;  //  80  xyz = world pos of baseCell, w = probe spacing
    ivec4 gridDim;     //  96  xyz = Nx,Ny,Nz   w = total probe count
    ivec4 baseCell;    // 112  xyz = absolute lattice cell of the window corner
    ivec4 atlas;       // 128  x=tilesX y=tilesY z=irradiance side w=distance side
    ivec4 rays;        // 144  x=raysPerProbe y=probesPerFrame z=updateBase w=frameIndex
    vec4  tune;        // 160  x=hysteresis y=normalBias z=giStrength w=maxRayDist
    ivec4 misc;        // 176  x=debugMode y=useGbufNormal z=visibility w=unused
    ivec4 screen;      // 192  x=screenW y=screenH z=outW w=outH
    ivec4 rect;        // 208  resolve dispatch rect, output-image space
    vec4  debug;       // 224  x=debugGain y=probeRadius z=distSharpness w=unused
    vec4  tune2;       // 240  x=giContrast  y/z/w reserved (G3 Chebyshev)
} gp;

// std430: vec3 has 16-byte alignment, so the trailing uint packs into the same
// 16 bytes and this matches GIProbeStateEntry in vk_gi_probe.cpp.
//
// Entirely CPU-owned and re-uploaded every frame: the blend pass has to know
// whether a probe had history BEFORE this frame's trace, and only the CPU knows
// both the schedule and the grid scroll.  That is why this buffer is PER
// frame-in-flight slot — the CPU writes it while the previous frame may still be
// reading.  G4's measured statistics deliberately do NOT live here; see
// GIProbeStats in gi_probe_blend.comp.
struct GIProbeState {
    vec3  offset;   //  0  G4 relocation from the lattice point; zero until then
    uint  flags;    // 12  GIPROBE_FLAG_*
};

#define GIPROBE_FLAG_TRACED  1u // has been traced at least once since it entered the window
#define GIPROBE_FLAG_INSIDE  2u // classified as buried inside a solid brush (G4)
// Classified as sitting in the void OUTSIDE the sealed level hull (G4).  A
// distinct flag from INSIDE because it is a distinct failure and needs its own
// colour in the overlay, but it gets the same treatment: zero weight.
//
// Doom 3 maps are hollow shells, so "not in a room" usually means empty space
// rather than solid, and such a probe's rays MISS — which sets backface 0, so
// the backface statistic reads it as wholesome open air.  It then contributes
// the miss shader's near-black ambient at full weight and drags every surface
// near the map boundary toward black.  Missing is not back-facing; it needs its
// own counter.
#define GIPROBE_FLAG_OUTSIDE 4u

// Either classification means "do not let this probe light anything".
#define GIPROBE_FLAG_UNUSABLE (GIPROBE_FLAG_INSIDE | GIPROBE_FLAG_OUTSIDE)

layout(set = GIPROBE_SET, binding = 3, std430) buffer GIProbeStateBuf {
    GIProbeState probes[];
} probeState;

// ---------------------------------------------------------------------------
// Lattice and storage addressing
// ---------------------------------------------------------------------------

int gip_ProbeCount(void)
{
    return max(gp.gridDim.w, 1);
}

// Linear index within the resident window -> local xyz.
ivec3 gip_LocalFromLinear(int li)
{
    int nx = max(gp.gridDim.x, 1);
    int ny = max(gp.gridDim.y, 1);
    return ivec3(li % nx, (li / nx) % ny, li / (nx * ny));
}

// Positive modulo — GLSL's % keeps the sign of the dividend, and absolute cell
// coordinates go negative all over a Doom 3 map.
ivec3 gip_WrapCell(ivec3 c)
{
    ivec3 n = max(gp.gridDim.xyz, ivec3(1));
    return ((c % n) + n) % n;
}

int gip_StorageIndex(ivec3 absCell)
{
    ivec3 n = max(gp.gridDim.xyz, ivec3(1));
    ivec3 w = gip_WrapCell(absCell);
    return w.x + n.x * (w.y + n.y * w.z);
}

vec3 gip_LatticePos(ivec3 absCell)
{
    return vec3(absCell) * gp.gridOrigin.w;
}

vec3 gip_ProbePos(ivec3 absCell)
{
    return gip_LatticePos(absCell) + probeState.probes[gip_StorageIndex(absCell)].offset;
}

// Same, for callers that already hold the storage index — the resolve does, and
// gip_StorageIndex is three integer modulos it would otherwise redo per corner.
vec3 gip_ProbePosAt(ivec3 absCell, int storageIdx)
{
    return gip_LatticePos(absCell) + probeState.probes[storageIdx].offset;
}

bool gip_CellInGrid(ivec3 absCell)
{
    ivec3 l = absCell - gp.baseCell.xyz;
    return all(greaterThanEqual(l, ivec3(0))) && all(lessThan(l, max(gp.gridDim.xyz, ivec3(1))));
}

// Round-robin schedule (G1).  updateBase comes from a PER-SLOT counter on the
// CPU, never tr.frameCount — a frameCount-derived index advances in lockstep
// with vk.currentFrame and pins each slot to one fixed subset forever, which is
// the bug that cost the GI checkerboard a session.
//
// The trace rgen and the blend pass must agree on this exactly, so it lives
// here rather than being open-coded twice.
ivec3 gip_ScheduledCell(int slot)
{
    int li = (gp.rays.z + slot) % gip_ProbeCount();
    return gp.baseCell.xyz + gip_LocalFromLinear(li);
}

// ---------------------------------------------------------------------------
// Octahedral mapping (Cigolle et al. / DDGI)
// ---------------------------------------------------------------------------

vec2 gip_SignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Unit vector -> [-1,1]^2
vec2 gip_OctEncode(vec3 n)
{
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    n /= max(l1, 1e-8);
    return (n.z < 0.0) ? ((1.0 - abs(n.yx)) * gip_SignNotZero(n.xy)) : n.xy;
}

// [-1,1]^2 -> unit vector
vec3 gip_OctDecode(vec2 e)
{
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * gip_SignNotZero(v.xy);
    return normalize(v);
}

// ---------------------------------------------------------------------------
// Atlas addressing.  `side` is the FULL tile side including the one-texel
// border; the interior is side-2 on each axis.
// ---------------------------------------------------------------------------

ivec2 gip_TileOrigin(int storageIdx, int side)
{
    int tx = max(gp.atlas.x, 1);
    return ivec2(storageIdx % tx, storageIdx / tx) * side;
}

ivec2 gip_AtlasSize(int side)
{
    return ivec2(max(gp.atlas.x, 1) * side, max(gp.atlas.y, 1) * side);
}

// Interior texel (0..side-3) -> absolute atlas texel.
ivec2 gip_InteriorTexel(int storageIdx, ivec2 interior, int side)
{
    return gip_TileOrigin(storageIdx, side) + ivec2(1) + interior;
}

// Direction of the interior texel's CENTRE.
vec3 gip_TexelDirection(ivec2 interior, int side)
{
    float interiorSide = float(max(side - 2, 1));
    vec2 uv = (vec2(interior) + 0.5) / interiorSide;
    return gip_OctDecode(uv * 2.0 - 1.0);
}

// Normalised UV for a bilinear fetch, with the octahedral coordinate already
// computed.  oct == 0 lands on the boundary between the border and the first
// interior texel, which is what makes the border the wrapped neighbour a
// bilinear tap needs.
//
// Split out because the resolve fetches EIGHT probes in the same direction (the
// receiver's normal): encode once, vary only the tile.
vec2 gip_AtlasUVOct(int storageIdx, vec2 oct, int side)
{
    vec2 texel = vec2(gip_TileOrigin(storageIdx, side)) + 1.0 + oct * float(max(side - 2, 1));
    return texel / vec2(gip_AtlasSize(side));
}

vec2 gip_AtlasUV(int storageIdx, vec3 dir, int side)
{
    return gip_AtlasUVOct(storageIdx, gip_OctEncode(normalize(dir)) * 0.5 + 0.5, side);
}

// Border texel -> the interior texel it mirrors.  The octahedron's seam folds
// the map back on itself, so the top row continues from the top row REVERSED,
// and the corners come from the diagonally opposite interior corner.
ivec2 gip_BorderSource(ivec2 t, int side)
{
    int last = side - 1;
    int inHi = side - 2; // last interior texel index in tile coords

    if (t.x == 0 && t.y == 0)
        return ivec2(inHi, inHi);
    if (t.x == last && t.y == 0)
        return ivec2(1, inHi);
    if (t.x == 0 && t.y == last)
        return ivec2(inHi, 1);
    if (t.x == last && t.y == last)
        return ivec2(1, 1);

    if (t.y == 0)
        return ivec2(last - t.x, 1);
    if (t.y == last)
        return ivec2(last - t.x, inHi);
    if (t.x == 0)
        return ivec2(1, last - t.y);
    return ivec2(inHi, last - t.y); // t.x == last
}

// ---------------------------------------------------------------------------
// Chebyshev visibility (G3, DDGI / Majercik et al. 2019)
//
// Upper-bounds the probability that the probe can actually SEE the point it is
// about to light, from the mean and mean-square occluder distance stored in its
// visibility map.  This is the whole of pillar 2 for probe GI: without it a
// probe on the far side of a thin wall interpolates its room's light into a
// sealed one.
//
// BOTH arguments are normalised by r_rtGIProbeMaxRayDist — the moments because
// rg16f cannot hold a raw second moment (see gi_probe_blend.comp), and distNorm
// to match.  They MUST agree: the D^2 in the variance cancels the D^2 in the
// squared difference only if it does, and mismatching them scales the test by
// 512 with no symptom other than "visibility does nothing" or "everything is
// black".
// ---------------------------------------------------------------------------
float gip_Chebyshev(vec2 moments, float distNorm)
{
    float mean = moments.x;
    if (distNorm <= mean)
        return 1.0; // in front of the mean occluder — visible, no bound needed

    float variance = abs(mean * mean - moments.y);
    float d = distNorm - mean;
    float v = variance / (variance + d * d);
    // Cubed: the raw ratio falls off far too gently and leaves a haze of leaked
    // light past every wall.  DDGI cubes it for the same reason.
    return max(v * v * v, 0.0);
}

// ---------------------------------------------------------------------------
// Ray direction sets
// ---------------------------------------------------------------------------

uint gip_hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

float gip_rand(uint seed)
{
    return float(gip_hash(seed)) / 4294967296.0;
}

// Spherical Fibonacci — near-uniform over the whole sphere for any n, and
// deterministic, so the blend pass can regenerate the exact directions the rgen
// traced without storing them.
vec3 gip_SphericalFibonacci(int i, int n)
{
    const float PI2 = 6.28318530717958647692;
    const float INV_PHI = 0.61803398874989484820;

    float phi = PI2 * fract(float(i) * INV_PHI);
    float cosT = 1.0 - (2.0 * float(i) + 1.0) / float(max(n, 1));
    float sinT = sqrt(clamp(1.0 - cosT * cosT, 0.0, 1.0));
    return vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
}

// Per-update random rotation of the whole direction set.  Without it the same
// 128 fixed directions are traced forever and their sampling bias bakes into
// the EMA instead of averaging out.
mat3 gip_RandomRotation(uint seed)
{
    float u1 = gip_rand(seed * 747796405u + 2891336453u);
    float u2 = gip_rand(seed * 2654435761u + 1013904223u);
    float u3 = gip_rand(seed * 1597334677u + 1664525u);

    // Uniform axis on the sphere, uniform angle about it (Rodrigues below).
    float z = u1 * 2.0 - 1.0;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 6.28318530717958647692 * u2;
    vec3 a = vec3(r * cos(phi), r * sin(phi), z);

    float ang = 6.28318530717958647692 * u3;
    float c = cos(ang);
    float s = sin(ang);
    float t = 1.0 - c;

    return mat3(t * a.x * a.x + c,       t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c,       t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}

// ---------------------------------------------------------------------------
// Screen ray, for the resolve and its overlays.
//
// Same near-plane-minus-EYE construction as vf_RayDirForUV: Doom 3's far plane
// is infinite (proj[10] = -0.999), so unprojecting at ndcZ = +1 lands thousands
// of units BEHIND the eye and a near-minus-far direction points backwards.
// Clip Z is GL-convention [-1,1] and the viewport has negative height, hence
// the Y flip — mirror rt_ReconstructWorldPos, do not assume Vulkan [0,1] Z.
// ---------------------------------------------------------------------------
vec3 gip_RayDirForUV(vec2 uv)
{
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    vec4 nearH = gp.invViewProj * vec4(ndc, -1.0, 1.0);
    vec3 pNear = nearH.xyz / nearH.w;
    return normalize(pNear - gp.cameraPosW.xyz);
}

#endif // GI_PROBE_COMMON_GLSL
