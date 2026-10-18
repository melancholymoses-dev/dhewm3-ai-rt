# Dhewm3-AI-RT Settings Inventory

Cvars added for the Vulkan / ray-tracing renderer and the first-/third-person player model, with the trim target for the settings menu (`neo/framework/Dhewm3SettingsMenu.cpp`).

**Re-audited 2026-10-18** against `main` after the reflection arcs (8d roughness, 8f GUI
reflections, 8g pipeline refactor Stages 1-4). Measured counts: **255 `r_rt*`/`r_vk*`/`r_fsr*`
cvars, 116 menu widgets, 44 debug modes.** Execution plan: `plans/20261010_cleanup_polish.md`
Phase 1.

**Verdict key** (replaces the old P/A/D tiers — the old letters map M/C/C-or-X)

| Verdict | Meaning | Where it lives |
|---|---|---|
| **M** | Menu. Player understands it and it changes the look or cost visibly | Main RT page or the preset |
| **C** | Console only, kept and documented. Real knob, rarely touched | this doc §2 |
| **X** | Delete — with its UBO field, shader branch, menu widget and CMake entry | nowhere |

Target: **~30 M**, **~120 C**, **~105 X**. That is 116 menu widgets down to ~30.

## 1. Player menu (the ~30 **M** cvars)

| Menu section | Widget | Cvar |
|---|---|---|
| Ray Tracing | Enable RT | `r_useRayTracing` |
| Features | Shadows / AO / Reflections / GI / Volumetric light | `r_rtShadows`, `r_rtAO`, `r_rtReflections`, `r_rtGI`, `r_rtVol` |
| Features | Player shadow + reflection | `g_showPlayerShadow` |
| Quality | **New** preset Low / Medium / High / Ultra (see §3) | writes the **C** sample/res cvars |
| Look | GI strength, Fog density | `r_rtGIStrength`, `r_rtVolExtinction` |
| Look | Tonemap on, Exposure | `r_rtTonemap`, `r_rtTonemapExposure` |
| Look | Bloom on, Bloom strength | `r_rtBloom`, `r_rtBloomStrength` |
| **Reflections** | Reflection strength, Highlight ceiling | `r_rtReflectionBlend`, `r_rtReflMaxLuminance` |
| **Reflections** | Reflectivity scale, Reflectivity contrast | `r_rtSpecF0Scale`, `r_rtSpecF0Gamma` |
| **Reflections** | Half-res opaque trace, Live GUI screens | `r_rtReflOpaqueHalfRes`, `r_rtGuiReflections` |
| Upscaling | FSR mode, Quality, Sharpness | `r_fsr`, `r_fsrQuality`, `r_fsrSharpness` |
| Player model | Show own body, Third person, Third-person distance | `pm_showFirstPersonBody`, `pm_thirdPerson`, `pm_thirdPersonRange` |
| Experimental | Auto-relight | `r_rtAutoRelight` |

