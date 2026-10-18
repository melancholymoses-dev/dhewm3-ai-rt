/*
===========================================================================

dhewm3-rt Vulkan ray tracing - environment reflection ray pipeline and dispatch.

Phase 5.3 "cheap approximation":
  Traces one mirror-reflect ray per non-sky pixel.  The closest-hit and
  miss shaders return a direction-based colour tint (no texture/material
  lookup) so metallic/shiny surfaces get plausible environment reflections
  at minimal GPU cost.

The result is stored in an RGBA16F buffer that is sampled by the lighting
interaction fragment shader and blended into the specular contribution,
weighted by the specular map value.

Known limitation: the interaction shader is called once per light, so the
reflection contribution is accumulated N times for N lights illuminating a
pixel.  Moving reflections to a dedicated post-process pass is the
correct fix (planned for a later step).

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

static idCVar r_rtReflectionDistance("r_rtReflectionDistance", "1000.0", CVAR_RENDERER | CVAR_FLOAT,
                                     "Max reflection ray travel distance in world units (default 1000.0)");

static idCVar r_rtReflectionBlend("r_rtReflectionBlend", "6.0", CVAR_RENDERER | CVAR_FLOAT,
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
                              "Multiplies Stage 2's packed per-pixel roughness before the GGX lobe samples it.\n"
                              "1.0 = roughness as written to the G-buffer. Retune here once real geometry (not\n"
                              "just the r_rtReflectionDebugMode 12 preview) is visible through the lobe.",
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
                               "traces. 0.004 = pre-R1 behaviour.");

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

// Reflection refactor Stage 1 (20261017_reflection_pipeline_refactor.md). Quality setting
// (taste contract #7): default 0 until converged A/B screenshots match.
idCVar r_rtReflShadowPicks("r_rtReflShadowPicks", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                           "Shadow rays per opaque reflection hit. 0 = deterministic loop (up to 8 rays,\n"
                           "lights past the budget unshadowed). 1-2 = importance-sample that many lights\n"
                           "(unbiased, noisier; the denoiser absorbs it). Glass always uses 0.",
                           0, 2);

// Volumetrics in reflections (docs/plans/20261013_reflection_volumetrics.md). The
// reflection ray marches the same froxel scatter grid the primary view resolves, so
// light shafts — the flashlight beam above all — show up in glass. Needs
// r_rtVolFroxel 1
static idCVar r_rtReflVol("r_rtReflVol", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                          "march the froxel scatter grid along reflection rays, so volumetric light shafts "
                          "appear in reflections (needs r_rtVolFroxel 1)");
static idCVar r_rtReflVolSteps("r_rtReflVolSteps", "6", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                               "samples per reflection ray through the froxel scatter grid (1-32). Raise if the "
                               "reflected beam bands; brightness should not change with this.");

static idCVar r_rtReflVolGain("r_rtReflVolGain", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                              "scale on reflected in-scatter. 1.0 is the literal integral — since the "
                              "mean-direction grid landed, there is no phase-angle bias left to compensate for.");

idCVar r_rtReflectionMode("r_rtReflectionMode", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                          "1 = glass only. Reflections are dispatched over the union screen rect of the\n"
                          "    view's SURFTYPE_GLASS surfaces and skipped entirely when the view holds no glass;\n"
                          "    opaque geometry never reflects, so no per-pixel F0/Fresnel work is done at all.\n"
                          "2 = legacy full-screen. Every non-sky pixel is traced and gated by the specular-map F0\n"
                          "    remap (r_rtSpecF0Scale/Gamma) plus r_rtReflectionMinWeight. Predates Stages 2-3\n"
                          "    (roughness/GGX) and ignores r_rtReflRoughnessMax — kept as an A/B baseline.\n"
                          "3 = glass + opaque-with-roughness (default).\n"
                          "    Full-screen like mode 2, but opaque surfaces rougher than r_rtReflRoughnessMax\n"
                          "    skip the reflection ray entirely and take GI alone, so the GGX lobe never\n"
                          "    double-counts GI's own near-diffuse contribution at high roughness.\n"
                          "Use r_rtReflections 0 to turn the feature off entirely.",
                          1, 3);

idCVar r_rtReflDenoise("r_rtReflDenoise", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                       "0 = off (reflections sampled raw, pre-Stage-1 behaviour).\n"
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
                                "r_rtReflDenoise 2 only. À-trous passes (0-8); effective radius is 2^N - 1 pixels.\n"
                                "Same default as r_rtGIAtrousIterations.",
                                0, 8);
// Refl-refactor Stage 3 replaces r_rtReflAtrousSigmaZ (raw NDC depth, dead past ~20 units).
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
    "0 = off. Retired, now no-ops: 1, 5, 7, 8, 9, 10 (numbers kept stable).\n"
    "2 = output G-buffer world normal as RGB (reflect_ray.rgen) — a grate floor should look\n"
    "    bumpy/detailed, not flat single-colour; silhouette edges must match geometry.\n"
    "3 = output G-buffer F0 (alpha) greyscale (reflect_ray.rgen) — most of the scene near-black,\n"
    "    metal/wet trim grey, nothing solid white.\n"
    "4 = tint pixels using the rt_ReconstructNormal fallback (G-buffer alpha == 0) magenta\n"
    "    (reflect_ray.rgen) — should be only sky/translucents once the prepass covers the scene.\n"
    "6 = raw traced radiance, before reflBlend and before the Schlick/glass weight. The\n"
    "    decisive 'dark subject vs weak interface' measurement — a dim player here means the\n"
    "    lighting chain is short (B2/B4), a bright one means the glass weight is (B3).\n"
    "11 = reflection denoiser history age / rejection mask (needs r_rtReflDenoise >= 1, else this\n"
    "    just shows the normal unfiltered reflection — reflect_ray.rgen has no mode-11 case of\n"
    "    its own): green = accepted, brightness = history age; red = rejected this frame\n"
    "    (disocclusion or camera cut).\n"
    "12 = Stage 2 per-pixel roughness (gbufAlbedo alpha, Toksvig estimate) as greyscale —\n"
    "    rough concrete should read bright, polished trim and glass dark, and the gradient\n"
    "    should track visible bump detail rather than the diffuse texture.\n"
    "13 = Stage 4 GI/reflection handoff (r_rtReflRoughnessMax): white = glass (always\n"
    "    reflects), blue = opaque taking a reflection ray (and GI — both), green = opaque\n"
    "    over the ceiling or below the F0 floor (GI alone). Forces r_rtReflectionMode 3\n"
    "    regardless of the cvar, so sweeping r_rtReflRoughnessMax moves the blue/green\n"
    "    boundary live.\n"
    "14 = à-trous branch view (needs r_rtReflDenoise 2): grey = glass, dark blue = untraced,\n"
    "    red = roughness 0 (filter skipped), yellow = traced but no traced neighbour (blur is a\n"
    "    no-op), green = filtered, brighter = more of the blurred result used.\n"
    "15 = à-trous edge-stop weight (needs r_rtReflDenoise 2): grey level = mean step-1 weight\n"
    "    over traced neighbours, white = blends freely, black = stopped. Floors should be\n"
    "    white, rails and creases outlined dark. Yellow = no traced neighbour; untraced/glass\n"
    "    as mode 14. Tune r_rtReflAtrousSigmaPlane/NormalPower here.\n"
    "Modes 2-4, 6, 12 and 13 require r_rtReflections 1 — they ride on the reflection ray\n"
    "dispatch, force the full-screen launch grid, and are displayed via refl_composite.frag\n"
    "with blending disabled (replace, not add). The per-surface glass overlay is suppressed\n"
    "while they are active.");

// ---------------------------------------------------------------------------
// UBO layout matching reflect_ray.rgen ReflParams block
//
//   mat4   invViewProj  offset  0  size 64
//   float  maxDist      offset 64  size  4
//   uint   frameIndex   offset 68  size  4
//   ivec2  screenSize   offset 72  size  8  (ivec2 std140 align=8 → 72 is fine)
//   float  reflBlend      offset 80  size  4  (r_rtReflectionBlend, applied at imageStore)
//   float  roughnessScale offset 84  size  4  (r_rtReflRoughnessScale, Stage 3 — was grazingMax)
//   int    debugMode      offset 88  size  4  (r_rtReflectionDebugMode, Step 9)
//   int    sceneHasGlass  offset 92  size  4  (P6 — skip glass probe when 0)
//   float  minWeight      offset 96  size  4  (r_rtReflectionMinWeight, R1)
//   int    rectOriginX    offset 100 size  4  (R6 launch-grid origin — was 104; grazingGain's
//                                               old slot is gone — roughLumExp now fills offset 124)
//   int    rectOriginY    offset 104 size  4
//   int    reflMode       offset 108 size  4  (r_rtReflectionMode — 1/2/3, Stage 4 adds 3)
//   int    emitterCount   offset 112 size  4  (vk_rt_emitters.cpp, 0 = none)
//   float  glowGain       offset 116 size  4  (r_rtReflGlowGain, 0 when r_rtReflGlow is off)
//   int    glowTriCount   offset 120 size  4  (vk_rt_emitters.cpp glow triangles)
//   float  roughLumExp    offset 124 size  4  (r_rtReflRoughLumExp — fills the slot std140
//                                               left before the mat4 when grazingGain went)
//   mat4   viewProj       offset 128 size 64  (froxel grid's own, GL convention)
//   vec4   froxelCamPos   offset 192 size 16  (xyz)
//   vec4   froxelCamFwd   offset 208 size 16  (xyz)
//   float  froxelDNear    offset 224 size  4
//   float  froxelLogRange offset 228 size  4
//   float  froxelInvLogRange offset 232 size 4
//   int    froxelNz       offset 236 size  4
//   int    volSteps       offset 240 size  4  (0 = no volume bound this frame)
//   float  volGain        offset 244 size  4
//   float  volIsoMix      offset 248 size  4  (r_rtVolIsotropicMix, Stage 5)
//   int    glassNormalMode offset 252 size 4  (r_rtGlassNormalMode)
//   float  glassCreaseRatio offset 256 size 4  (r_rtGlassCreaseRatio)
//   float  guiReflGain    offset 260 size  4  (r_rtGuiReflGain)
//   float  roughnessMax   offset 264 size  4  (r_rtReflRoughnessMax, Stage 4 — took one of
//                                               the two trailing pad floats; _pad0 is now [1])
//   float  maxLuminance   offset 268 size  4  (r_rtReflMaxLuminance, 2026-10-16 addendum —
//                                               took the last pad float; block still ends at
//                                               272, already a multiple of 16, no new padding)
//   int    shadowPicks    offset 272 size  4  (r_rtReflShadowPicks, refl-refactor Stage 1)
//   total: 288 bytes — std140 rounds the block up to a multiple of
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
    float roughnessScale;  // r_rtReflRoughnessScale (Stage 3) — was grazingMax's slot
    int32_t debugMode;     // r_rtReflectionDebugMode — 2/3/4 handled in rgen (Step 9)
    int32_t sceneHasGlass; // P6 — 0 when no MAT_FLAG_GLASS geometry in the TLAS
    float minWeight;       // r_rtReflectionMinWeight — pre-trace Fresnel cull (R1)
    int32_t rectOriginX;   // R6 — launch-grid origin; rgen adds this to gl_LaunchIDEXT
    int32_t rectOriginY;
    int32_t reflMode;     // r_rtReflectionMode — 1 glass-only, 2 legacy full-screen, 3 glass+opaque (Stage 4)
    int32_t emitterCount; // projectile glow discs in the emitter SSBO (binding 6)
    float glowGain;       // r_rtReflGlowGain
    int32_t glowTriCount; // additive translucent triangles after the discs in binding 6
    float roughLumExp;    // r_rtReflRoughLumExp — reuses the pre-mat4 slot retired with grazingGain

    // Froxel scatter grid (20261013_reflection_volumetrics.md). Mirrors
    // VolFroxelSlices in vol_froxel_sample.glsl plus the forward projection the
    // world -> grid lookup needs; filled from VK_RT_GetFroxelSampleParams so the
    // mapping is the one the fill actually used.
    float viewProj[16];
    float froxelCamPos[4];
    float froxelCamFwd[4];
    float froxelDNear;
    float froxelLogRange;
    float froxelInvLogRange;
    int32_t froxelNz;
    int32_t volSteps; // 0 = no usable grid this frame; the rgen takes no samples
    float volGain;
    float volIsoMix;         // r_rtVolIsotropicMix — Stage 5, for rt_PhaseFromMeanDir
    int32_t glassNormalMode; // r_rtGlassNormalMode — 0 interpolated, 1 face, 2 crease-aware
    float glassCreaseRatio;  // r_rtGlassCreaseRatio — mode 2 flattens when devRatio < this
    float guiReflGain;       // r_rtGuiReflGain — scales live GUI screen hits (MAT_FLAG_GUI_LIVE)
    float roughnessMax;      // r_rtReflRoughnessMax (Stage 4) — took one of the two trailing
                             // pad floats Stage 3 left here; _pad0 shrinks to match
    float maxLuminance;      // r_rtReflMaxLuminance (2026-10-16 addendum) — took the last pad
                             // float; block still lands on exactly 272, no new padding needed
    int32_t shadowPicks;     // r_rtReflShadowPicks — 0 deterministic, 1-2 stochastic (opaque only)
    int32_t _pad0[3];        // std140 block size rounds to 16
};
static_assert(sizeof(ReflParamsUBO) == 288, "ReflParamsUBO size mismatch");

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
// Stage 2 (rt_projected_light_cookies.md): vk_gi.cpp's GILightBuffer::lights[]/
// cookies[] are fixed-size in the GLSL mirror now (rt_light_eval.glsl), so the
// null-light fallback below must be sized to match, not just big enough for
// the header fields.
extern uint32_t VK_RT_GetGILightBufferSize(void);

extern idCVar r_useRayTracing;
extern idCVar r_vkLogRT;
extern idCVar r_rtReflGlow;        // vk_rt_emitters.cpp
extern idCVar r_rtReflGlowGain;    // vk_rt_emitters.cpp
extern idCVar r_rtVolIsotropicMix; // vk_vol.cpp — Stage 5, shared phase parameter

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
// Null G-buffer normal/F0 image (Stage 3.5, see docs/plans/gbuffer_normal_pass.md).
// 1x1 image cleared to (0.5, 0.5, 1.0, 0.0) — B == 1.0 is the "never written"
// sentinel, see rt_gbuf_normal_fmt.glsl — bound to gbufNormalSampler
// whenever the real per-frame gbufNormal image doesn't exist
// (vk.gbufferSupported false). Alpha 0 is the same "no data" sentinel the real
// image uses, so the consuming rgen falls back to rt_ReconstructNormal exactly
// as it did before this feature existed.
//
// P9 (Wave 3): AO (binding 4) and GI (binding 5) now bind the same G-buffer and
// share this fallback via VK_RT_GetNullGbufNormalView() below — one image, one
// sentinel convention, three consumers.
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

// P9: shared with the AO and GI descriptor sets (vk_ao.cpp / vk_gi.cpp), which bind
// the same G-buffer image and need the same fallback. The create call is idempotent;
// VK_RT_InitAO and VK_RT_InitGI both call this at init time so the one-time
// submit+wait inside never lands mid-frame. RT shutdown tears down all three
// subsystems together, so the shared lifetime is safe.
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
// VK_RT_CreateReflImages
// Allocates per-frame RGBA16F reflection buffers at the given resolution.
// ---------------------------------------------------------------------------

static void VK_RT_CreateReflImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &rb = vkRT.reflBuffer[i];
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

            // A2 (amd_vulkan_cleanup.md): clear to black at creation, matching every
            // other RT image (vk_gi.cpp:429, vk_temporal.cpp:1005, vk_vol.cpp:407). This
            // rgen is full-screen, so exposure is narrower than AO's — limited to frames
            // where the reflection dispatch is skipped entirely — but the fix is the same
            // shape and belongs with it.
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

static void VK_RT_DestroyReflImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRTImage_t &rb = vkRT.reflBuffer[i];
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
// Reflection temporal denoiser images (Stage 1, 20261013_reflection_roughness.md)
//
// Single shared instance each, not per-slot — same reasoning as aoHistory/
// giHistory: the EMA recurrence is sequential across frames, and all RT work
// goes through one queue in submission order, so a shared image never races.
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

    // Transition UNDEFINED -> GENERAL and clear to black, same one-time pattern as
    // VK_RT_CreateReflImages. Stage mask is broad (ALL_COMMANDS) rather than one
    // specific consumer's stage: reflHistory is read by the compute resolve pass,
    // the composite/overlay fragment shaders, and the legacy interaction shader.
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

static void VK_RT_CreateReflDenoiseImages(uint32_t width, uint32_t height)
{
    // reflHistory is sampled by the composite/overlay/interaction fragment shaders;
    // reflMoments is read back only by this file's own compute pass. Both are
    // ping-ponged (see vk_raytracing.h) to avoid a read/write race between the
    // reprojected read at Q and this frame's write at P within one dispatch.
    for (int i = 0; i < 2; i++)
    {
        if (!VK_RT_AllocReflAuxImage(vkRT.reflHistory[i], width, height,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl: failed to allocate reflHistory[%d]", i);
        if (!VK_RT_AllocReflAuxImage(vkRT.reflMoments[i], width, height, VK_IMAGE_USAGE_STORAGE_BIT))
            common->Warning("VK RT Refl: failed to allocate reflMoments[%d]", i);
    }

    vkRT.reflPingPong = 0;
    vkRT.reflHistoryValid = false;
    vkRT.reflPrevCamPos.Zero();
    vkRT.reflPrevCamFwd.Zero();
}

static void VK_RT_DestroyReflDenoiseImages(void)
{
    for (int i = 0; i < 2; i++)
    {
        VK_RT_FreeReflAuxImage(vkRT.reflHistory[i]);
        VK_RT_FreeReflAuxImage(vkRT.reflMoments[i]);
    }
    vkRT.reflHistoryValid = false;
}

// Spatial filter scratch (r_rtReflDenoise 2) — per-slot ping-pong across the N
// passes of a single frame's dispatch, unlike reflHistory's cross-frame ping-pong.
static void VK_RT_CreateReflAtrousImages(uint32_t width, uint32_t height)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!VK_RT_AllocReflAuxImage(vkRT.reflAtrousA[i], width, height,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl Atrous: failed to allocate reflAtrousA[%d]", i);
        if (!VK_RT_AllocReflAuxImage(vkRT.reflAtrousB[i], width, height,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            common->Warning("VK RT Refl Atrous: failed to allocate reflAtrousB[%d]", i);
        vkRT.reflAtrousReadView[i] = VK_NULL_HANDLE;
    }
}

static void VK_RT_DestroyReflAtrousImages(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_RT_FreeReflAuxImage(vkRT.reflAtrousA[i]);
        VK_RT_FreeReflAuxImage(vkRT.reflAtrousB[i]);
        vkRT.reflAtrousReadView[i] = VK_NULL_HANDLE;
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
    // binding 5: G-buffer normal/F0 sampler (Stage 3.5, see
    //            docs/plans/gbuffer_normal_pass.md). Always bound: the real
    //            per-frame image when vk.gbufferSupported, otherwise a 1x1
    //            null image (a=0, same "no data" sentinel) — see
    //            VK_RT_CreateNullGbufNormal / VK_RT_DispatchReflections.
    // binding 6: projectile emitter SSBO (vk_rt_emitters.cpp), one per frame slot.

    // binding 7: froxel scatter grid, sampler3D — in-scatter along the reflected ray
    //            (20261013_reflection_volumetrics.md).
    // binding 8: froxel mean-direction grid, sampler3D — Stage 5. Bound as a pair
    //            with 7; the scatter grid is unphased without it.
    // binding 9: G-buffer albedo sampler (Stage 2, 20261013_reflection_roughness.md) —
    //            only its alpha (packed class + roughness) is read here, for debug
    //            mode 12 today and the GGX lobe from Stage 3 on. Same fallback
    //            convention as binding 5 — VK_RT_GetNullGbufNormalView() when unsupported.
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
    // COMBINED_IMAGE_SAMPLER count is doubled: depth (binding 2) + gbufNormal
    // (binding 5, Stage 3.5) are both bound per set now.
    VkDescriptorPoolSize poolSizes[5] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_MAX_FRAMES_IN_FLIGHT},
        // depth + gbufNormal + froxelScatter + froxelDir + gbufAlbedo (Stage 2)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MAX_FRAMES_IN_FLIGHT * 5},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_MAX_FRAMES_IN_FLIGHT * 2}, // GI light SSBO + emitters
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &vkRT.reflDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.reflDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.reflDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.reflDescSetLastUpdatedFrameCount[i] = -1;

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
// VK_RT_InitReflTemporalPipeline (Stage 1, 20261013_reflection_roughness.md)
// Compute pipeline for refl_temporal_resolve.comp. Mirrors
// vk_temporal.cpp's VK_RT_InitTemporalPipeline (AO's EMA pass), with three
// storage images instead of two and a UBO instead of a push-constant block
// (see ReflTemporalUBO's comment for why).
// ---------------------------------------------------------------------------

static void VK_RT_InitReflTemporalPipeline(void)
{
    // binding 0: currentImage  (reflBuffer[slot], readonly)
    // binding 1: srcHistory    (reflHistory[ping], readonly — last frame's result)
    // binding 2: dstHistory    (reflHistory[pong], writeonly — this frame's result)
    // binding 3: srcMoments    (reflMoments[ping], readonly)
    // binding 4: dstMoments    (reflMoments[pong], writeonly)
    // binding 5: depthSampler
    // binding 6: gbufNormalSampler
    // binding 7: motionSampler (motionVectors[slot])
    // binding 8: ReflTemporalUBO (dynamic)
    //
    // src/dst are separate bindings, not one read-write image, because this pass
    // reprojects: it reads at Q and writes at P, and P == Q for some other
    // invocation in the same dispatch with no ordering between them. A single
    // read-write image would race; ping-ponging across frames (vk_raytracing.h)
    // avoids it entirely — src is strictly last frame's, dst is strictly this
    // frame's, and nothing in this dispatch ever writes what it also reads.
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

    VkDescriptorPoolSize poolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * VK_MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolCI = {};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = VK_MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 3;
    poolCI.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &poolCI, NULL, &vkRT.reflTemporalDescPool));

    VkDescriptorSetLayout layouts[VK_MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = vkRT.reflTemporalDescLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.reflTemporalDescPool;
    dsAlloc.descriptorSetCount = VK_MAX_FRAMES_IN_FLIGHT;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, vkRT.reflTemporalDescSets));
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.reflTemporalDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Refl: temporal denoiser pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflAtrousPipeline (r_rtReflDenoise 2, Stage 1 addendum)
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
    //   binding 4: COMBINED_IMAGE_SAMPLER — gbufNormal (normal edge stop, Stage 3)
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
    const int totalSets = VK_MAX_FRAMES_IN_FLIGHT * 3;
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
    for (int i = 0; i < totalSets; i++)
        layouts[i] = vkRT.reflAtrousDescLayout;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = vkRT.reflAtrousDescPool;
    dsAlloc.descriptorSetCount = totalSets;
    dsAlloc.pSetLayouts = layouts;
    // Allocate flat: [0][0],[0][1],[0][2],[1][0],[1][1],[1][2]
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &dsAlloc, &vkRT.reflAtrousDescSets[0][0]));

    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
        vkRT.reflAtrousDescSetLastUpdatedFrameCount[i] = -1;

    common->Printf("VK RT Refl Atrous: spatial filter pipeline initialized\n");
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflections (public entry)
// ---------------------------------------------------------------------------

void VK_RT_InitReflections(void)
{
    VK_RT_InitReflPipeline();
    VK_RT_InitReflTemporalPipeline();
    VK_RT_InitReflAtrousPipeline();
    VK_RT_ResizeReflections(vk.swapchainExtent.width, vk.swapchainExtent.height);
    // Step 8: only when vk.gbufferSupported — reflect_ray.rgen's Fresnel/debug-mode
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
    // Composite pipeline (Step 8)
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
    // Temporal denoiser (Stage 1)
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
    VK_RT_DestroyReflDenoiseImages();

    // Spatial filter (r_rtReflDenoise 2, Stage 1 addendum)
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
    VK_RT_DestroyReflAtrousImages();

    VK_RT_DestroyNullLightSsbo();
    VK_RT_DestroyNullGbufNormal();
    VK_RT_ShutdownReflEmitters();
    VK_RT_DestroyReflImages();
}

// ---------------------------------------------------------------------------
// VK_RT_ResizeReflections (public)
// ---------------------------------------------------------------------------

void VK_RT_ResizeReflections(uint32_t width, uint32_t height)
{
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_REFLECTIONS);
    VK_RT_DestroyReflImages();
    VK_RT_CreateReflImages(width, height);
    VK_RT_DestroyReflDenoiseImages();
    VK_RT_CreateReflDenoiseImages(width, height);
    VK_RT_DestroyReflAtrousImages();
    VK_RT_CreateReflAtrousImages(width, height);

    // Force descriptor set refresh at next dispatch
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkRT.reflDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.reflTemporalDescSetLastUpdatedFrameCount[i] = -1;
        vkRT.reflAtrousDescSetLastUpdatedFrameCount[i] = -1;
    }
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflections (public)
// Dispatch reflection rays for the full screen.  Called once per frame.
// Must be outside a render pass.  On entry depth is ATTACHMENT_OPTIMAL;
// transitions to READ_ONLY_OPTIMAL for the dispatch, then restores.
// On exit reflBuffer[currentFrame] is in GENERAL layout, readable by
// the interaction fragment shader (barrier issued before returning).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VK_RT_GlassScreenRect (R6)
// Union of every SURFTYPE_GLASS surface's frontend scissorRect, in Vulkan Y-down
// pixels. False = no glass in view, caller skips the dispatch. Y-flip matches
// VK_ComputeDrawSurfScissor.
//
// The rect indexes reflBuffer, which stays display-sized but is written
// identity-mapped into the render sub-rect, so the frontend's display-space rect
// has to be scaled into render space and clamped against renderExtent — not
// against the buffer. Getting this wrong traces a region the glass overlay never
// samples, which reads as reflections quietly going missing rather than as
// visible corruption.
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
        if (surf->material->GetSurfaceType() != SURFTYPE_GLASS)
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

// The launch rect actually traced this frame, and the frame it was traced on — read
// by VK_RT_DispatchReflTemporal so it blends exactly the pixels reflect_ray.rgen
// wrote and nothing stale outside that rect (debug mode or glass-rect dependent).
static VkRect2D s_reflDispatchRect[VK_MAX_FRAMES_IN_FLIGHT] = {};
static int s_reflDispatchRectFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};

void VK_RT_DispatchReflections(VkCommandBuffer cmd, const viewDef_t *viewDef)
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
    static int s_lastReflDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastReflDispatchFrame[frameIdx] == tr.frameCount)
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Refl: skip duplicate dispatch frame=%d slot=%d\n", tr.frameCount, frameIdx);
        return;
    }
    s_lastReflDispatchFrame[frameIdx] = tr.frameCount;

    vkRTImage_t &rb = vkRT.reflBuffer[frameIdx];
    if (rb.image == VK_NULL_HANDLE)
    {
        common->Printf("VK RT Refl: skip — reflBuffer[%d] image is NULL\n", frameIdx);
        return;
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl: frame=%d slot=%d size=%ux%u tlas=%p pipeline=%p gbuf=%s\n", tr.frameCount, frameIdx,
                       rb.width, rb.height, (void *)vkRT.tlas[frameIdx].handle, (void *)vkRT.reflPipeline,
                       (vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE) ? "on" : "off");

    // R6: resolve the launch rect before anything else — a glass-free view must not pay
    // the depth transitions, UBO upload or descriptor churn either. Safe to return here
    // because the depth barrier pair below is symmetric (ATTACHMENT on entry and exit).
    const int reflDebugMode = r_rtReflectionDebugMode.GetInteger();
    const bool reflDebugActive = (reflDebugMode >= 2 && reflDebugMode <= REFL_DEBUG_MAX_MODE);
    const bool glassOnly = (r_rtReflectionMode.GetInteger() == 1) && !reflDebugActive;
    int32_t rectX = 0, rectY = 0;
    uint32_t rectW = rb.width, rectH = rb.height;
    if (glassOnly && !VK_RT_GlassScreenRect(viewDef, rb.width, rb.height, &rectX, &rectY, &rectW, &rectH))
    {
        if (r_vkLogRT.GetInteger() >= 1)
            common->Printf("VK RT Refl: no glass in view — dispatch skipped entirely\n");
        return;
    }

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
    ubo.debugMode = r_rtReflectionDebugMode.GetInteger();
    ubo.sceneHasGlass = vkRT.sceneHasGlass ? 1 : 0;
    ubo.minWeight = idMath::ClampFloat(0.0f, 1.0f, r_rtReflectionMinWeight.GetFloat());
    ubo.roughLumExp = idMath::ClampFloat(0.0f, 8.0f, r_rtReflRoughLumExp.GetFloat());

    ubo.rectOriginX = rectX;
    ubo.rectOriginY = rectY;
    // Debug modes other than 13 want the full, uncensored per-pixel data (never gated
    // by mode 1's glass-only rect or mode 3's roughness ceiling) — mode 13 is the one
    // exception, since it exists specifically to visualize that ceiling, so it forces
    // mode 3 regardless of what r_rtReflectionMode is actually set to.
    // Modes 14/15 keep the real mode so they classify the pixels normal play traces.
    if (reflDebugActive && reflDebugMode != 14 && reflDebugMode != 15)
        ubo.reflMode = (reflDebugMode == 13) ? 3 : 2;
    else
        ubo.reflMode = idMath::ClampInt(1, 3, r_rtReflectionMode.GetInteger());
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
            // Stage 5: the same clamp vk_vol_froxel.cpp applies, so the reflected
            // segment and the primary view evaluate one phase function, not two.
            ubo.volIsoMix = idMath::ClampFloat(0.0f, 1.0f, r_rtVolIsotropicMix.GetFloat());
        }
    }
    ubo.glassNormalMode = idMath::ClampInt(0, 2, r_rtGlassNormalMode.GetInteger());
    ubo.glassCreaseRatio = idMath::ClampFloat(0.0f, 1.0f, r_rtGlassCreaseRatio.GetFloat());
    extern idCVar r_rtGuiReflGain; // vk_gui_texture.cpp
    ubo.guiReflGain = idMath::ClampFloat(0.0f, 16.0f, r_rtGuiReflGain.GetFloat());
    memcpy(uboMapped, &ubo, sizeof(ReflParamsUBO));

    // --- Update descriptor set (once per frame slot when frameCount changes).
    // Also force refresh when bound resources changed inside the same frame to
    // avoid stale cached descriptors after RT resource churn.
    static VkAccelerationStructureKHR s_lastReflTlasHandle[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastReflStorageView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastReflDepthView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkBuffer s_lastReflLightSsbo[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastReflGbufView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastReflGbufAlbedoView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastReflFroxelView[VK_MAX_FRAMES_IN_FLIGHT] = {};
    static VkImageView s_lastReflFroxelDirView[VK_MAX_FRAMES_IN_FLIGHT] = {};

    // Pick the GI light SSBO for this frame; fall back to the null SSBO if not ready.
    VkBuffer lightSsbo = (vkRT.giLightSsbo[frameIdx] != VK_NULL_HANDLE) ? vkRT.giLightSsbo[frameIdx] : s_nullLightSsbo;

    // G-buffer normal/F0 (Stage 3.5): real per-frame image when supported, else the
    // 1x1 null image (a=0, same "no data" sentinel) — see VK_RT_CreateNullGbufNormal.
    // Both are kept in SHADER_READ_ONLY_OPTIMAL here: the real one by the barrier in
    // vk_backend.cpp (Step 6), the null one permanently since nothing else writes it.
    const bool haveGbuf = vk.gbufferSupported && vkRT.gbufNormal[frameIdx].view != VK_NULL_HANDLE;
    VkImageView gbufView = haveGbuf ? vkRT.gbufNormal[frameIdx].view : s_nullGbufView;

    // gbufAlbedo (Stage 2): same supported/null fallback as gbufNormal above — the
    // shared null image is never written, so the class decodes to the "ordinary, zero
    // roughness" default rather than anything path-dependent.
    const bool haveGbufAlbedo = vk.gbufferSupported && vkRT.gbufAlbedo[frameIdx].view != VK_NULL_HANDLE;
    VkImageView gbufAlbedoView = haveGbufAlbedo ? vkRT.gbufAlbedo[frameIdx].view : s_nullGbufView;

    // Swaps between the live grid and the null one whenever r_rtVolFroxel is toggled,
    // and changes outright when the res cvars force a realloc.
    const VkImageView froxelView = VK_RT_GetFroxelScatterView();
    // Stage 5: fetched through the same predicate, so the pair can never end up
    // split across a toggle or a realloc.
    const VkImageView froxelDirView = VK_RT_GetFroxelDirView();

    const bool reflResourceChanged =
        (s_lastReflTlasHandle[frameIdx] != vkRT.tlas[frameIdx].handle) ||
        (s_lastReflStorageView[frameIdx] != rb.view) || (s_lastReflDepthView[frameIdx] != vk.depthSampledView) ||
        (s_lastReflLightSsbo[frameIdx] != lightSsbo) || (s_lastReflGbufView[frameIdx] != gbufView) ||
        (s_lastReflGbufAlbedoView[frameIdx] != gbufAlbedoView) || (s_lastReflFroxelView[frameIdx] != froxelView) ||
        (s_lastReflFroxelDirView[frameIdx] != froxelDirView);

    if (reflResourceChanged && vkRT.reflDescSetLastUpdatedFrameCount[frameIdx] == tr.frameCount &&
        r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK RT Refl: forcing descriptor refresh due to in-frame resource change frame=%d slot=%d "
                       "tlas=%p reflView=%p depthView=%p gbufView=%p\n",
                       tr.frameCount, frameIdx, (void *)vkRT.tlas[frameIdx].handle, (void *)rb.view,
                       (void *)vk.depthSampledView, (void *)gbufView);
    }

    bool refreshSet = (vkRT.reflDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount) || reflResourceChanged;
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.reflDescSets[frameIdx];

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = {};
        tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &vkRT.tlas[frameIdx].handle;

        VkDescriptorImageInfo reflImgInfo = {};
        reflImgInfo.imageView = rb.view;
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

        // Stage 5's mean-direction grid. Same trilinear sampler: interpolating the
        // vector shortens it where neighbouring cells disagree, which reads as more
        // isotropic — the correct degradation, see rt_PhaseFromMeanDir.
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
        vkRT.reflDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
        s_lastReflTlasHandle[frameIdx] = vkRT.tlas[frameIdx].handle;
        s_lastReflStorageView[frameIdx] = rb.view;
        s_lastReflDepthView[frameIdx] = vk.depthSampledView;
        s_lastReflLightSsbo[frameIdx] = lightSsbo;
        s_lastReflGbufView[frameIdx] = gbufView;
        s_lastReflGbufAlbedoView[frameIdx] = gbufAlbedoView;
        s_lastReflFroxelView[frameIdx] = froxelInfo.imageView;
        s_lastReflFroxelDirView[frameIdx] = froxelDirInfo.imageView;
    }

    // --- Dispatch ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipelineLayout, 0, 1,
                            &vkRT.reflDescSets[frameIdx], 1, &uboOff);
    // set=1: material table (MatTable SSBO, VtxAddrTable, IdxAddrTable, bindless textures)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkRT.reflPipelineLayout, 1, 1,
                            &vkRT.matDescSet[vk.currentFrame], 0, NULL);

    if (r_vkLogRT.GetInteger() >= 1)
        // glowTris is the separate population: additive translucent surfaces the BLAS drops,
        // which is the only way a beam or sprite reaches a reflection as geometry.  emitters
        // counts discs (projectiles, smoke) and says nothing about those.
        common->Printf(
            "VK RT Refl: dispatch %ux%u at (%d,%d) mode=%d maxDist=%.1f emitters=%d glowTris=%d volSteps=%d\n", rectW,
            rectH, rectX, rectY, ubo.reflMode, ubo.maxDist, ubo.emitterCount, ubo.glowTriCount, ubo.volSteps);

    s_reflDispatchRect[frameIdx] = VkRect2D{{rectX, rectY}, {rectW, rectH}};
    s_reflDispatchRectFrame[frameIdx] = tr.frameCount;

    vkCmdTraceRaysKHR(cmd, &vkRT.reflRgenRegion, &vkRT.reflMissRegion, &vkRT.reflHitRegion, &vkRT.reflCallRegion, rectW,
                      rectH, 1);

    // --- Barrier: reflection write -> fragment shader read, and (Stage 1,
    // 20261013_reflection_roughness.md) -> VK_RT_DispatchReflTemporal's compute read ---
    {
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &depthRestore);
    }

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl: dispatch complete\n");
}

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflTemporal (public)
// Temporal EMA denoise of reflBuffer (Stage 1). See refl_temporal_resolve.comp
// for the per-pixel algorithm. No-op (and reflHistoryValid stays whatever it
// was) whenever nothing new was traced into reflBuffer this frame — matches
// VK_RT_DispatchReflections's own early-outs exactly via s_reflDispatchRectFrame.
// ---------------------------------------------------------------------------

void VK_RT_DispatchReflTemporal(VkCommandBuffer cmd, const viewDef_t *viewDef)
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
    // re-run the EMA against reflBuffer a second time and read back what it just wrote.
    static int s_lastReflTemporalDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastReflTemporalDispatchFrame[frameIdx] == tr.frameCount)
        return;

    if (s_reflDispatchRectFrame[frameIdx] != tr.frameCount)
        return; // VK_RT_DispatchReflections traced nothing this frame — nothing to blend

    const VkRect2D dispatchRect = s_reflDispatchRect[frameIdx];
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    if (vkRT.reflHistory[0].image == VK_NULL_HANDLE || vkRT.reflHistory[1].image == VK_NULL_HANDLE ||
        vkRT.reflMoments[0].image == VK_NULL_HANDLE || vkRT.reflMoments[1].image == VK_NULL_HANDLE ||
        vkRT.gbufNormal[frameIdx].view == VK_NULL_HANDLE || vkRT.motionVectors[frameIdx].view == VK_NULL_HANDLE)
        return;

    s_lastReflTemporalDispatchFrame[frameIdx] = tr.frameCount;

    // reflHistory/reflMoments are shared across both frame-in-flight slots (ping-ponged
    // across frames, not slots), so this dispatch's read of last frame's slot and write
    // of this frame's slot must be ordered after whatever last touched them — the other
    // slot's compute write, or that write's own downstream fragment consumers. Same
    // reasoning and same barrier shape as VK_RT_DispatchTemporalResolveAO's equivalent
    // (vk_temporal.cpp): submission order on the single RT queue already sequences the
    // two; this barrier supplies the memory-visibility guarantee ordering alone doesn't.
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
        VK_RT_DetectCameraCut(viewDef, vkRT.reflPrevCamPos, vkRT.reflPrevCamFwd, vkRT.reflHistoryValid, "ReflTemporal");
    // !prevFrameValid (subview/mirror/2D-overlay/first-frame boundary, tr_local.h) means
    // viewDef->prevViewProjMatrix may be stale or zeroed — the depth disocclusion test
    // would divide by a zero prevClip.w. Force a reset rather than letting that surface
    // as a NaN-driven rejection.
    const bool reset = !vkRT.reflHistoryValid || cut.isCut || !viewDef->prevFrameValid;
    if (reset && cut.isCut && r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl Temporal: camera cut detected posDelta=%.3f angleDelta=%.3f — resetting history\n",
                       cut.posDelta, cut.angleDelta);
    vkRT.reflHistoryValid = true;

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
    // Not exposed as cvars (Stage 1 scope, see the plan) — tune here if a scene needs it.
    ubo.varianceGain = 4.0f;
    ubo.depthRejectThresh = 0.02f;
    ubo.normalRejectCos = 0.8f;
    ubo.scissorOffsetX = dispatchRect.offset.x;
    ubo.scissorOffsetY = dispatchRect.offset.y;
    ubo.scissorExtentX = (int32_t)dispatchRect.extent.width;
    ubo.scissorExtentY = (int32_t)dispatchRect.extent.height;
    ubo.resetHistory = reset ? 1 : 0;
    ubo.debugMode = r_rtReflectionDebugMode.GetInteger();
    ubo.maxAge = 32;
    ubo.fireflyClamp = idMath::ClampFloat(0.0f, 8.0f, r_rtReflAtrousFireflyClamp.GetFloat());
    ubo._pad0[0] = 0.0f;
    memcpy(uboMapped, &ubo, sizeof(ReflTemporalUBO));

    // Read last frame's result, write the other slot — see vk_raytracing.h's comment
    // on reflPingPong for why this can't be one read-write image per buffer.
    const int srcIdx = vkRT.reflPingPong;
    const int dstIdx = 1 - srcIdx;

    // --- Update descriptor set (once per frame slot; src/dst flip every frame, so this
    // must actually run every frame rather than being skipped as "already up to date") ---
    bool refreshSet = (vkRT.reflTemporalDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount);
    if (refreshSet)
    {
        VkDescriptorSet ds = vkRT.reflTemporalDescSets[frameIdx];

        VkDescriptorImageInfo currInfo = {};
        currInfo.imageView = vkRT.reflBuffer[frameIdx].view;
        currInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo srcHistInfo = {};
        srcHistInfo.imageView = vkRT.reflHistory[srcIdx].view;
        srcHistInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dstHistInfo = {};
        dstHistInfo.imageView = vkRT.reflHistory[dstIdx].view;
        dstHistInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo srcMomInfo = {};
        srcMomInfo.imageView = vkRT.reflMoments[srcIdx].view;
        srcMomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo dstMomInfo = {};
        dstMomInfo.imageView = vkRT.reflMoments[dstIdx].view;
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
        vkRT.reflTemporalDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
    }

    // --- Dispatch ---
    uint32_t groupsX = (dispatchRect.extent.width + 7) / 8;
    uint32_t groupsY = (dispatchRect.extent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflTemporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflTemporalPipelineLayout, 0, 1,
                            &vkRT.reflTemporalDescSets[frameIdx], 1, &uboOff);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // dstIdx now holds this frame's result — next frame (and this frame's fragment
    // consumers, via VK_RT_GetReflReadView) read from here.
    vkRT.reflPingPong = dstIdx;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl Temporal: dispatch slot=%d alpha=%.3f glassAlpha=%.3f rect=(%d,%d %u,%u) reset=%d "
                       "ping=%d->%d\n",
                       frameIdx, ubo.baseAlpha, ubo.glassAlpha, dispatchRect.offset.x, dispatchRect.offset.y,
                       (unsigned int)dispatchRect.extent.width, (unsigned int)dispatchRect.extent.height,
                       ubo.resetHistory, srcIdx, dstIdx);

    // --- Barrier: compute write -> fragment read (composite/overlay/interaction) and
    // next frame's own compute read of reflHistory/reflMoments ---
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

// ---------------------------------------------------------------------------
// VK_RT_DispatchReflAtrous (public) — r_rtReflDenoise 2, Stage 1 addendum.
// Roughness-gated à-trous spatial filter, run on top of the temporal resolve's
// output. Mirrors VK_RT_DispatchGIAtrous's N-pass ping-pong structure
// (vk_gi.cpp) — see refl_atrous.comp for the per-pixel roughness gate itself.
// Must run after VK_RT_DispatchReflTemporal and before the resumed render pass.
// ---------------------------------------------------------------------------
void VK_RT_DispatchReflAtrous(VkCommandBuffer cmd, const viewDef_t *viewDef)
{
    const int frameIdx = vk.currentFrame;

    // Guard against duplicate dispatch in the same frame (weapon subview etc.) — mirrors
    // VK_RT_DispatchReflTemporal's own guard. Must come before the reflAtrousReadView
    // clear below: a second call this frame must leave the first call's result (already
    // the correct one to read, and already composited once) alone rather than null it
    // out, and must not re-run the N full-screen passes against bufA/B a second time.
    static int s_lastReflAtrousDispatchFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};
    if (s_lastReflAtrousDispatchFrame[frameIdx] == tr.frameCount)
        return;

    // Stale from a previous frame (denoise toggled down, or an early return below)
    // must never leak forward — VK_RT_GetReflReadView checks this every frame.
    vkRT.reflAtrousReadView[frameIdx] = VK_NULL_HANDLE;

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

    if (s_reflDispatchRectFrame[frameIdx] != tr.frameCount)
        return; // nothing traced this frame — mirrors VK_RT_DispatchReflTemporal's own guard

    const VkRect2D dispatchRect = s_reflDispatchRect[frameIdx];
    if (dispatchRect.extent.width == 0 || dispatchRect.extent.height == 0)
        return;

    vkRTImage_t &bufA = vkRT.reflAtrousA[frameIdx];
    vkRTImage_t &bufB = vkRT.reflAtrousB[frameIdx];
    if (bufA.image == VK_NULL_HANDLE || bufB.image == VK_NULL_HANDLE)
        return;
    if (vkRT.gbufAlbedo[frameIdx].view == VK_NULL_HANDLE || vkRT.gbufNormal[frameIdx].view == VK_NULL_HANDLE)
        return; // bound below with no other check — VK_RT_DispatchReflTemporal checks its
                // own gbuf views the same way before dispatching

    s_lastReflAtrousDispatchFrame[frameIdx] = tr.frameCount;

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
    //                  view's underlying image alternates with reflPingPong)
    // DS[frameIdx][1]: bufA(sampler) -> bufB — stable after images are created
    // DS[frameIdx][2]: bufB(sampler) -> bufA — stable after images are created
    if (vkRT.reflAtrousDescSetLastUpdatedFrameCount[frameIdx] != tr.frameCount)
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
        srcInfos[0] = {vkRT.reflSampler, VK_RT_GetReflReadView(frameIdx), VK_IMAGE_LAYOUT_GENERAL}; // temporal -> A
        srcInfos[1] = {vkRT.reflSampler, bufA.view, VK_IMAGE_LAYOUT_GENERAL};                       // A -> B
        srcInfos[2] = {vkRT.reflSampler, bufB.view, VK_IMAGE_LAYOUT_GENERAL};                       // B -> A

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
                w.dstSet = vkRT.reflAtrousDescSets[frameIdx][s];
                w.dstBinding = (uint32_t)b;
                w.descriptorCount = 1;
                w.descriptorType =
                    (b == 1) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = infos[b];
            }
        }
        vkUpdateDescriptorSets(vk.device, 15, writes, 0, NULL);
        vkRT.reflAtrousDescSetLastUpdatedFrameCount[frameIdx] = tr.frameCount;
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
        pc.debugMode = r_rtReflectionDebugMode.GetInteger();
        pc.normalPower = normalPower;
        memcpy(pc.invViewProj, invViewProj, sizeof(pc.invViewProj));

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkRT.reflAtrousPipelineLayout, 0, 1,
                                &vkRT.reflAtrousDescSets[frameIdx][descIdx], 0, NULL);
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
    vkRT.reflAtrousReadView[frameIdx] = finalInA ? bufA.view : bufB.view;

    if (r_vkLogRT.GetInteger() >= 1)
        common->Printf("VK RT Refl Atrous: dispatch slot=%d iters=%d rect=(%d,%d %u,%u)\n", frameIdx, iters,
                       dispatchRect.offset.x, dispatchRect.offset.y, (unsigned int)dispatchRect.extent.width,
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

// View for any downstream reflection consumer. See the header comment.
VkImageView VK_RT_GetReflReadView(int frameIdx)
{
    const int debugMode = r_rtReflectionDebugMode.GetInteger();
    // Modes 2-4, 6, 12 and 13 are instantaneous per-frame visualizations baked into
    // reflBuffer by reflect_ray.rgen itself — routing them through the EMA'd history
    // would smear them across frames and defeat the point. Mode 11 is the one
    // exception: it IS the denoiser's own history/rejection state.
    // Modes 14/15 are the à-trous pass's own views, so they read the filter output.
    if (debugMode >= 2 && debugMode <= REFL_DEBUG_MAX_MODE && debugMode != 11 && debugMode != 14 && debugMode != 15)
        return vkRT.reflBuffer[frameIdx].view;
    // Mode 11 IS the denoiser's own history/rejection state (written directly into
    // reflHistory by refl_temporal_resolve.comp, alpha forced to 1.0 — see its own
    // comment) — show it exactly as the temporal pass wrote it. The spatial filter
    // would otherwise treat that alpha as "a real traced pixel" and blur the red/
    // green rejection boundary the mode exists to let you read precisely.
    if (debugMode != 11 && r_rtReflDenoise.GetInteger() >= 2 && vkRT.reflAtrousReadView[frameIdx] != VK_NULL_HANDLE)
        return vkRT.reflAtrousReadView[frameIdx];
    if (r_rtReflDenoise.GetInteger() > 0 && vkRT.reflHistoryValid &&
        vkRT.reflHistory[vkRT.reflPingPong].view != VK_NULL_HANDLE)
        return vkRT.reflHistory[vkRT.reflPingPong].view;
    return vkRT.reflBuffer[frameIdx].view;
}

// ---------------------------------------------------------------------------
// VK_RT_InitReflCompositePipeline (Step 8, see docs/plans/gbuffer_normal_pass.md)
// Fullscreen pass that blends reflBuffer onto the framebuffer once per view,
// replacing the disabled per-light reflection block in interaction.frag.
// Deliberately as simple as VK_RT_InitGICompositePipeline's pipeline (single
// sampler binding): the Schlick Fresnel term and debug-mode visualization both
// live in reflect_ray.rgen instead of here — see refl_composite.frag's header
// comment for why (depth can't be sampled while bound read-write by the render
// pass this composite draws inside).
//
// Two pipeline objects share this layout/shader: reflCompositePipeline (additive,
// normal operation) and reflCompositeDebugPipeline (blend disabled) selected by
// VK_RT_CompositeReflections when r_rtReflectionDebugMode is active, so the debug
// visualization baked into reflBuffer by the rgen replaces the lit scene instead
// of adding onto it.
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
    // visualization (baked into reflBuffer by the rgen) isn't muddied by additive
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
    vkRTImage_t &rb = vkRT.reflBuffer[frameIdx];
    if (rb.image == VK_NULL_HANDLE || vkRT.reflSampler == VK_NULL_HANDLE)
        return;

    const int debugMode = r_rtReflectionDebugMode.GetInteger();
    const bool debugActive = (debugMode >= 2 && debugMode <= REFL_DEBUG_MAX_MODE);

    // R6: glass-only has nothing for this pass — opaque is never traced and glass is
    // composited per-surface. It would also read stale texels outside the traced rect.
    // Checked before the descriptor write so the skipped path costs nothing.
    if (r_rtReflectionMode.GetInteger() == 1 && !debugActive)
        return;

    // Update the descriptor set for this frame slot. The reflection image may
    // have been recreated (resize), so always write it before drawing.
    // VK_RT_GetReflReadView: reflHistory when the denoiser is on and valid (except
    // debug modes 2-4, 6, 12 and 13, which are rgen's own unfiltered per-frame visualizations).
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
