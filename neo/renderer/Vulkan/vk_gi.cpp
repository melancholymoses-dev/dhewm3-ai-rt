/*
===========================================================================

dhewm3-rt Vulkan ray tracing - one-bounce global illumination.

Owns the shared half of GI: the per-frame light selection and SSBO upload, the
GIParams UBO, the RT pipeline that gi_probe_trace.rgen and gi_ray.rchit live in,
the receiver-albedo modulation, and the composite onto the framebuffer.  The
sampling itself is world-space probes, in vk_gi_probe.cpp.

  Pipeline layout, mirroring vk_reflections.cpp:
    set=0  per-frame: TLAS, GI storage image, depth, GI params UBO
    set=1  material table (matDescLayout — shared with reflections)
    set=2  probe atlases and params (owned by vk_gi_probe.cpp)

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtGI("r_rtGI", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INTEGER,
              "Enable one-bounce GI (Phase 6.1, ambient colour bleeding)");

idCVar r_rtGIRadius("r_rtGIRadius", "128.0", CVAR_RENDERER | CVAR_FLOAT, "Max GI bounce ray distance in world units");

idCVar r_rtGIStrength("r_rtGIStrength", "0.20", CVAR_RENDERER | CVAR_FLOAT,
                      "Global scale applied to the GI buffer before compositing");

idCVar r_rtGIContrast("r_rtGIContrast", "0.6", CVAR_RENDERER | CVAR_FLOAT,
                      "GI colour contrast boost [0-1]: subtracts minimum channel and rescales to original brightness. "
                      "0 = off, 1 = full effect");

idCVar r_rtGIDirectScale("r_rtGIDirectScale", "1.", CVAR_RENDERER | CVAR_FLOAT,
                         "Multiplier on direct interaction lighting when GI is active. Reduce below 1.0 "
                         "to compensate for GI-added luminance and keep overall brightness consistent "
                         "with the original game. Applied verbatim — it is not coupled to r_rtGIStrength");

static idCVar r_rtGIBounceScale(
    "r_rtGIBounceScale", "2.0", CVAR_RENDERER | CVAR_FLOAT,
    "Multiplier applied to each light's irradiance contribution in the GI bounce");

static idCVar r_rtGIEmissiveScale("r_rtGIEmissiveScale", "2.0", CVAR_RENDERER | CVAR_FLOAT,
                                  "Multiplier on emissive surface (SL_AMBIENT) contribution to GI and reflections");

// Point-light falloff shape shared by every indirect consumer
// — GI, reflections, volumetrics. Direct surface lighting is
// unaffected: the raster interaction pass samples Doom's real falloff images.
static idCVar r_rtGIFalloffMode("r_rtGIFalloffMode", "1", CVAR_RENDERER | CVAR_INTEGER,
                                "Point-light falloff for RT indirect lighting: 0 = legacy (flat to 80%% of the "
                                "box, then a knee), 1 = (1-t)^2 raster-like, 2 = (1-t^2)^2 smooth");

// 1.0 is Doom's own light volume;
// Also sizes the sphere pre-cull, which must not clip the falloff.
static idCVar r_rtGIFalloffReach("r_rtGIFalloffReach", "1.0", CVAR_RENDERER | CVAR_FLOAT,
                                 "How far past the light's box a point light reaches, in box-normalised units. "
                                 "1.0 = stop at the box face (vanilla containment), 1.5 = initial RT throw");

static idCVar r_rtGIMaxBounceLights(
    "r_rtGIMaxBounceLights", "16", CVAR_RENDERER | CVAR_INTEGER,
    "Max uploaded lights evaluated by GI bounce shading (GI pass only; reflections unaffected)");

static idCVar r_rtGIStochasticLights("r_rtGIStochasticLights", "2", CVAR_RENDERER | CVAR_INTEGER,
                                     "P3: shadow rays fired per GI bounce hit (1-2). Lights are importance-sampled by "
                                     "unshadowed contribution and divided by their selection probability (unbiased). "
                                     "0 = legacy path, one shadow ray for every light up to r_rtGIMaxBounceLights");

static idCVar r_rtGIMaxLights("r_rtGIMaxLights", "128", CVAR_RENDERER | CVAR_INTEGER,
                              "Max GI lights to sample per frame (1-128, capped at VK_GI_MAX_LIGHTS)");

static idCVar r_rtGILightStratify(
    "r_rtGILightStratify", "1", CVAR_RENDERER | CVAR_BOOL,
    "L1: stratified GI light selection — distance tiers with per-tier importance sort, quotas and "
    "hysteresis, uploaded importance-first. 0 = legacy nearest-to-camera sort (A/B comparison)");

static idCVar r_rtGILightCollectRadiusScale(
    "r_rtGILightCollectRadiusScale", "4.0", CVAR_RENDERER | CVAR_FLOAT,
    "Camera-space GI light collection radius scale. Effective collect radius = r_rtGIRadius * scale. "
    "DEMOTED (portal_area_lights.md): only used by the legacy sphere collector, i.e. when "
    "r_rtGIAreaLights is 0 or the camera is outside the world (areaNum == -1)");

static idCVar r_rtGIAreaLights(
    "r_rtGIAreaLights", "1", CVAR_RENDERER | CVAR_BOOL,
    "portal_area_lights.md Stage 1: gather GI/vol candidate lights by walking the portal-area graph "
    "from the camera's area (respects walls and closed doors) instead of a camera-centred sphere. "
    "0 = legacy sphere collector (A/B comparison). The sphere path is also the automatic fallback "
    "when the camera is outside the world (viewDef areaNum == -1)");

static idCVar r_rtGIAreaHops("r_rtGIAreaHops", "2", CVAR_RENDERER | CVAR_INTEGER,
                             "portal_area_lights.md: BFS portal-hop depth from the camera's area. "
                             "2 = current room, neighbours, neighbours-of-neighbours");

static idCVar r_rtGIAreaBoundsEpsilon(
    "r_rtGIAreaBoundsEpsilon", "24", CVAR_RENDERER | CVAR_FLOAT,
    "portal_area_lights.md Stage 1.75: BFS seeds from every portal area overlapping a "
    "+/-N unit box around the camera (world->BoundsInAreas), not just the single "
    "PointInArea(camera) classification. Fixes hop-count popping when straddling an area "
    "boundary plane (e.g. standing in a doorway/threshold) — a hard point classification "
    "flips the ENTIRE hop numbering for every light on a sub-unit step. 0 = old single-area seed");

static idCVar r_rtGIMaxLightDist(
    "r_rtGIMaxLightDist", "2048", CVAR_RENDERER | CVAR_FLOAT,
    "portal_area_lights.md: absolute light distance safety cap within BFS-visited areas — guards "
    "pathological mega-areas where a single area spans the whole map. Area-walk path only");

static idCVar r_rtGIWhiteWeight(
    "r_rtGIWhiteWeight", "0.25", CVAR_RENDERER | CVAR_FLOAT,
    "auto_relight.md §0: importance multiplier floor for fully desaturated (white) lights "
    "in GI/vol light selection [0-1]. Saturated lights always score ×1.0; white lights score "
    "×this. Lower = colored accents beat white fills more strongly for the limited light slots. "
    "1.0 = no saturation weighting (legacy)");

// flicker factorization.
// A flickering light is a scalar s(t) on an otherwise static light, and the CPU knows it exactly
// every frame, so its bounce transport can be cached NORMALISED (at s = 1) and
// rescaled at resolve time instead of being low-passed away by the probe EMA.
//
// Clamped to 0..1(!)
static idCVar r_rtGIProbeFastBuckets("r_rtGIProbeFastBuckets", "1", CVAR_RENDERER | CVAR_INTEGER,
                                     "G5b: cache flickering lights' probe GI in a separate bucket, normalised by their "
                                     "own flicker gain, and rescale it at resolve time. 1 = on, 0 = old behaviour ");

static idCVar r_rtGIFlickerThreshold("r_rtGIFlickerThreshold", "0.15", CVAR_RENDERER | CVAR_FLOAT,
                                     "G5b: relative per-frame luminance change, against the light's own running "
                                     "peak, that classifies it as fast. Lower = more lights get factorized");

static idCVar r_rtGIFlickerHold("r_rtGIFlickerHold", "2.0", CVAR_RENDERER | CVAR_FLOAT,
                                "G5b: time constant over which a fast light's peak colour L-hat decays toward "
                                "its current one, seconds.");

static idCVar r_rtGIAlbedo("r_rtGIAlbedo", "1", CVAR_RENDERER | CVAR_BOOL,
                           "multiply denoised GI by the receiving surface's albedo "
                           "(gbufAlbedo G-buffer target) before composite. 0 = legacy raw-radiance "
                           "add (A/B) — dark/black materials get the same GI wash as white ones. "
                           "No-op when vk.gbufferSupported is false");

// ---------------------------------------------------------------------------
// GI light SSBO — mirrors GILightBuffer in gi_ray.rchit GLSL
// ---------------------------------------------------------------------------

#define VK_GI_MAX_LIGHTS 128

// rt_projected_light_cookies.md Stage 1 — GILightEntry::flags bit for "this
// light has a cookie entry at the same index in GILightBuffer::cookies[]".
#define GI_LIGHT_FLAG_HAS_COOKIE 0x1u

// G5b — this light is classified as flickering, so colorIntensity.rgb holds
// Lhat (its running PEAK colour, not its current one) and fastScale holds
// s = current/peak. EVERY consumer must multiply the two back together to get
// the light as it is right now. Volumetrics and reflections read this flag and
// nothing else.
#define GI_LIGHT_FLAG_FAST 0x2u

// ...and this light's probe transport is the one cached in fast bucket 0, at
// s = 1, for the resolve to rescale.
//
// A separate bit because the two statements are not the same, and conflating
// them breaks the volumetrics: one gain is shared by a whole bucket, so the
// bucket can hold at most ONE light before the gain stops being exact for any
// of them (two flickering lights in different rooms would each get the mean —
// the lit room's bounce halved and the dark room's doubled, which is the
// pillar-2 failure this chunk exists to remove). Only the highest-importance
// flickering light is bucketed; the rest keep FAST — so their shafts and
// reflections still flicker correctly — and their probe GI falls back to
// today's EMA-smeared behaviour, which is no worse than before G5b.
//
// Lifting this to several lights needs a per-PROBE gain, not a per-bucket one.
// r_rtGIProbeDebug 8's red channel is the measurement that says whether it is
// worth the extra float in GIProbeStateEntry.
#define GI_LIGHT_FLAG_FAST_BUCKET 0x4u

// Stage 6 (20261013_reflection_volumetrics.md) — this light's cookie entry carries a
// valid planeF/falloffIndex, so the axial attenuation comes from the light's
// lightFalloffImage the way GL's interaction pass does it.
//
// Independent of HAS_COOKIE on purpose: a projected light whose every stage condition
// failed has no projection image but still has a falloff image, and that is the term
// that decides how its pool reads with distance.
#define GI_LIGHT_FLAG_HAS_FALLOFF 0x8u

struct GILightEntry
{
    float posRadius[4];      // xyz = world pos, w = sphere pre-cull radius
    float colorIntensity[4]; // rgb = light colour (Lhat when FAST), a = intensity
    float coneDir[4];        // projected: xyz=normalised dir, w=cos(halfAngle); zeroed for point
    float boxExtents[4];     // point: xyz=AABB half-extents, w=0
                             // projected: w=max reach along cone axis; xyz=0
    uint32_t lightType;      // 0 = point, 1 = projected/spot
    uint32_t flags;          // GI_LIGHT_FLAG_* bitmask
    // G5b, in what used to be two words of alignment pad — no size change.
    float fastScale;     // s = current/peak luminance, 1.0 for a steady light
    uint32_t fastBucket; // which fast bucket this light's transport is cached in
    float emitPos[4];    // xyz = globalLightOrigin, w unused — pads to 96 bytes
};

// rt_projected_light_cookies.md Stage 1 — one entry per GILightEntry slot,
// same index, meaningful only when that entry's flags has HAS_COOKIE set.
// planeS/planeT already have the stage's texture matrix folded in — sampling
// is just two dot products plus a divide, no matrix multiply at hit time.
struct GILightCookie
{
    float planeS[4];
    float planeT[4];
    float planeQ[4];
    float planeF[4];       // lightProject[3], world space: S = dot(vec4(pos,1), planeF)
    uint32_t imageIndex;   // bindless slot, vk_material_table.cpp
    uint32_t falloffIndex; // ditto, for the light's lightFalloffImage
    uint32_t pad[2];
};
static_assert(sizeof(GILightCookie) == 80, "GILightCookie size mismatch");

struct GILightBuffer
{
    int32_t numLights;
    float bounceScale;   // r_rtGIBounceScale — per-light irradiance multiplier
    float giRadius;      // r_rtGIRadius — hit-point light evaluation window (shader-side)
    float emissiveScale; // r_rtGIEmissiveScale — emissive surface contribution multiplier
    int32_t falloffMode; // r_rtGIFalloffMode — T4b, consumed by rt_LightBoxAtten
    float falloffReach;  // r_rtGIFalloffReach — also sizes posRadius[3]'s sphere pre-cull
    float pad[2];        // pads header to 32 bytes (RTLight's std430 base alignment is 16)
    GILightEntry lights[VK_GI_MAX_LIGHTS];
    // mirrored in GLSL as RTLightBuf::cookies[RT_LIGHT_MAX_LIGHTS]
    // (rt_light_eval.glsl), 1:1 with lights[] by index.
    GILightCookie cookies[VK_GI_MAX_LIGHTS];
};
static_assert(sizeof(GILightBuffer) == 32 + VK_GI_MAX_LIGHTS * 96 + VK_GI_MAX_LIGHTS * 80,
              "GILightBuffer size mismatch");

uint32_t VK_RT_GetGILightBufferSize(void)
{
    return (uint32_t)sizeof(GILightBuffer);
}

// ---------------------------------------------------------------------------
// G5b — flicker classifier
//
// A flickering Doom 3 light is a scalar on an otherwise static light: its
// position, volume, cone and cookie are all constant and only its RGB registers
// move. That factors:
//
//     E_probe = SUM_stable L_j·G_j  +  s(t) · SUM_fast Lhat_k·G_k
//
// where G is geometric transport and Lhat is the light's PEAK colour. The right
// term is cached at s = 1, which makes it time-invariant and therefore immune to
// the probe EMA's ~9 s time constant; the resolve multiplies the current s(t)
// back in. Nothing here needs extra rays.
//
// Two things the verdict must get right or the mechanism backfires:
//
//   - A MOVING light changes G, not s. Factorizing it would cache its transport
//     at the old position and then confidently rescale that stale glow forever,
//     which is worse than the lag it replaces. Any origin/axis change drops the
//     light back to the stable bucket and resets its history.
//   - The classification is STICKY (r_rtGIFlickerHold). Crossing between buckets
//     costs a full EMA crossfade at both ends, so a classifier that flaps at the
//     threshold is a visible pulse generator.
//
// Lhat is derived from a decaying running peak rather than parsed out of the light
// material, so `flicker`/`pdflicker` tables, hand-written expressions and script
// setShaderParm all work with no material knowledge at all.
// ---------------------------------------------------------------------------

#define VK_GI_FLICKER_MAX_LIGHT_IDX 4096

// Clamp on r_rtGIProbeFastBuckets. Each bucket costs the resolve 8 more
// scattered bilinear taps and the irradiance atlas another probeCount tiles, so
// K grows only on evidence from r_rtGIProbeDebug 8.
#define VK_GI_MAX_FAST_BUCKETS 1

struct GIFlickerState
{
    // Lhat itself, and nothing else: the peak LUMINANCE is derived from this
    // vector, never stored beside it. Two fields drifting apart is exactly how
    // Lhat * s stops equalling the light's current colour — see the decay below.
    idVec3 peakColor;
    float lastLum;    // previous sighting's luminance, for the delta test
    float minScale;   // lowest s seen recently; 1 - this is the flicker DEPTH
    uint32_t shapeId; // hash of every transport-affecting field, see below
    int lastFrame;    // tr.frameCount of the last sighting
    int lastMs;       // renderView.time of the last sighting, for the peak decay
    bool isFast;      // has this light ever been seen to change? see below
};

static GIFlickerState s_flicker[VK_GI_FLICKER_MAX_LIGHT_IDX];

// Hash of everything that changes a light's geometric transport G, so that only
// its amplitude s is left varying — which is the entire premise of factorizing
// it. The field list mirrors idRenderWorldLocal::UpdateLightDef's own
// "shape stays the same" test (RenderWorld.cpp:464), minus the parts that
// cannot affect G, plus the light shader: a shader swap changes the cookie, and
// the cookie is part of the transport.
static uint32_t VK_RT_LightShapeId(const renderLight_t &p, const idMaterial *shader)
{
    uint32_t h = 2166136261u; // FNV-1a
    const auto mix = [&h](const void *data, size_t bytes) {
        const uint8_t *b = (const uint8_t *)data;
        for (size_t i = 0; i < bytes; i++)
        {
            h ^= (uint32_t)b[i];
            h *= 16777619u;
        }
    };

    mix(&p.axis, sizeof(p.axis));
    mix(&p.origin, sizeof(p.origin));
    mix(&p.lightCenter, sizeof(p.lightCenter));
    mix(&p.lightRadius, sizeof(p.lightRadius));
    mix(&p.target, sizeof(p.target));
    mix(&p.right, sizeof(p.right));
    mix(&p.up, sizeof(p.up));
    mix(&p.start, sizeof(p.start));
    mix(&p.end, sizeof(p.end));
    mix(&p.pointLight, sizeof(p.pointLight));
    mix(&p.parallel, sizeof(p.parallel));
    mix(&shader, sizeof(shader));
    return h;
}

struct GIFlickerVerdict
{
    idVec3 lhat;  // colour to upload (current colour unless fast)
    float scale;  // s = current/peak luminance; 1.0 unless fast
    float depth;  // 1 - lowest s seen recently: how DEEPLY this light flickers
    bool fast;    // classified flickering, and factorizable
    bool tracked; // false when the light index is past the tracking table
};

static float VK_RT_LightLuminance(const idVec3 &c)
{
    return Max(0.0f, 0.299f * c.x + 0.587f * c.y + 0.114f * c.z);
}

// nowMs is renderView.time — the game's ms clock, so the hold window and the
// peak decay both survive a frame-rate change.
static void VK_RT_ClassifyFlicker(int li, const renderLight_t &p, const idMaterial *shader, const idVec3 &color,
                                  int nowMs, GIFlickerVerdict &out)
{
    out.lhat = color;
    out.scale = 1.0f;
    out.depth = 0.0f;
    out.fast = false;
    out.tracked = false;

    if (r_rtGIProbeFastBuckets.GetInteger() <= 0)
        return;
    if (li < 0 || li >= VK_GI_FLICKER_MAX_LIGHT_IDX)
        return; // past the table — treated as steady, which is the safe direction

    out.tracked = true;

    GIFlickerState &st = s_flicker[li];
    const float lum = VK_RT_LightLuminance(color);
    const float holdSec = Max(0.05f, r_rtGIFlickerHold.GetFloat());
    const uint32_t shapeId = VK_RT_LightShapeId(p, shader);

    // Two ways the history stops describing this light at all. Both throw the
    // verdict away, because both mean the cached transport is for a different
    // light or a different geometry:
    //   reshaped — origin, volume, emitter offset or cookie changed, so G moved
    //              and rescaling the cache would confidently smear the old one.
    //              This is what keeps moving lights out of the fast bucket.
    //   rewound  — renderView.time went backwards (level load, demo seek, save
    //              restore). Also the only thing that catches a lightDef INDEX
    //              being reused by a new map, since tr.frameCount does not reset
    //              across a load.
    const bool reshaped = (shapeId != st.shapeId);
    const bool rewound = (nowMs < st.lastMs);

    if (reshaped || rewound)
    {
        st.peakColor = color;
        st.lastLum = lum;
        st.minScale = 1.0f;
        st.shapeId = shapeId;
        st.lastFrame = tr.frameCount;
        st.lastMs = nowMs;
        st.isFast = false; // a light that just moved is never fast, however it flickers
        return;
    }

    // A gap in sightings is different: the light left the gather set and came
    // back, unchanged, and we simply were not looking. Its peak and its verdict
    // are still good — only lastLum is from an arbitrary time ago, so the delta
    // measured against it would be an artifact of the gap rather than a flicker.
    // Refresh the timestamps and skip one delta test.
    //
    // Dropping the verdict here instead would make walking out of a room and
    // back cost a full re-earn: one flicker cycle to reclassify plus the ~9 s
    // crossfade between buckets, every time.
    if ((tr.frameCount - st.lastFrame) > 2)
    {
        st.lastLum = lum;
        st.lastFrame = tr.frameCount;
        st.lastMs = nowMs;
        if (!st.isFast)
            st.peakColor = color;
    }

    // Exponential decay of the peak over the hold window, so Lhat tracks a light
    // that permanently dims instead of pinning at a level it will never reach
    // again. A steady light's peak converges to its own value.
    //
    // The decay is applied to the COLOUR, not to a luminance stored beside it.
    const float dtSec = idMath::ClampFloat(0.0f, holdSec, (float)(nowMs - st.lastMs) * 0.001f);
    const idVec3 decayed = st.peakColor * idMath::Exp(-dtSec / holdSec);
    st.peakColor = (lum >= VK_RT_LightLuminance(decayed)) ? color : decayed;
    const float peakLum = VK_RT_LightLuminance(st.peakColor);

    // Relative change measured against the PEAK, not against the previous
    // sample: a light spending most of its cycle near zero would otherwise show
    // enormous relative deltas on tiny absolute ones.
    //
    // The verdict LATCHES, and never expires while the light keeps its shape.
    // Latching costs nothing, because a light
    // sitting in the fast bucket at s = 1 is treated exactly as the stable
    // bucket would treat it. r_rtGIFlickerHold now governs only the peak decay
    // above, which is what returns s to 1 for a light that settles.
    const float threshold = Max(0.0f, r_rtGIFlickerThreshold.GetFloat());
    if (peakLum > 1e-4f && idMath::Fabs(lum - st.lastLum) / peakLum > threshold)
        st.isFast = true;

    st.lastLum = lum;
    st.shapeId = shapeId;
    st.lastFrame = tr.frameCount;
    st.lastMs = nowMs;

    if (!st.isFast || peakLum <= 1e-4f)
        return; // steady — or too dark for the ratio below to mean anything

    const float scale = idMath::ClampFloat(0.0f, 1.0f, lum / peakLum);

    // Flicker DEPTH: how far this light actually drops, not merely that it
    // moves.
    // Needed because the fast bucket holds exactly one light and IMPORTANCE
    // alone picks the wrong one: (bright light flickering a bit vs light turning on/off rapidly)
    st.minScale = Min(scale, st.minScale + dtSec / holdSec);
    st.minScale = idMath::ClampFloat(0.0f, 1.0f, st.minScale);

    out.fast = true;
    out.lhat = st.peakColor;
    out.scale = scale;
    out.depth = 1.0f - st.minScale;
}

static void VK_RT_ResetFlickerState(void)
{
    memset(s_flicker, 0, sizeof(s_flicker));
}

// Per-bucket gain handed to gi_probe_resolve.comp through GIProbeParams. One
// gain is shared by every fast light in the bucket, so at K=1 it is the
// Lhat-luminance-weighted mean of s: exact when a single fast light reaches a
// probe, and a smooth degradation when two flicker out of phase within reach of
// the same one. r_rtGIProbeDebug 8 counts how often that actually happens.
//
// Zero — not one — when no fast light is uploaded: the fast atlas still holds
// decaying history from the last one, and a gain of 1 would republish it.
static float s_giFastGain[VK_GI_MAX_FAST_BUCKETS] = {0.0f};
static int s_giFastLightCount = 0;

int VK_RT_GIFastBucketCount(void)
{
    return idMath::ClampInt(0, VK_GI_MAX_FAST_BUCKETS, r_rtGIProbeFastBuckets.GetInteger());
}

float VK_RT_GIFastBucketGain(int bucket)
{
    if (bucket < 0 || bucket >= VK_GI_MAX_FAST_BUCKETS)
        return 0.0f;
    return s_giFastGain[bucket];
}

int VK_RT_GIFastLightCount(void)
{
    return s_giFastLightCount;
}

// ---------------------------------------------------------------------------
// GI UBO layout matching gi_ray.rchit's GIParams block (std140)
//
//   mat4  invViewProj    offset   0  size 64
//   float giRadius       offset  64  size  4
//   uint  frameIndex     offset  68  size  4
//   float giStrength     offset  72  size  4
//   (4 bytes std140 padding — ivec2 has align 8)
//   ivec2 screenSize     offset  80  size  8
//   ivec2 scissorOffset  offset  88  size  8
//   ivec2 scissorExtent  offset  96  size  8
//   int   maxBounceLights   offset 104  size  4
//   float giContrast        offset 108  size  4
//   int   stochasticLights  offset 112  size  4
//   int   useGbufNormal     offset 116  size  4
//   float playerExcludeRadius offset 120 size 4
//   int   playerClassValid    offset 124 size 4
//   total: 128 bytes
//
// gi_ray.rchit declares a matching block (it reads maxBounceLights, frameIndex
// and stochasticLights), so any field added here must be mirrored there — a
// prefix mismatch silently shifts every offset after it.  Appending at the end
// is safe: rchit stops at stochasticLights, and a shorter trailing block leaves
// earlier offsets intact.  The padding at 76 is explicit on this side only;
// GLSL's std140 inserts it on its own.
// ---------------------------------------------------------------------------

struct GIParamsUBO
{
    float invViewProj[16]; // column-major 4x4
    float giRadius;
    uint32_t frameIndex;
    float giStrength;
    float _pad0; // std140 alignment for the ivec2s below
    int32_t screenWidth;
    int32_t screenHeight;
    int32_t scissorOffsetX;
    int32_t scissorOffsetY;
    int32_t scissorExtentX;
    int32_t scissorExtentY;
    int32_t maxBounceLights;
    float giContrast;
    int32_t stochasticLights;
    int32_t useGbufNormal;
    float playerExcludeRadius;
    int32_t playerClassValid;
};
static_assert(sizeof(GIParamsUBO) == 128, "GIParamsUBO size mismatch");

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);
// P9: shared 1x1 G-buffer fallback owned by vk_reflections.cpp (alpha 0 = "no data").
extern VkImageView VK_RT_GetNullGbufNormalView(void);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;
extern idCVar r_rtGbufNormals;                // P9 — defined in vk_gbuffer.cpp
extern float VK_RT_PlayerExcludeRadius(void); // vk_shadows.cpp — 0 unless the fp body is drawn
extern idCVar r_rtVolMaxLights;               // defined in vk_vol.cpp — cap for the vol-only selection built below
float g_vkVolMaxLightReach = 0.0f;            // consumed by VK_RT_Pass2CullRadius (vk_accelstruct.cpp)
extern idCVar r_rtVolMaxDist;                 // defined in vk_vol.cpp — vol march reach, used to filter that selection

// ---------------------------------------------------------------------------
// VK_RT_CreateGILightSsbos / VK_RT_DestroyGILightSsbos
// Per-frame host-visible SSBO for GI light data.
// Size is fixed (VK_GI_MAX_LIGHTS entries) — allocated once, filled every frame.
// ---------------------------------------------------------------------------

static void VK_RT_CreateGILightSsbos(void)
{
    const VkDeviceSize bufSize = sizeof(GILightBuffer);
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_CreateBuffer(bufSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &vkRT.giLightSsbo[i], &vkRT.giLightSsboMemory[i]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.giLightSsboMemory[i], 0, bufSize, 0, &vkRT.giLightSsboMapped[i]));
        // Zero out so numLights=0 by default
        memset(vkRT.giLightSsboMapped[i], 0, bufSize);

        // Dedicated vol selection buffer — same layout/size, filled separately below.
        VK_CreateBuffer(bufSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &vkRT.volLightSsbo[i], &vkRT.volLightSsboMemory[i]);
        VK_CHECK(vkMapMemory(vk.device, vkRT.volLightSsboMemory[i], 0, bufSize, 0, &vkRT.volLightSsboMapped[i]));
        memset(vkRT.volLightSsboMapped[i], 0, bufSize);
    }
}

static void VK_RT_DestroyGILightSsbos(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vkRT.giLightSsboMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.giLightSsboMemory[i]);
            vkRT.giLightSsboMapped[i] = NULL;
        }
        if (vkRT.giLightSsbo[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.giLightSsbo[i], NULL);
            vkRT.giLightSsbo[i] = VK_NULL_HANDLE;
        }
        if (vkRT.giLightSsboMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.giLightSsboMemory[i], NULL);
            vkRT.giLightSsboMemory[i] = VK_NULL_HANDLE;
        }

        if (vkRT.volLightSsboMapped[i] != NULL)
        {
            vkUnmapMemory(vk.device, vkRT.volLightSsboMemory[i]);
            vkRT.volLightSsboMapped[i] = NULL;
        }
        if (vkRT.volLightSsbo[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, vkRT.volLightSsbo[i], NULL);
            vkRT.volLightSsbo[i] = VK_NULL_HANDLE;
        }
        if (vkRT.volLightSsboMemory[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, vkRT.volLightSsboMemory[i], NULL);
            vkRT.volLightSsboMemory[i] = VK_NULL_HANDLE;
        }
    }
}

// ---------------------------------------------------------------------------
// VK_RT_CreateGIImages
// Allocates per-frame RGBA16F GI buffers at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateGIImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &gb = vkRT.giBuffer[i];
        gb.width = width;
        gb.height = height;

        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &gb.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, gb.image, &memReq);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice, &memProps);
        uint32_t memTypeIdx = UINT32_MAX;
        for (uint32_t m = 0; m < memProps.memoryTypeCount; m++)
        {
            if ((memReq.memoryTypeBits & (1u << m)) &&
                (memProps.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                memTypeIdx = m;
                break;
            }
        }
        if (memTypeIdx == UINT32_MAX)
        {
            common->Error("VK RT GI: no device-local memory type for GI image");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &gb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, gb.image, gb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = gb.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &gb.view));

        // Transition UNDEFINED → GENERAL so rgen can imageStore on first dispatch.
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo cbAlloc = {};
            cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAlloc.commandPool = vk.commandPool;
            cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAlloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmpCmd, &beginInfo);

            VkImageSubresourceRange subRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            // Transition UNDEFINED → GENERAL (required before clear and imageStore).
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = gb.image;
            barrier.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            // Clear to black so unwritten pixels don't contain garbage.
            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, gb.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

            // Second barrier: transfer write → shader write for rgen imageStore.
            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.image = gb.image;
            barrier2.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                 0, 0, NULL, 0, NULL, 1, &barrier2);

            vkEndCommandBuffer(tmpCmd);

            VkFenceCreateInfo fenceCI = {};
            fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &tmpCmd;
            vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, fence);
            VK_Prof_WaitForFences(VKPROF_WAIT_GI, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk.device, fence, NULL);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }
}

static void VK_RT_DestroyGIImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &gb = vkRT.giBuffer[i];
        if (gb.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, gb.view, NULL);
            gb.view = VK_NULL_HANDLE;
        }
        if (gb.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, gb.image, NULL);
            gb.image = VK_NULL_HANDLE;
        }
        if (gb.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, gb.memory, NULL);
            gb.memory = VK_NULL_HANDLE;
        }
        gb.width = 0;
        gb.height = 0;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitGIPipeline
// Creates the GI RT pipeline, SBT, descriptor sets, and sampler.
//
// Shader groups:
//   Group 0: miss   (gi_ray.rmiss)
//   Group 1: hit    (gi_ray.rchit + gi_ray.rahit for alpha-discard)
//   Group 2: shadow miss (gi_shadow.rmiss)
//   Group 3: rgen   (gi_probe_trace.rgen)
//
// The probe raygen is the pipeline's ONLY raygen, and Vulkan requires at least
// one, so a missing gi_probe_trace.rgen.spv now disables GI outright rather than
// falling back — the per-pixel raygen it used to share this pipeline with is gone.
// ---------------------------------------------------------------------------

static void VK_RT_InitGIPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: TLAS
    // binding 1: GI RGBA16F storage image
    // binding 2: depth sampler (COMBINED_IMAGE_SAMPLER)
    // binding 3: GI params UBO (dynamic)

    // binding 0: TLAS — rgen fires primary GI rays; rchit fires inline shadow rays
    // binding 1: GI RGBA16F storage image (rgen write)
    // binding 2: depth sampler (rgen read)
    // binding 3: GI params UBO (rgen + rchit read, dynamic)
    // binding 4: GI light list SSBO (rchit read — bounce light evaluation)
    // binding 5: G-buffer normal/F0 sampler (P9 — rgen, replaces rt_ReconstructNormal)
    // binding 6: gbufAlbedo sampler — alpha is the surface class (rgen, player exclusion)
    VkDescriptorSetLayoutBinding bindings[7] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[3].descriptorCount = 1;
    // CLOSEST_HIT is required, not optional: gi_ray.rchit reads this block
    // (maxBounceLights, stochasticLights, frameIndex).  Accessing a descriptor from
    // a stage missing from stageFlags is undefined behaviour — it reads garbage or
    // faults depending on driver.
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 6: gbufAlbedo — alpha is the surface class (view-local or not).
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giDescLayout));

    // --- Pipeline layout ---
    // set=0: per-frame resources (TLAS, GI image, depth, UBO)
    // set=1: material table (shared with reflections/shadows)
    // set=2: probe resources — gi_probe_trace.rgen's atlases, scratch, params and
    //        state. Mandatory now that the probe raygen is this pipeline's only
    //        raygen. Owned by vk_gi_probe.cpp; VK_RT_InitGIProbeLayout runs first.
    VkDescriptorSetLayout giLayouts[3] = {vkRT.giDescLayout, vkRT.matDescLayout, VK_RT_GIProbeDescLayout()};
    const bool haveProbeSet = (giLayouts[2] != VK_NULL_HANDLE);
    if (!haveProbeSet)
    {
        common->Warning("VK RT GI: probe descriptor layout missing — GI disabled");
        return;
    }
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 3;
    plInfo.pSetLayouts = giLayouts;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rmiss.spv");
    VkShaderModule rchitModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rchit.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/gi_ray.rahit.spv");
    VkShaderModule shadowMissModule = VK_LoadSPIRV("glprogs/glsl/gi_shadow.rmiss.spv");
    // The probe raygen, and now the only one. No longer optional: without it the
    // pipeline has no raygen stage at all, which is invalid.
    VkShaderModule probeRgenModule = VK_LoadSPIRV("glprogs/glsl/gi_probe_trace.rgen.spv");

    if (rmissModule == VK_NULL_HANDLE || rchitModule == VK_NULL_HANDLE || rahitModule == VK_NULL_HANDLE ||
        shadowMissModule == VK_NULL_HANDLE || probeRgenModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI: failed to load GI shader modules — GI disabled");
        if (rmissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rmissModule, NULL);
        if (rchitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rchitModule, NULL);
        if (rahitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rahitModule, NULL);
        if (shadowMissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, shadowMissModule, NULL);
        if (probeRgenModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, probeRgenModule, NULL);
        return;
    }

    // --- Shader stages ---
    // Stage 0: gi miss (sky ambient)
    // Stage 1: rchit (albedo + light eval + shadow trace)
    // Stage 2: rahit (alpha-discard for GI rays)
    // Stage 3: shadow miss (clears occluded flag)
    // Stage 4: probe rgen
    VkPipelineShaderStageCreateInfo stages[5] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[0].module = rmissModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[1].module = rchitModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[2].module = rahitModule;
    stages[2].pName = "main";

    stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[3].module = shadowMissModule;
    stages[3].pName = "main";

    stages[4].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[4].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[4].module = probeRgenModule;
    stages[4].pName = "main";

    // --- Shader groups ---
    // Group 0: gi miss (missIndex=0 in traceRayEXT)
    // Group 1: hit (rchit + rahit)
    // Group 2: shadow miss (missIndex=1 in shadow traceRayEXT calls from rchit)
    // Group 3: probe rgen — vkCmdTraceRaysKHR points at giProbeRgenRegion.
    VkRayTracingShaderGroupCreateInfoKHR groups[4] = {};

    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0; // gi miss
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[1].generalShader = VK_SHADER_UNUSED_KHR;
    groups[1].closestHitShader = 1; // gi_ray.rchit
    groups[1].anyHitShader = 2;     // gi_ray.rahit — alpha-discard
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 3; // shadow miss
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 4; // gi_probe_trace.rgen
    groups[3].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

    const uint32_t stageCount = 5u;
    const uint32_t groupCount = 4u;

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount = stageCount;
    rtPipeInfo.pStages = stages;
    rtPipeInfo.groupCount = groupCount;
    rtPipeInfo.pGroups = groups;
    rtPipeInfo.maxPipelineRayRecursionDepth = 2; // primary GI ray + inline shadow ray
    rtPipeInfo.layout = vkRT.giPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.giPipeline));

    vkDestroyShaderModule(vk.device, rmissModule, NULL);
    vkDestroyShaderModule(vk.device, rchitModule, NULL);
    vkDestroyShaderModule(vk.device, rahitModule, NULL);
    vkDestroyShaderModule(vk.device, shadowMissModule, NULL);
    vkDestroyShaderModule(vk.device, probeRgenModule, NULL);

    // --- Shader Binding Table ---
    // SBT layout:
    //   slot 0 (rgen region)     : rgen
    //   slot 1 (miss region[0])  : gi miss  (missIndex=0 — GI rays)
    //   slot 2 (miss region[1])  : shadow miss (missIndex=1 — shadow rays from rchit)
    //   slot 3 (hit region[0])   : hit group (rchit + rahit)
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(vk.physicalDevice, &props2);

    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    const uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;

    auto alignUp = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
    uint32_t handleSizeAligned = alignUp(handleSize, handleAlignment);
    uint32_t stride = alignUp(handleSizeAligned, baseAlignment);

    // 4 groups, 6 SBT records:
    //   miss:  records 0-1 (groups 0, 2)  — gi miss + shadow miss
    //   hit:   records 2-4 (group 1, replicated)
    //   rgen:  record  5   (group 3, probe raygen)
    //
    // The hit region is 3 records wide, not 1, because TLAS instances carry a
    // hardcoded instanceShaderBindingTableRecordOffset of 0 or 2 (2 =
    // rtPlayer body/held weapon, see vk_accelstruct.cpp). That offset is
    // per-instance and shared by every pipeline that traces this TLAS, so the hit
    // region must contain record 2 or an out-of-range fetch pulls a garbage
    // shader handle. Record 1 of the region is unused padding — nothing traces
    // with sbtRecordOffset 1 here — but it must exist for record 2 to be
    // addressable.
    const uint32_t sbtRecords = 6u;
    uint32_t sbtSize = sbtRecords * stride;

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vkRT.sbtGIBuffer,
                    &vkRT.sbtGIMemory);

    // Fetch every group handle in pipeline order:
    //   [gi-miss, hit, shadow-miss, probe-rgen]
    uint8_t *handles = (uint8_t *)alloca(groupCount * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.giPipeline, 0, groupCount, groupCount * handleSize,
                                                  handles));

    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtGIMemory, 0, sbtSize, 0, (void **)&sbtData));
    // SBT slot 0 = gi miss (group 0)
    memcpy(sbtData + 0 * stride, handles + 0 * handleSize, handleSize);
    // SBT slot 1 = shadow miss (group 2)
    memcpy(sbtData + 1 * stride, handles + 2 * handleSize, handleSize);
    // SBT slots 2..4 = hit group (group 1), replicated so instance record
    // offsets 0..2 all resolve to the same (only) hit group.
    for (int i = 2; i < 5; i++)
        memcpy(sbtData + i * stride, handles + 1 * handleSize, handleSize);
    // SBT slot 5 = probe rgen (group 3)
    memcpy(sbtData + 5 * stride, handles + 3 * handleSize, handleSize);
    vkUnmapMemory(vk.device, vkRT.sbtGIMemory);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtGIBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    // miss:  slots 0–1 (stride * 2 region, covers gi-miss[0] and shadow-miss[1])
    // hit:   slots 2–4 (stride * 3 region, same handle in all three)
    // rgen:  slot 5
    vkRT.giMissRegion = {sbtBase + 0 * stride, stride, 2 * stride};
    vkRT.giHitRegion = {sbtBase + 2 * stride, stride, 3 * stride};
    vkRT.giCallRegion = {0, 0, 0};
    // A raygen region must be exactly ONE record wide and its address aligned to
    // shaderGroupBaseAlignment. `stride` is already rounded up to that alignment,
    // so record 5 lands aligned; a region sized to the whole remaining table
    // instead is the classic way to get a device lost here.
    vkRT.giProbeRgenRegion = {sbtBase + 5 * stride, stride, stride};

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI SBT: stride=%u sbtBytes=%u base=0x%llx (4 groups: gi-miss+hit+shadow-miss+probe-rgen) "
                       "miss=0x%llx (missRecords=%u) hit=0x%llx (hitRecords=%u)\n",
                       stride, sbtSize, (unsigned long long)sbtBase,
                       (unsigned long long)vkRT.giMissRegion.deviceAddress,
                       (unsigned)(vkRT.giMissRegion.size / vkRT.giMissRegion.stride),
                       (unsigned long long)vkRT.giHitRegion.deviceAddress,
                       (unsigned)(vkRT.giHitRegion.size / vkRT.giHitRegion.stride));

    // --- Descriptor pool and sets ---
    // COMBINED_IMAGE_SAMPLER count is tripled: depth (binding 2), gbufNormal
    // (binding 5, P9) and gbufAlbedo (binding 6) — three per frame slot.
    VkDescriptorPoolSize poolSizes[5] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MAX_FRAMES_IN_FLIGHT * 3},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giDescSetLastUpdatedFrameCount[i] = -1;

    // --- GI buffer sampler (linear-clamp, used by interaction shader) ---
    if (vkRT.giSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.giSampler));
    }

    common->Printf("VK RT GI: pipeline initialized\n");
}

// Matches CompositePC in gi_composite.frag.
struct giCompositePC_t
{
    int32_t useAO;
    float aoStrength;
    int32_t debugMode;
    float debugGain;
};

extern idCVar r_rtAOIndirectStrength; // vk_ao.cpp
extern idCVar r_rtAODebug;

static bool s_giCompositeAOValid = false;  // AO bound by this frame's VK_RT_CompositeGI
static int s_giCompositeWrittenFrame = -1; // tr.frameCount of the last descriptor write

static void VK_RT_DrawGIComposite(VkCommandBuffer cmd, VkPipeline pipeline, int debugMode)
{
    giCompositePC_t pc = {};
    pc.useAO = s_giCompositeAOValid ? 1 : 0;
    pc.aoStrength = idMath::ClampFloat(0.0f, 1.0f, r_rtAOIndirectStrength.GetFloat());
    pc.debugMode = debugMode;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.giCompositeLayout, 0, 1,
                            &vkRT.giCompositeDescSets[vk.currentFrame], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.giCompositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    // 3 vertices, no vertex buffer — the vert shader generates the triangle from gl_VertexIndex.
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// VK_RT_InitGICompositePipeline
// Fullscreen additive pipeline that blends the GI buffer onto the framebuffer
// once per view, before the per-light interaction draws.
// ---------------------------------------------------------------------------

static void VK_RT_InitGICompositePipeline(void)
{
    // --- Descriptor set layout: binding 0 = GI, binding 1 = AO ---
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (int i = 0; i < 2; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.giCompositeDescLayout));

    // --- Pipeline layout ---
    VkPushConstantRange pcRange = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(giCompositePC_t)};
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.giCompositeDescLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.giCompositeLayout));

    // --- Shader modules ---
    VkShaderModule vertMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.vert.spv");
    VkShaderModule fragMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.frag.spv");
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI: failed to load composite shaders — composite pass disabled");
        if (vertMod != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, vertMod, NULL);
        if (fragMod != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, fragMod, NULL);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    // No vertex input — triangle is generated from gl_VertexIndex in the vert shader.
    VkPipelineVertexInputStateCreateInfo vertInput = {};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa = {};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No depth test and no depth write — GI is a post-geometry pass.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    // Additive blend: GI is added on top of whatever is already in the framebuffer.
    // Alpha: ZERO+ONE so we don't disturb the alpha channel.
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    // Attachments 1-3 (gbufNormal, gbufAlbedo, motionVectors) are written only by the
    // G-buffer prepass; every other pipeline on vk.hdrRenderPass supplies write-mask-0
    // fillers so the subpass's per-attachment blend-state count always matches.
    VkPipelineColorBlendAttachmentState blendAttachments[VK_HDR_COLOR_ATTACHMENT_COUNT] = {colorBlend};
    const uint32_t blendCount = VK_FillHdrBlendAttachments(blendAttachments);

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = blendCount;
    blendState.pAttachments = blendAttachments;

    // Dynamic viewport and scissor (composite covers full framebuffer).
    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &msaa;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = vkRT.giCompositeLayout;
    pipelineInfo.renderPass = vk.hdrRenderPass;
    pipelineInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.giCompositePipeline));

    // Debug variant (r_rtAODebug): blend off, so the view replaces the frame instead of adding to it.
    blendAttachments[0].blendEnable = VK_FALSE;
    VK_CHECK(
        vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.giCompositeDebugPipeline));

    vkDestroyShaderModule(vk.device, vertMod, NULL);
    vkDestroyShaderModule(vk.device, fragMod, NULL);

    // --- Descriptor pool and sets (one per frame in flight, 2 samplers each) ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT * 2};
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.giCompositeDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giCompositeDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giCompositeDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giCompositeDescSets));

    common->Printf("VK RT GI: composite pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitGI (public entry)
// ---------------------------------------------------------------------------

void VK_RT_InitGI(void)
{
    // Renderer init only — this is NOT a level-load hook (vk_backend.cpp calls
    // VK_RT_InitGI once, from Vulkan startup). Reuse of a lightDef index by a
    // new map is caught inside the classifier instead, by its shape-hash and
    // time-rollback re-seeds; this call only guarantees a clean table on the
    // first frame rather than whatever the loader left behind.
    VK_RT_ResetFlickerState();
    VK_RT_CreateGILightSsbos();
    // Part B: the probe descriptor set layout must exist before the GI pipeline
    // layout is built — it goes in as set 2, and the probe raygen is a group in
    // that same pipeline.
    VK_RT_InitGIProbeLayout();
    VK_RT_InitGIPipeline();
    // P9: ensure the shared null G-buffer image exists at init (idempotent) so its
    // one-time submit + queue wait never lands mid-frame at the first dispatch.
    VK_RT_GetNullGbufNormalView();
    VK_RT_InitGICompositePipeline();
    VK_RT_ResizeGI(vk.swapchainExtent.width, vk.swapchainExtent.height);
    VK_RT_InitGIAlbedoMod();
    VK_RT_InitGIProbe();
}

// ---------------------------------------------------------------------------
// VK_RT_ShutdownGI (public)
// ---------------------------------------------------------------------------

void VK_RT_ShutdownGI(void)
{
    // Before the GI pipeline layout goes: it holds giProbeDescLayout as set 2.
    VK_RT_ShutdownGIProbe();

    if (vkRT.giDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giDescPool, NULL);
        vkRT.giDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giDescLayout, NULL);
        vkRT.giDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giPipeline, NULL);
        vkRT.giPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giPipelineLayout, NULL);
        vkRT.giPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.sbtGIBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.sbtGIBuffer, NULL);
        vkRT.sbtGIBuffer = VK_NULL_HANDLE;
    }
    if (vkRT.sbtGIMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.sbtGIMemory, NULL);
        vkRT.sbtGIMemory = VK_NULL_HANDLE;
    }
    if (vkRT.giSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.giSampler, NULL);
        vkRT.giSampler = VK_NULL_HANDLE;
    }

    // Composite pipeline cleanup.
    if (vkRT.giCompositeDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giCompositeDescPool, NULL);
        vkRT.giCompositeDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositeDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giCompositeDescLayout, NULL);
        vkRT.giCompositeDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositeDebugPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giCompositeDebugPipeline, NULL);
        vkRT.giCompositeDebugPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giCompositePipeline, NULL);
        vkRT.giCompositePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giCompositeLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giCompositeLayout, NULL);
        vkRT.giCompositeLayout = VK_NULL_HANDLE;
    }

    VK_RT_ShutdownGIAlbedoMod();
    VK_RT_DestroyGIImages();
    VK_RT_DestroyGILightSsbos();
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeGI (public)
// ---------------------------------------------------------------------------

void VK_RT_ResizeGI(uint32_t width, uint32_t height)
{
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_GI);
    VK_RT_DestroyGIImages();
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.giDescSetLastUpdatedFrameCount[i] = -1;
    VK_RT_CreateGIImages(width, height);

    // Gated on the scratch already existing: VK_RT_InitGI calls this BEFORE
    // VK_RT_InitGIAlbedoMod, and allocating here too would leak that first pair.
    if (vkRT.giScratchA[0].image != VK_NULL_HANDLE)
        VK_RT_ResizeGIAlbedoMod(width, height);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGI (public)
// Builds and publishes set 0 for the GI pipeline — GIParams UBO, TLAS, light
// SSBO, depth and G-buffer descriptors — and traces nothing itself.
//
// It still exists, and still has to run before the probe trace, because the
// probe raygen shares this pipeline and gi_ray.rchit reads set 0.  The per-pixel
// launch that used to follow this setup is gone with gi_ray.rgen.
//
// Must be outside a render pass.  Depth must be in ATTACHMENT_OPTIMAL on entry;
// this function transitions to READ_ONLY_OPTIMAL and restores.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VK_RT_UploadGILights (public)
// Populate the per-frame GI light SSBO with candidate world lights.
// Called every frame BEFORE any RT dispatch that needs light data (reflections,
// GI).  Calling it more than once per frame is safe — the buffer is reset then
// refilled each call.
//
// Gathering (Stage 1, portal_area_lights.md): candidates come from a BFS over
// the portal-area graph rooted at the camera's area (r_rtGIAreaHops deep,
// closed doors block), reading each visited area's lightRefs — id's live,
// volume-exact per-area light lists.  Lights behind sealed walls are never
// admitted; a big room's own lights are admitted regardless of distance.
// The legacy camera-sphere cull remains as fallback (r_rtGIAreaLights 0, or
// viewDef->areaNum == -1 when noclipped outside the world).
//
// Ordering contract (L1, rt_optimization_tuning.md): lights are selected via
// stratified distance tiers and uploaded importance-first, because consumers
// walk a prefix of the buffer (gi_ray.rchit: maxBounceLights, reflect_ray.rchit:
// shadow budget, vol_froxel_fill.comp: maxLights).
//
// Independent of r_rtGILightBounce so that reflection hit shaders can always
// evaluate lighting even when the GI bounce feature is disabled.
// ---------------------------------------------------------------------------
void VK_RT_UploadGILights(const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized || viewDef == NULL)
        return;

    const int frameIdx = vk.currentFrame;
    if (vkRT.giLightSsboMapped[frameIdx] == NULL)
        return;
    if (tr.primaryWorld == NULL)
        return;

    // Upload once per frame slot from the main world view only.
    // Secondary/subview passes (GUI, mirrors, etc.) can have no world lights
    // and must not clear the already-populated GI light list.
    if (viewDef->renderWorld != tr.primaryWorld || viewDef->isSubview)
        return;

    static int s_lastGIUploadFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastGIUploadFrame[frameIdx] == tr.frameCount)
        return;
    s_lastGIUploadFrame[frameIdx] = tr.frameCount;

    GILightBuffer *lb = (GILightBuffer *)vkRT.giLightSsboMapped[frameIdx];
    lb->numLights = 0;
    lb->bounceScale = idMath::ClampFloat(0.0f, 100.0f, r_rtGIBounceScale.GetFloat());
    lb->giRadius = Max(1.0f, r_rtGIRadius.GetFloat());
    lb->emissiveScale = idMath::ClampFloat(0.0f, 100.0f, r_rtGIEmissiveScale.GetFloat());
    lb->falloffMode = idMath::ClampInt(0, 2, r_rtGIFalloffMode.GetInteger());
    lb->falloffReach = idMath::ClampFloat(1.0f, 3.0f, r_rtGIFalloffReach.GetFloat());

    const float giRadius = lb->giRadius;
    const float lightCollectScale = idMath::ClampFloat(0.25f, 8.0f, r_rtGILightCollectRadiusScale.GetFloat());
    const float lightCollectRadius = giRadius * lightCollectScale;
    const idVec3 camPos = viewDef->renderView.vieworg;
    idRenderWorldLocal *world = tr.primaryWorld;
    const int numLightDefs = world->lightDefs.Num();
    const int maxLights = idMath::ClampInt(1, VK_GI_MAX_LIGHTS, r_rtGIMaxLights.GetInteger());

    // Stage 1 (portal_area_lights.md): topology-aware gathering. Instead of a
    // camera-centred Euclidean sphere (admits lights through sealed walls, goes
    // dark in rooms larger than the sphere), BFS the portal graph from the
    // camera's area and gather each visited area's lightRefs — the per-area light
    // lists id's R_CreateLightRefs already maintains by pushing light volumes
    // through the BSP / flooding portals. The sphere path remains as runtime
    // fallback (r_rtGIAreaLights 0) and for camera-outside-world (areaNum == -1).
    static const int kMaxGatherAreas = 4096; // BFS visited-stamp/queue capacity; Doom 3 maps have < 200 areas
    const int areaHops = idMath::ClampInt(0, 64, r_rtGIAreaHops.GetInteger());
    const bool useAreaWalk = r_rtGIAreaLights.GetBool() && viewDef->areaNum >= 0 &&
                             viewDef->areaNum < world->numPortalAreas && world->portalAreas != NULL &&
                             world->numPortalAreas <= kMaxGatherAreas;

    // Area walk: distance is demoted from admission criterion to a generous
    // safety cap (mega-area guard). Sphere fallback: distance IS the admission.
    const float maxLightDist = Max(1.0f, r_rtGIMaxLightDist.GetFloat());
    const float distCullSq = useAreaWalk ? maxLightDist * maxLightDist : lightCollectRadius * lightCollectRadius;

    // Collect candidates (area walk or sphere); selection/ordering happens below (L1).
    struct Candidate
    {
        float distSq;
        float importance;   // L1: intensity × luminance × radius² / max(distSq, radius²), + hysteresis boost
        int tier;           // L1: distance tier 0-3 (0-128 / 128-320 / 320-768 / 768+)
        int lightIdx;       // index into lightDefs — stable identity for hysteresis
        float flickerDepth; // G5b: 1 - lowest s seen recently. 0 for a steady light
        GILightEntry entry;
        GILightCookie cookie; // valid only when entry.flags & GI_LIGHT_FLAG_HAS_COOKIE
    };

    // Use a fixed-size stack buffer — numLightDefs can be in the hundreds.
    static Candidate s_candidates[1024];
    int numCandidates = 0;

    // L1 hysteresis state: which lightDef indices were uploaded by the previous
    // upload (stamped with that upload's frameCount). A light selected last frame
    // gets a small importance boost so selection doesn't flicker at the cut line.
    static const int kHysteresisMaxLightIdx = 4096;
    static int s_lightSelectedFrame[kHysteresisMaxLightIdx];
    static int s_prevUploadFrameNum = -1;
    const int prevUploadFrame = s_prevUploadFrameNum;

    const float whiteWeight = idMath::ClampFloat(0.0f, 1.0f, r_rtGIWhiteWeight.GetFloat());

    // G5b: the classifier's clock. renderView.time is the game's own millisecond
    // clock, so the hold window survives a frame-rate change and a pause.
    const int nowMs = viewDef->renderView.time;

    // Per-light admission + candidate build, shared by both gather paths.
    // gatherArea/gatherHop are provenance for the dump: hop>=0 means BFS at that
    // depth; hop==-1 with gatherArea>=0 means Stage 1.5 (reached via the view-flood
    // union, not the BFS — on screen but outside hop range); gatherArea==-1 (always
    // paired with hop==-1) means the sphere fallback path.
    const auto considerLight = [&](int li, const idRenderLightLocal *lightLocal, int gatherArea, int gatherHop) {
        const renderLight_t &p = lightLocal->parms;

        // suppressLightInViewID is set on worldMuzzleFlash (third-person weapon light).
        // Skip it — volumetrics should follow the first-person (muzzleFlash) light only.
        if (p.suppressLightInViewID != 0)
            return;

        // Parallel (directional/sky) lights are infinite — no volume boundary or falloff,
        // so they cannot contribute meaningful single-scatter volumetrics.
        if (p.parallel)
            return;

        const float dSq = (p.origin - camPos).LengthSqr();
        if (dSq > distCullSq)
            return;

        // p.parallel already excluded above.
        const bool isProjected = !p.pointLight;

        // Light.cpp only fills in lightRadius for point lights (Light.cpp:115-123);
        // a real projected/spot light's lightRadius is always (0,0,0). Reading it
        // unconditionally here made every projected light's radius 0 — failing the
        // admit gate below and zeroing its importance even if admitted.
        // Projected range is light_end (R_SetLightProject's falloff plane); Light.cpp defaults
        // end to target. Sentry flashlights use target "1 0 0" + end 640, so |target| reads 1.
        const float projReach = Max((p.axis * p.end).Length(), (p.axis * p.target).Length());
        float radius = isProjected ? projReach : Max(Max(p.lightRadius.x, p.lightRadius.y), p.lightRadius.z);

        // The light's colour is the EVALUATED colour of its material's chosen
        // stage, not its raw shaderParms.
        //
        // This is what the raster interaction path uses (vk_backend.cpp:4190,
        // `lightColor[i] = regs[stage->color.registers[i]]`), and reading parms
        // instead silently drops every time-driven expression Doom 3 writes its
        // light behaviour in. `lights/square_flicker` is
        //     red ((.25 * blinktable[Parm4 + time*15*Parm3]) + .75) * Parm0
        // so Parm0..2 are the CONSTANT amplitude and the whole flicker lives in
        // the table lookup. Every RT consumer — GI bounce, volumetrics,
        // reflections — therefore saw flickering lights as perfectly steady, and
        // had done since the light SSBO was written. G5b cannot detect a flicker
        // that never reaches it, and no probe update rate can track a signal
        // that is not in the data (which is what G5's fix-1 measurement was
        // really showing).
        //
        // Evaluated once here and reused by the cookie block below, which needs
        // the same registers and must agree on the same chosen stage.
        const idMaterial *lightShader = lightLocal->lightShader;
        float *lightRegs = NULL;
        int chosenStage = -1;
        int numPassingStages = 0;
        if (lightShader != NULL)
        {
            const int numRegs = lightShader->GetNumRegisters();
            lightRegs = (float *)_alloca(sizeof(float) * numRegs);
            lightShader->EvaluateRegisters(lightRegs, p.shaderParms, viewDef, p.referenceSound);

            for (int s = 0; s < lightShader->GetNumStages(); s++)
            {
                const shaderStage_t *stage = lightShader->GetStage(s);
                if (!lightRegs[stage->conditionRegister])
                    continue;
                if (!stage->texture.image)
                    continue;
                numPassingStages++;
                if (chosenStage < 0)
                    chosenStage = s; // v1: first passing stage — see the doc note on multi-stage lights
            }
        }

        // No usable stage (no shader, or every stage's condition failed) leaves
        // the parms as the only statement of the light's colour.
        float r = p.shaderParms[SHADERPARM_RED];
        float g = p.shaderParms[SHADERPARM_GREEN];
        float b = p.shaderParms[SHADERPARM_BLUE];
        if (chosenStage >= 0)
        {
            const shaderStage_t *stage = lightShader->GetStage(chosenStage);
            r = lightRegs[stage->color.registers[0]];
            g = lightRegs[stage->color.registers[1]];
            b = lightRegs[stage->color.registers[2]];
        }
        const idVec3 parmColorDbg(p.shaderParms[SHADERPARM_RED], p.shaderParms[SHADERPARM_GREEN],
                                  p.shaderParms[SHADERPARM_BLUE]);
        const idVec3 evalColorDbg(r, g, b);

        // G5b: classify before anything downstream reads the colour. A fast
        // light is uploaded at its PEAK colour with s carried alongside, so
        // everything from here on — importance, saturation weight, the shader's
        // reservoir weights — sees a time-invariant light and stops reacting to
        // the flicker. Without that, a light in its dark phase scores near-zero
        // importance, loses its upload slot, and the bucket it feeds starves.
        GIFlickerVerdict flicker;
        VK_RT_ClassifyFlicker(li, p, lightLocal->lightShader, idVec3(r, g, b), nowMs, flicker);
        const float curLumDbg = VK_RT_LightLuminance(idVec3(r, g, b));
        r = flicker.lhat.x;
        g = flicker.lhat.y;
        b = flicker.lhat.z;

        // 2026-08-30: parm3 is the material TIMESCALE on a light, not an intensity.
        //
        // RenderWorld.h:61-62 aliases two names onto slot 3 — SHADERPARM_ALPHA and
        // SHADERPARM_TIMESCALE. Entities/models use the alpha meaning; lights use the
        // other one, and Light.cpp:159 spawns the map key through it explicitly:
        //
        //     args->GetFloat( "shaderParm3", "1", parms[SHADERPARM_TIMESCALE] );
        //
        // So a light's parm3 scales shader time for its material's time-driven
        // expressions. lights/cloudscroll2 is `translate time * .03, time * .03`, and
        // mars_city1's light_5253 sets shaderParm3 9000 as that texture's scroll rate.
        // Reading it as a brightness multiplier gave that one light importance 1631
        // against 0.24 for the next-ranked light: it monopolised the GI/vol upload
        // slots and blew the volumetric march into a full-screen wash that no density,
        // strength or anisotropy could pull back, since the multiplier sits upstream of
        // all three (the fill's contrib line). 40 retail light entities set parm3
        // outside [0,1], up to 80000 (recycling1 "level_fog") — concentrated in the big
        // maps: recycling1, enpro, cpu, cpuboss, hell1, site3.
        //
        // Nothing is lost by dropping it. A Doom 3 light carries brightness entirely in
        // its RGB registers — tr_render.cpp:872-874 applies backEnd.lightScale to
        // lightColor[0..2] only and draw_common.cpp:2079 pushes the light colour through
        // qglColor3fv, three components; idLight's fade/SetLightLevel paths scale parms
        // 0-2. There is no scalar intensity channel to lose.
        //
        // Why this hid for so long: the spawn default above is "1", so the multiplier is
        // exactly 1.0 on essentially every light, and the old `< 0.001f -> 1.0f` patch
        // covered the zero-timescale case. Both benign values, so the read looked right
        // anywhere it was tested.
        //
        // The one place that used slot 3 as a real intensity was our own auto-relight
        // synthesis (vk_auto_relight.cpp) — it now bakes r_rtAutoRelightIntensity into
        // the RGB parms instead, so brightness arrives here through the same channel for
        // every light regardless of origin, and that cvar finally affects the direct
        // lighting from those fixtures too rather than GI/volumetrics alone.
        //
        // Kept as an explicit 1.0 rather than deleted: GILightEntry.colorIntensity.a and
        // the shaders' lightIntens stay in place for a future source to drive
        // deliberately. The rule is only that it must not silently inherit parm3.
        const float intensity = 1.0f;

        // §0 (auto_relight.md): shared real/fake judgment — replaces the old
        // entity-noShadows-only test, which let material-ambient washes through
        // GI/vol while rejecting small colored accents flagged entity-noShadows.
        const vkRTLightClass_t lightClass = VK_RT_ClassifyLight(p, lightLocal->lightShader);
        const bool admitted = (lightClass == RT_LIGHT_REAL || lightClass == RT_LIGHT_ACCENT) && radius >= 1.0f;

        // §0: saturation weighting for ranking only — a fully white light's importance
        // is scaled by whiteWeight, a fully saturated light is untouched. This does NOT
        // affect admission, only which admitted lights win the limited upload slots.
        const float maxChan = Max(r, Max(g, b));
        const float minChan = Min(r, Min(g, b));
        const float sat = (maxChan > 1e-4f) ? (maxChan - minChan) / maxChan : 0.0f;
        const float satWeight = whiteWeight + (1.0f - whiteWeight) * sat;

        if (!admitted)
            return;

        if (numCandidates < 1024)
        {
            Candidate &c = s_candidates[numCandidates++];
            c.distSq = dSq;
            c.lightIdx = li;
            c.flickerDepth = flicker.depth;

            // L1 distance tier (world units): 0-128 / 128-320 / 320-768 / 768+.
            const float dist = idMath::Sqrt(dSq);
            c.tier = (dist < 128.0f) ? 0 : (dist < 320.0f) ? 1 : (dist < 768.0f) ? 2 : 3;

            // L1 perceptual importance. The / max(distSq, radius²) term approximates
            // received flux without re-collapsing to pure distance: inside a light's
            // own radius the divisor stops shrinking, so nearby lights compare by
            // intensity·luminance alone.
            const float lum = Max(0.0f, 0.299f * r + 0.587f * g + 0.114f * b);
            const float baseImportance = intensity * lum * radius * radius / Max(dSq, radius * radius);
            c.importance = baseImportance * satWeight;
            if (li < kHysteresisMaxLightIdx && prevUploadFrame >= 0 && s_lightSelectedFrame[li] == prevUploadFrame)
                c.importance *= 1.15f; // hysteresis: incumbents survive small importance dips

            c.entry.posRadius[0] = p.origin.x;
            c.entry.posRadius[1] = p.origin.y;
            c.entry.posRadius[2] = p.origin.z;
            c.entry.posRadius[3] = radius;
            // Emitter, see GILightEntry::emitPos. Taken from the renderer's own derived
            // value rather than recomputed from parms, so it cannot drift from what the
            // GL path uses (R_DeriveLightData sets it, tr_lightrun.cpp:472). The
            // parallel-light branch there (origin + dir * 100000) is unreachable here —
            // considerLight rejects p.parallel above.
            c.entry.emitPos[0] = lightLocal->globalLightOrigin.x;
            c.entry.emitPos[1] = lightLocal->globalLightOrigin.y;
            c.entry.emitPos[2] = lightLocal->globalLightOrigin.z;
            c.entry.emitPos[3] = 0.0f;
            c.entry.colorIntensity[0] = r;
            c.entry.colorIntensity[1] = g;
            c.entry.colorIntensity[2] = b;
            c.entry.colorIntensity[3] = intensity;
            // Fill volume geometry: AABB half-extents for point lights, cone for projected.
            // isProjected computed earlier alongside the admission-time radius fix.
            if (!isProjected)
            {
                c.entry.boxExtents[0] = p.lightRadius.x;
                c.entry.boxExtents[1] = p.lightRadius.y;
                c.entry.boxExtents[2] = p.lightRadius.z;
                c.entry.boxExtents[3] = 0.0f;
                c.entry.coneDir[0] = c.entry.coneDir[1] = c.entry.coneDir[2] = c.entry.coneDir[3] = 0.0f;
                // Widen sphere pre-cull to cover the halo zone outside the box. Must
                // track falloffReach or the cull clips the falloff before it reaches 0.
                c.entry.posRadius[3] = radius * lb->falloffReach;
            }
            else
            {
                // p.target is LOCAL space; rotate by p.axis to get world-space direction.
                // Cone angle comes from target vs right/up; range from projReach (light_end).
                idVec3 toTarget = p.axis * p.target;
                float targetLen = toTarget.Length();
                idVec3 dir = (targetLen > 0.001f) ? toTarget / targetLen : idVec3(0, 0, 1);
                float maxHalf = Max(p.right.Length(), p.up.Length());
                float cosHalf = targetLen / idMath::Sqrt(targetLen * targetLen + maxHalf * maxHalf);
                float reach = projReach;
                c.entry.coneDir[0] = dir.x;
                c.entry.coneDir[1] = dir.y;
                c.entry.coneDir[2] = dir.z;
                c.entry.coneDir[3] = cosHalf;
                c.entry.boxExtents[0] = c.entry.boxExtents[1] = c.entry.boxExtents[2] = 0.0f;
                c.entry.boxExtents[3] = reach * 1.1f;
                c.entry.posRadius[3] = reach * 1.1f; // override sphere pre-cull to match cone reach
            }
            // lightType: 0=point, 1=scene directed/spot, 2=player flashlight.
            // allowLightInViewID is set on muzzleFlash (first-person weapon light).
            c.entry.lightType = isProjected ? (p.allowLightInViewID != 0 ? 2u : 1u) : 0u;
            c.entry.flags = flicker.fast ? GI_LIGHT_FLAG_FAST : 0u;
            c.entry.fastScale = flicker.scale;
            c.entry.fastBucket = 0u; // K is clamped to 1 — see r_rtGIProbeFastBuckets

            // Stage 6 (20261013_reflection_volumetrics.md) — axial attenuation for
            // projected lights comes from the light's falloffImage, texgenned by
            // lightProject[3], exactly as interaction.vert:90 does it.
            //
            // Projected only. Point lights keep rt_LightBoxAtten, whose falloffMode and
            // falloffReach are tuned by eye; swapping them to the image is a separate
            // retune, not this stage.
            //
            // No stage texture matrix: GL folds it into S/T and leaves the falloff plane
            // alone (vk_backend.cpp:4701). lightProject is world-space already by
            // tr_lightrun.cpp:449-454.
            if (isProjected)
            {
                const idPlane &fPlane = lightLocal->lightProject[3];
                for (int comp = 0; comp < 4; comp++)
                    c.cookie.planeF[comp] = fPlane[comp];
                c.cookie.falloffIndex = VK_RT_GetOrAssignTexIndex(lightLocal->falloffImage);
                c.entry.flags |= GI_LIGHT_FLAG_HAS_FALLOFF;
            }
            else
            {
                // The Candidate array is static and reused, so stale planes from a
                // previous light would otherwise survive behind a cleared flag.
                for (int comp = 0; comp < 4; comp++)
                    c.cookie.planeF[comp] = 0.0f;
                c.cookie.falloffIndex = 0u;
            }

            // rt_projected_light_cookies.md Stage 1 — light-cookie stage detection.
            // Deliberately NOT gated on isProjected — see project_light_cookie_stage1
            // memory for why point lights need this too (fanblade3 is one).
            //
            // Registers and the chosen stage come from the colour block above
            // rather than being re-evaluated here: the cookie and the light
            // colour MUST come from the same stage, or a multi-stage light gets
            // one stage's gobo tinted by another stage's colour.
            if (lightShader != NULL)
            {
                float *regs = lightRegs;
                const int numPassing = numPassingStages;

                if (chosenStage >= 0)
                {
                    const shaderStage_t *stage = lightShader->GetStage(chosenStage);
                    const idPlane &sPlane = lightLocal->lightProject[0];
                    const idPlane &tPlane = lightLocal->lightProject[1];
                    const idPlane &qPlane = lightLocal->lightProject[2];

                    if (stage->texture.hasMatrix)
                    {
                        float m[16];
                        RB_GetShaderTextureMatrix(regs, &stage->texture, m);
                        // Fold texture matrix into the planes (S'=m00*S+m01*T+m03*Q, etc).
                        for (int comp = 0; comp < 4; comp++)
                        {
                            c.cookie.planeS[comp] = m[0] * sPlane[comp] + m[4] * tPlane[comp] + m[12] * qPlane[comp];
                            c.cookie.planeT[comp] = m[1] * sPlane[comp] + m[5] * tPlane[comp] + m[13] * qPlane[comp];
                        }
                    }
                    else
                    {
                        for (int comp = 0; comp < 4; comp++)
                        {
                            c.cookie.planeS[comp] = sPlane[comp];
                            c.cookie.planeT[comp] = tPlane[comp];
                        }
                    }
                    for (int comp = 0; comp < 4; comp++)
                        c.cookie.planeQ[comp] = qPlane[comp];
                    c.cookie.imageIndex = VK_RT_GetOrAssignTexIndex(stage->texture.image);
                    c.cookie.pad[0] = c.cookie.pad[1] = 0u;
                    c.entry.flags |= GI_LIGHT_FLAG_HAS_COOKIE;
                }
            }
        }
    };

    int areasVisited = 0;   // area-walk breadcrumb for the summary log below
    int viewStampAreas = 0; // Stage 1.5 breadcrumb: areas added by the view-flood union, not the BFS
    if (useAreaWalk)
    {
        // BFS the portal graph from the camera's area, gathering each visited
        // area's lightRefs chain. Visited stamps are per-gather-pass counters
        // (NOT tr.frameCount — see feedback_per_slot_counters), so no clearing.
        static int s_areaVisitStamp[kMaxGatherAreas];
        static int s_lightVisitStamp[kHysteresisMaxLightIdx]; // dedupe lights spanning multiple areas
        static int s_gatherStamp = 0;
        s_gatherStamp++; // starts at 1, never matches the zero-initialised arrays

        struct areaHop_t
        {
            int area;
            int hop;
        };
        static areaHop_t s_bfsQueue[kMaxGatherAreas];
        int head = 0, tail = 0;

        // Stage 1.75 (portal_area_lights.md): seed the BFS from every area
        // touching a small box around the camera, not just the single
        // PointInArea(camera) classification. PointInArea is a hard plane
        // test — standing in a doorway/threshold, a sub-unit step can flip
        // which side you're classified on, which flips the hop number of
        // EVERY light in both rooms (hop=0 <-> hop=1, etc.) even though nothing
        // visible changed. Seeding both areas at hop=0 removes the flip.
        const float boundsEps = r_rtGIAreaBoundsEpsilon.GetFloat();
        int numSeedAreas = 0;
        if (boundsEps > 0.0f)
        {
            static int s_seedAreas[64];
            const idBounds seedBounds = idBounds(camPos).Expand(boundsEps);
            numSeedAreas = world->BoundsInAreas(seedBounds, s_seedAreas, 64);
            for (int i = 0; i < numSeedAreas; i++)
            {
                const int a = s_seedAreas[i];
                if (a < 0 || a >= world->numPortalAreas || s_areaVisitStamp[a] == s_gatherStamp)
                    continue;
                s_areaVisitStamp[a] = s_gatherStamp;
                s_bfsQueue[tail].area = a;
                s_bfsQueue[tail].hop = 0;
                tail++;
            }
        }
        if (numSeedAreas == 0)
        {
            // Fallback: bounds probe found nothing (epsilon disabled, or an
            // edge case at the world boundary) — behave exactly as before.
            s_areaVisitStamp[viewDef->areaNum] = s_gatherStamp;
            s_bfsQueue[tail].area = viewDef->areaNum;
            s_bfsQueue[tail].hop = 0;
            tail++;
        }

        while (head < tail)
        {
            const areaHop_t cur = s_bfsQueue[head++];
            const portalArea_t *area = &world->portalAreas[cur.area];

            for (const areaReference_t *lref = area->lightRefs.areaNext; lref != &area->lightRefs;
                 lref = lref->areaNext)
            {
                const idRenderLightLocal *light = lref->light;
                if (light == NULL)
                    continue;
                const int li = light->index;
                // Dedupe: a light's volume typically spans several areas. Indices
                // beyond the stamp array (never in practice) skip dedupe and may
                // burn a duplicate candidate slot — harmless.
                if (li >= 0 && li < kHysteresisMaxLightIdx)
                {
                    if (s_lightVisitStamp[li] == s_gatherStamp)
                        continue;
                    s_lightVisitStamp[li] = s_gatherStamp;
                }
                considerLight(li, light, cur.area, cur.hop);
            }

            if (cur.hop >= areaHops)
                continue;
            for (const portal_t *portal = area->portals; portal != NULL; portal = portal->next)
            {
                // Closed doors: same PS_BLOCK_VIEW test the view flood uses.
                if (portal->doublePortal->blockingBits & PS_BLOCK_VIEW)
                    continue;
                const int into = portal->intoArea;
                if (into < 0 || into >= world->numPortalAreas)
                    continue;
                if (s_areaVisitStamp[into] == s_gatherStamp)
                    continue;
                s_areaVisitStamp[into] = s_gatherStamp;
                s_bfsQueue[tail].area = into;
                s_bfsQueue[tail].hop = cur.hop + 1;
                tail++;
            }
        }
        areasVisited = tail;

        // Stage 1.5 (portal_area_lights.md): union in every area id's own view-
        // frustum flood found actually visible this frame (portalArea_t::viewCount,
        // stamped by FindViewLightsAndEntities — the same flood the standard
        // renderer already runs to decide what to draw, so this costs nothing new).
        // Fixes what the BFS alone can't: standing still, looking across a room at
        // a light whose area is outside r_rtGIAreaHops range but plainly on screen
        // (a mapper's big open room, or a window/doorway sightline into a room the
        // BFS hasn't reached yet). The BFS handles the complementary case — a light
        // behind a blind corner you can't see yet but are about to walk into.
        // Degrades gracefully if a subview/mirror bumped tr.viewCount again since
        // the primary view's flood by the time this runs: worst case is silently
        // contributing nothing extra that frame (falls back to BFS-only), not a
        // correctness bug — see feedback_per_slot_counters, same per-gather-pass
        // stamp discipline as the BFS above (not tr.frameCount-keyed).
        for (int a = 0; a < world->numPortalAreas; a++)
        {
            if (world->portalAreas[a].viewCount != tr.viewCount)
                continue;
            if (s_areaVisitStamp[a] == s_gatherStamp)
                continue; // already covered by the BFS
            s_areaVisitStamp[a] = s_gatherStamp;
            viewStampAreas++;

            const portalArea_t *vArea = &world->portalAreas[a];
            for (const areaReference_t *lref = vArea->lightRefs.areaNext; lref != &vArea->lightRefs;
                 lref = lref->areaNext)
            {
                const idRenderLightLocal *light = lref->light;
                if (light == NULL)
                    continue;
                const int li = light->index;
                if (li >= 0 && li < kHysteresisMaxLightIdx)
                {
                    if (s_lightVisitStamp[li] == s_gatherStamp)
                        continue;
                    s_lightVisitStamp[li] = s_gatherStamp;
                }
                considerLight(li, light, a, -1); // hop=-1: reached via view-stamp, not the BFS
            }
        }
        areasVisited += viewStampAreas;
    }
    else
    {
        // Legacy/fallback: every lightDef in the world vs. the camera sphere.
        for (int li = 0; li < numLightDefs; li++)
        {
            const idRenderLightLocal *lightLocal = world->lightDefs[li];
            if (lightLocal == NULL)
                continue;
            considerLight(li, lightLocal, -1, -1);
        }
    }

    // --- L1 (rt_optimization_tuning.md): stratified selection + importance ordering ---
    //
    // Pure nearest-to-camera selection (legacy path below) lets dim fill lights near
    // the camera crowd out a bright light 200u away, and makes distant sources pop
    // as the player moves. Instead: bucket by distance tier, sort each tier by
    // importance, give each tier a guaranteed slot quota, and upload the final set
    // ordered importance-first
    const int toUpload = Min(numCandidates, maxLights);

    if (!r_rtGILightStratify.GetBool())
    {
        // Legacy nearest-first path, kept for A/B comparison.
        if (numCandidates > 1 && numCandidates > maxLights)
        {
            const auto cmpDist = [](const void *a, const void *b) -> int {
                const Candidate *ca = (const Candidate *)a;
                const Candidate *cb = (const Candidate *)b;
                if (ca->distSq < cb->distSq)
                    return -1;
                if (ca->distSq > cb->distSq)
                    return 1;
                return 0;
            };
            qsort(s_candidates, numCandidates, sizeof(s_candidates[0]), cmpDist);
        }

        if (r_vkLogRT.GetInteger() >= 1)
        {
            if (useAreaWalk)
                common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                               "areaWalk start=%d visited=%d(+%d viewStamp) hops=%d (legacy distance sort)\n",
                               toUpload, numCandidates, maxLights, numLightDefs, giRadius, viewDef->areaNum,
                               areasVisited, viewStampAreas, areaHops);
            else
                common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                               "collectRadius=%.1f (legacy distance sort)\n",
                               toUpload, numCandidates, maxLights, numLightDefs, giRadius, lightCollectRadius);
        }
        for (int i = 0; i < toUpload; i++)
        {
            const int idx = lb->numLights++;
            lb->lights[idx] = s_candidates[i].entry;
            lb->cookies[idx] = s_candidates[i].cookie;
        }

        // Vol-specific selection (portal_area_lights.md follow-up, 2026-08-22): same
        // pool, walked in the same (here: distance) order, filtered to lights that
        // can possibly reach within r_rtVolMaxDist of the camera, capped separately
        // from GI's own maxLights. See the stratified path below for the full
        // rationale — this legacy A/B path gets the same treatment for consistency.
        {
            GILightBuffer *volLb = (GILightBuffer *)vkRT.volLightSsboMapped[frameIdx];
            volLb->numLights = 0;
            volLb->bounceScale = lb->bounceScale;
            volLb->giRadius = lb->giRadius;
            volLb->emissiveScale = lb->emissiveScale;
            volLb->falloffMode = lb->falloffMode;
            volLb->falloffReach = lb->falloffReach;
            const int volCutoff = idMath::ClampInt(1, VK_GI_MAX_LIGHTS, r_rtVolMaxLights.GetInteger());
            const float volMaxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
            for (int i = 0; i < numCandidates && volLb->numLights < volCutoff; i++)
            {
                const Candidate &c = s_candidates[i];
                const float reach = volMaxDist + c.entry.posRadius[3];
                if (c.distSq > reach * reach)
                    continue; // light's sphere/cone can never reach a march sample — skip, don't burn a slot
                const int idx = volLb->numLights++;
                volLb->lights[idx] = c.entry;
                volLb->cookies[idx] = c.cookie;
            }
        }
        return;
    }

    // Sort tier-ascending, importance-descending: tiers become contiguous runs,
    // each run best-first, so per-tier quota picks are just prefixes.
    if (numCandidates > 1)
    {
        const auto cmpTierImportance = [](const void *a, const void *b) -> int {
            const Candidate *ca = (const Candidate *)a;
            const Candidate *cb = (const Candidate *)b;
            if (ca->tier != cb->tier)
                return ca->tier - cb->tier;
            if (ca->importance > cb->importance)
                return -1;
            if (ca->importance < cb->importance)
                return 1;
            return 0;
        };
        qsort(s_candidates, numCandidates, sizeof(s_candidates[0]), cmpTierImportance);
    }

    int tierStart[4] = {0, 0, 0, 0};
    int tierCount[4] = {0, 0, 0, 0};
    for (int i = 0; i < numCandidates; i++)
        tierCount[s_candidates[i].tier]++;
    for (int t = 1; t < 4; t++)
        tierStart[t] = tierStart[t - 1] + tierCount[t - 1];

    // Quotas 48/40/24/16 of 128 slots, scaled to maxLights. Guaranteed floors so
    // far-tier bright sources always get representation; unused quota flows to
    // other tiers (near-first) in the second pass.
    int quota[4];
    quota[0] = (maxLights * 48) / 128;
    quota[1] = (maxLights * 40) / 128;
    quota[2] = (maxLights * 24) / 128;
    quota[3] = maxLights - quota[0] - quota[1] - quota[2];

    int take[4];
    int used = 0;
    for (int t = 0; t < 4; t++)
    {
        take[t] = Min(quota[t], tierCount[t]);
        used += take[t];
    }
    for (int t = 0; t < 4 && used < toUpload; t++)
    {
        const int extra = Min(toUpload - used, tierCount[t] - take[t]);
        take[t] += extra;
        used += extra;
    }

    // Gather winners (each tier's best-first prefix), then order the final list
    // by importance so prefix-walking consumers see the lights that matter most.
    static const Candidate *s_selected[VK_GI_MAX_LIGHTS];
    int numSelected = 0;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < take[t] && numSelected < VK_GI_MAX_LIGHTS; i++)
            s_selected[numSelected++] = &s_candidates[tierStart[t] + i];

    if (numSelected > 1)
    {
        const auto cmpImportance = [](const void *a, const void *b) -> int {
            const Candidate *ca = *(const Candidate *const *)a;
            const Candidate *cb = *(const Candidate *const *)b;
            if (ca->importance > cb->importance)
                return -1;
            if (ca->importance < cb->importance)
                return 1;
            return 0;
        };
        qsort(s_selected, numSelected, sizeof(s_selected[0]), cmpImportance);
    }

    if (r_vkLogRT.GetInteger() >= 1)
    {
        if (useAreaWalk)
            common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                           "areaWalk start=%d visited=%d(+%d viewStamp) hops=%d tiers=%d/%d/%d/%d "
                           "take=%d/%d/%d/%d\n",
                           numSelected, numCandidates, maxLights, numLightDefs, giRadius, viewDef->areaNum,
                           areasVisited, viewStampAreas, areaHops, tierCount[0], tierCount[1], tierCount[2],
                           tierCount[3], take[0], take[1], take[2], take[3]);
        else
            common->Printf("VK RT GI lights: upload=%d candidates=%d maxLights=%d lightDefs=%d giRadius=%.1f "
                           "collectRadius=%.1f tiers=%d/%d/%d/%d take=%d/%d/%d/%d\n",
                           numSelected, numCandidates, maxLights, numLightDefs, giRadius, lightCollectRadius,
                           tierCount[0], tierCount[1], tierCount[2], tierCount[3], take[0], take[1], take[2], take[3]);
    }

    // Vol-specific selection (portal_area_lights.md follow-up, 2026-08-22): GI's own
    // selection above (s_selected) can legitimately include a light that's admitted
    // for bounce/reflection purposes but sits far outside anything the froxel grid
    // can ever sample (e.g. a room revealed down a long hallway by the Stage
    // 1.5 view-flood union — real for GI bounce off a surface near it, irrelevant to
    // fog scattered near the camera). The vol pass used to read a raw prefix of GI's
    // buffer, so a burst of those admitted-but-unreachable lights could silently
    // evict a genuinely nearby, fog-relevant light purely by consuming shared
    // slots — the root cause of the hallway on/off pop. Fix: walk GI's own
    // importance-ordered selection (so near/far tier balance and hero-light
    // protection carry over unchanged) and keep only lights whose sphere/cone can
    // possibly reach within r_rtVolMaxDist of the camera, into a SEPARATE buffer
    // vol_froxel_fill.comp reads instead of GI's.
    static const Candidate *s_volSelected[VK_GI_MAX_LIGHTS];
    int numVolSelected = 0;
    {
        const int volCutoff = idMath::ClampInt(1, VK_GI_MAX_LIGHTS, r_rtVolMaxLights.GetInteger());
        const float volMaxDist = Max(1.0f, r_rtVolMaxDist.GetFloat());
        float maxReach = 0.0f;
        for (int i = 0; i < numSelected && numVolSelected < volCutoff; i++)
        {
            const Candidate *c = s_selected[i];
            const float reach = volMaxDist + c->entry.posRadius[3];
            if (c->distSq > reach * reach)
                continue; // can never reach a march sample — skip, don't burn a vol slot
            s_volSelected[numVolSelected++] = c;
            maxReach = Max(maxReach, c->entry.posRadius[3]);
        }
        g_vkVolMaxLightReach = maxReach; // read by the next TLAS build's pass-2 radius
    }

    // G5b: count the flickering lights as they go in, and note which one wins
    // the single fast bucket — see GI_LIGHT_FLAG_FAST_BUCKET on why it holds
    // exactly one light.
    //
    // Ranked on importance x flicker DEPTH, not importance alone.
    int bucketIdx = -1;
    int bucketSel = -1;
    float bucketScore = 0.0f;
    s_giFastLightCount = 0;

    for (int i = 0; i < numSelected; i++)
    {
        const int idx = lb->numLights++;
        lb->lights[idx] = s_selected[i]->entry;
        lb->cookies[idx] = s_selected[i]->cookie;
        if (s_selected[i]->lightIdx < kHysteresisMaxLightIdx)
            s_lightSelectedFrame[s_selected[i]->lightIdx] = tr.frameCount;

        if ((lb->lights[idx].flags & GI_LIGHT_FLAG_FAST) == 0u)
            continue;

        s_giFastLightCount++;
        const float score = s_selected[i]->importance * s_selected[i]->flickerDepth;
        if (score > bucketScore)
        {
            bucketScore = score;
            bucketIdx = idx;
            bucketSel = i;
        }
    }

    // Exactly one light carries the bucket bit, so its gain is exact rather
    // than an average over lights that may not even share a room.
    for (int k = 0; k < VK_GI_MAX_FAST_BUCKETS; k++)
        s_giFastGain[k] = 0.0f;

    if (bucketIdx >= 0)
    {
        lb->lights[bucketIdx].flags |= GI_LIGHT_FLAG_FAST_BUCKET;
        lb->lights[bucketIdx].fastBucket = 0u;
        s_giFastGain[0] = idMath::ClampFloat(0.0f, 1.0f, lb->lights[bucketIdx].fastScale);
    }

    s_prevUploadFrameNum = tr.frameCount;

    GILightBuffer *volLb = (GILightBuffer *)vkRT.volLightSsboMapped[frameIdx];
    volLb->numLights = 0;
    volLb->bounceScale = lb->bounceScale;
    volLb->giRadius = lb->giRadius;
    volLb->emissiveScale = lb->emissiveScale;
    volLb->falloffMode = lb->falloffMode;
    volLb->falloffReach = lb->falloffReach;
    for (int i = 0; i < numVolSelected; i++)
    {
        const int idx = volLb->numLights++;
        volLb->lights[idx] = s_volSelected[i]->entry;
        volLb->cookies[idx] = s_volSelected[i]->cookie;
    }
}

// Convert viewDef->scissor (GL Y-up) to VkRect2D (VK Y-down).
// Part B: the GIParams dynamic-UBO offset VK_RT_DispatchGI established this
// frame, so vk_gi_probe.cpp can bind set 0 for the probe raygen without
// allocating a second, identical block.  Returns false when this frame's GI
// dispatch did not run — binding a stale offset would point gi_ray.rchit at the
// previous frame's light list, which is exactly the kind of fault that shows up
// as an unexplainable one-frame flash rather than an error.
static uint32_t s_giParamsOffset[VK_MAX_FRAMES_IN_FLIGHT] = {};
static int s_giParamsFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};

bool VK_RT_GIParamsBinding(int frameIdx, uint32_t *outOffset)
{
    if (frameIdx < 0 || frameIdx >= VK_MAX_FRAMES_IN_FLIGHT)
        return false;
    if (s_giParamsFrame[frameIdx] != tr.frameCount)
        return false;
    if (outOffset != NULL)
        *outOffset = s_giParamsOffset[frameIdx];
    return true;
}

static VkRect2D VK_RT_GI_ComputeDispatchRect(const viewDef_t *viewDef)
{
    const int w = (int)vk.renderExtent.width;
    const int h = (int)vk.renderExtent.height;
    const idScreenRect &s = viewDef->scissor;

    VkRect2D r;
    r.offset.x = idMath::ClampInt(0, w - 1, s.x1);
    r.offset.y = idMath::ClampInt(0, h - 1, h - 1 - s.y2);

    const int rw = s.x2 - s.x1 + 1;
    const int rh = s.y2 - s.y1 + 1;
    if (rw <= 0 || rh <= 0)
        return VkRect2D{{0, 0}, {0, 0}};

    r.extent.width = (uint32_t)idMath::ClampInt(1, w - r.offset.x, rw);
    r.extent.height = (uint32_t)idMath::ClampInt(1, h - r.offset.y, rh);
    return r;
}

void VK_RT_DispatchGI(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return;
    if (vkRT.giPipeline == VK_NULL_HANDLE)
    {
        common->Printf("VK RT GI: skip — pipeline is NULL\n");
        return;
    }

    const int frameIdx = vk.currentFrame;

    // Guard against duplicate dispatch in the same frame.
    static int s_lastGIDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastGIDispatchFrame[frameIdx] == tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI: skip duplicate dispatch frame=%d slot=%d\n", tr.frameCount, frameIdx);
        return;
    }
    s_lastGIDispatchFrame[frameIdx] = tr.frameCount;

    vkRTImage_t &gb = vkRT.giBuffer[frameIdx];
    if (gb.image == VK_NULL_HANDLE)
    {
        common->Printf("VK RT GI: skip — giBuffer[%d] image is NULL\n", frameIdx);
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: frame=%d slot=%d size=%ux%u\n", tr.frameCount, frameIdx, gb.width, gb.height);

    // --- Depth barrier: ATTACHMENT → READ_ONLY for rgen depth sampling ---
    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    {
        VkImageMemoryBarrier depthToRead = {};
        depthToRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthToRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthToRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthToRead.image = vk.depthImage;
        depthToRead.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Build UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    GIParamsUBO ubo = {};

    {
        const float *proj = viewDef->projectionMatrix;
        const float *mv = viewDef->worldSpace.modelViewMatrix;
        float vp[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
            {
                vp[c * 4 + r] = 0.0f;
                for (int k = 0; k < 4; k++)
                    vp[c * 4 + r] += proj[k * 4 + r] * mv[c * 4 + k];
            }
        idMat4 vpMat(idVec4(vp[0], vp[1], vp[2], vp[3]), idVec4(vp[4], vp[5], vp[6], vp[7]),
                     idVec4(vp[8], vp[9], vp[10], vp[11]), idVec4(vp[12], vp[13], vp[14], vp[15]));
        idMat4 invVP = vpMat.Inverse();
        memcpy(ubo.invViewProj, invVP.ToFloatPtr(), 16 * sizeof(float));
    }

    // Guard: skip dispatch if the matrix contains NaN (singular VP on degenerate frame).
    {
        bool hasNaN = false;
        for (int i = 0; i < 16; i++)
            if (ubo.invViewProj[i] != ubo.invViewProj[i])
            {
                hasNaN = true;
                break;
            }
        if (hasNaN)
        {
            common->Warning("VK RT GI: invViewProj contains NaN — skipping dispatch");
            // Restore depth layout before returning.
            VkImageMemoryBarrier depthRestore = {};
            depthRestore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            depthRestore.dstAccessMask =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthRestore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthRestore.image = vk.depthImage;
            depthRestore.subresourceRange = {depthAspect, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &depthRestore);
            return;
        }
    }

    VkRect2D dispatchRect = VK_RT_GI_ComputeDispatchRect(viewDef);

    ubo.giRadius = Max(1.0f, r_rtGIRadius.GetFloat());
    ubo._pad0 = 0.0f;
    ubo.frameIndex = (uint32_t)(tr.frameCount);
    ubo.giStrength = idMath::ClampFloat(0.0f, 4.0f, r_rtGIStrength.GetFloat());
    // NDC denominator for depth->world reconstruction, so it must be the extent
    // the depth buffer was rasterised through, not the (display-sized) GI buffer.
    ubo.screenWidth = (int32_t)vk.renderExtent.width;
    ubo.screenHeight = (int32_t)vk.renderExtent.height;
    ubo.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    ubo.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    ubo.scissorExtentX = (int32_t)dispatchRect.extent.width;
    ubo.scissorExtentY = (int32_t)dispatchRect.extent.height;

    // maxBounceLights: 0 takes rchit's albedo-only fallback; otherwise
    // caps the light loop so the GI pass evaluates fewer lights than reflections.
    // The SSBO numLights is NOT modified — reflections always see the full list.
    ubo.maxBounceLights = idMath::ClampInt(0, VK_GI_MAX_LIGHTS, r_rtGIMaxBounceLights.GetInteger());
    ubo.giContrast = idMath::ClampFloat(0.0f, 1.0f, r_rtGIContrast.GetFloat());
    // shadow rays per bounce hit. 0 = legacy (one per light, up to
    // maxBounceLights); 1-2 = importance-sampled picks. Clamped to 2 because the
    // estimator in rt_light_eval.glsl keeps exactly two reservoirs in registers.
    ubo.stochasticLights = idMath::ClampInt(0, 2, r_rtGIStochasticLights.GetInteger());
    ubo.useGbufNormal = (vk.gbufferSupported && r_rtGbufNormals.GetBool()) ? 1 : 0;
    ubo.playerExcludeRadius = VK_RT_PlayerExcludeRadius();
    // Per-pixel surface class from the prepass (binding 6 below).  Zero also when the fp
    // body isn't drawn: there is nothing to exclude, so the rgen skips the fetch.
    const bool haveClass = vk.gbufferSupported && vkRT.gbufAlbedo[frameIdx].view != VK_NULL_HANDLE;
    ubo.playerClassValid = (haveClass && ubo.playerExcludeRadius > 0.0f) ? 1 : 0;
    memcpy(uboMapped, &ubo, sizeof(GIParamsUBO));

    // Part B: publish this frame's dynamic offset for the probe trace. It shares
    // set 0 with the per-pixel launch because gi_ray.rchit — which both raygens
    // reach — reads this block, and every field it reads (maxBounceLights,
    // frameIndex, stochasticLights) is per-frame, not per-launch. The descriptor
    // refresh below always rewrites binding 3 for this frame, so the buffer the
    // set points at is this one.
    s_giParamsOffset[frameIdx] = uboOff;
    s_giParamsFrame[frameIdx] = tr.frameCount;

    // Estimator transitions — one line per change, so a crash/perf report says
    // which one was live.  Per bounce hit; the probe raygen sets the ray count.
    if (r_vkLogRT.GetInteger() >= 1)
    {
        static int s_lastStochastic = -1;
        if (ubo.stochasticLights != s_lastStochastic)
        {
            s_lastStochastic = ubo.stochasticLights;
            if (ubo.stochasticLights > 0)
                common->Printf("[GI] stochastic: %d shadow ray(s)/hit (legacy worst case %d)\n",
                               ubo.stochasticLights, ubo.maxBounceLights);
            else
                common->Printf("[GI] stochastic OFF — up to %d shadow rays/hit\n", ubo.maxBounceLights);
        }
    }

    // DEBUG: log light counts once per second
    if (vkRT.giLightSsboMapped[frameIdx] != NULL && r_vkLogRT.GetInteger() >= 1)
    {
        static int s_lastLogFrame = -9999;
        if (tr.frameCount - s_lastLogFrame > 60)
        {
            s_lastLogFrame = tr.frameCount;
            const GILightBuffer *lb = (const GILightBuffer *)vkRT.giLightSsboMapped[frameIdx];
            int viewLightCount = 0;
            for (const viewLight_t *vl = viewDef->viewLights; vl; vl = vl->next)
                viewLightCount++;

            common->Printf("[GI] viewLights=%d  uploaded=%d  falloff=%d/%.2f  camPos=(%.0f,%.0f,%.0f)\n",
                           viewLightCount, lb->numLights, lb->falloffMode, lb->falloffReach,
                           viewDef->renderView.vieworg.x, viewDef->renderView.vieworg.y, viewDef->renderView.vieworg.z);
        }
    }

    // --- Update descriptor set (once per frame slot when frameCount changes) ---
    static VkAccelerationStructureKHR s_lastGITlasHandle[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGIStorageView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGIDepthView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastGIGbufView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    // P9: G-buffer normal/F0 — real per-frame image when supported, else the shared
    // 1x1 null image (a = 0 sentinel → the consumer falls back to rt_ReconstructNormal).
    // Already in SHADER_READ_ONLY_OPTIMAL via the once-per-RT-block barrier in
    // vk_backend.cpp, which runs well before the GI dispatch.
    const bool haveGbuf = vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE;
    VkImageView gbufView = haveGbuf ? vkRT.gbufNormal[frameIdx].view : VK_RT_GetNullGbufNormalView();
    // Binding 6: the same prepass's albedo target, read for its alpha (surface class).
    // Independent validity check — the two images are created together, but the rgen's
    // playerClassValid must describe the one it actually samples.
    static VkImageView s_lastGIClassView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    VkImageView classView = haveClass ? vkRT.gbufAlbedo[frameIdx].view : VK_RT_GetNullGbufNormalView();

    const bool resourceChanged = (s_lastGITlasHandle[frameIdx] != vkRT.tlas[frameIdx].handle) ||
                                 (s_lastGIStorageView[frameIdx] != gb.view) ||
                                 (s_lastGIDepthView[frameIdx] != vk.depthSampledView) ||
                                 (s_lastGIGbufView[frameIdx] != gbufView) || (s_lastGIClassView[frameIdx] != classView);

    bool refreshSet = (vkRT.giDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount) || resourceChanged;
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.giDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo giImgInfo = {};
        giImgInfo.imageView = gb.view;
        giImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(GIParamsUBO);

        VkDescriptorBufferInfo lightSsboInfo = {};
        lightSsboInfo.buffer = vkRT.giLightSsbo[frameIdx];
        lightSsboInfo.offset = 0;
        lightSsboInfo.range = sizeof(GILightBuffer);

        // Binding 5: G-buffer normal/F0 (P9). texelFetch in the rgen, so filter/wrap
        // state is irrelevant — reuse the depth sampler.
        VkDescriptorImageInfo gbufInfo = {};
        gbufInfo.sampler = vkRT.depthSampler;
        gbufInfo.imageView = gbufView;
        gbufInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Binding 6: surface class.  Same barrier window as gbufNormal above — both are
        // moved to SHADER_READ_ONLY once per RT block, well before this dispatch.
        VkDescriptorImageInfo classInfo = {};
        classInfo.sampler = vkRT.depthSampler;
        classInfo.imageView = classView;
        classInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[7] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &tlasWrite;
        writes[0].dstSet = ds;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = ds;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &giImgInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = ds;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &depthInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = ds;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[3].pBufferInfo = &uboInfo;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = ds;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &lightSsboInfo;

        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = ds;
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &gbufInfo;

        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = ds;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = &classInfo;

        vkUpdateDescriptorSets(vk.device, 7, writes, 0, NULL);
        vkRT.giDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastGITlasHandle[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastGIStorageView[frameIdx] = gb.view;
        s_lastGIDepthView[frameIdx] = vk.depthSampledView;
        s_lastGIGbufView[frameIdx] = gbufView;
        s_lastGIClassView[frameIdx] = classView;
    }

    // No trace here, and no pipeline/descriptor binds either: the probe trace is
    // the only consumer of this pipeline and binds all three sets itself. What
    // this function publishes is the descriptor CONTENT plus s_giParamsOffset,
    // which VK_RT_GIParamsBinding hands to that trace.

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL ---
    {
        VkImageMemoryBarrier depthRestore = {};
        depthRestore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthRestore.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthRestore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRestore.image = vk.depthImage;
        depthRestore.subresourceRange = {depthAspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &depthRestore);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: dispatch complete\n");
}

// ---------------------------------------------------------------------------
// VK_RT_CompositeGI (public)
// Additively blends the GI buffer onto the current framebuffer using a
// fullscreen triangle.  Must be called inside the main render pass, before
// the first per-light interaction draw.  Rebinds no pipeline state that the
// caller cannot restore with a simple vkCmdBindPipeline.
// ---------------------------------------------------------------------------

void VK_RT_CompositeGI(VkCommandBuffer cmd)
{
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool())
        return;
    if (vkRT.giCompositePipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;
    vkRTImage_t &gb = vkRT.giBuffer[frameIdx];
    if (gb.image == VK_NULL_HANDLE || vkRT.giSampler == VK_NULL_HANDLE)
        return;

    // Wherever the GI chain last left giReadView — the probe resolve aims it at
    // giBuffer, the albedo-mod pass at its own scratch output.
    VkImageView readView = vkRT.giReadView[frameIdx];
    if (readView == VK_NULL_HANDLE)
        readView = gb.view; // safety fallback

    // Update the descriptor set for this frame slot.  The GI image may have
    // been recreated (resize), so always write it before drawing.
    VkDescriptorImageInfo imgInfo[2] = {};
    imgInfo[0].sampler = vkRT.giSampler;
    imgInfo[0].imageView = readView;
    imgInfo[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    s_giCompositeAOValid = VK_RT_GetAODescriptor(&imgInfo[1]);

    VkWriteDescriptorSet writes[2] = {};
    for (int i = 0; i < 2; i++)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = vkRT.giCompositeDescSets[frameIdx];
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfo[i];
    }
    vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
    s_giCompositeWrittenFrame = tr.frameCount;

    // Viewport and scissor are inherited from the caller (VK_RB_DrawView resumes
    // the render pass with the Y-flipped full viewport and s_viewScissor already
    // bound).  Do not override them here: widening the scissor to the full
    // swapchain extent would leave subsequent interaction draws using the wrong
    // region, and the GI buffer outside the dispatch rect is already cleared to
    // black so blending it adds nothing visually.
    VK_RT_DrawGIComposite(cmd, vkRT.giCompositePipeline, 0);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI: composite drawn frame=%d slot=%d ao=%d\n", tr.frameCount, frameIdx,
                       s_giCompositeAOValid ? 1 : 0);
}

// ---------------------------------------------------------------------------
// VK_RT_CompositeGIDebug (public)
// r_rtAODebug view, drawn late (after the shader passes) with the replace-blend
// pipeline.  Reuses the descriptor set VK_RT_CompositeGI wrote this frame: updating
// a set already referenced by this command buffer is not allowed.
// ---------------------------------------------------------------------------

void VK_RT_CompositeGIDebug(VkCommandBuffer cmd)
{
    const int mode = r_rtAODebug.GetInteger();
    if (mode <= 0 || vkRT.giCompositeDebugPipeline == VK_NULL_HANDLE)
        return;
    if (s_giCompositeWrittenFrame != tr.frameCount)
    {
        static int s_warnedFrame = -1;
        if (s_warnedFrame != tr.frameCount && r_vkLogRT.GetInteger() >= 1)
        {
            s_warnedFrame = tr.frameCount;
            common->Printf("VK RT AO DEBUG: skipped, GI composite did not run this frame (needs r_rtGI 1)\n");
        }
        return;
    }
    VK_RT_DrawGIComposite(cmd, vkRT.giCompositeDebugPipeline, mode);
}

// ---------------------------------------------------------------------------
// GI scratch ping-pong images
//
// The albedo-mod pass cannot write in place (its source is a read-only sampler),
// so it writes to whichever of these two giReadView is not currently pointing at.
// Inherited from the deleted à-trous chain, which needed a genuine ping-pong; one
// pass only ever uses one of the pair per frame, but keeping both is what lets the
// pass pick a destination that is provably not its own source.
// ---------------------------------------------------------------------------

static bool VK_RT_AllocGIScratchImage(vkRTImage_t &img, uint32_t width, uint32_t height)
{
    img.width = width;
    img.height = height;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imgInfo.extent = {width, height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &img.image));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vk.device, img.image, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice, &memProps);
    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t m = 0; m < memProps.memoryTypeCount; m++)
    {
        if ((memReq.memoryTypeBits & (1u << m)) &&
            (memProps.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            memTypeIdx = m;
            break;
        }
    }
    if (memTypeIdx == UINT32_MAX)
    {
        common->Warning("VK RT GI: no device-local memory for GI scratch image");
        vkDestroyImage(vk.device, img.image, NULL);
        img.image = VK_NULL_HANDLE;
        img.memory = VK_NULL_HANDLE;
        img.view = VK_NULL_HANDLE;
        img.width = 0;
        img.height = 0;
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memTypeIdx;
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &img.memory));
    VK_CHECK(vkBindImageMemory(vk.device, img.image, img.memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &img.view));

    // Transition UNDEFINED → GENERAL and clear to black, so a frame that samples
    // one of these before the pass has written it reads zero rather than garbage.
    {
        VkCommandBuffer tmpCmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbAlloc = {};
        cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool = vk.commandPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(vk.device, &cbAlloc, &tmpCmd));

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmpCmd, &beginInfo);

        VkImageSubresourceRange subRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageMemoryBarrier b1 = {};
        b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b1.srcAccessMask = 0;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b1.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b1.image = img.image;
        b1.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b1);

        VkClearColorValue clearBlack = {};
        vkCmdClearColorImage(tmpCmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

        VkImageMemoryBarrier b2 = {};
        b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b2.image = img.image;
        b2.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &b2);

        vkEndCommandBuffer(tmpCmd);

        VkFenceCreateInfo fenceCI = {};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        VK_CHECK(vkCreateFence(vk.device, &fenceCI, NULL, &fence));

        VkSubmitInfo submitI = {};
        submitI.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitI.commandBufferCount = 1;
        submitI.pCommandBuffers = &tmpCmd;
        vkQueueSubmit(vk.graphicsQueue, 1, &submitI, fence);
        VK_Prof_WaitForFences(VKPROF_WAIT_GI, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk.device, fence, NULL);
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
    }
    return true;
}

static void VK_RT_FreeGIScratchImage(vkRTImage_t &img)
{
    if (img.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(vk.device, img.view, NULL);
        img.view = VK_NULL_HANDLE;
    }
    if (img.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(vk.device, img.image, NULL);
        img.image = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, img.memory, NULL);
        img.memory = VK_NULL_HANDLE;
    }
    img.width = 0;
    img.height = 0;
}

static void VK_RT_DestroyGIScratchImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeGIScratchImage(vkRT.giScratchA[i]);
        VK_RT_FreeGIScratchImage(vkRT.giScratchB[i]);
    }
}

static void VK_RT_CreateGIScratchImages(uint32_t width, uint32_t height)
{
    VK_RT_DestroyGIScratchImages();
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocGIScratchImage(vkRT.giScratchA[i], width, height))
            common->Warning("VK RT GI: failed to allocate giScratchA slot %d — no albedo modulation", i);
        if (!VK_RT_AllocGIScratchImage(vkRT.giScratchB[i], width, height))
            common->Warning("VK RT GI: failed to allocate giScratchB slot %d — no albedo modulation", i);
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitGIAlbedoModPipeline (docs/plans/gi_albedo_target.md)
// ---------------------------------------------------------------------------

static void VK_RT_InitGIAlbedoModPipeline(void)
{
    // gi_albedo_mod.comp bindings:
    //   binding 0: COMBINED_IMAGE_SAMPLER — denoised GI (giSrc, sampler2D)
    //   binding 1: COMBINED_IMAGE_SAMPLER — receiver albedo (albedoSampler, sampler2D)
    //   binding 2: STORAGE_IMAGE          — output GI (giDst, rgba16f image2D)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.giAlbedoModDescLayout));

    // Push constants: 24 bytes matching gi_albedo_mod.comp's PC block (6 int32s).
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 24;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.giAlbedoModDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.giAlbedoModPipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/gi_albedo_mod.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT GI Albedo Mod: failed to load gi_albedo_mod.comp.spv — GI albedo modulation disabled");
        return;
    }

    VkPipelineShaderStageCreateInfo stageCI = {};
    stageCI.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCI.module = compModule;
    stageCI.pName = "main";

    VkComputePipelineCreateInfo pipeCI = {};
    pipeCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.stage = stageCI;
    pipeCI.layout = vkRT.giAlbedoModPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.giAlbedoModPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    // Pool: 1 set per frame slot, rewritten every dispatch (giReadView/albedo/scratch
    // all vary frame to frame — no benefit to the atrous-style "reuse if unchanged").
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2 * VK_MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1 * VK_MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.giAlbedoModDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.giAlbedoModDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.giAlbedoModDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.giAlbedoModDescSets));

    common->Printf("VK RT GI Albedo Mod: pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// Public entry points — GI receiver-albedo modulation
// ---------------------------------------------------------------------------

void VK_RT_InitGIAlbedoMod(void)
{
    VK_RT_InitGIAlbedoModPipeline();
    VK_RT_CreateGIScratchImages(vk.swapchainExtent.width, vk.swapchainExtent.height);
}

void VK_RT_ResizeGIAlbedoMod(uint32_t width, uint32_t height)
{
    VK_RT_CreateGIScratchImages(width, height);
}

void VK_RT_ShutdownGIAlbedoMod(void)
{
    VK_RT_DestroyGIScratchImages();
    if (vkRT.giAlbedoModPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.giAlbedoModPipeline, NULL);
        vkRT.giAlbedoModPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.giAlbedoModPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.giAlbedoModPipelineLayout, NULL);
        vkRT.giAlbedoModPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.giAlbedoModDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.giAlbedoModDescPool, NULL);
        vkRT.giAlbedoModDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.giAlbedoModDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.giAlbedoModDescLayout, NULL);
        vkRT.giAlbedoModDescLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchGIAlbedoMod (public)
// ---------------------------------------------------------------------------

void VK_RT_DispatchGIAlbedoMod(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtGI.GetBool() || !r_rtGIAlbedo.GetBool())
        return;
    if (!vk.gbufferSupported)
        return; // no gbufAlbedo target to read — legacy raw-radiance composite
    // A probe overlay has claimed giBuffer: it is a measurement, not radiance,
    // and tinting it by the receiver's albedo would make the readout unreadable.
    // Mode 0 is the real resolve and DOES want modulating — probe irradiance is
    // albedo-free.
    if (VK_RT_GIProbeDebugMode() != 0)
        return;
    if (vkRT.giAlbedoModPipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;

    const vkRTImage_t &albedoBuf = vkRT.gbufAlbedo[frameIdx];
    if (albedoBuf.view == VK_NULL_HANDLE)
        return;

    vkRTImage_t &bufA = vkRT.giScratchA[frameIdx];
    vkRTImage_t &bufB = vkRT.giScratchB[frameIdx];
    if (bufA.image == VK_NULL_HANDLE || bufB.image == VK_NULL_HANDLE)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT GI Albedo Mod: skip — scratch images not ready (slot %d)\n", frameIdx);
        return;
    }

    // Scratch selection: whichever of A/B giReadView is NOT currently pointing at.
    // Normally that is giBuffer (the probe resolve's output), matching neither, so
    // A wins; the comparison is what keeps this correct if a pass is ever inserted
    // ahead of it that already left giReadView on one of the pair.
    const bool srcIsA = (vkRT.giReadView[frameIdx] == bufA.view);
    vkRTImage_t &dst = srcIsA ? bufB : bufA;

    const VkRect2D dispatchRect = VK_RT_GI_ComputeDispatchRect(viewDef);
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    // The probe resolve's last barrier targets FRAGMENT_SHADER for the composite;
    // make its result visible to this dispatch's COMPUTE_SHADER read too.
    {
        VkMemoryBarrier srcBarrier = {};
        srcBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        srcBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &srcBarrier, 0, NULL, 0, NULL);
    }

    // Rewritten every call — giReadView/scratch/albedo view all vary frame to frame.
    VkDescriptorSet ds = vkRT.giAlbedoModDescSets[frameIdx];

    VkDescriptorImageInfo srcInfo = {vkRT.giSampler, vkRT.giReadView[frameIdx], VK_IMAGE_LAYOUT_GENERAL};
    // texelFetch in the shader — filter/wrap state is irrelevant; reuse the depth
    // sampler, same convention as gbufNormal reads elsewhere (P9).
    VkDescriptorImageInfo albedoInfo = {vkRT.depthSampler, albedoBuf.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dstInfo = {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &albedoInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = ds;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(vk.device, 3, writes, 0, NULL);

    struct AlbedoModPC
    {
        int32_t screenWidth;
        int32_t screenHeight;
        int32_t scissorOffsetX;
        int32_t scissorOffsetY;
        int32_t scissorExtentX;
        int32_t scissorExtentY;
    } pc;
    static_assert(sizeof(pc) == 24, "AlbedoModPC size mismatch");
    pc.screenWidth = (int32_t)dst.width;
    pc.screenHeight = (int32_t)dst.height;
    pc.scissorOffsetX = (int32_t)dispatchRect.offset.x;
    pc.scissorOffsetY = (int32_t)dispatchRect.offset.y;
    pc.scissorExtentX = (int32_t)dispatchRect.extent.width;
    pc.scissorExtentY = (int32_t)dispatchRect.extent.height;

    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giAlbedoModPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.giAlbedoModPipelineLayout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cmd, vkRT.giAlbedoModPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24, &pc);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Output becomes the new giReadView; composite reads it via FRAGMENT sampling.
    VkMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    dstBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &dstBarrier, 0, NULL, 0, NULL);

    vkRT.giReadView[frameIdx] = dst.view;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT GI Albedo Mod: applied slot=%d dst=%s\n", frameIdx, srcIsA ? "B" : "A");
}