The Reflections group is new in this revision. F0 is the 8d/8g win — `r_rtSpecF0Scale/Gamma`
are the two knobs that set how reflective the world reads, and both are already tuned
(0.35 / 2.5, up from the audit's 0.2 / 3). `r_rtReflOpaqueHalfRes` is in the menu because it
is worth 30% of GPU frame time at no visible cost, so it is a real perf choice, not a debug
A/B.

`CVAR_ARCHIVE` still missing on `r_rtGIStrength`, `r_rtVolGain`, `r_rtGIRadius`,
`r_rtGISamples`, `r_rtAORadius`, `r_rtVolMaxDist`, `r_rtAOTemporalAlpha` — Phase 5a fixes
this. `r_rtDenoise` (the dead "Temporal Denoising" checkbox) is **resolved**: the menu now
binds `r_rtReflDenoise`, and no `r_rtDenoise` exists in the renderer. §4's row is closed.

## 2. Full table

### Core RT and Vulkan feature switches

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_useRayTracing` | Core | Master RT switch | 1 | P | = |
| `r_rtShadows` | Core | RT shadows on/off | 1 | P | = |
| `r_rtAO` | Core | RT ambient occlusion on/off | 1 | P | = |
| `r_rtReflections` | Core | RT reflections on/off | 1 | P | = |
| `r_rtGI` | Core | RT global illumination on/off | 1 | P | = |
| `r_rtVol` | Core | RT volumetric lighting on/off | 1 | P | = |
| `r_rtDeformedTangents` | Core | Use deformed (skinned) tangents in RT shading | 1 | D | = |
| `r_rtGbufNormals` | Core | AO/GI take normals from the G-buffer | 1 | D | = |
| `r_rtNearDynRadius` | TLAS | Off-screen dynamic entities within this radius are forced into the TLAS | 512 | A | = |
| `r_rtTlasFallbackRadius` | TLAS | Pass-2 radius for off-screen static geometry (0 = auto from GI/vol/reflection ranges) | 0 | A | = |
| `r_rtTlasDump` | TLAS | One-shot dump of the next TLAS build | 0 | D | = |
| `r_vkLogTLASSlow` | TLAS | Section breakdown of TLAS CPU time on frames over N ms (pass1/2/3, materials, bindless, build) | 0 | D | = |
| `r_vkBLASPoolCheck` | TLAS | Validate BLAS storage/scratch/geometry pool ranges on build and refit | 0 | D | = |
| `r_vkRTReflDataDiag` | TLAS | Reflection hit-data diagnostics | 0 | D | = |

### Shadows

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtShadowSamples` | Shadows | Shadow rays per pixel | 2 | A (preset) | 1 / 2 / 4 by preset |
| `r_rtShadowBlur` | Shadows | Shadow blur radius/passes | 6 | A | = |
| `r_rtShadowBlurEnable` | Shadows | Blur on/off | 1 | A | = |
| `r_rtShadowBlurDepthAware` | Shadows | Depth-aware blur | 1 | D | = |
| `r_rtShadowBlurDepthThreshold` | Shadows | Depth edge threshold for the blur | 0.003 | D | = |
| `r_rtShadowBlurMinRect` | Shadows | Skip blur for lights with small dispatch rects | 64 | D | = |
| `r_rtShadowRayBias` | Shadows | Ray origin bias against self-shadowing | 0.25 | A | = |
| `r_rtShadowBiasErrMargin` | Shadows | Safety multiple on the depth-reconstruction bias floor | 6.0 | D | = |
| `r_rtShadowSoftRadiusScale` | Shadows | Light radius → soft-shadow source radius | 0.08 | A | = |
| `r_rtShadowSoftRadiusMin` | Shadows | Min soft-shadow source radius | 0.0 | D | = |
| `r_rtShadowSoftRadiusMax` | Shadows | Max soft-shadow source radius | 6.0 | D | = |
| `r_rtShadowMinSamples` | Shadows | Floor on shadow rays/pixel | 1 | D | = |
| `r_rtShadowVolumeCull` | Shadows | Skip shadow rays outside the light volume (debug mode 11 shows the cull). Cleanup 1b deletes it — default is the shipped behaviour | 1 | **X** |
| `r_rtShadowTemporalJitter` | Shadows | Vary sample pattern per frame | 1 | **X** |
| `r_rtShadowTemporalJitterMinSamples` | Shadows | Min samples before temporal jitter applies | 4 | D | = |
| `r_rtShadowStablePattern` | Shadows | Stable per-light jitter seed | 0 | D | = |
| `r_rtShadowMaskProjected` | Shadows | Apply RT shadow mask to projected lights | 1 | D | = |
| `r_rtFlashlightBias` | Shadows | Ray bias for the flashlight | 17 | A | = |
| `r_rtShadowDebugMode` | Shadows | Visualize shadow-pass internals | 0 | D | = |
| `r_rtUnlockNoShadows` | Shadows | Give ACCENT noShadows lights a real RT shadow | 0 | A | = |
| `r_rtLightAccentMaxRadius` | Shadows | noShadows light radius below this is "accent" | 300 | A | = |

### Ambient occlusion

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtAOSamples` | AO | AO rays per pixel | 4 | A (preset) | 2 / 4 / 4 / 8 |
| `r_rtAORadius` | AO | Max AO ray length (keep below probe spacing) | 24 | A | = |
| `r_rtAOFalloff` | AO | Smooth distance falloff of occlusion | 1 | D | = |
| `r_rtAOIndirectStrength` | AO | How strongly AO darkens GI | 1.0 | A | = |
| `r_rtAODirectStrength` | AO | How strongly AO darkens direct light with GI on | 0.0 | A | = |
| `r_rtAOFadeStart` | AO | Distance where AO starts fading | 2000 | D | = |
| `r_rtAOFadeEnd` | AO | Distance where AO ends | 3000 | D | = |
| `r_rtAODebug` | AO | AO/GI debug view | 0 | D | = |
| `r_rtAODebugGain` | AO | Gain for AO debug views | 4.0 | D | = |

### Reflections and specular

Rewritten 2026-10-18. The 8d roughness, 8f GUI and 8g refactor arcs added 24 cvars this
table had never listed, and moved six defaults. Defaults below are read from source.

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtReflectionMode` | Reflections | 1 = glass only, 3 = opaque + glass (default). Mode 2 is legacy, deleted in cleanup Phase 2 | **3** | C |
| `r_rtReflectionDistance` | Reflections | Max reflection ray distance | **1000** | C |
| `r_rtReflectionBlend` | Reflections | Reflection strength. The one brightness knob | 6.0 | **M** |
| `r_rtReflMaxLuminance` | Reflections | Flat ceiling on incident reflected light — the single-sample GGX lobe's outlier guard. Rescaled in 8g 2b when the ceiling moved past the Fresnel multiply | **6.0** | **M** |
| `r_rtSpecF0Scale` | Reflections | F0 remap scale: how reflective the world reads | **0.35** | **M** |
| `r_rtSpecF0Gamma` | Reflections | F0 remap gamma: reflectivity contrast | **2.5** | **M** |
| `r_rtReflOpaqueHalfRes` | Reflections | Trace opaque reflections at half res, upsampled before the denoisers. **Worth −30% `gpu.total`** at no visible cost, so it is a perf setting, not an A/B | 1 | **M** |
| `r_rtGuiReflections` | Reflections | Live GUI screens (bioscan panels, monitors) in reflections | 1 | **M** |
| `r_rtReflShadowPicks` | Reflections | Shadow rays per reflection hit: 0 = deterministic ≤8, 1-2 = stochastic. 1 is `gpu.Refl` −26% | 1 | C |
| `r_rtReflRoughnessScale` | Reflections | GGX lobe width multiplier on the packed Toksvig roughness | **0.6** | C |
| `r_rtReflRoughnessFloor` | Reflections | Minimum roughness — stops perfectly-smooth mirror behaviour on nominally flat panels | 0.25 | C |
| `r_rtReflRoughnessMax` | Reflections | Above this roughness an opaque pixel hands off to GI instead of tracing (debug mode 13 shows the boundary) | 0.5 | C |
| `r_rtReflRoughLumExp` | Reflections | Exponent tightening the luminance ceiling as roughness rises | — | C |
| `r_rtReflDenoise` | Reflections | 0 off, 1 temporal only, 2 temporal + roughness-gated à-trous | 2 | C |
| `r_rtReflDenoiseAlpha` | Reflections | Opaque temporal EMA alpha | 0.2 | C |
| `r_rtReflDenoiseAlphaGlass` | Reflections | Glass temporal EMA alpha (glass skips the à-trous entirely) | 0.35 | C |
| `r_rtReflAtrousIterations` | Reflections | À-trous passes | 3 | C → folded by 8g Stage 5 |
| `r_rtReflAtrousRoughnessGain` | Reflections | How much roughness widens the blur ("wash" gain) | 3.0 | C → folded by 8g Stage 5 |
| `r_rtReflAtrousFireflyClamp` | Reflections | Local firefly clamp before temporal and each à-trous pass | 2.5 | C |
| `r_rtReflAtrousSigmaPlane` | Reflections | Plane-distance edge stop (8g Stage 3; replaced `…SigmaZ`) | 0.02 | **X** |
| `r_rtReflAtrousNormalPower` | Reflections | Normal edge-stop exponent. Low (4) because bumps vary the shading normal | 4 | **X** |
| `r_rtReflectionMinWeight` | Reflections | Skip rays below this **coarse** (mip-4 F0) Fresnel weight — panel-scale, not texel-scale. A value tuned against per-pixel F0 reads far stricter here (8g 2b) | 0.04 | C |
| `r_rtReflVol` | Reflections | March the reflected segment through `froxelScatter` (arc 8c) | 1 | C |
| `r_rtReflVolGain` / `r_rtReflVolSteps` | Reflections | Reflected-beam gain and march steps | 1.0 / 6 | C |
| `r_rtReflGlow` / `…Gain` | Reflections | Emissive glow in reflections, and its gain | 1 / 1.0 | C |
| `r_rtReflGlowDist` | Reflections | Glow reach | 1024 | C |
| `r_rtReflGlowSmoke` / `…SmokeMinLum` | Reflections | Glow through smoke, min luminance | 1 / 0.3 | C |
| `r_rtReflGlowScale`, `r_rtReflGlowSmokeCell` | Reflections | Redundant with Gain / an internal grid size | 1.0 / 32 | **X** |
| `r_rtReflFlashGlow` / `…Gain` / `…Radius` / `…Offset` | Reflections | Flashlight lamp glow in reflections — the viewmodel can never reach a reflection ray, so the lamp is drawn analytically | 1 / 1.5 / 1.5 / 0.0 | C |
| `r_rtGlassNormalMode` | Reflections | Glass normal: 0 interpolated, 1 face, 2 crease-aware (arc 8b) | 2 | C |
| `r_rtGlassCreaseRatio` | Reflections | Mode 2 threshold. Tuned once and settled | 0.25 | **X** |
| `r_rtReflAmbientScale` | Reflections | Fraction of a shadowed light still added in reflections. Nonzero leaks through walls — a wrong-by-design knob | 0 | **X** |
| `r_rtGuiReflGain` | Reflections | GUI screen brightness in reflections | 0.3 | C |
| `r_rtGuiTexInterval` / `r_rtGuiTexRadius` | Reflections | GUI slot-texture refresh period (frames) and admission radius | 2 / 256 | C |
| `r_rtReflectionDebugMode` | Reflections | Reflection debug view — the one reflection overlay. 9 live modes, trimmed to 6 in cleanup Phase 1d | 0 | C |
<!-- r_rtSpecGrazingMax/Gain retired, roughness plan Stage 3 (2026-10-16): real Schlick-GGX
     replaced the grazing-angle hack they tuned. -->

`r_rtReflMaxLuminance` reads **6.0** in source, not the 8.0 that `20261017_reflection_pipeline_refactor.md`
Stage 2b says it left as the default — the eye-tuned Slow-machine value was committed. The
other three knobs from that tuning session (`r_rtSpecF0Scale 0.332`, `Gamma 2.46`,
`r_rtReflRoughLumExp 2.5`) were **not**, so a user `.cfg` carrying them will override the
menu (see the read-user-cfg rule). Reconcile the four before writing the presets.

### Global illumination (screen-space bounce)

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtGIStrength` | GI | Overall GI brightness | 0.20 | P | = |
| `r_rtGISamples` | GI | Bounce rays per pixel (1-8) | 4 | A (preset) | 1 / 2 / 4 / 6 |
| `r_rtGIRadius` | GI | Max bounce ray distance | 128 | A | = |
| `r_rtGIContrast` | GI | GI contrast | 0.6 | A | = |
| `r_rtGIDirectScale` | GI | Direct-light scale in the bounce | 1.0 | A | = |
| `r_rtGIBounceScale` | GI | Per-light irradiance multiplier in the bounce | 2.0 | A | = |
| `r_rtGIEmissiveScale` | GI | Emissive surface contribution | 2.0 | A | = |
| `r_rtGIFalloffMode` | GI | Point-light falloff shape (shared with reflections + vol) | 1 | A | = |
| `r_rtGIFalloffReach` | GI | Falloff reach (1.0 = stop at light box) | 1.0 | A | = |
| `r_rtGIAlbedo` | GI | Multiply denoised GI by receiver albedo | 1 | D | = |
| `r_rtGICheckerboard` | GI | Checkerboard tracing | 1 | A (preset) | off on Ultra |
| `r_rtGITemporal` | GI | Temporal accumulation | 1 | A | = |
| `r_rtGITemporalAlpha` | GI | EMA blend factor | 0.5 | A | = |
| `r_rtGIAtrous` | GI | A-trous spatial filter | 1 | A | = |
| `r_rtGIAtrousIterations` | GI | A-trous passes | 3 | A | = |
| `r_rtGIAtrousSigmaL` | GI | A-trous luminance sigma | 0.2 | D | = |
| `r_rtGIAtrousSigmaZ` | GI | A-trous depth sigma | 0.01 | D | = |

### GI light selection

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtGIMaxLights` | GI lights | Max lights collected | 128 | A | = |
| `r_rtGIMaxBounceLights` | GI lights | Max lights evaluated in the bounce | 16 | A (preset) | 8 / 16 / 16 / 24 |
| `r_rtGIStochasticLights` | GI lights | Lights sampled stochastically per pixel | 2 | A | = |
| `r_rtGILightStratify` | GI lights | Stratified distance-tier selection | 1 | D | = |
| `r_rtGILightCollectRadiusScale` | GI lights | Legacy sphere collector radius scale | 4.0 | D | = |
| `r_rtGIAreaLights` | GI lights | Collect lights by walking the portal-area graph | 1 | D | = |
| `r_rtGIAreaHops` | GI lights | Portal-hop depth | 2 | A | = |
| `r_rtGIAreaBoundsEpsilon` | GI lights | Camera box for BFS seeding | 24 | D | = |
| `r_rtGIMaxLightDist` | GI lights | Absolute light distance cap | 2048 | D | = |
| `r_rtGIWhiteWeight` | GI lights | Importance floor for desaturated lights | 0.25 | D | = |
| `r_rtGILightDump` | GI lights | Dump light selection | 0 | D | = |
| `r_rtGIFlickerThreshold` | GI lights | Flicker detection threshold | 0.15 | D | = |
| `r_rtGIFlickerHold` | GI lights | Flicker hold time | 2.0 | D | = |
| `r_rtGIProbeFastBuckets` | GI lights | Separate cache bucket for flickering lights | 1 | D | = |

### GI probes

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtGIProbes` | GI probes | Probe grid on/off | 1 | A | = |
| `r_rtGIProbeSpacing` | GI probes | Probe spacing (world units) | 64 | A | = |
| `r_rtGIProbeCountX/Y/Z` | GI probes | Grid dimensions | 32 / 32 / 16 | A | = |
| `r_rtGIProbeRays` | GI probes | Rays per probe update | 128 | A (preset) | 64 / 128 / 128 / 192 |
| `r_rtGIProbeUpdatesPerFrame` | GI probes | Probes updated per frame | 1024 | A (preset) | 512 / 1024 / 1024 / 2048 |
| `r_rtGIProbeHysteresis` | GI probes | Temporal blend (higher = slower, smoother) | 0.97 | A | = |
| `r_rtGIProbeNormalBias` | GI probes | Sample normal bias | 8.0 | D | = |
| `r_rtGIProbeViewBias` | GI probes | Sample view bias | 8.0 | D | = |
| `r_rtGIProbeInsideThreshold` | GI probes | Inside-geometry threshold | 0.25 | D | = |
| `r_rtGIProbeOutsideThreshold` | GI probes | Outside threshold | 0.9 | D | = |
| `r_rtGIProbeVisibility` | GI probes | Visibility (Chebyshev) weighting | 1 | D | = |
| `r_rtGIProbeMaxRayDist` | GI probes | Max probe ray distance | 512 | D | = |
| `r_rtGIProbeDistSharpness` | GI probes | Distance-weight sharpness | 50 | D | = |
| `r_rtGIProbeDebug` / `DebugGain` / `DebugRadius` / `Dump` | GI probes | Probe visualisation and dump | 0 / 4 / 4 / 0 | D | = |

### Volumetric lighting

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtVolExtinction` | Volumetrics | Fog density | 0.0006 | P | = |
| `r_rtVolSamples` | Volumetrics | March samples | 8 | A (preset) | 4 / 8 / 8 / 12 |
| `r_rtVolMaxDist` | Volumetrics | Max march distance | 512 | A | = |
| `r_rtVolMaxLights` | Volumetrics | Max lights | 96 | A | = |
| `r_rtVolHalfRes` | Volumetrics | Half-resolution trace | 1 | A (preset) | off on Ultra |
| `r_rtVolUpsampleDepthSigma` | Volumetrics | Bilateral upsample depth falloff | 24 | D | = |
| `r_rtVolWhiteNoiseMix` | Volumetrics | Noise mix for sampling | 0.025 | D | = |
| `r_rtVolAlbedo` | Volumetrics | Medium albedo | 1.0 | A | = |
| `r_rtVolGain` | Volumetrics | Point-light scatter gain | 25 | A | = |
| `r_rtVolAnisotropy` | Volumetrics | Point-light phase anisotropy | 0.4 | A | = |
| `r_rtVolIsotropicMix` | Volumetrics | Isotropic phase mix | 0.45 | A | = |
| `r_rtVolDirectedGain` | Volumetrics | Spot/projected light gain | 70 | A | = |
| `r_rtVolDirectedAnisotropy` | Volumetrics | Spot/projected anisotropy | 0.6 | A | = |
| `r_rtVolFlashlightGain` | Volumetrics | Flashlight beam gain | 50 | A | = |
| `r_rtVolFlashlightAnisotropy` | Volumetrics | Flashlight anisotropy | 0.7 | A | = |
| `r_rtVolAttenuateBackground` | Volumetrics | Scene × transmittance, not add-only | 1 | A | = |
| `r_rtVolTemporal` / `r_rtVolTemporalAlpha` | Volumetrics | Temporal accumulation and blend | 1 / 0.5 | A | = |
| `r_rtVolBilateral` / `r_rtVolBilateralSigma` | Volumetrics | Bilateral filter and sigma | 1 / 2.0 | D | = |
| `r_rtVolFroxel` | Volumetrics | Froxel-grid path | 1 | A | = |
| `r_rtVolFroxelResX/Y/Z` | Volumetrics | Froxel grid resolution | 240 / 135 / 96 | A (preset) | scale with preset |
| `r_rtVolFroxelFarTransmittance` | Volumetrics | Transmittance floor that sets the froxel far plane | 0.002 | D | = |
| `r_rtVolProjEmitterBias` | Volumetrics | Projected-light emitter offset | 24 | D | = |
| `r_rtVolProjFalloff` / `r_rtVolProjFalloffDist` | Volumetrics | Projected-light falloff mode and distance | 2 / 512 | D | = |
| `r_rtVolDebugMode` / `DebugGain` / `Dump` | Volumetrics | Debug views and dump | 0 / 20 / 0 | D | = |
| `r_rtVolFroxelDebug` / `DebugSlice` / `DebugGain` / `Dump` | Volumetrics | Froxel debug | 0 / 32 / 20 / 0 | D | = |

### Temporal / denoise (AO + shared)

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtAOTemporal` | Temporal | AO temporal accumulation | 1 | A | = |
| `r_rtAOTemporalAlpha` | Temporal | AO EMA blend | 0.5 | A | = |
| `r_rtAtrousIterations` | Temporal | AO A-trous passes | 4 | A | = |
| `r_rtAtrousSigmaDepth` / `r_rtAtrousSigmaLuminance` | Temporal | AO A-trous edge-stops | 0.01 / 0.1 | D | = |
| `r_rtTemporalCutPosThreshold` | Temporal | Camera move per frame that resets history | 6 | D | = |
| `r_rtTemporalCutAngleThreshold` | Temporal | Camera rotation per frame that resets history | 4 | D | = |

### Tonemap and bloom

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtTonemap` | Tonemap | Tonemap on/off | 1 | P | = |
| `r_rtTonemapExposure` | Tonemap | Exposure | 1.0 | P | = |
| `r_rtTonemapToe` | Tonemap | Curve toe | 2.7 | A | = |
| `r_rtTonemapLinStart` | Tonemap | Linear section start | 0.3 | A | = |
| `r_rtTonemapLinLen` | Tonemap | Linear section length | 0.40 | A | = |
| `r_rtBloom` | Bloom | Bloom on/off | 0 | P | = (revisit once tuned) |
| `r_rtBloomStrength` | Bloom | Bloom strength | 0.15 | P | = |
| `r_rtBloomThreshold` | Bloom | Brightness threshold | 0.8 | A | = |
| `r_rtBloomEmissiveThreshold` | Bloom | Emissive threshold | 0.5 | A | = |
| `r_rtBloomKnee` | Bloom | Threshold softness | 0.5 | D | = |
| `r_rtBloomLitWeight` | Bloom | Lit-surface contribution | 0 | D | = |
| `r_rtBloomMips` | Bloom | Blur mip count | 4 | D | = |
| `r_rtBloomDebug` | Bloom | Debug view | 0 | D | = |

### FSR upscaling

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_fsr` | FSR | Upscale mode | 0 | P | = |
| `r_fsrQuality` | FSR | Quality preset (drives render scale) | 0 | P | = |
| `r_fsrSharpness` | FSR | Sharpening | 0.5 | P | = |
| `r_fsrRenderScale` | FSR | Explicit render scale | 1.0 | A | = |
| `r_fsrJitter` | FSR | Camera jitter | 1 | A | = |
| `r_fsrMotionScale` | FSR | Motion-vector scale | 16 | D | = |
| `r_fsrAutoReactive` | FSR | Auto reactive mask | 1 | A | = |
| `r_fsrReactiveCutoff` / `r_fsrReactiveScale` | FSR | Reactive mask tuning | 0.05 / 1.0 | D | = |
| `r_fsrTcMask` | FSR | Transparency/composition mask | 1 | A | = |
| `r_fsrMipBias` / `r_fsrMipBiasOffset` | FSR | Texture mip bias for the lower render res | 1 / -1.0 | A | = |
| `r_fsrDebug` | FSR | Debug mode | 0 | D | = |
| `r_vkBumpMipBias` | Textures | Extra positive mip bias on bump samplers | 0.5 | A | = |

### Auto-relight (experimental)

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtAutoRelight` | Auto-relight | Synthesize lights for dark areas | 0 | P | = (experimental) |
| `r_rtAutoRelightMax` | Auto-relight | Global synthesized-light cap | 64 | A | = |
| `r_rtAutoRelightMaxPerArea` | Auto-relight | Per-area cap | 4 | A | = |
| `r_rtAutoRelightIntensity` | Auto-relight | Intensity | 0.5 | A | = |
| `r_rtAutoRelightReach` | Auto-relight | Placement reach | 160 | D | = |
| `r_rtAutoRelightOffset` | Auto-relight | Placement offset from surface | 8 | D | = |
| `r_rtAutoRelightDedupeDist` | Auto-relight | Suppress near existing map lights | 96 | D | = |
| `r_rtAutoRelightDebug` | Auto-relight | Debug logging level | 1 | D | 0 |
| `r_rtAutoRelightDebugColor` | Auto-relight | Colour synthesized lights | 0 | D | = |

### Player exclusion (shadow/GI rays vs. the player body)

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_rtShadowPlayerExcludeDist` | Player model | Exclude player body from shadow rays when closer to a light than this | 30 | A | = |
| `r_rtPlayerExcludeRadius` | Player model | Cull player body from shadow/GI rays near the eye | 40 | A | = |

### Vulkan backend (debug / stability)

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `r_vkWeaponDepthAlways` | Vulkan | Force weapon depth-hack to ALWAYS | 0 | D | = |
| `r_vkPolyOffsetSlopeMin` | Vulkan | Min slope depth bias for polygonOffset | 0.0 | D | = |
| `r_vkShadowStableMode` | Vulkan | Force Z-fail shadow volumes | 0 | D | = |
| `r_vkInsideLightHysteresisEnter` / `Exit` | Vulkan | Inside-light hysteresis margins | 0.5 / 1.5 | D | = |
| `r_vkShadowFlipOps`, `r_vkShadowFullScissor`, `r_vkLightFullScissor` | Vulkan | Stencil shadow A/B switches | 0 | D | = |
| `r_vkSerializeFrames` | Vulkan | Wait for GPU between frames | 0 | D | = |
| `r_vkLowPerturbationMode`, `r_vkSplitSubmitMask`, `r_vkSplitSubmitVerbose` | Vulkan | DEVICE_LOST bisection helpers | 1 / 0 / 0 | D | = |
| `r_vkRTProfile` | Vulkan | 0=off, 1=window summaries, 2=one row per frame | 0 | D | = |
| `r_vkRTProfileWindow` | Vulkan | Frames per summary window (1-256) | 30 | D | = |
| `r_vkRTProfileStats` | Vulkan | Summary columns: 1=mean 2=p5 4=p50 8=p95 16=max | 29 | D | = |
| `r_vkRTProfileGroups` | Vulkan | Console filter only — all groups are always collected and always in the CSV. Leave at 63 | 63 | D | = |
| `r_vkRTProfileSpikeMs` | Vulkan | Dump every metric for frames over this backend ms (0=off); ignored while a CSV is open | 0 | D | = |
| `r_vkRTProfileCSV` | Vulkan | Per-frame CSV path (empty=off). Suppresses the console block in favour of one heartbeat line per window | "" | D | = |
| `r_vkDebugLabels` | Vulkan | VK_EXT_debug_utils pass labels for Nsight/RGP/RenderDoc, including a region per light inside Interactions | 0 | D | = |
| `r_vkVcCheck`, `r_vkLogBufferGarbage` | Vulkan | Vertex-cache validation / accounting | 0 | D | = |
| `r_vkLogRT`, `r_vkLogShadowBranch`, `r_vkLogShadowGeom`, `r_vkLogSubmitInfo`, `r_vkLogTranslucent` | Vulkan | Logging | 0 | D | = |
| `r_vkRTDebugInteractions`, `r_vkRTDebugSurface`, `r_vkRTDebugMaxLogsPerFrame`, `r_vkRTInterLogN`, `r_vkRTLogCaptureReset`, `r_vkRTDebugLightTextures`, `r_vkRTDebugLights`, `r_vkRTLightLogN`, `r_vkRTDebugLightFilter` | Vulkan | RT interaction/light logging | 0 / "" / 24 / 0 … | D | = |

### Player model: first person, third person

| Name | Group | Purpose | Default | Tier | Rec. |
|---|---|---|---|---|---|
| `pm_showFirstPersonBody` | FP/TP model | Show own torso and legs in first person | 0 | P | 0 until the artifact/perf criteria in `docs/plans/20261001_see_first_person_player_model.md` pass, then 1 |
| `pm_firstPersonBodyOffset` | FP/TP model | Body shift along its forward axis (negative = back) | -4 | A | = |
| `pm_firstPersonBodyMaxPitch` | FP/TP model | Max downward pitch while the body is shown | 70 | A | = |
| `pm_firstPersonBodyDebug` | FP/TP model | Log body create/free and pose | 0 | D | = |
| `pm_ladderLegAnim` | FP/TP model | Step/hang leg anim on ladders | 1 | A | = |
| `pm_thirdPerson` | FP/TP model | Third-person camera | 0 | P | = |
| `pm_thirdPersonRange` | FP/TP model | Camera distance | 80 | P | = |
| `pm_thirdPersonHeight` | FP/TP model | Camera height offset | 0 | A | = |
| `pm_thirdPersonAngle` | FP/TP model | Camera angle (0 behind, 180 front) | 0 | A | = |
| `pm_thirdPersonClip` | FP/TP model | Clip camera against world | 1 | A | = |
| `pm_thirdPersonDeath` | FP/TP model | Third person on death | 0 | A | = |
| `pm_modelView` | FP/TP model | Camera from the player model's POV | 0 | D | = |
| `g_showPlayerShadow` | FP/TP model | Player model casts shadow / appears in reflections | 1 | P | = |

### Debug mode census (2026-10-18)

44 live modes across 8 cvars, measured by grepping the shader `debugMode ==` branches rather
than the help strings — the two had drifted (the shadow help documents no mode 12, and
`r_rtShadowBackfaceCull`, which that mode visualized, no longer exists). Cleanup Phase 1d
trims to 18. Retired numbers become documented no-ops so doc and memory references keep
resolving.

| Cvar | Live modes | Count | Keep after 1d |
|---|---|---|---|
| `r_rtReflectionDebugMode` | 2,3,4,6,11,12,13,14,15 (1,5,7,8,9,10 already no-ops) | 9 | 2,3,11,12,13,15 |
| `r_rtShadowDebugMode` | 1,2,3,5,7,8,9,10,11 | 9 | 9, 10 |
| `r_rtGIProbeDebug` | 1-8 | 8 | 1, 3, 4 |
| `r_fsrDebug` | 1 FSR1-ran border, 2 view logging, 3 mask overlay, 4 point-magnify A/B, 5 FSR2 bleed, 6 EASU-only, 7 motion vectors | 7 | 3, 7 |
| `r_rtVolFroxelDebug` | 1 slice, 2 light-count heatmap, 3 grid-mapping error, 4 blocked cone hits | 4 | 1, 2, 3 |
| `r_rtAODebug` | 1 raw mask, 2 GI×AO, 3 GI without AO | 3 | 1, 2, 3 |
| `r_rtVolDebugMode` | 1 transmittance, 2 raw scatter | 2 | none (goes with the march path, Phase 4) |
| `r_rtBloomDebug` | 1 HDR range bands, 2 raw extract | 2 | 1 |

## 3. Recommended quality preset

One new `r_rtQuality` cvar (menu combo) writes these; the individual cvars stay available in the console and Advanced node.
The numbers below are starting points, not measured.

| Cvar | Low | Medium | High (= current) | Ultra |
|---|---|---|---|---|
| `r_rtShadowSamples` | 1 | 2 | 2 | 4 |
| `r_rtAOSamples` | 2 | 4 | 4 | 8 |
| `r_rtGISamples` | 1 | 2 | 4 | 6 |
| `r_rtGIProbeRays` | 64 | 128 | 128 | 192 |
| `r_rtGIProbeUpdatesPerFrame` | 512 | 1024 | 1024 | 2048 |
| `r_rtGIMaxBounceLights` | 8 | 16 | 16 | 24 |
| `r_rtVolSamples` | 4 | 8 | 8 | 12 |
| `r_rtVolHalfRes` | 1 | 1 | 1 | 0 |
| `r_rtVolFroxelRes{X,Y,Z}` | 120/68/48 | 160/90/64 | 240/135/96 | 320/180/128 |
| `r_rtGICheckerboard` | 1 | 1 | 1 | 0 |

## 4. Menu cleanup checklist

| Change | File | Check |
|---|---|---|
| Drop every **C** and **X** widget from the menu | `Dhewm3SettingsMenu.cpp` ~2749-3060 | `rtCVars.*` bindings 116 → ~30, all **M** rows from §1 |
| Add the new Reflections group (6 widgets, §1) | same | Sliders move the image live; F0 scale/gamma visibly change how reflective the world reads |
| Add `r_rtQuality` combo + apply function | menu + `RenderSystem_init.cpp` | Selecting a preset changes the §3 cvars in `cvarlist` |
| Add `CVAR_ARCHIVE` to the **M** cvars that lack it (see §1) | Vulkan `.cpp` files | Value survives a restart |
| Reconcile the four eye-tuned reflection defaults | `vk_reflections.cpp` | A fresh config and a tuned `.cfg` produce the same image |
| ~~Remove or define `r_rtDenoise`~~ | — | **Done.** Menu binds `r_rtReflDenoise`; no `r_rtDenoise` exists |
| Add `pm_showFirstPersonBody`, `pm_thirdPerson`, `pm_thirdPersonRange` | menu "Player model" group | Toggling in menu changes the in-game camera/body |
