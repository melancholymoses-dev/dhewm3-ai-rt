/*
===========================================================================

dhewm3-rt Vulkan ray tracing - reflection ray pipeline, denoise and composite.

reflect_ray.rgen traces one GGX-sampled reflection ray per pixel into a per-chain
RGBA16F image (chain 0 opaque, chain 1 glass).  Opaque can trace at half res and be
upsampled; both chains then run a temporal EMA, and opaque a roughness-gated à-trous
filter.  refl_composite.frag adds the result onto the frame once per view;
glass_refl_overlay.frag draws glass per pane.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_upscale.h"
#include "renderer/Vulkan/vk_rt_emitters.h"

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

// r_rtReflections declared in RenderSystem_init.cpp

static idCVar r_rtReflectionDistance("r_rtReflectionDistance", "1000.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                                     "Max reflection ray travel distance in world units (default 1000.0)");

static idCVar r_rtReflectionBlend("r_rtReflectionBlend", "6.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
                                  "Scale factor for reflection radiance, applied in reflect_ray.rgen. Max 10.");

// Specular Reflection and Roughness

idCVar r_rtSpecF0Scale("r_rtSpecF0Scale", "0.35", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                       "Scale factor applied after the power-curve remap of specular-map luminance to F0.\n"
                       "Reduces how much even bright specular texels contribute to reflections.\n"
                       "Tune with r_rtReflectionDebugMode 3 to see the resulting F0.");

idCVar r_rtSpecF0Gamma("r_rtSpecF0Gamma", "2.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                       "Power exponent for remapping raw specular-map luminance to physically plausible F0.\n"
                       "Higher values restrict reflections to only the brightest specular texels (true metal/wet).\n"
                       "Tune with r_rtReflectionDebugMode 3.");

idCVar r_rtReflRoughnessScale("r_rtReflRoughnessScale", "0.6", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                              "Multiplies the packed per-pixel roughness before the GGX lobe samples it.\n"
                              "1.0 = roughness as written to the G-buffer (preview: r_rtReflectionDebugMode 12).",
                              0.0f, 4.0f);

idCVar r_rtReflRoughnessMax("r_rtReflRoughnessMax", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                            "Roughness ceiling for r_rtReflectionMode 3: opaque surfaces rougher than this skip\n"
                            "the reflection ray and take GI alone. r_rtReflectionDebugMode 13 visualizes the\n"
                            "boundary (blue = reflection + GI, green = GI only, white = glass).",
                            0.0f, 1.0f);

idCVar r_rtReflRoughnessFloor("r_rtReflRoughnessFloor", "0.25", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                              "Minimum roughness for any opaque surface's G-buffer entry (r_rtReflectionDebugMode\n"
                              "12 previews it). 0 = Toksvig-only, flat bump maps can read as a perfect mirror.",
                              0.0f, 1.0f);

idCVar r_rtReflectionMinWeight("r_rtReflectionMinWeight", "0.04", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                               "Skip the reflection ray and its shadow rays when the pixel's COARSE Fresnel weight\n"
                               "(fixed-mip specular F0) is below this. Panel-scale, not texel-scale. Glass always\n"
                               "traces.");

idCVar r_rtReflMaxLuminance("r_rtReflMaxLuminance", "6.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                            "Hard luminance ceiling on the incident light an opaque reflection ray returns\n"
                            "(after reflBlend, before Fresnel), independent of r_rtReflAtrousFireflyClamp's\n"
                            "per-pixel outlier test. Scaled down by roughness via r_rtReflRoughLumExp.\n"
                            "0 = unbounded. Glass is unaffected.",
                            0.0f, 64.0f);
static idCVar r_rtReflRoughLumExp(
    "r_rtReflRoughLumExp", "2.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
    "Opaque reflection ceiling = r_rtReflMaxLuminance * (1 - roughness)^this. Stops a\n"
    "rough surface returning a full-brightness LED/lamp from one lucky GGX ray (speckle);\n"
    "dim reflected walls sit under the ceiling and pass unchanged. 0 = no roughness scaling.",
    0.0f, 8.0f);

idCVar r_rtReflShadowPicks("r_rtReflShadowPicks", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                           "Shadow rays per opaque reflection hit. 0 = deterministic loop (up to 8 rays,\n"
                           "lights past the budget unshadowed). 1-2 = importance-sample that many lights\n"
                           "(unbiased, noisier; the denoiser absorbs it). Glass always uses 0.",
                           0, 2);

// Reflection rays march the primary view's froxel scatter grid, so light shafts show in glass.
static idCVar r_rtReflVol("r_rtReflVol", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                          "march the froxel scatter grid along reflection rays, so volumetric light shafts "
                          "appear in reflections (needs r_rtVol 1)");
static idCVar r_rtReflVolSteps("r_rtReflVolSteps", "6", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                               "samples per reflection ray through the froxel scatter grid (1-32). Raise if the "
                               "reflected beam bands; brightness should not change with this.");

// hitT/volSteps alone stretches the step count over the whole segment, so a ray that
// missed (hitT = r_rtReflectionDistance) puts ~167 world units of path length behind one
// trilinear froxel tap. Near-camera cells are small and bright, and grazing rays skim
// through them, so that over-estimate outlined reflective silhouettes in white.
static idCVar r_rtReflVolStepLen("r_rtReflVolStepLen", "48", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                 "max world-unit step for the reflected in-scatter march. Caps how much path "
                                 "length one froxel tap can represent; the march then covers only the first "
                                 "r_rtReflVolSteps * this units of the segment. 0 = uncapped (old behaviour).",
                                 0.0f, 512.0f);

static idCVar r_rtReflVolGain("r_rtReflVolGain", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                              "scale on reflected in-scatter. 1.0 is the literal integral.");

idCVar r_rtReflectionMode("r_rtReflectionMode", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                          "1 = glass only. Reflections are dispatched over the union screen rect of the\n"
                          "    view's SURFTYPE_GLASS surfaces and skipped entirely when the view holds no glass;\n"
                          "    opaque geometry never reflects.\n"
                          "3 = glass + opaque, GGX by roughness (default).\n"
                          "Use r_rtReflections 0 to turn the feature off entirely.",
                          1, 3);

idCVar r_rtReflOpaqueHalfRes("r_rtReflOpaqueHalfRes", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                             "trace opaque reflections at half resolution (one ray per 2x2 block) and "
                             "reconstruct full res with a joint bilateral upsample before denoising. ~1/4 the "
                             "reflection rays. The per-pixel Fresnel weight is still computed at full res, so "
                             "material detail is unaffected. Glass is never half res.");

idCVar r_rtReflDenoise("r_rtReflDenoise", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                       "0 = off (reflections sampled raw).\n"
                       "1 = temporal EMA accumulation.\n"
                       "2 = temporal + roughness-gated à-trous spatial filter (VK_RT_DispatchReflAtrous)",
                       0, 2);
idCVar r_rtReflDenoiseAlpha("r_rtReflDenoiseAlpha", "0.2", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                            "Opaque-surface EMA alpha: 0=use only history, 1=use only current frame. Fixed —\n"
                            "no variance boost, since on rough opaque surfaces high variance is GGX noise,\n"
                            "not motion. Glass uses r_rtReflDenoiseAlphaGlass instead.");
idCVar r_rtReflDenoiseAlphaGlass("r_rtReflDenoiseAlphaGlass", "0.35", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                 "Glass EMA floor. Kept higher than the opaque one: history reprojects with the\n"
                                 "glass surface's motion, not the reflected image's parallax, so a low floor smears\n"
                                 "a sharp mirror reflection under camera motion.");
idCVar r_rtReflAtrousIterations("r_rtReflAtrousIterations", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                                "r_rtReflDenoise 2 only. À-trous passes (0-8); effective radius is 2^N - 1 pixels.", 0,
                                8);
// Plane distance, not raw NDC depth, which stops discriminating past ~20 units.
idCVar r_rtReflAtrousSigmaPlane("r_rtReflAtrousSigmaPlane", "0.02", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                "r_rtReflDenoise 2 only. Edge stop on a tap's distance from the centre pixel's\n"
                                "plane, as a fraction of view distance. Lower = stops sooner at rails and steps.\n"
                                "Tune with r_rtReflectionDebugMode 15.",
                                0.001f, 1.0f);
idCVar r_rtReflAtrousNormalPower("r_rtReflAtrousNormalPower", "4", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                 "r_rtReflDenoise 2 only. Normal edge stop = dot(nCentre, nTap)^this, on the\n"
                                 "bump-mapped normal. Splits creases; too high breaks the wash up along bump\n"
                                 "detail. Tune with r_rtReflectionDebugMode 15.",
                                 0.0f, 64.0f);
idCVar r_rtReflAtrousRoughnessGain("r_rtReflAtrousRoughnessGain", "3.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                   "r_rtReflDenoise 2 only. Multiplies roughness before the spatial filter's mix()\n"
                                   "gate, so moderately rough surfaces reach a full wash sooner than roughness\n"
                                   "alone would. Roughness 0 (glass, screens) is unaffected and always stays sharp.");
idCVar r_rtReflAtrousFireflyClamp("r_rtReflAtrousFireflyClamp", "2.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                                  "Opaque reflections. Each 3x3 tap is capped at this factor times the\n"
                                  "second-brightest tap's luminance, so a lone bright texel (a single-sample\n"
                                  "GGX firefly) drops to its brightest neighbour's level. Applied to the raw\n"
                                  "sample before temporal accumulation (r_rtReflDenoise >= 1) and on every\n"
                                  "à-trous pass (r_rtReflDenoise 2), with no absolute floor.\n"
                                  "1 = remove lone texels outright; higher lets them stand out. 0 disables.");

idCVar r_rtGlassNormalMode("r_rtGlassNormalMode", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                           "Reflection-plane normal on glass.\n"
                           "0 = vertex-interpolated (smooth; patch creases read as curved mirrors).\n"
                           "1 = geometric face normal (flat facets).\n"
                           "2 = crease-aware: face normal on triangles whose vertex normals tilt lopsidedly\n"
                           "    (flat facet beside a smoothed crease), interpolated on symmetric ones (real\n"
                           "    arcs). See r_rtGlassCreaseRatio.",
                           0, 2);
idCVar r_rtGlassCreaseRatio("r_rtGlassCreaseRatio", "0.25", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                            "r_rtGlassNormalMode 2: a glass triangle uses the face normal when its smallest\n"
                            "vertex-normal deviation from the face normal is below this fraction of its largest.",
                            0.0f, 1.0f);

idCVar r_rtReflectionDebugMode(
    "r_rtReflectionDebugMode", "0", CVAR_RENDERER | CVAR_INTEGER,
    "0 = off.\n"
    "2 = output G-buffer world normal as RGB (reflect_ray.rgen) — a grate floor should look\n"
    "    bumpy/detailed, not flat single-colour; silhouette edges must match geometry.\n"
    "3 = output G-buffer F0 (alpha) greyscale (reflect_ray.rgen) — most of the scene near-black,\n"
    "    metal/wet trim grey, nothing solid white.\n"
    "11 = denoiser history age / rejection (needs r_rtReflDenoise >= 1): green = accepted,\n"
    "    brightness = history age; red = rejected this frame (disocclusion or camera cut).\n"
    "12 = per-pixel roughness (gbufAlbedo alpha, Toksvig estimate) as greyscale —\n"
    "    rough concrete should read bright, polished trim and glass dark, and the gradient\n"
    "    should track visible bump detail rather than the diffuse texture.\n"
    "13 = GI/reflection handoff (r_rtReflRoughnessMax): white = glass (always\n"
    "    reflects), blue = opaque taking a reflection ray (and GI — both), green = opaque\n"
    "    over the ceiling or below the F0 floor (GI alone). Forces r_rtReflectionMode 3\n"
    "    regardless of the cvar, so sweeping r_rtReflRoughnessMax moves the blue/green\n"
    "    boundary live.\n"
    "15 = à-trous edge-stop weight (needs r_rtReflDenoise 2): grey level = mean step-1 weight\n"
    "    over traced neighbours, white = blends freely, black = stopped. Floors should be\n"
    "    white, rails and creases outlined dark. Yellow = no traced neighbour, grey = glass,\n"
    "    dark blue = untraced. Tune r_rtReflAtrousSigmaPlane/NormalPower here.\n"
    "Modes 2, 3, 12 and 13 require r_rtReflections 1 — they ride on the reflection ray\n"
    "dispatch, force the full-screen launch grid, and are displayed via refl_composite.frag\n"
    "with blending disabled (replace, not add). The per-surface glass overlay is suppressed\n"
    "while they are active.");

// One place decides which modes exist; a retired number must not switch on the debug
// path (forced mode 3, no glass overlay, replace-blend composite).
int VK_RT_ValidReflDebugMode(void)
{
    switch (r_rtReflectionDebugMode.GetInteger())
    {
    case 2:
    case 3:
    case 11:
    case 12:
    case 13:
    case 15:
        return r_rtReflectionDebugMode.GetInteger();
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// UBO layout matching reflect_ray.rgen ReflParams block
//
//   mat4   invViewProj  offset  0  size 64
//   float  maxDist      offset 64  size  4
//   uint   frameIndex   offset 68  size  4
//   ivec2  screenSize   offset 72  size  8  (ivec2 std140 align=8 → 72 is fine)
//   float  reflBlend      offset 80  size  4  (r_rtReflectionBlend, applied at imageStore)
//   float  roughnessScale offset 84  size  4  (r_rtReflRoughnessScale)
//   int    debugMode      offset 88  size  4  (r_rtReflectionDebugMode)
//   int    sceneHasGlass  offset 92  size  4  (skip glass probe when 0)
//   float  minWeight      offset 96  size  4  (r_rtReflectionMinWeight)
//   int    rectOriginX    offset 100 size  4  (launch-grid origin)
//   int    rectOriginY    offset 104 size  4
//   int    reflMode       offset 108 size  4  (r_rtReflectionMode — 1, 3)
//   int    emitterCount   offset 112 size  4  (vk_rt_emitters.cpp, 0 = none)
//   float  glowGain       offset 116 size  4  (r_rtReflGlowGain, 0 when r_rtReflGlow is off)
//   int    glowTriCount   offset 120 size  4  (vk_rt_emitters.cpp glow triangles)
//   float  roughLumExp    offset 124 size  4  (r_rtReflRoughLumExp)
//   mat4   viewProj       offset 128 size 64  (froxel grid's own, GL convention)
//   vec4   froxelCamPos   offset 192 size 16  (xyz)
//   vec4   froxelCamFwd   offset 208 size 16  (xyz)
//   float  froxelDNear    offset 224 size  4
//   float  froxelLogRange offset 228 size  4
//   float  froxelInvLogRange offset 232 size 4
//   int    froxelNz       offset 236 size  4
//   int    volSteps       offset 240 size  4  (0 = no volume bound this frame)
//   float  volGain        offset 244 size  4
//   float  volIsoMix      offset 248 size  4  (r_rtVolIsotropicMix)
//   int    glassNormalMode offset 252 size 4  (r_rtGlassNormalMode)
//   float  glassCreaseRatio offset 256 size 4  (r_rtGlassCreaseRatio)
//   float  guiReflGain    offset 260 size  4  (r_rtGuiReflGain)
//   float  roughnessMax   offset 264 size  4  (r_rtReflRoughnessMax)
//   float  maxLuminance   offset 268 size  4  (r_rtReflMaxLuminance)
//   int    shadowPicks    offset 272 size  4  (r_rtReflShadowPicks)
//   int    population     offset 276 size  4
//   int    glassRectX     offset 280 size  4
//   int    glassRectY     offset 284 size  4
//   int    glassRectW     offset 288 size  4
//   int    glassRectH     offset 292 size  4
//   int    reflScale      offset 296 size  4  (1 full res, 2 half res)
//   float  volStepLen     offset 300 size  4  (r_rtReflVolStepLen)
//   total: 304 bytes — std140 rounds the block up to a multiple of
//   16, and uboInfo.range is sizeof(ReflParamsUBO), which must not be smaller.
// ---------------------------------------------------------------------------

struct ReflParamsUBO
{
    float invViewProj[16];
    float maxDist;
    uint32_t frameIndex;
    int32_t screenWidth;
    int32_t screenHeight;
    float reflBlend;       // r_rtReflectionBlend — baked into imageStore in rgen
    float roughnessScale;  // r_rtReflRoughnessScale
    int32_t debugMode;     // r_rtReflectionDebugMode — 2/3/4 handled in rgen
    int32_t sceneHasGlass; // 0 when no REAL_GLASS geometry in the TLAS
    float minWeight;       // r_rtReflectionMinWeight — pre-trace Fresnel cull
    int32_t rectOriginX;   // launch-grid origin; rgen adds this to gl_LaunchIDEXT
    int32_t rectOriginY;
    int32_t reflMode;     // r_rtReflectionMode — 1 glass-only, 3 glass+opaque
    int32_t emitterCount; // projectile glow discs in the emitter SSBO (binding 6)
    float glowGain;       // r_rtReflGlowGain
    int32_t glowTriCount; // additive translucent triangles after the discs in binding 6
    float roughLumExp;    // r_rtReflRoughLumExp — reuses the pre-mat4 slot retired with grazingGain

    // Froxel scatter grid. Mirrors
    float viewProj[16];
    float froxelCamPos[4];
    float froxelCamFwd[4];
    float froxelDNear;
    float froxelLogRange;
    float froxelInvLogRange;
    int32_t froxelNz;
    int32_t volSteps; // 0 = no usable grid this frame; the rgen takes no samples
    float volGain;
    float volIsoMix;         // r_rtVolIsotropicMix — for rt_PhaseFromMeanDir
    int32_t glassNormalMode; // r_rtGlassNormalMode — 0 interpolated, 1 face, 2 crease-aware
    float glassCreaseRatio;  // r_rtGlassCreaseRatio — mode 2 flattens when devRatio < this
    float guiReflGain;       // r_rtGuiReflGain — scales live GUI screen hits (MAT_FLAG_GUI_LIVE)
    float roughnessMax;      // r_rtReflRoughnessMax
    float maxLuminance;      // r_rtReflMaxLuminance
    int32_t shadowPicks;     // r_rtReflShadowPicks — 0 deterministic, 1-2 stochastic (opaque only)

    // Which population this dispatch owns. See REFL_POP_* below.
    int32_t population;
    // Union of the SURFTYPE_GLASS scissors, in the same pixels as the launch grid.
    // REFL_POP_OPAQUE fires its glass-suppression probe only inside this rect —
    // outside it no glass can exist, so the probe can only miss. Extent 0 = no glass.
    int32_t glassRectX;
    int32_t glassRectY;
    int32_t glassRectW;
    int32_t glassRectH;
    // reflImage is 1/reflScale the G-buffer's size. 1 full res, 2 half res.
    int32_t reflScale;
    float volStepLen; // r_rtReflVolStepLen — caps the volumetric march step length
};

// Which surfaces a reflect_ray.rgen dispatch is responsible for (ubo.population).
// BOTH is a single dispatch for both populations, used by every debug mode,
// so those keep one code path and one image.
#define REFL_POP_BOTH 0
#define REFL_POP_GLASS 1
#define REFL_POP_OPAQUE 2
static_assert(sizeof(ReflParamsUBO) == 304, "ReflParamsUBO size mismatch");
// This UBO comes from the shared per-frame ring, whose stride is INTERACTION_UBO_SIZE
// (384, vk_backend.cpp). Outgrowing a slot would silently scribble over the next one.
static_assert(sizeof(ReflParamsUBO) <= 384, "ReflParamsUBO exceeds the UBO ring stride");

// ---------------------------------------------------------------------------
// UBO layout matching refl_temporal_resolve.comp's ReflTemporalParams block.
// Two mat4s alone exceed the guaranteed 128-byte push-constant minimum, hence
// a UBO (same VK_AllocUBOForShadow dynamic-UBO path ReflParamsUBO uses above).
//
//   mat4  invViewProj        offset   0  size 64  (current frame, GL convention)
//   mat4  prevViewProj       offset  64  size 64  (previous frame, GL convention —
//                                                   viewDef->prevViewProjMatrix)
//   int   screenWidth        offset 128  size  4
//   int   screenHeight       offset 132  size  4
//   float baseAlpha          offset 136  size  4  (r_rtReflDenoiseAlpha)
//   float varianceGain       offset 140  size  4
//   float depthRejectThresh  offset 144  size  4
//   float normalRejectCos    offset 148  size  4
//   int   scissorOffsetX     offset 152  size  4
//   int   scissorOffsetY     offset 156  size  4
//   int   scissorExtentX     offset 160  size  4
//   int   scissorExtentY     offset 164  size  4
//   int   resetHistory       offset 168  size  4  (camera cut or first use)
//   int   debugMode          offset 172  size  4  (r_rtReflectionDebugMode)
//   int   maxAge             offset 176  size  4
//   float glassAlpha         offset 180  size  4  (r_rtReflDenoiseAlphaGlass)
//   float fireflyClamp       offset 184  size  4  (r_rtReflAtrousFireflyClamp)
//   float _pad0[1]           offset 188  size  4
//   total: 192 bytes, already a multiple of 16.
// ---------------------------------------------------------------------------

struct ReflTemporalUBO
{
    float invViewProj[16];
    float prevViewProj[16];
    int32_t screenWidth;
    int32_t screenHeight;
    float baseAlpha;
    float varianceGain;
    float depthRejectThresh;
    float normalRejectCos;
    int32_t scissorOffsetX;
    int32_t scissorOffsetY;
    int32_t scissorExtentX;
    int32_t scissorExtentY;
    int32_t resetHistory;
    int32_t debugMode;
    int32_t maxAge;
    float glassAlpha;
    float fireflyClamp;
    float _pad0[1];
};
static_assert(sizeof(ReflTemporalUBO) == 192, "ReflTemporalUBO size mismatch");

// ---------------------------------------------------------------------------
// Forward declarations (defined in other files)
// ---------------------------------------------------------------------------

extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);
extern VkShaderModule VK_LoadSPIRV(const char *path);
extern bool VK_AllocUBOForShadow(VkBuffer *outBuf, uint32_t *outOffset, void **outMapped);
// GILightBuffer's arrays are fixed-size in the GLSL mirror (rt_light_eval.glsl), so the
// null-light fallback must be the full size, not just the header.
extern uint32_t VK_RT_GetGILightBufferSize(void);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;
extern idCVar r_rtReflGlow;        // vk_rt_emitters.cpp
extern idCVar r_rtReflGlowGain;    // vk_rt_emitters.cpp
extern idCVar r_rtVolIsotropicMix; // vk_vol.cpp — shared phase parameter

// ---------------------------------------------------------------------------
// Null light SSBO — zeroed buffer with numLights = 0, bound when the GI light
// SSBO is not yet available (GI not initialised, first frame, etc.). Sized to
// match GILightBuffer exactly (not just its header) because RTLightBuf's
// lights[]/cookies[] are fixed-size arrays in the GLSL mirror — a smaller
// buffer bound here would be undersized against the shader's declared block.
// The rchit shader checks numLights <= 0 and falls back to raw albedo before
// ever indexing lights[]/cookies[], so the zeroed body is never read.
// ---------------------------------------------------------------------------
static VkBuffer s_nullLightSsbo = VK_NULL_HANDLE;
static VkDeviceMemory s_nullLightSsboMemory = VK_NULL_HANDLE;

static void VK_RT_CreateNullLightSsbo()
{
    if (s_nullLightSsbo != VK_NULL_HANDLE)
        return;

    const VkDeviceSize bufSize = VK_RT_GetGILightBufferSize();

    VK_CreateBuffer(bufSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s_nullLightSsbo,
                    &s_nullLightSsboMemory);

    void *mapped;
    vkMapMemory(vk.device, s_nullLightSsboMemory, 0, bufSize, 0, &mapped);
    memset(mapped, 0, bufSize); // numLights = 0 (first 4 bytes); rest never read
    vkUnmapMemory(vk.device, s_nullLightSsboMemory);
}

static void VK_RT_DestroyNullLightSsbo()
{
    if (s_nullLightSsbo != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, s_nullLightSsbo, NULL);
        s_nullLightSsbo = VK_NULL_HANDLE;
    }
    if (s_nullLightSsboMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, s_nullLightSsboMemory, NULL);
        s_nullLightSsboMemory = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Null G-buffer normal/F0 image: 1x1 cleared to (0.5, 0.5, 1.0, 0.0), bound when there is
// no real gbufNormal.  B == 1.0 is the "never written" sentinel (rt_gbuf_normal_fmt.glsl)
// and alpha 0 the "no data" one, so consumers reconstruct normals.  Shared with AO and GI
// via VK_RT_GetNullGbufNormalView().
// ---------------------------------------------------------------------------
static VkImage s_nullGbufImage = VK_NULL_HANDLE;
static VkDeviceMemory s_nullGbufMemory = VK_NULL_HANDLE;
static VkImageView s_nullGbufView = VK_NULL_HANDLE;

static void VK_RT_CreateNullGbufNormal(void)
{
    if (s_nullGbufImage != VK_NULL_HANDLE)
        return;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {1, 1, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &s_nullGbufImage));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vk.device, s_nullGbufImage, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = VK_FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &s_nullGbufMemory));
    VK_CHECK(vkBindImageMemory(vk.device, s_nullGbufImage, s_nullGbufMemory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = s_nullGbufImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &s_nullGbufView));

    // One-time transition + clear, same manual command-buffer pattern as VK_RT_CreateReflImages.
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

        VkImageMemoryBarrier toTransferDst = {};
        toTransferDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransferDst.srcAccessMask = 0;
        toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransferDst.image = s_nullGbufImage;
        toTransferDst.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &toTransferDst);

        VkClearColorValue clearVal = {};
        clearVal.float32[0] = 0.5f;
        clearVal.float32[1] = 0.5f;
        clearVal.float32[2] = 1.0f;
        clearVal.float32[3] = 0.0f;
        vkCmdClearColorImage(tmpCmd, s_nullGbufImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &subRange);

        VkImageMemoryBarrier toShaderRead = toTransferDst;
        toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                             NULL, 0, NULL, 1, &toShaderRead);

        vkEndCommandBuffer(tmpCmd);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &tmpCmd;
        VK_CHECK(vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
        VK_Prof_QueueWaitIdle(VKPROF_WAIT_REFLECTIONS, vk.graphicsQueue);
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
    }
}

// Shared with AO and GI.  Idempotent; their Init calls it so the one-time submit+wait
// never lands mid-frame.  RT shutdown tears all three down together.
VkImageView VK_RT_GetNullGbufNormalView(void)
{
    VK_RT_CreateNullGbufNormal();
    return s_nullGbufView;
}

static void VK_RT_DestroyNullGbufNormal(void)
{
    if (s_nullGbufView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(vk.device, s_nullGbufView, NULL);
        s_nullGbufView = VK_NULL_HANDLE;
    }
    if (s_nullGbufImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(vk.device, s_nullGbufImage, NULL);
        s_nullGbufImage = VK_NULL_HANDLE;
    }
    if (s_nullGbufMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, s_nullGbufMemory, NULL);
        s_nullGbufMemory = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// The chain every full-screen consumer reads (composite and all debug modes).
// ---------------------------------------------------------------------------
// Chain 0 owns opaque surfaces; chain 1 owns glass.
static vkReflChain_t *VK_RT_ReflChain(void)
{
    return &vkRT.reflChain[0];
}

static vkReflChain_t *VK_RT_ReflGlassChain(void)
{
    return &vkRT.reflChain[VK_REFL_CHAIN_COUNT > 1 ? 1 : 0];
}

// Set by VK_RT_DispatchReflections: true when glass was traced into its own chain this
// slot, false when one REFL_POP_BOTH dispatch put both populations in chain 0. The glass
// overlay has to read whichever image actually holds the pane's radiance.
static bool s_reflSplitActive[VK_MAX_FRAMES_IN_FLIGHT] = {};

// ---------------------------------------------------------------------------
// VK_RT_CreateReflImages
// Allocates per-frame RGBA16F reflection buffers at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateReflImages(vkReflChain_t *chain, uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &rb = chain->trace[i];
        rb.width = width;
        rb.height = height;

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
        VK_CHECK(vkCreateImage(vk.device, &imgInfo, NULL, &rb.image));

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(vk.device, rb.image, &memReq);

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
            common->Error("VK RT Refl: no device-local memory type for reflection image");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIdx;
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &rb.memory));
        VK_CHECK(vkBindImageMemory(vk.device, rb.image, rb.memory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = rb.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &rb.view));

        // Transition UNDEFINED → GENERAL so rgen can imageStore on first dispatch
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

            // Clear to black at creation like every other RT image, for frames where the
            // dispatch is skipped.
            VkImageSubresourceRange subRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = rb.image;
            barrier.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            VkClearColorValue clearBlack = {};
            vkCmdClearColorImage(tmpCmd, rb.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

            VkImageMemoryBarrier barrier2 = {};
            barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier2.image = rb.image;
            barrier2.subresourceRange = subRange;
            vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                 0, 0, NULL, 0, NULL, 1, &barrier2);

            vkEndCommandBuffer(tmpCmd);

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &tmpCmd;
            int submitResult = vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            if (submitResult != 0)
            {
                common->Printf("VK: vkQueueSubmit failed with error %d in VK_RT_CreateReflImages\n", submitResult);
                fflush(NULL);
            }

            VK_Prof_QueueWaitIdle(VKPROF_WAIT_REFLECTIONS, vk.graphicsQueue);
            vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
        }
    }
}

static void VK_RT_DestroyReflImages(vkReflChain_t *chain)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &rb = chain->trace[i];
        if (rb.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk.device, rb.view, NULL);
            rb.view = VK_NULL_HANDLE;
        }
        if (rb.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk.device, rb.image, NULL);
            rb.image = VK_NULL_HANDLE;
        }
        if (rb.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk.device, rb.memory, NULL);
            rb.memory = VK_NULL_HANDLE;
        }
        rb.width = 0;
        rb.height = 0;
    }
}

// ---------------------------------------------------------------------------
// Reflection temporal denoiser images
//
// Shared, not per slot, like aoHistory: the EMA is sequential across frames on one queue.
// ---------------------------------------------------------------------------

static bool VK_RT_AllocReflAuxImage(vkRTImage_t &img, uint32_t width, uint32_t height, VkImageUsageFlags usage)
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
    imgInfo.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
        common->Warning("VK RT Refl: no device-local memory type for denoiser aux image");
        vkDestroyImage(vk.device, img.image, NULL);
        img.image = VK_NULL_HANDLE;
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

    // UNDEFINED -> GENERAL and clear to black.  ALL_COMMANDS because compute and fragment
    // passes both read the history.
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

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = img.image;
        barrier.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        VkClearColorValue clearBlack = {};
        vkCmdClearColorImage(tmpCmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &clearBlack, 1, &subRange);

        VkImageMemoryBarrier barrier2 = {};
        barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier2.image = img.image;
        barrier2.subresourceRange = subRange;
        vkCmdPipelineBarrier(tmpCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier2);

        vkEndCommandBuffer(tmpCmd);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &tmpCmd;
        VK_CHECK(vkQueueSubmit(vk.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
        VK_Prof_QueueWaitIdle(VKPROF_WAIT_REFLECTIONS, vk.graphicsQueue);
        vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &tmpCmd);
    }

    return true;
}

static void VK_RT_FreeReflAuxImage(vkRTImage_t &img)
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

static void VK_RT_CreateReflDenoiseImages(vkReflChain_t *chain, uint32_t width, uint32_t height)
{
    // history feeds the composite/overlay; moments only this file's compute pass.  Both are
    // ping-ponged so the reprojected read never races this frame's write.
    for (int i = 0; i < 2; i++)
    {
        if (!VK_RT_AllocReflAuxImage(chain->history[i], width, height,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl[%s]: failed to allocate history[%d]", chain->name, i);
        if (!VK_RT_AllocReflAuxImage(chain->moments[i], width, height, VK_IMAGE_USAGE_STORAGE_BIT))
            common->Warning("VK RT Refl[%s]: failed to allocate moments[%d]", chain->name, i);
    }

    chain->pingPong = 0;
    chain->historyValid = false;
    chain->prevCamPos.Zero();
    chain->prevCamFwd.Zero();
}

static void VK_RT_DestroyReflDenoiseImages(vkReflChain_t *chain)
{
    for (int i = 0; i < 2; i++)
    {
        VK_RT_FreeReflAuxImage(chain->history[i]);
        VK_RT_FreeReflAuxImage(chain->moments[i]);
    }
    chain->historyValid = false;
}

// The rgen's half-res target: quarter the area of trace[], only for halfResCapable chains.
// STORAGE for the rgen, SAMPLED for refl_upsample.comp.
static void VK_RT_CreateReflHalfImages(vkReflChain_t *chain, uint32_t width, uint32_t height)
{
    if (!chain->halfResCapable)
        return;

    const uint32_t hw = Max(1u, (width + 1) / 2);
    const uint32_t hh = Max(1u, (height + 1) / 2);

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocReflAuxImage(chain->traceHalf[i], hw, hh,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl[%s]: failed to allocate traceHalf[%d] — half res unavailable", chain->name, i);
    }
}

static void VK_RT_DestroyReflHalfImages(vkReflChain_t *chain)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        VK_RT_FreeReflAuxImage(chain->traceHalf[i]);
}

static void VK_RT_CreateReflAtrousImages(vkReflChain_t *chain, uint32_t width, uint32_t height)
{
    // A chain that never runs the filter has no use for the scratch — two full-screen
    // RGBA16F images per frame slot is real memory to leave unallocated.
    if (!chain->spatialFilter)
        return;

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocReflAuxImage(chain->atrousA[i], width, height,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl Atrous[%s]: failed to allocate atrousA[%d]", chain->name, i);
        if (!VK_RT_AllocReflAuxImage(chain->atrousB[i], width, height,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl Atrous[%s]: failed to allocate atrousB[%d]", chain->name, i);
        chain->atrousReadView[i] = VK_NULL_HANDLE;
    }
}

static void VK_RT_DestroyReflAtrousImages(vkReflChain_t *chain)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeReflAuxImage(chain->atrousA[i]);
        VK_RT_FreeReflAuxImage(chain->atrousB[i]);
        chain->atrousReadView[i] = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflPipeline
// Creates the reflection RT pipeline, SBT, descriptor sets, and sampler.
// ---------------------------------------------------------------------------

static void VK_RT_InitReflPipeline(void)
{
    // --- Descriptor set layout ---
    // binding 0: TLAS
    // binding 1: reflection RGBA16F storage image
    // binding 2: depth sampler (COMBINED_IMAGE_SAMPLER)
    // binding 3: reflection params UBO (dynamic)
    // binding 4: GI light SSBO — used by rchit to evaluate per-light irradiance
    //            at the reflection hit point without a full G-buffer.
    // binding 5: G-buffer normal/F0 sampler; the null image when unsupported.
    // binding 6: projectile emitter SSBO (vk_rt_emitters.cpp), one per frame slot.
    // binding 7: froxel scatter grid, sampler3D — in-scatter along the reflected ray.
    // binding 8: froxel mean-direction grid, sampler3D, paired with 7.
    // binding 9: G-buffer albedo sampler — alpha only (class + roughness) for the GGX lobe
    //            and debug mode 12; the null image when unsupported.
    VkDescriptorSetLayoutBinding bindings[10] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    // rchit also needs TLAS access to fire shadow rays toward lights.
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
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; // rchit reads lights

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 10;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.reflDescLayout));

    // --- Pipeline layout ---
    // set=0: per-frame resources (TLAS, storage image, depth, UBO)
    // set=1: material table (MatTable, VtxAddrTable, IdxAddrTable, bindless textures)
    VkDescriptorSetLayout reflLayouts[2] = {vkRT.reflDescLayout, vkRT.matDescLayout};
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts = reflLayouts;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.reflPipelineLayout));

    // --- Shader modules ---
    VkShaderModule rgenModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rgen.spv");
    VkShaderModule rmissModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rmiss.spv");
    VkShaderModule rchitModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rchit.spv");
    VkShaderModule rahitModule = VK_LoadSPIRV("glprogs/glsl/reflect_ray.rahit.spv");
    VkShaderModule probeAhitModule = VK_LoadSPIRV("glprogs/glsl/glass_probe.rahit.spv");
    VkShaderModule probeMissModule = VK_LoadSPIRV("glprogs/glsl/glass_probe.rmiss.spv");
    VkShaderModule probeChitModule = VK_LoadSPIRV("glprogs/glsl/glass_probe.rchit.spv");
    VkShaderModule playerChitModule = VK_LoadSPIRV("glprogs/glsl/player_reflect.rchit.spv");
    // Reuse GI shadow miss: fires when a shadow ray from rchit reaches a light without being blocked.
    VkShaderModule shadowMissModule = VK_LoadSPIRV("glprogs/glsl/gi_shadow.rmiss.spv");

    if (rgenModule == VK_NULL_HANDLE || rmissModule == VK_NULL_HANDLE || rchitModule == VK_NULL_HANDLE ||
        rahitModule == VK_NULL_HANDLE || probeMissModule == VK_NULL_HANDLE || probeAhitModule == VK_NULL_HANDLE ||
        probeChitModule == VK_NULL_HANDLE || playerChitModule == VK_NULL_HANDLE || shadowMissModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Refl: failed to load reflection shader modules — reflections disabled");
        if (rgenModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rgenModule, NULL);
        if (rmissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rmissModule, NULL);
        if (rchitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rchitModule, NULL);
        if (rahitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, rahitModule, NULL);
        if (probeMissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, probeMissModule, NULL);
        if (probeAhitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, probeAhitModule, NULL);
        if (probeChitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, probeChitModule, NULL);
        if (playerChitModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, playerChitModule, NULL);
        if (shadowMissModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(vk.device, shadowMissModule, NULL);
        return;
    }

    // --- Shader stages ---
    // Stage 0: rgen
    // Stage 1: main rmiss
    // Stage 2: main rchit           — world geometry reflection hits (fires shadow rays)
    // Stage 3: main rahit           — alpha-discard for non-opaque world geometry
    // Stage 4: glass probe rmiss
    // Stage 5: glass probe rchit    — records glass hit position/normal for rgen
    // Stage 6: player_reflect rchit — player body: close-range pass-through, mirror-distance visible
    // Stage 7: glass probe rahit    — record glass hit any
    // Stage 8: shadow miss          — clears occluded when shadow ray reaches a light unblocked
    VkPipelineShaderStageCreateInfo stages[9] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = "main";

    stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[3].module = rahitModule;
    stages[3].pName = "main";

    stages[4].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[4].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[4].module = probeMissModule;
    stages[4].pName = "main";

    stages[5].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[5].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[5].module = probeChitModule;
    stages[5].pName = "main";

    stages[6].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[6].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[6].module = playerChitModule;
    stages[6].pName = "main";

    stages[7].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[7].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[7].module = probeAhitModule;
    stages[7].pName = "main";

    stages[8].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[8].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[8].module = shadowMissModule;
    stages[8].pName = "main";

    // --- Shader groups ---
    // Group 0: rgen
    // Group 1: main miss            (miss index 0 in miss region)
    // Group 2: glass probe miss     (miss index 1 in miss region)
    // Group 3: shadow miss          (miss index 2 in miss region) — used by rchit shadow rays
    //
    // Hit region — indexed by instanceShaderBindingTableRecordOffset + sbtHitOffset:
    //
    //   instanceSBTOffset=0 (world geometry):
    //     + sbtHitOffset 0 → hit slot 0 → Group 4: world main hit
    //     + sbtHitOffset 1 → hit slot 1 → Group 5: world glass probe hit
    //
    //   instanceSBTOffset=2 (player body, rtPlayer):
    //     + sbtHitOffset 0 → hit slot 2 → Group 6: player main hit
    //     + sbtHitOffset 1 → hit slot 3 → Group 7: player glass probe hit (never fires — mask 0xFD excludes player)
    //
    // SBT memory layout:
    //   [0]=rgen  [1]=main_miss  [2]=probe_miss  [3]=shadow_miss  [4]=world_main  [5]=world_probe  [6]=player_main
    //   [7]=player_probe
    // reflRgenRegion → [0] reflMissRegion → [1..3] reflHitRegion → [4..7]
    VkRayTracingShaderGroupCreateInfoKHR groups[8] = {};

    // Group 0: rgen
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 1: main miss
    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: glass probe miss
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 4; // stage 4 = probeMissModule
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 3: shadow miss — gi_shadow.rmiss (stage 8), miss index 2.
    // Fires when a shadow ray from rchit reaches a light without being blocked — sets occluded=false.
    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 8; // gi_shadow.rmiss
    groups[3].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 4: world main hit — rchit (stage 2) + rahit (stage 3)
    groups[4].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].generalShader = VK_SHADER_UNUSED_KHR;
    groups[4].closestHitShader = 2; // reflect_ray.rchit
    groups[4].anyHitShader = 3;     // reflect_ray.rahit — alpha-discard
    groups[4].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 5: world glass probe hit — glass_probe.rchit (stage 5) + glass_probe.rahit (stage 7).
    // glass_probe.rahit adds MAT_FLAG_PLAYER_BODY skip on top of alpha-test logic so non-opaque
    // player surfaces (perforated arm/weapon textures) are ignored by the probe.  Combined with
    // the 0xFD cullMask in rgen this gives two layers of player exclusion.
    groups[5].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[5].generalShader = VK_SHADER_UNUSED_KHR;
    groups[5].closestHitShader = 5; // glass_probe.rchit
    groups[5].anyHitShader = 7;     // glass_probe.rahit — player-body skip + alpha-discard
    groups[5].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 6: player main hit — player_reflect.rchit (stage 6), no rahit needed (player is opaque).
    // Close-range hits pass through; mirror-distance hits return the player's diffuse colour.
    groups[6].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[6].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[6].generalShader = VK_SHADER_UNUSED_KHR;
    groups[6].closestHitShader = 6; // player_reflect.rchit
    groups[6].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[6].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 7: player glass probe hit — placeholder, never invoked.
    // Glass probe uses mask 0xFD which excludes player instances (mask 0x02).
    // This slot must exist in the SBT so the indexing arithmetic stays valid.
    // Reuse glass_probe.rchit + main rahit as safe defaults.
    groups[7].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[7].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[7].generalShader = VK_SHADER_UNUSED_KHR;
    groups[7].closestHitShader = 5; // glass_probe.rchit (safe reuse — never fires)
    groups[7].anyHitShader = 7;     // glass_probe.rahit  (safe reuse — never fires)
    groups[7].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Stages (9) vs groups (8): stages can be shared across groups, so the counts can differ.
    // Stage 7 (glass_probe.rahit) is referenced by groups 5 and 7 — no extra group needed.

    // --- RT pipeline ---
    VkRayTracingPipelineCreateInfoKHR rtPipeInfo = {};
    rtPipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtPipeInfo.stageCount = 9;
    rtPipeInfo.pStages = stages;
    rtPipeInfo.groupCount = 8;
    rtPipeInfo.pGroups = groups;
    // Depth 2: reflection ray (from rgen) + shadow ray (from rchit). Glass probe is sequential
    // from rgen and does not add recursion depth.
    rtPipeInfo.maxPipelineRayRecursionDepth = 2;
    rtPipeInfo.layout = vkRT.reflPipelineLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipeInfo, NULL,
                                            &vkRT.reflPipeline));

    vkDestroyShaderModule(vk.device, rgenModule, NULL);
    vkDestroyShaderModule(vk.device, rmissModule, NULL);
    vkDestroyShaderModule(vk.device, rchitModule, NULL);
    vkDestroyShaderModule(vk.device, rahitModule, NULL);
    vkDestroyShaderModule(vk.device, probeMissModule, NULL);
    vkDestroyShaderModule(vk.device, probeAhitModule, NULL);
    vkDestroyShaderModule(vk.device, probeChitModule, NULL);
    vkDestroyShaderModule(vk.device, playerChitModule, NULL);
    vkDestroyShaderModule(vk.device, shadowMissModule, NULL);

    // --- Shader Binding Table ---
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
    // Layout: [0]=rgen [1]=main_miss [2]=probe_miss [3]=shadow_miss [4]=world_main [5]=world_probe [6]=player_main
    // [7]=player_probe
    uint32_t sbtSize = 8 * stride;

    VK_CreateBuffer(sbtSize,
                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vkRT.sbtReflBuffer,
                    &vkRT.sbtReflMemory);

    uint8_t *handles = (uint8_t *)alloca(8 * handleSize);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, vkRT.reflPipeline, 0, 8, 8 * handleSize, handles));

    uint8_t *sbtData;
    VK_CHECK(vkMapMemory(vk.device, vkRT.sbtReflMemory, 0, sbtSize, 0, (void **)&sbtData));
    for (int i = 0; i < 8; i++)
        memcpy(sbtData + i * stride, handles + i * handleSize, handleSize);
    vkUnmapMemory(vk.device, vkRT.sbtReflMemory);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = vkRT.sbtReflBuffer;
    VkDeviceAddress sbtBase = vkGetBufferDeviceAddressKHR(vk.device, &addrInfo);

    // rgen: slot 0.  miss region: slots 1-3 (main miss, probe miss, shadow miss).
    // hit region: slots 4-7 (world main, world probe, player main, player probe).
    vkRT.reflRgenRegion = {sbtBase + 0 * stride, stride, stride};
    vkRT.reflMissRegion = {sbtBase + 1 * stride, stride, 3 * stride};
    vkRT.reflHitRegion = {sbtBase + 4 * stride, stride, 4 * stride};
    vkRT.reflCallRegion = {0, 0, 0};

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl SBT: stride=%u sbtBytes=%u base=0x%llx (8 groups) miss=0x%llx (missRecords=%u) "
                       "hit=0x%llx (hitRecords=%u)\n",
                       stride, sbtSize, (unsigned long long)sbtBase,
                       (unsigned long long)vkRT.reflMissRegion.deviceAddress,
                       (unsigned)(vkRT.reflMissRegion.size / vkRT.reflMissRegion.stride),
                       (unsigned long long)vkRT.reflHitRegion.deviceAddress,
                       (unsigned)(vkRT.reflHitRegion.size / vkRT.reflHitRegion.stride));

    // --- Descriptor pool and sets ---
    // One set per frame slot PER CHAIN — the chains differ in their storage image.
    const uint32_t traceSets = VK_MAX_FRAMES_IN_FLIGHT * VK_REFL_CHAIN_COUNT;
    VkDescriptorPoolSize poolSizes[5] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, traceSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, traceSets},
        // depth + gbufNormal + froxelScatter + froxelDir + gbufAlbedo
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, traceSets * 5},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, traceSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, traceSets * 2}, // GI light SSBO + emitters
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = traceSets;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.reflDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflDescLayout;
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
    {
        vkReflChain_t *chain = &vkRT.reflChain[c];
        VkDescriptorSetAllocateInfo dsAlloc = {};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = vkRT.reflDescPool;
        dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
        dsAlloc.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, chain->traceDescSets));
        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
            chain->traceDescSetLastUpdated[i] = -1;
    }

    // --- Reflection buffer sampler (linear-clamp, used by interaction shader) ---
    if (vkRT.reflSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &vkRT.reflSampler));
    }

    VK_RT_CreateNullLightSsbo();
    VK_RT_CreateNullGbufNormal();
    VK_RT_InitReflEmitters();

    common->Printf("VK RT Refl: pipeline initialized\n");
}

// Defined further below, after VK_RT_DispatchReflections.
static void VK_RT_InitReflCompositePipeline(void);

// ---------------------------------------------------------------------------
// VK_RT_InitReflTemporalPipeline
// Compute pipeline for refl_temporal_resolve.comp. Mirrors
// vk_temporal.cpp's VK_RT_InitTemporalPipeline (AO's EMA pass), with three
// storage images instead of two and a UBO instead of a push-constant block
// (see ReflTemporalUBO's comment for why).
// ---------------------------------------------------------------------------

static void VK_RT_InitReflTemporalPipeline(void)
{
    // binding 0: currentImage  (chain->trace[slot], readonly)
    // binding 1: srcHistory    (chain->history[ping], readonly — last frame's result)
    // binding 2: dstHistory    (chain->history[pong], writeonly — this frame's result)
    // binding 3: srcMoments    (chain->moments[ping], readonly)
    // binding 4: dstMoments    (chain->moments[pong], writeonly)
    // binding 5: depthSampler
    // binding 6: gbufNormalSampler
    // binding 7: motionSampler (motionVectors[slot])
    // binding 8: ReflTemporalUBO (dynamic)
    //
    // Separate src/dst: the pass reprojects (reads Q, writes P), so one read-write image
    // would race between invocations.
    VkDescriptorSetLayoutBinding bindings[9] = {};

    for (int i = 0; i < 5; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    for (int i = 5; i < 8; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 9;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.reflTemporalDescLayout));

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.reflTemporalDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.reflTemporalPipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/refl_temporal_resolve.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Refl: failed to load refl_temporal_resolve.comp.spv — denoiser disabled");
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
    pipeCI.layout = vkRT.reflTemporalPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.reflTemporalPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    const uint32_t temporalSets = VK_MAX_FRAMES_IN_FLIGHT * VK_REFL_CHAIN_COUNT;
    VkDescriptorPoolSize poolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * temporalSets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * temporalSets},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, temporalSets},
    };
    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = temporalSets;
    poolCI.poolSizeCount = 3;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.reflTemporalDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflTemporalDescLayout;
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
    {
        vkReflChain_t *chain = &vkRT.reflChain[c];
        VkDescriptorSetAllocateInfo dsAlloc = {};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = vkRT.reflTemporalDescPool;
        dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
        dsAlloc.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, chain->temporalDescSets));
        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
            chain->temporalDescSetLastUpdated[i] = -1;
    }

    common->Printf("VK RT Refl: temporal denoiser pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflAtrousPipeline (r_rtReflDenoise 2)
// ---------------------------------------------------------------------------

static const uint32_t REFL_ATROUS_PC_SIZE = 112;

// Inverse of this view's GL-convention view-projection, column-major, as the
// reflection shaders' rt_ReconstructWorldPos expects.
static void VK_RT_ReflInvViewProj(const viewDef_t *viewDef, float out[16])
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
    memcpy(out, invVP.ToFloatPtr(), 16 * sizeof(float));
}

static void VK_RT_InitReflAtrousPipeline(void)
{
    // refl_atrous.comp bindings:
    //   binding 0: COMBINED_IMAGE_SAMPLER — input reflection buffer (ping-pong)
    //   binding 1: STORAGE_IMAGE          — output reflection buffer (rgba16f, ping-pong)
    //   binding 2: COMBINED_IMAGE_SAMPLER — depth sampler
    //   binding 3: COMBINED_IMAGE_SAMPLER — gbufAlbedo (roughness gate)
    //   binding 4: COMBINED_IMAGE_SAMPLER — gbufNormal (normal edge stop)
    VkDescriptorSetLayoutBinding bindings[5] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 5;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.reflAtrousDescLayout));

    // Push constants: AtrousReflPC in VK_RT_DispatchReflAtrous (matches refl_atrous.comp).
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = REFL_ATROUS_PC_SIZE;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.reflAtrousDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.reflAtrousPipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/refl_atrous.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Refl Atrous: failed to load refl_atrous.comp.spv — spatial filter disabled");
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
    pipeCI.layout = vkRT.reflAtrousPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.reflAtrousPipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    // Pool: 3 descriptor sets per frame slot (pass0: temporal view -> A; pass1/3/5: A -> B;
    // pass2/4/6: B -> A — same precomputed-triplet scheme as GI's atrous).
    const int totalSets = VK_MAX_FRAMES_IN_FLIGHT * 3 * VK_REFL_CHAIN_COUNT;
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 4 * totalSets; // src + depth + gbufAlbedo + gbufNormal per set
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1 * totalSets;

    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = totalSets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.reflAtrousDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT * 3];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT * 3; i++)
        layouts[i] = vkRT.reflAtrousDescLayout;

    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
    {
        vkReflChain_t *chain = &vkRT.reflChain[c];
        VkDescriptorSetAllocateInfo dsAlloc = {};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = vkRT.reflAtrousDescPool;
        dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT * 3;
        dsAlloc.pSetLayouts = layouts;
        // Allocate flat: [0][0],[0][1],[0][2],[1][0],[1][1],[1][2]
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, &chain->atrousDescSets[0][0]));

        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
            chain->atrousDescSetLastUpdated[i] = -1;
    }

    common->Printf("VK RT Refl Atrous: spatial filter pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflections (public entry)
// ---------------------------------------------------------------------------

// Per-slot frame guards must start BELOW any real tr.frameCount, so the first frame
// is not mistaken for a duplicate dispatch. vkRT is zero-initialised, and frame 0 is
// a real frame, so -1 is the only safe initial value.
static void VK_RT_InitReflChainState(void)
{
    static const char *chainNames[VK_REFL_CHAIN_COUNT] = {"opaque", "glass"};
    static const bool chainSpatial[VK_REFL_CHAIN_COUNT] = {true, false};
    static const bool chainHalfRes[VK_REFL_CHAIN_COUNT] = {true, false};

    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
    {
        vkReflChain_t *chain = &vkRT.reflChain[c];
        chain->name = chainNames[c];
        chain->spatialFilter = chainSpatial[c];
        chain->halfResCapable = chainHalfRes[c];
        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        {
            chain->upsampleDescSetLastUpdated[i] = -1;
            chain->dispatchRectFrame[i] = -1;
            chain->lastTraceFrame[i] = -1;
            chain->lastTemporalFrame[i] = -1;
            chain->lastAtrousFrame[i] = -1;
            chain->traceDescSetLastUpdated[i] = -1;
            chain->temporalDescSetLastUpdated[i] = -1;
            chain->atrousDescSetLastUpdated[i] = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflUpsamplePipeline
// Compute pipeline for refl_upsample.comp. A load failure leaves the pipeline NULL,
// which VK_RT_ReflHalfResActive treats as "half res unavailable" — the renderer then
// traces full res rather than displaying an unreconstructed quarter-size image.
// ---------------------------------------------------------------------------
// mat4 (64) + 3 ints + 3 floats. Must match refl_upsample.comp's Push block.
#define REFL_UPSAMPLE_PC_SIZE 88

static void VK_RT_InitReflUpsamplePipeline(void)
{
    // binding 0 reflHalf (sampler), 1 reflFull (storage), 2 depth, 3 gbufNormal, 4 gbufAlbedo
    VkDescriptorSetLayoutBinding bindings[5] = {};
    for (int i = 0; i < 5; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType =
            (i == 1) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 5;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutCI, NULL, &vkRT.reflUpsampleDescLayout));

    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = REFL_UPSAMPLE_PC_SIZE;

    VkPipelineLayoutCreateInfo plCI = {};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &vkRT.reflUpsampleDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plCI, NULL, &vkRT.reflUpsamplePipelineLayout));

    VkShaderModule compModule = VK_LoadSPIRV("glprogs/glsl/refl_upsample.comp.spv");
    if (compModule == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Refl: failed to load refl_upsample.comp.spv — half-res tracing disabled");
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
    pipeCI.layout = vkRT.reflUpsamplePipelineLayout;
    VK_CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeCI, NULL, &vkRT.reflUpsamplePipeline));
    vkDestroyShaderModule(vk.device, compModule, NULL);

    const uint32_t sets = VK_MAX_FRAMES_IN_FLIGHT * VK_REFL_CHAIN_COUNT;
    VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets},
    };
    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = sets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.reflUpsampleDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflUpsampleDescLayout;
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
    {
        vkReflChain_t *chain = &vkRT.reflChain[c];
        VkDescriptorSetAllocateInfo dsAlloc = {};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = vkRT.reflUpsampleDescPool;
        dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
        dsAlloc.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, chain->upsampleDescSets));
        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
            chain->upsampleDescSetLastUpdated[i] = -1;
    }

    common->Printf("VK RT Refl: half-res upsample pipeline initialized\n");
}

void VK_RT_InitReflections(void)
{
    VK_RT_InitReflChainState();
    VK_RT_InitReflPipeline();
    VK_RT_InitReflUpsamplePipeline();
    VK_RT_InitReflTemporalPipeline();
    VK_RT_InitReflAtrousPipeline();
    VK_RT_ResizeReflections(vk.swapchainExtent.width, vk.swapchainExtent.height);
    // Only when vk.gbufferSupported — reflect_ray.rgen's Fresnel/debug-mode
    // work (which this composite pass displays) needs gbufNormal to exist.
    if (vk.gbufferSupported)
        VK_RT_InitReflCompositePipeline();
}

// ---------------------------------------------------------------------------
// VK_RT_ShutdownReflections (public)
// ---------------------------------------------------------------------------

void VK_RT_ShutdownReflections(void)
{
    if (vkRT.reflDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.reflDescPool, NULL);
        vkRT.reflDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.reflDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.reflDescLayout, NULL);
        vkRT.reflDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.reflPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflPipeline, NULL);
        vkRT.reflPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.reflPipelineLayout, NULL);
        vkRT.reflPipelineLayout = VK_NULL_HANDLE;
    }
    if (vkRT.sbtReflBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vk.device, vkRT.sbtReflBuffer, NULL);
        vkRT.sbtReflBuffer = VK_NULL_HANDLE;
    }
    if (vkRT.sbtReflMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vk.device, vkRT.sbtReflMemory, NULL);
        vkRT.sbtReflMemory = VK_NULL_HANDLE;
    }
    if (vkRT.reflSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vk.device, vkRT.reflSampler, NULL);
        vkRT.reflSampler = VK_NULL_HANDLE;
    }
    // Composite pipeline
    if (vkRT.reflCompositeDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.reflCompositeDescPool, NULL);
        vkRT.reflCompositeDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.reflCompositeDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.reflCompositeDescLayout, NULL);
        vkRT.reflCompositeDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.reflCompositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflCompositePipeline, NULL);
        vkRT.reflCompositePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflCompositeDebugPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflCompositeDebugPipeline, NULL);
        vkRT.reflCompositeDebugPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflCompositeLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.reflCompositeLayout, NULL);
        vkRT.reflCompositeLayout = VK_NULL_HANDLE;
    }
    // Temporal denoiser
    if (vkRT.reflTemporalDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.reflTemporalDescPool, NULL);
        vkRT.reflTemporalDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.reflTemporalDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.reflTemporalDescLayout, NULL);
        vkRT.reflTemporalDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.reflTemporalPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflTemporalPipeline, NULL);
        vkRT.reflTemporalPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflTemporalPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.reflTemporalPipelineLayout, NULL);
        vkRT.reflTemporalPipelineLayout = VK_NULL_HANDLE;
    }
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
        VK_RT_DestroyReflDenoiseImages(&vkRT.reflChain[c]);

    // Spatial filter (r_rtReflDenoise 2)
    if (vkRT.reflAtrousDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.reflAtrousDescPool, NULL);
        vkRT.reflAtrousDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.reflAtrousDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.reflAtrousDescLayout, NULL);
        vkRT.reflAtrousDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.reflAtrousPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflAtrousPipeline, NULL);
        vkRT.reflAtrousPipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflAtrousPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.reflAtrousPipelineLayout, NULL);
        vkRT.reflAtrousPipelineLayout = VK_NULL_HANDLE;
    }
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
        VK_RT_DestroyReflAtrousImages(&vkRT.reflChain[c]);

    // Half-res reconstruction
    if (vkRT.reflUpsampleDescPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(vk.device, vkRT.reflUpsampleDescPool, NULL);
        vkRT.reflUpsampleDescPool = VK_NULL_HANDLE;
    }
    if (vkRT.reflUpsampleDescLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(vk.device, vkRT.reflUpsampleDescLayout, NULL);
        vkRT.reflUpsampleDescLayout = VK_NULL_HANDLE;
    }
    if (vkRT.reflUpsamplePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(vk.device, vkRT.reflUpsamplePipeline, NULL);
        vkRT.reflUpsamplePipeline = VK_NULL_HANDLE;
    }
    if (vkRT.reflUpsamplePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(vk.device, vkRT.reflUpsamplePipelineLayout, NULL);
        vkRT.reflUpsamplePipelineLayout = VK_NULL_HANDLE;
    }
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
        VK_RT_DestroyReflHalfImages(&vkRT.reflChain[c]);

    VK_RT_DestroyNullLightSsbo();
    VK_RT_DestroyNullGbufNormal();
    VK_RT_ShutdownReflEmitters();
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
        VK_RT_DestroyReflImages(&vkRT.reflChain[c]);
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeReflections (public)
// ---------------------------------------------------------------------------

void VK_RT_ResizeReflections(uint32_t width, uint32_t height)
{
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_REFLECTIONS);

    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
    {
        vkReflChain_t *chain = &vkRT.reflChain[c];

        VK_RT_DestroyReflImages(chain);
        VK_RT_CreateReflImages(chain, width, height);
        VK_RT_DestroyReflDenoiseImages(chain);
        VK_RT_CreateReflDenoiseImages(chain, width, height);
        VK_RT_DestroyReflAtrousImages(chain);
        VK_RT_CreateReflAtrousImages(chain, width, height);
        VK_RT_DestroyReflHalfImages(chain);
        VK_RT_CreateReflHalfImages(chain, width, height);

        // Force descriptor set refresh at next dispatch, and drop any launch rect
        // recorded against the images that were just freed.
        for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        {
            chain->traceDescSetLastUpdated[i] = -1;
            chain->upsampleDescSetLastUpdated[i] = -1;
            chain->temporalDescSetLastUpdated[i] = -1;
            chain->atrousDescSetLastUpdated[i] = -1;
            chain->dispatchRectFrame[i] = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflections (public)
// Dispatch reflection rays for the full screen.  Called once per frame.
// Must be outside a render pass.  On entry depth is ATTACHMENT_OPTIMAL;
// transitions to READ_ONLY_OPTIMAL for the dispatch, then restores.
// On exit the chain's trace[currentFrame] is in GENERAL layout, readable by
// the interaction fragment shader (barrier issued before returning).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VK_RT_GlassScreenRect
// Union of every glass pane's (VK_RT_IsGlassPane) frontend scissorRect, in Vulkan Y-down pixels.
// False = no glass in view.  The display-space rect is scaled into render space and
// clamped to renderExtent, not the buffer; otherwise glass reflections silently go missing.
// ---------------------------------------------------------------------------

static bool VK_RT_GlassScreenRect(const viewDef_t *viewDef, uint32_t bufW, uint32_t bufH, int32_t *outX, int32_t *outY,
                                  uint32_t *outW, uint32_t *outH)
{
    if (!viewDef || !viewDef->drawSurfs)
        return false;

    idScreenRect glassRect;
    glassRect.Clear();
    bool found = false;

    for (int i = 0; i < viewDef->numDrawSurfs; i++)
    {
        const drawSurf_t *surf = viewDef->drawSurfs[i];
        if (!surf || !surf->material)
            continue;
        if (!VK_RT_IsGlassPane(surf->material))
            continue;
        if (surf->scissorRect.IsEmpty())
            continue;
        glassRect.Union(surf->scissorRect);
        found = true;
    }

    if (!found || glassRect.IsEmpty())
        return false;

    // 2px pad: glass_refl_overlay.frag samples this buffer bilinearly.
    glassRect.Expand();
    glassRect.Expand();

    // Valid region of the buffer: the render sub-rect, never the whole allocation.
    const int w = idMath::ClampInt(1, (int)bufW, (int)vk.renderExtent.width);
    const int h = idMath::ClampInt(1, (int)bufH, (int)vk.renderExtent.height);

    // Shift by the viewport (display space), then scale into render space.
    idScreenRect disp = glassRect;
    disp.x1 += viewDef->viewport.x1;
    disp.x2 += viewDef->viewport.x1;
    disp.y1 += viewDef->viewport.y1;
    disp.y2 += viewDef->viewport.y1;
    const idScreenRect s = VK_RT_ScaleDisplayRect(disp);

    const int rw = s.x2 - s.x1 + 1;
    const int rh = s.y2 - s.y1 + 1;
    if (rw <= 0 || rh <= 0)
        return false;

    const int x = idMath::ClampInt(0, w - 1, s.x1);
    const int y = idMath::ClampInt(0, h - 1, h - 1 - s.y2); // GL top -> VK top

    *outX = x;
    *outY = y;
    *outW = (uint32_t)idMath::ClampInt(1, w - x, rw);
    *outH = (uint32_t)idMath::ClampInt(1, h - y, rh);
    return true;
}

// The launch rect is recorded on the chain (dispatchRect/dispatchRectFrame) so the
// two filters blend exactly the pixels reflect_ray.rgen wrote and nothing stale
// outside that rect — the rect is debug-mode and glass-rect dependent.

// Each clause falls back to full res rather than show an unreconstructed image: REFL_POP_BOTH
// and debug modes stay exact, and a failed load or allocation leaves a NULL.
static bool VK_RT_ReflHalfResActive(const vkReflChain_t *chain, int frameIdx, int population)
{
    return chain->halfResCapable && population == REFL_POP_OPAQUE && r_rtReflOpaqueHalfRes.GetBool() &&
           vkRT.reflUpsamplePipeline != VK_NULL_HANDLE && chain->traceHalf[frameIdx].image != VK_NULL_HANDLE &&
           chain->trace[frameIdx].image != VK_NULL_HANDLE;
}

// Reconstruct chain->trace[] from chain->traceHalf[]. Runs between the trace and the
// temporal resolve, so everything downstream sees a full-res image and needs no
// knowledge of the half-res grid at all.
static void VK_RT_DispatchReflUpsample(VkCommandBuffer cmd, const viewDef_t *viewDef, vkReflChain_t *chain)
{
    const int frameIdx = vk.currentFrame;
    const vkRTImage_t &dst = chain->trace[frameIdx];
    const vkRTImage_t &src = chain->traceHalf[frameIdx];

    if (vkRT.gbufNormal[frameIdx].view == VK_NULL_HANDLE || vkRT.gbufAlbedo[frameIdx].view == VK_NULL_HANDLE)
        return; // nothing to reconstruct against; the caller already traced half res,
                // so this frame's reflections are lost rather than wrong

    if (chain->upsampleDescSetLastUpdated[frameIdx] != tr.frameCount)
    {
        VkDescriptorImageInfo srcInfo = {vkRT.reflSampler, src.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstInfo = {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo depthInfo = {vkRT.depthSampler, vk.depthSampledView,
                                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        // texelFetch only, so the sampler's filter/wrap state is irrelevant.
        VkDescriptorImageInfo normalInfo = {vkRT.depthSampler, vkRT.gbufNormal[frameIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo albedoInfo = {vkRT.depthSampler, vkRT.gbufAlbedo[frameIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const VkDescriptorImageInfo *infos[5] = {&srcInfo, &dstInfo, &depthInfo, &normalInfo, &albedoInfo};
        VkWriteDescriptorSet writes[5] = {};
        for (int b = 0; b < 5; b++)
        {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = chain->upsampleDescSets[frameIdx];
            writes[b].dstBinding = (uint32_t)b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType =
                (b == 1) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = infos[b];
        }
        vkUpdateDescriptorSets(vk.device, 5, writes, 0, NULL);
        chain->upsampleDescSetLastUpdated[frameIdx] = tr.frameCount;
    }

    struct UpsamplePC
    {
        float invViewProj[16];
        int32_t screenWidth;
        int32_t screenHeight;
        int32_t reflScale;
        float sigmaPlane;
        float normalPower;
        float roughnessScale;
    } pc;
    static_assert(sizeof(pc) == REFL_UPSAMPLE_PC_SIZE, "UpsamplePC size mismatch");

    VK_RT_ReflInvViewProj(viewDef, pc.invViewProj);
    // Depth is rasterised at render extent; the refl images are display-sized.
    pc.screenWidth = Min((int32_t)dst.width, (int32_t)vk.renderExtent.width);
    pc.screenHeight = Min((int32_t)dst.height, (int32_t)vk.renderExtent.height);
    pc.reflScale = 2;
    // Shared with the à-trous edge stops on purpose: one tuning knob for "what counts
    // as the same surface" across both reconstruction and filtering.
    pc.sigmaPlane = idMath::ClampFloat(0.001f, 1.0f, r_rtReflAtrousSigmaPlane.GetFloat());
    pc.normalPower = idMath::ClampFloat(0.0f, 64.0f, r_rtReflAtrousNormalPower.GetFloat());
    // Must match the rgen's lobe: the Fresnel weight recomputed here has to be the
    // same function of roughness the traced radiance was demodulated by.
    pc.roughnessScale = idMath::ClampFloat(0.0f, 4.0f, r_rtReflRoughnessScale.GetFloat());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflUpsamplePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflUpsamplePipelineLayout, 0, 1,
                            &chain->upsampleDescSets[frameIdx], 0, NULL);
    vkCmdPushConstants(cmd, vkRT.reflUpsamplePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, REFL_UPSAMPLE_PC_SIZE,
                       &pc);
    vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl Upsample[%s]: %ux%u -> %ux%u slot=%d\n", chain->name, src.width, src.height,
                       dst.width, dst.height, frameIdx);

    // Compute write -> the temporal resolve's compute read, and the fragment consumers
    // that sample the trace image directly when the denoiser is off.
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

static void VK_RT_DispatchReflChain(VkCommandBuffer cmd, const viewDef_t *viewDef, vkReflChain_t *chain, int population,
                                    const VkRect2D &glassRect)
{
    if (!vkRT.isInitialized)
        return;
    if (!vkRT.tlas[vk.currentFrame].isValid)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtReflections.GetBool())
        return;
    if (vkRT.reflPipeline == VK_NULL_HANDLE)
    {
        common->Printf("VK RT Refl: skip — pipeline is NULL\n");
        return;
    }

    const int frameIdx = vk.currentFrame;

    // Guard against duplicate dispatch in the same frame (weapon subview etc.)
    if (chain->lastTraceFrame[frameIdx] == tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Refl[%s]: skip duplicate dispatch frame=%d slot=%d\n", chain->name, tr.frameCount,
                           frameIdx);
        return;
    }
    chain->lastTraceFrame[frameIdx] = tr.frameCount;

    vkRTImage_t &rb = chain->trace[frameIdx];
    if (rb.image == VK_NULL_HANDLE)
    {
        common->Printf("VK RT Refl[%s]: skip — trace[%d] image is NULL\n", chain->name, frameIdx);
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl[%s]: frame=%d slot=%d size=%ux%u tlas=%p pipeline=%p gbuf=%s\n", chain->name,
                       tr.frameCount, frameIdx, rb.width, rb.height, (void *)vkRT.tlas[frameIdx].handle,
                       (void *)vkRT.reflPipeline,
                       (vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE) ? "on" : "off");

    // Resolve the launch rect first so a glass-free view pays nothing.  Glass launches over
    // the glass rect (resolved once by the caller); opaque and BOTH launch full-screen.
    const int reflDebugMode = VK_RT_ValidReflDebugMode();
    const bool reflDebugActive = (reflDebugMode != 0);
    const bool glassOnly = (r_rtReflectionMode.GetInteger() == 1) && !reflDebugActive;
    const bool launchOverGlassRect = (population == REFL_POP_GLASS) || (population == REFL_POP_BOTH && glassOnly);

    int32_t rectX = 0, rectY = 0;
    uint32_t rectW = rb.width, rectH = rb.height;
    if (launchOverGlassRect)
    {
        if (glassRect.extent.width == 0 || glassRect.extent.height == 0)
        {
            if (r_vkLogRT.GetInteger() >= 1)
                common->Printf("VK RT Refl[%s]: no glass in view — dispatch skipped entirely\n", chain->name);
            return;
        }
        rectX = glassRect.offset.x;
        rectY = glassRect.offset.y;
        rectW = glassRect.extent.width;
        rectH = glassRect.extent.height;
    }

    // Half res: the rgen writes the half image and refl_upsample.comp rebuilds rb.  rectX/Y are
    // (0,0) for opaque, but divide anyway so gcoord = coord * reflScale stays exact.
    const bool halfRes = VK_RT_ReflHalfResActive(chain, frameIdx, population);
    const int reflScale = halfRes ? 2 : 1;
    const VkImageView storageView = halfRes ? chain->traceHalf[frameIdx].view : rb.view;

    // The launch grid is the traced lattice; dispatchRect stays in FULL-res pixels
    // below, because the temporal and à-trous passes run on rb, not on the half image.
    const uint32_t launchW = halfRes ? (rectW + 1) / 2 : rectW;
    const uint32_t launchH = halfRes ? (rectH + 1) / 2 : rectH;

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

    // --- Build / fill UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    ReflParamsUBO ubo = {};
    VK_RT_ReflInvViewProj(viewDef, ubo.invViewProj);

    ubo.maxDist = Max(1.0f, r_rtReflectionDistance.GetFloat());
    ubo.frameIndex = (uint32_t)(tr.frameCount);
    // NDC denominator for depth->world reconstruction, so it must be the extent
    // the depth buffer was rasterised through, not the (display-sized) refl buffer.
    ubo.screenWidth = (int32_t)vk.renderExtent.width;
    ubo.screenHeight = (int32_t)vk.renderExtent.height;
    // Ceiling matches the settings-menu slider range.
    ubo.reflBlend = idMath::ClampFloat(0.0f, 10.0f, r_rtReflectionBlend.GetFloat());
    ubo.roughnessScale = idMath::ClampFloat(0.0f, 4.0f, r_rtReflRoughnessScale.GetFloat());
    ubo.debugMode = VK_RT_ValidReflDebugMode();
    ubo.sceneHasGlass = vkRT.sceneHasGlass ? 1 : 0;
    ubo.minWeight = idMath::ClampFloat(0.0f, 1.0f, r_rtReflectionMinWeight.GetFloat());
    ubo.roughLumExp = idMath::ClampFloat(0.0f, 8.0f, r_rtReflRoughLumExp.GetFloat());

    ubo.rectOriginX = rectX / reflScale;
    ubo.rectOriginY = rectY / reflScale;
    ubo.reflScale = reflScale;
    ubo.population = population;
    // Only REFL_POP_OPAQUE reads these, to bound where its glass-suppression probe can
    // possibly hit. Zero extent means "no glass in view" and skips the probe outright.
    ubo.glassRectX = glassRect.offset.x;
    ubo.glassRectY = glassRect.offset.y;
    ubo.glassRectW = (int32_t)glassRect.extent.width;
    ubo.glassRectH = (int32_t)glassRect.extent.height;
    // Debug modes want ungated per-pixel data; 13 visualizes the roughness ceiling so it
    // forces mode 3.  Mode 15 keeps the real mode to classify what normal play traces.
    int reflMode = r_rtReflectionMode.GetInteger();
    if (reflMode != 1 && reflMode != 3)
    {
        common->Warning("r_rtReflectionMode %d invalid (1 or 3); using 3", reflMode);
        r_rtReflectionMode.SetInteger(3);
        reflMode = 3;
    }
    ubo.reflMode = (reflDebugActive && reflDebugMode != 14 && reflDebugMode != 15) ? 3 : reflMode;

    ubo.roughnessMax = idMath::ClampFloat(0.0f, 1.0f, r_rtReflRoughnessMax.GetFloat());
    ubo.maxLuminance = Max(0.0f, r_rtReflMaxLuminance.GetFloat());
    ubo.shadowPicks = idMath::ClampInt(0, 2, r_rtReflShadowPicks.GetInteger());
    {
        // Confirms the knob reaches the pass when A/B-ing it.
        static int s_lastPicks = -1;
        if (ubo.shadowPicks != s_lastPicks)
        {
            s_lastPicks = ubo.shadowPicks;
            common->Printf("[Refl] opaque hit shadow rays: %s\n", ubo.shadowPicks > 0
                                                                      ? va("stochastic, %d pick(s)", ubo.shadowPicks)
                                                                      : "deterministic, up to 8");
        }
    }
    ubo.emitterCount = (int32_t)VK_RT_GetReflEmitterCount(frameIdx);
    ubo.glowTriCount = (int32_t)VK_RT_GetReflGlowTriCount(frameIdx);
    // 0 when off, so the glow triangles are also dropped in the rgen.
    ubo.glowGain = r_rtReflGlow.GetBool() ? Max(0.0f, r_rtReflGlowGain.GetFloat()) : 0.0f;

    // Froxel scatter grid for in-scatter along the reflected ray. volSteps 0 is the
    // "no volume this frame" signal — the binding still holds the null grid, but the
    // rgen must not spend taps discovering that it is black.
    {
        vkFroxelSampleParams_t fs;
        if (VK_RT_GetFroxelSampleParams(viewDef, &fs))
        {
            memcpy(ubo.viewProj, fs.viewProj, sizeof(ubo.viewProj));
            for (int i = 0; i < 3; i++)
            {
                ubo.froxelCamPos[i] = fs.camPos[i];
                ubo.froxelCamFwd[i] = fs.camFwd[i];
            }
            ubo.froxelDNear = fs.dNear;
            ubo.froxelLogRange = fs.logRange;
            ubo.froxelInvLogRange = fs.invLogRange;
            ubo.froxelNz = fs.nz;
            ubo.volSteps = r_rtReflVol.GetBool() ? idMath::ClampInt(1, 32, r_rtReflVolSteps.GetInteger()) : 0;
            ubo.volGain = Max(0.0f, r_rtReflVolGain.GetFloat());
            ubo.volStepLen = Max(0.0f, r_rtReflVolStepLen.GetFloat());
            // Same clamp as vk_vol_froxel.cpp, so both evaluate one phase function.
            ubo.volIsoMix = idMath::ClampFloat(0.0f, 1.0f, r_rtVolIsotropicMix.GetFloat());
        }
    }
    ubo.glassNormalMode = idMath::ClampInt(0, 2, r_rtGlassNormalMode.GetInteger());
    ubo.glassCreaseRatio = idMath::ClampFloat(0.0f, 1.0f, r_rtGlassCreaseRatio.GetFloat());
    extern idCVar r_rtGuiReflGain; // vk_gui_texture.cpp
    ubo.guiReflGain = idMath::ClampFloat(0.0f, 16.0f, r_rtGuiReflGain.GetFloat());
    memcpy(uboMapped, &ubo, sizeof(ReflParamsUBO));

    // --- Update descriptor set once per slot per frameCount, or when bound resources changed.
    // The caches live on the chain, since chains bind different storage images.
    // Pick the GI light SSBO for this frame; fall back to the null SSBO if not ready.
    VkBuffer lightSsbo = (vkRT.giLightSsbo[frameIdx] != VK_NULL_HANDLE) ? vkRT.giLightSsbo[frameIdx] : s_nullLightSsbo;

    // G-buffer normal/F0, or the null image.  Both are SHADER_READ_ONLY_OPTIMAL here.
    const bool haveGbuf = vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE;
    VkImageView gbufView = haveGbuf ? vkRT.gbufNormal[frameIdx].view : s_nullGbufView;

    // gbufAlbedo with the same null fallback; its class decodes to "ordinary, zero roughness".
    const bool haveGbufAlbedo = vk.gbufferSupported && vkRT.gbufAlbedo[frameIdx].view != VK_NULL_HANDLE;
    VkImageView gbufAlbedoView = haveGbufAlbedo ? vkRT.gbufAlbedo[frameIdx].view : s_nullGbufView;

    // Swaps between the live grid and the null one whenever volumetrics go on or
    // off, and changes outright when the res cvars force a realloc.
    const VkImageView froxelView = VK_RT_GetFroxelScatterView();
    // Same predicate as the scatter view, so the pair never splits across a toggle or realloc.
    const VkImageView froxelDirView = VK_RT_GetFroxelDirView();

    const bool reflResourceChanged =
        (chain->lastTlas[frameIdx] != vkRT.tlas[frameIdx].handle) ||
        (chain->lastStorageView[frameIdx] != storageView) || (chain->lastDepthView[frameIdx] != vk.depthSampledView) ||
        (chain->lastLightSsbo[frameIdx] != lightSsbo) || (chain->lastGbufView[frameIdx] != gbufView) ||
        (chain->lastGbufAlbedoView[frameIdx] != gbufAlbedoView) || (chain->lastFroxelView[frameIdx] != froxelView) ||
        (chain->lastFroxelDirView[frameIdx] != froxelDirView);

    if (reflResourceChanged && chain->traceDescSetLastUpdated[frameIdx] == tr.frameCount && r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK RT Refl[%s]: forcing descriptor refresh due to in-frame resource change frame=%d slot=%d "
                       "tlas=%p reflView=%p depthView=%p gbufView=%p\n",
                       chain->name, tr.frameCount, frameIdx, (void *)vkRT.tlas[frameIdx].handle, (void *)storageView,
                       (void *)vk.depthSampledView, (void *)gbufView);
    }

    bool refreshSet = (chain->traceDescSetLastUpdated[frameIdx] != tr.frameCount) || reflResourceChanged;
    if (refreshSet)
    {
        VkDescriptorSet ds = chain->traceDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo reflImgInfo = {};
        reflImgInfo.imageView = storageView;
        reflImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(ReflParamsUBO);

        VkDescriptorBufferInfo lightSsboInfo = {};
        lightSsboInfo.buffer = lightSsbo;
        lightSsboInfo.offset = 0;
        lightSsboInfo.range = VK_WHOLE_SIZE;

        VkDescriptorImageInfo gbufInfo = {};
        gbufInfo.sampler = vkRT.depthSampler; // texelFetch ignores filter/wrap state; reuse to avoid a new sampler
        gbufInfo.imageView = gbufView;
        gbufInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo gbufAlbedoInfo = {};
        gbufAlbedoInfo.sampler = vkRT.depthSampler; // texelFetch only, same reasoning as gbufInfo
        gbufAlbedoInfo.imageView = gbufAlbedoView;
        gbufAlbedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo emitterInfo = {};
        emitterInfo.buffer = VK_RT_GetReflEmitterBuffer(frameIdx);
        emitterInfo.offset = 0;
        emitterInfo.range = VK_WHOLE_SIZE;

        // Froxel scatter grid — trilinear, and GENERAL because the grids never leave
        // that layout. The null 1x1x1 grid stands in when the froxel path is down,
        // with volSteps 0 to match.
        VkDescriptorImageInfo froxelInfo = {};
        froxelInfo.sampler = VK_RT_GetFroxelSampler();
        froxelInfo.imageView = froxelView;
        froxelInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Mean-direction grid, trilinear: disagreeing cells shorten the vector toward
        // isotropic, the correct degradation (rt_PhaseFromMeanDir).
        VkDescriptorImageInfo froxelDirInfo = {};
        froxelDirInfo.sampler = VK_RT_GetFroxelSampler();
        froxelDirInfo.imageView = froxelDirView;
        froxelDirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[10] = {};

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
        writes[1].pImageInfo = &reflImgInfo;

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
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].pBufferInfo = &emitterInfo;

        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = ds;
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[7].pImageInfo = &froxelInfo;

        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = ds;
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[8].pImageInfo = &froxelDirInfo;

        writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[9].dstSet = ds;
        writes[9].dstBinding = 9;
        writes[9].descriptorCount = 1;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[9].pImageInfo = &gbufAlbedoInfo;

        vkUpdateDescriptorSets(vk.device, 10, writes, 0, NULL);
        chain->traceDescSetLastUpdated[frameIdx] = tr.frameCount;
        chain->lastTlas[frameIdx] = vkRT.tlas[frameIdx].handle;
        chain->lastStorageView[frameIdx] = storageView;
        chain->lastDepthView[frameIdx] = vk.depthSampledView;
        chain->lastLightSsbo[frameIdx] = lightSsbo;
        chain->lastGbufView[frameIdx] = gbufView;
        chain->lastGbufAlbedoView[frameIdx] = gbufAlbedoView;
        chain->lastFroxelView[frameIdx] = froxelInfo.imageView;
        chain->lastFroxelDirView[frameIdx] = froxelDirInfo.imageView;
    }

    // --- Dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipelineLayout, 0, 1,
                            &chain->traceDescSets[frameIdx], 1, &uboOff);
    // set=1: material table (MatTable SSBO, VtxAddrTable, IdxAddrTable, bindless textures)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipelineLayout, 1, 1,
                            &vkRT.matDescSet[vk.currentFrame], 0, NULL);

    if (r_vkLogRT.GetInteger() >= 1)
        // glowTris is the separate population: additive translucent surfaces the BLAS drops,
        // which is the only way a beam or sprite reaches a reflection as geometry.  emitters
        // counts discs (projectiles, smoke) and says nothing about those.
        common->Printf("VK RT Refl[%s]: dispatch %ux%u at (%d,%d) pop=%d scale=%d mode=%d maxDist=%.1f emitters=%d "
                       "glowTris=%d volSteps=%d\n",
                       chain->name, launchW, launchH, rectX, rectY, population, reflScale, ubo.reflMode, ubo.maxDist,
                       ubo.emitterCount, ubo.glowTriCount, ubo.volSteps);

    chain->dispatchRect[frameIdx] = VkRect2D{{rectX, rectY}, {rectW, rectH}};
    chain->dispatchRectFrame[frameIdx] = tr.frameCount;

    vkCmdTraceRaysKHR(cmd, &vkRT.reflRgenRegion, &vkRT.reflMissRegion, &vkRT.reflHitRegion, &vkRT.reflCallRegion,
                      launchW, launchH, 1);

    // --- Barrier: reflection write -> fragment read and VK_RT_DispatchReflTemporal's compute read ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    // Reconstruct full res before anything downstream reads it, inside the depth READ_ONLY
    // window the upsample's plane reconstruction needs.
    if (halfRes)
        VK_RT_DispatchReflUpsample(cmd, viewDef, chain);

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL for the upcoming render pass ---
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
        common->Printf("VK RT Refl[%s]: dispatch complete\n", chain->name);
}

void VK_RT_DispatchReflections(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    const int frameIdx = vk.currentFrame;
    const int debugMode = VK_RT_ValidReflDebugMode();
    const bool debugActive = (debugMode != 0);
    const int mode = idMath::ClampInt(1, 3, r_rtReflectionMode.GetInteger());

    // Resolve the glass rect once: the glass dispatch launches over it and the opaque
    // dispatch bounds its suppression probe by it, so they must not disagree.
    VkRect2D glassRect = {{0, 0}, {0, 0}};
    {
        const vkRTImage_t &rb = VK_RT_ReflChain()->trace[frameIdx];
        int32_t gx = 0, gy = 0;
        uint32_t gw = 0, gh = 0;
        if (rb.image != VK_NULL_HANDLE && VK_RT_GlassScreenRect(viewDef, rb.width, rb.height, &gx, &gy, &gw, &gh))
            glassRect = VkRect2D{{gx, gy}, {gw, gh}};
    }

    // Every debug mode keep the single REFL_POP_BOTH
    // dispatch into the opaque chain — one image for the visualization to land in, and
    // old mode 2's semantics ("one pass, glass by alpha sign") preserved exactly.
    const bool split = (mode != 2) && !debugActive;
    s_reflSplitActive[frameIdx] = split;

    if (!split)
    {
        VK_RT_DispatchReflChain(cmd, viewDef, VK_RT_ReflChain(), REFL_POP_BOTH, glassRect);
        return;
    }

    // Glass first so its log line reads before opaque's; the two write disjoint images,
    // so no barrier is needed between them.
    VK_RT_DispatchReflChain(cmd, viewDef, VK_RT_ReflGlassChain(), REFL_POP_GLASS, glassRect);
    if (mode == 3)
        VK_RT_DispatchReflChain(cmd, viewDef, VK_RT_ReflChain(), REFL_POP_OPAQUE, glassRect);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflTemporal (public)
// Temporal EMA denoise of the traced image (algorithm in refl_temporal_resolve.comp).
// No-op, historyValid untouched, when nothing was traced this frame (dispatchRectFrame).
// ---------------------------------------------------------------------------

static void VK_RT_DispatchReflTemporalChain(VkCommandBuffer cmd, const viewDef_t *viewDef, vkReflChain_t *chain)
{
    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtReflections.GetBool() || r_rtReflDenoise.GetInteger() <= 0)
        return;
    if (vkRT.reflTemporalPipeline == VK_NULL_HANDLE)
        return;
    if (!vk.gbufferSupported)
        return; // no motion vectors / G-buffer normal to reproject against

    const int frameIdx = vk.currentFrame;

    // Guard against duplicate dispatch in the same frame (weapon subview etc.) — mirrors
    // VK_RT_DispatchReflections's own guard. Without it, a second call this frame would
    // re-run the EMA against the traced image a second time and read back its own write.
    if (chain->lastTemporalFrame[frameIdx] == tr.frameCount)
        return;

    if (chain->dispatchRectFrame[frameIdx] != tr.frameCount)
        return; // the trace dispatch traced nothing this frame — nothing to blend

    const VkRect2D dispatchRect = chain->dispatchRect[frameIdx];
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    if (chain->history[0].image == VK_NULL_HANDLE || chain->history[1].image == VK_NULL_HANDLE ||
        chain->moments[0].image == VK_NULL_HANDLE || chain->moments[1].image == VK_NULL_HANDLE ||
        vkRT.gbufNormal[frameIdx].view == VK_NULL_HANDLE || vkRT.motionVectors[frameIdx].view == VK_NULL_HANDLE)
        return;

    chain->lastTemporalFrame[frameIdx] = tr.frameCount;

    // history/moments are shared across slots.  Submission order sequences the accesses;
    // this barrier adds the memory visibility (as in vk_temporal.cpp's AO resolve).
    {
        VkMemoryBarrier reflHistBarrier = {};
        reflHistBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        reflHistBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        reflHistBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &reflHistBarrier, 0, NULL, 0, NULL);
    }

    // --- Depth barrier: ATTACHMENT -> READ_ONLY, same dance as VK_RT_DispatchReflections ---
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Camera-cut detection (shared helper — vk_temporal.cpp) ---
    vkRTCameraCutResult_t cut =
        VK_RT_DetectCameraCut(viewDef, chain->prevCamPos, chain->prevCamFwd, chain->historyValid, "ReflTemporal");
    // !prevFrameValid means prevViewProjMatrix may be zeroed (prevClip.w = 0), so reset
    // rather than reject on NaN.
    const bool reset = !chain->historyValid || cut.isCut || !viewDef->prevFrameValid;
    if (reset && cut.isCut && r_vkLogRT.GetInteger() >= 1)
        common->Printf(
            "VK RT Refl Temporal[%s]: camera cut detected posDelta=%.3f angleDelta=%.3f — resetting history\n",
            chain->name, cut.posDelta, cut.angleDelta);
    chain->historyValid = true;

    // --- Build UBO ---
    VkBuffer uboBuf;
    uint32_t uboOff;
    void *uboMapped;
    VK_AllocUBOForShadow(&uboBuf, &uboOff, &uboMapped);

    ReflTemporalUBO ubo = {};
    VK_RT_ReflInvViewProj(viewDef, ubo.invViewProj);
    memcpy(ubo.prevViewProj, viewDef->prevViewProjMatrix, 16 * sizeof(float));
    ubo.screenWidth = (int32_t)vk.renderExtent.width;
    ubo.screenHeight = (int32_t)vk.renderExtent.height;
    // Floor at 0.01, not 0.0: an exact-zero alpha can never rise even under high
    // variance (alpha = baseAlpha * (1 + gain*sqrt(relVar)) is 0 whenever baseAlpha
    // is), which would freeze history forever rather than merely smoothing hard.
    ubo.baseAlpha = idMath::ClampFloat(0.01f, 1.0f, r_rtReflDenoiseAlpha.GetFloat());
    ubo.glassAlpha = idMath::ClampFloat(0.01f, 1.0f, r_rtReflDenoiseAlphaGlass.GetFloat());
    // Not exposed as cvars — tune here if a scene needs it.
    ubo.varianceGain = 4.0f;
    ubo.depthRejectThresh = 0.02f;
    ubo.normalRejectCos = 0.8f;
    ubo.scissorOffsetX = dispatchRect.offset.x;
    ubo.scissorOffsetY = dispatchRect.offset.y;
    ubo.scissorExtentX = (int32_t)dispatchRect.extent.width;
    ubo.scissorExtentY = (int32_t)dispatchRect.extent.height;
    ubo.resetHistory = reset ? 1 : 0;
    ubo.debugMode = VK_RT_ValidReflDebugMode();
    ubo.maxAge = 32;
    ubo.fireflyClamp = idMath::ClampFloat(0.0f, 8.0f, r_rtReflAtrousFireflyClamp.GetFloat());
    ubo._pad0[0] = 0.0f;
    memcpy(uboMapped, &ubo, sizeof(ReflTemporalUBO));

    // Read last frame's result, write the other slot — see vk_raytracing.h's comment
    // on vkReflChain_t::pingPong for why this can't be one read-write image per buffer.
    const int srcIdx = chain->pingPong;
    const int dstIdx = 1 - srcIdx;

    // --- Update descriptor set (once per frame slot; src/dst flip every frame, so this
    // must actually run every frame rather than being skipped as "already up to date") ---
    bool refreshSet = (chain->temporalDescSetLastUpdated[frameIdx] != tr.frameCount);
    if (refreshSet)
    {
        VkDescriptorSet ds = chain->temporalDescSets[frameIdx];

        VkDescriptorImageInfo currInfo = {};
        currInfo.imageView = chain->trace[frameIdx].view;
        currInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo srcHistInfo = {};
        srcHistInfo.imageView = chain->history[srcIdx].view;
        srcHistInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dstHistInfo = {};
        dstHistInfo.imageView = chain->history[dstIdx].view;
        dstHistInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo srcMomInfo = {};
        srcMomInfo.imageView = chain->moments[srcIdx].view;
        srcMomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dstMomInfo = {};
        dstMomInfo.imageView = chain->moments[dstIdx].view;
        dstMomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo gbufInfo = {};
        gbufInfo.sampler = vkRT.depthSampler; // texelFetch ignores filter/wrap state
        gbufInfo.imageView = vkRT.gbufNormal[frameIdx].view;
        gbufInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo motionInfo = {};
        motionInfo.sampler = vkRT.depthSampler; // texelFetch ignores filter/wrap state
        motionInfo.imageView = vkRT.motionVectors[frameIdx].view;
        motionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo = {};
        uboInfo.buffer = uboBuf;
        uboInfo.offset = 0;
        uboInfo.range = sizeof(ReflTemporalUBO);

        VkWriteDescriptorSet writes[9] = {};
        VkDescriptorImageInfo *imgInfos[8] = {&currInfo,   &srcHistInfo, &dstHistInfo, &srcMomInfo,
                                              &dstMomInfo, &depthInfo,   &gbufInfo,    &motionInfo};
        VkDescriptorType imgTypes[8] = {
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        };
        for (int i = 0; i < 8; i++)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = ds;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = imgTypes[i];
            writes[i].pImageInfo = imgInfos[i];
        }
        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = ds;
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[8].pBufferInfo = &uboInfo;

        vkUpdateDescriptorSets(vk.device, 9, writes, 0, NULL);
        chain->temporalDescSetLastUpdated[frameIdx] = tr.frameCount;
    }

    // --- Dispatch ---
    uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflTemporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflTemporalPipelineLayout, 0, 1,
                            &chain->temporalDescSets[frameIdx], 1, &uboOff);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // dstIdx now holds this frame's result — next frame (and this frame's fragment
    // consumers, via VK_RT_GetReflReadView) read from here.
    chain->pingPong = dstIdx;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl Temporal[%s]: dispatch slot=%d alpha=%.3f glassAlpha=%.3f rect=(%d,%d %u,%u) "
                       "reset=%d ping=%d->%d\n",
                       chain->name, frameIdx, ubo.baseAlpha, ubo.glassAlpha, dispatchRect.offset.x,
                       dispatchRect.offset.y, (unsigned int)dispatchRect.extent.width,
                       (unsigned int)dispatchRect.extent.height, ubo.resetHistory, srcIdx, dstIdx);

    // --- Barrier: compute write -> fragment read (composite/overlay/interaction) and
    // next frame's own compute read of history/moments ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &memBarrier, 0, NULL, 0, NULL);
    }

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL for the upcoming render pass ---
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, NULL, 0, NULL, 1, &depthRestore);
    }
}

// Every chain's own dispatchRectFrame guard makes an untraced chain a no-op, so this
// needs no knowledge of which populations ran this frame.
void VK_RT_DispatchReflTemporal(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
        VK_RT_DispatchReflTemporalChain(cmd, viewDef, &vkRT.reflChain[c]);
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflAtrous (public) — r_rtReflDenoise 2.
// Roughness-gated à-trous spatial filter, run on top of the temporal resolve's
// output.  See refl_atrous.comp for the per-pixel roughness gate itself.
// Must run after VK_RT_DispatchReflTemporal and before the resumed render pass.
// ---------------------------------------------------------------------------
// Defined below; the first pass reads whatever this chain's denoiser state says is
// current, which is exactly what downstream consumers would read.
static VkImageView VK_RT_ReflChainReadView(const vkReflChain_t *chain, int frameIdx);

static void VK_RT_DispatchReflAtrousChain(VkCommandBuffer cmd, const viewDef_t *viewDef, vkReflChain_t *chain)
{
    const int frameIdx = vk.currentFrame;

    // Duplicate-dispatch guard (weapon subview etc.).  Before the atrousReadView clear, so a
    // second call leaves the first call's result intact.
    if (chain->lastAtrousFrame[frameIdx] == tr.frameCount)
        return;

    // Stale from a previous frame (denoise toggled down, or an early return below)
    // must never leak forward — VK_RT_GetReflReadView checks this every frame.
    chain->atrousReadView[frameIdx] = VK_NULL_HANDLE;

    if (!chain->spatialFilter)
        return; // see vkReflChain_t::spatialFilter — every pass would be a copy

    if (!vkRT.isInitialized)
        return;
    if (!r_useRayTracing.GetBool() || !r_rtReflections.GetBool() || r_rtReflDenoise.GetInteger() < 2)
        return;
    if (vkRT.reflAtrousPipeline == VK_NULL_HANDLE)
        return;
    if (!vk.gbufferSupported)
        return; // no roughness to gate on

    const int iters = idMath::ClampInt(0, 8, r_rtReflAtrousIterations.GetInteger());
    if (iters == 0)
        return;

    if (chain->dispatchRectFrame[frameIdx] != tr.frameCount)
        return; // nothing traced this frame — mirrors the temporal pass's own guard

    const VkRect2D dispatchRect = chain->dispatchRect[frameIdx];
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    vkRTImage_t &bufA = chain->atrousA[frameIdx];
    vkRTImage_t &bufB = chain->atrousB[frameIdx];
    if (bufA.image == VK_NULL_HANDLE || bufB.image == VK_NULL_HANDLE)
        return;
    if (vkRT.gbufAlbedo[frameIdx].view == VK_NULL_HANDLE || vkRT.gbufNormal[frameIdx].view == VK_NULL_HANDLE)
        return; // bound below with no other check — VK_RT_DispatchReflTemporal checks its
                // own gbuf views the same way before dispatching

    chain->lastAtrousFrame[frameIdx] = tr.frameCount;

    // --- Depth barrier: ATTACHMENT -> READ_ONLY for the edge-stop depth reads ---
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             NULL, 0, NULL, 1, &depthToRead);
    }

    // --- Update descriptor sets (once per tr.frameCount per slot) ---
    // DS[frameIdx][0]: temporal readView(sampler) -> bufA — rebuilt each frame (that
    //                  view's underlying image alternates with chain->pingPong)
    // DS[frameIdx][1]: bufA(sampler) -> bufB — stable after images are created
    // DS[frameIdx][2]: bufB(sampler) -> bufA — stable after images are created
    if (chain->atrousDescSetLastUpdated[frameIdx] != tr.frameCount)
    {
        VkDescriptorImageInfo depthInfo = {};
        depthInfo.sampler = vkRT.depthSampler;
        depthInfo.imageView = vk.depthSampledView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo albedoInfo = {};
        albedoInfo.sampler = vkRT.depthSampler; // texelFetch ignores filter/wrap state
        albedoInfo.imageView = vkRT.gbufAlbedo[frameIdx].view;
        albedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo normalInfo = {};
        normalInfo.sampler = vkRT.depthSampler; // texelFetch ignores filter/wrap state
        normalInfo.imageView = vkRT.gbufNormal[frameIdx].view;
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo srcInfos[3] = {};
        srcInfos[0] = {vkRT.reflSampler, VK_RT_ReflChainReadView(chain, frameIdx),
                       VK_IMAGE_LAYOUT_GENERAL};                              // temporal -> A
        srcInfos[1] = {vkRT.reflSampler, bufA.view, VK_IMAGE_LAYOUT_GENERAL}; // A -> B
        srcInfos[2] = {vkRT.reflSampler, bufB.view, VK_IMAGE_LAYOUT_GENERAL}; // B -> A

        VkDescriptorImageInfo dstInfos[3] = {};
        dstInfos[0] = {VK_NULL_HANDLE, bufA.view, VK_IMAGE_LAYOUT_GENERAL};
        dstInfos[1] = {VK_NULL_HANDLE, bufB.view, VK_IMAGE_LAYOUT_GENERAL};
        dstInfos[2] = {VK_NULL_HANDLE, bufA.view, VK_IMAGE_LAYOUT_GENERAL};

        // Per set: binding 0 src, 1 dst, 2 depth, 3 gbufAlbedo, 4 gbufNormal.
        VkWriteDescriptorSet writes[15] = {};
        for (int s = 0; s < 3; s++)
        {
            const VkDescriptorImageInfo *infos[5] = {&srcInfos[s], &dstInfos[s], &depthInfo, &albedoInfo, &normalInfo};
            for (int b = 0; b < 5; b++)
            {
                VkWriteDescriptorSet &w = writes[s * 5 + b];
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = chain->atrousDescSets[frameIdx][s];
                w.dstBinding = (uint32_t)b;
                w.descriptorCount = 1;
                w.descriptorType =
                    (b == 1) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = infos[b];
            }
        }
        vkUpdateDescriptorSets(vk.device, 15, writes, 0, NULL);
        chain->atrousDescSetLastUpdated[frameIdx] = tr.frameCount;
    }

    const float sigmaPlane = idMath::ClampFloat(0.001f, 1.0f, r_rtReflAtrousSigmaPlane.GetFloat());
    const float normalPower = idMath::ClampFloat(0.0f, 64.0f, r_rtReflAtrousNormalPower.GetFloat());
    const float roughnessGain = r_rtReflAtrousRoughnessGain.GetFloat();
    const float fireflyClamp = r_rtReflAtrousFireflyClamp.GetFloat();
    float invViewProj[16];
    VK_RT_ReflInvViewProj(viewDef, invViewProj);
    // Depth is rasterised at render extent; the refl buffers may be larger.
    const int32_t renderW = Min((int32_t)bufA.width, (int32_t)vk.renderExtent.width);
    const int32_t renderH = Min((int32_t)bufA.height, (int32_t)vk.renderExtent.height);

    const uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    const uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflAtrousPipeline);

    for (int pass = 0; pass < iters; pass++)
    {
        // pass 0 -> DS[0] (temporal -> A); pass 1,3,5… -> DS[1] (A -> B); pass 2,4,6… -> DS[2] (B -> A).
        const int descIdx = (pass == 0) ? 0 : (1 + (pass - 1) % 2);

        struct AtrousReflPC
        {
            int32_t stepSize;
            float sigmaPlane;
            float roughnessGain;
            float fireflyClamp;
            int32_t screenWidth;
            int32_t screenHeight;
            int32_t scissorOffsetX;
            int32_t scissorOffsetY;
            int32_t scissorExtentX;
            int32_t scissorExtentY;
            int32_t debugMode;
            float normalPower;
            float invViewProj[16]; // offset 48, 16-aligned as the GLSL mat4 needs
        } pc;
        static_assert(sizeof(pc) == REFL_ATROUS_PC_SIZE, "AtrousReflPC size mismatch");

        pc.stepSize = 1 << pass;
        pc.sigmaPlane = sigmaPlane;
        pc.roughnessGain = roughnessGain;
        pc.fireflyClamp = fireflyClamp;
        pc.screenWidth = renderW;
        pc.screenHeight = renderH;
        pc.scissorOffsetX = dispatchRect.offset.x;
        pc.scissorOffsetY = dispatchRect.offset.y;
        pc.scissorExtentX = (int32_t)dispatchRect.extent.width;
        pc.scissorExtentY = (int32_t)dispatchRect.extent.height;
        pc.debugMode = VK_RT_ValidReflDebugMode();
        pc.normalPower = normalPower;
        memcpy(pc.invViewProj, invViewProj, sizeof(pc.invViewProj));

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflAtrousPipelineLayout, 0, 1,
                                &chain->atrousDescSets[frameIdx][descIdx], 0, NULL);
        vkCmdPushConstants(cmd, vkRT.reflAtrousPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, REFL_ATROUS_PC_SIZE,
                           &pc);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        const bool isLastPass = (pass == iters - 1);
        VkMemoryBarrier mb = {};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             isLastPass ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                        : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }

    // Update the read view to whichever buffer the final pass wrote.
    // pass 0 -> A, pass 1 -> B, pass 2 -> A, pass 3 -> B …
    const bool finalInA = ((iters - 1) % 2 == 0);
    chain->atrousReadView[frameIdx] = finalInA ? bufA.view : bufB.view;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl Atrous[%s]: dispatch slot=%d iters=%d rect=(%d,%d %u,%u)\n", chain->name, frameIdx,
                       iters, dispatchRect.offset.x, dispatchRect.offset.y, (unsigned int)dispatchRect.extent.width,
                       (unsigned int)dispatchRect.extent.height);

    // --- Depth barrier: restore ATTACHMENT_OPTIMAL for the upcoming render pass ---
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, NULL, 0, NULL, 1, &depthRestore);
    }
}

void VK_RT_DispatchReflAtrous(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    for (int c = 0; c < VK_REFL_CHAIN_COUNT; c++)
        VK_RT_DispatchReflAtrousChain(cmd, viewDef, &vkRT.reflChain[c]);
}

// View for any downstream reflection consumer. See the header comment.
static VkImageView VK_RT_ReflChainReadView(const vkReflChain_t *chain, int frameIdx)
{
    const int debugMode = VK_RT_ValidReflDebugMode();
    // Modes 2, 3, 12, 13 are per-frame rgen visualizations and skip the EMA.  11 is the
    // denoiser's own state; 15 is the à-trous view and reads the filter output.
    if (debugMode != 0 && debugMode != 11 && debugMode != 15)
        return chain->trace[frameIdx].view;
    // Mode 11 shows history as the temporal pass wrote it; the spatial filter would blur the
    // rejection boundary.
    if (debugMode != 11 && r_rtReflDenoise.GetInteger() >= 2 && chain->atrousReadView[frameIdx] != VK_NULL_HANDLE)
        return chain->atrousReadView[frameIdx];
    if (r_rtReflDenoise.GetInteger() > 0 && chain->historyValid &&
        chain->history[chain->pingPong].view != VK_NULL_HANDLE)
        return chain->history[chain->pingPong].view;
    return chain->trace[frameIdx].view;
}

VkImageView VK_RT_GetReflReadView(int frameIdx)
{
    return VK_RT_ReflChainReadView(VK_RT_ReflChain(), frameIdx);
}

VkImageView VK_RT_GetReflGlassReadView(int frameIdx)
{
    // When the populations weren't split this frame (mode 2, or any debug mode), the
    // pane's radiance is in the opaque chain behind the negative-alpha sentinel — the
    // overlay shader handles either, but it has to be pointed at the right image.
    vkReflChain_t *chain = s_reflSplitActive[frameIdx] ? VK_RT_ReflGlassChain() : VK_RT_ReflChain();
    return VK_RT_ReflChainReadView(chain, frameIdx);
}

bool VK_RT_ReflReadReady(int frameIdx)
{
    return VK_RT_ReflChain()->trace[frameIdx].image != VK_NULL_HANDLE &&
           VK_RT_ReflGlassChain()->trace[frameIdx].image != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflCompositePipeline
// Fullscreen pass that blends the reflection read view onto the framebuffer once per view.
// Single sampler: Fresnel and debug output live in reflect_ray.rgen, since depth can't be
// sampled inside this render pass.  reflCompositePipeline adds; reflCompositeDebugPipeline
// (blend off) replaces the scene while a debug mode is active.
// ---------------------------------------------------------------------------

static void VK_RT_InitReflCompositePipeline(void)
{
    // --- Descriptor set layout: 1 sampler binding ---
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, &vkRT.reflCompositeDescLayout));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &vkRT.reflCompositeDescLayout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &plInfo, NULL, &vkRT.reflCompositeLayout));

    // --- Shader modules — share gi_composite.vert.spv (generic fullscreen triangle) ---
    VkShaderModule vertMod = VK_LoadSPIRV("glprogs/glsl/gi_composite.vert.spv");
    VkShaderModule fragMod = VK_LoadSPIRV("glprogs/glsl/refl_composite.frag.spv");
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE)
    {
        common->Warning("VK RT Refl: failed to load composite shaders — composite pass disabled");
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

    // No depth test and no depth write — this is a post-geometry pass, same as GI/vol composite.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

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
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = vkRT.reflCompositeLayout;
    pipelineInfo.renderPass = vk.hdrRenderPass;
    pipelineInfo.subpass = 0;

    // --- Additive variant (normal operation) ---
    // Additive blend: reflection radiance is added on top of whatever is already
    // in the framebuffer. Alpha: ZERO+ONE so we don't disturb the alpha channel.
    VkPipelineColorBlendAttachmentState addBlend = {};
    addBlend.blendEnable = VK_TRUE;
    addBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    addBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    addBlend.colorBlendOp = VK_BLEND_OP_ADD;
    addBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    addBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    addBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    addBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    // Attachments 1-3 (gbufNormal, gbufAlbedo, motionVectors) are written only by the
    // G-buffer prepass; every other pipeline on vk.hdrRenderPass supplies write-mask-0
    // fillers so the subpass's per-attachment blend-state count always matches.
    VkPipelineColorBlendAttachmentState addAttachments[VK_HDR_COLOR_ATTACHMENT_COUNT] = {addBlend};
    const uint32_t blendCount = VK_FillHdrBlendAttachments(addAttachments);

    VkPipelineColorBlendStateCreateInfo addBlendState = {};
    addBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    addBlendState.attachmentCount = blendCount;
    addBlendState.pAttachments = addAttachments;
    pipelineInfo.pColorBlendState = &addBlendState;

    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.reflCompositePipeline));

    // --- Debug variant: blend disabled (replace), so r_rtReflectionDebugMode's
    // visualization (baked into the traced image by the rgen) isn't muddied by additive
    // blending onto the already-lit scene. ---
    VkPipelineColorBlendAttachmentState replaceBlend = {};
    replaceBlend.blendEnable = VK_FALSE;
    replaceBlend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState replaceAttachments[VK_HDR_COLOR_ATTACHMENT_COUNT] = {replaceBlend};
    VK_FillHdrBlendAttachments(replaceAttachments);

    VkPipelineColorBlendStateCreateInfo replaceBlendState = {};
    replaceBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    replaceBlendState.attachmentCount = blendCount;
    replaceBlendState.pAttachments = replaceAttachments;
    pipelineInfo.pColorBlendState = &replaceBlendState;

    VK_CHECK(
        vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkRT.reflCompositeDebugPipeline));

    vkDestroyShaderModule(vk.device, vertMod, NULL);
    vkDestroyShaderModule(vk.device, fragMod, NULL);

    // --- Descriptor pool and sets (one per frame in flight) ---
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VK_MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.reflCompositeDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflCompositeDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.reflCompositeDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.reflCompositeDescSets));

    common->Printf("VK RT Refl: composite pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_CompositeReflections (public)
// Additively blends the reflection buffer onto the current framebuffer using a
// fullscreen triangle. Must be called inside the main render pass, after
// VK_RT_CompositeGI. Skips glass pixels (negative alpha sentinel, written by
// reflect_ray.rgen) — those are composited separately by glass_refl_overlay.frag.
// ---------------------------------------------------------------------------

void VK_RT_CompositeReflections(VkCommandBuffer cmd)
{
    if (!r_useRayTracing.GetBool() || !r_rtReflections.GetBool())
        return;
    if (vkRT.reflCompositePipeline == VK_NULL_HANDLE)
        return;

    const int frameIdx = vk.currentFrame;
    if (!VK_RT_ReflReadReady(frameIdx) || vkRT.reflSampler == VK_NULL_HANDLE)
        return;

    const int debugMode = VK_RT_ValidReflDebugMode();
    const bool debugActive = (debugMode != 0);

    // Glass-only mode has nothing here: opaque isn't traced and glass composites per surface.
    if (r_rtReflectionMode.GetInteger() == 1 && !debugActive)
        return;

    // Update the descriptor set for this frame slot. The reflection image may
    // have been recreated (resize), so always write it before drawing.
    // VK_RT_GetReflReadView picks the denoised or raw image (see its debug-mode rules).
    VkDescriptorImageInfo imgInfo = {};
    imgInfo.sampler = vkRT.reflSampler;
    imgInfo.imageView = VK_RT_GetReflReadView(frameIdx);
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vkRT.reflCompositeDescSets[frameIdx];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    VkPipeline pipe = debugActive ? vkRT.reflCompositeDebugPipeline : vkRT.reflCompositePipeline;
    if (pipe == VK_NULL_HANDLE)
        return;

    // Viewport and scissor are inherited from the caller — see VK_RT_CompositeGI's
    // identical note: do not override them here.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkRT.reflCompositeLayout, 0, 1,
                            &vkRT.reflCompositeDescSets[frameIdx], 0, NULL);

    // 3 vertices, no vertex buffer — the vert shader generates the triangle from gl_VertexIndex.
    vkCmdDraw(cmd, 3, 1, 0, 0);

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl: composite drawn frame=%d slot=%d debugMode=%d\n", tr.frameCount, frameIdx,
                       debugMode);
}
