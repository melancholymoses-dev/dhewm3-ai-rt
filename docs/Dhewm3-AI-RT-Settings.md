# Dhewm3-AI-RT Settings Inventory

Cvars added for the Vulkan / ray-tracing renderer and the first-/third-person player model, with the trim target for the settings menu (`neo/framework/Dhewm3SettingsMenu.cpp`).

Re-audited 2026-10-18 against `main` after the reflection arcs (8d roughness, 8f GUI
reflections, 8g pipeline refactor Stages 1-4). Measured counts: **255 `r_rt*`/`r_vk*`/`r_fsr*`
cvars, 116 menu widgets, 44 debug modes.** Execution plan: `plans/20261010_cleanup_polish.md`
Phase 1.

Verdict key (replaces the old P/A/D tiers — the old letters map M/C/C-or-X)

| Verdict | Meaning | Where it lives |
|---|---|---|
| M | Menu. Player understands it and it changes the look or cost visibly | Main RT page or the preset |
| C | Console only, kept and documented. Real knob, rarely touched | this doc §2 |
| X | Delete — with its UBO field, shader branch, menu widget and CMake entry | nowhere |

Target: ~30 M, ~120 C, ~105 X. That is 116 menu widgets down to ~30.

## 1. Player menu (the ~30 M cvars)

| Menu section | Widget | Cvar |
|---|---|---|
| Ray Tracing | Enable RT | `r_useRayTracing` |
| Features | Shadows / AO / Reflections / GI / Volumetric light | `r_rtShadows`, `r_rtAO`, `r_rtReflections`, `r_rtGI`, `r_rtVol` |
| Features | Player shadow + reflection | `g_showPlayerShadow` |
| Quality | New preset Low / Medium / High / Ultra (see §3) | writes the C sample/res cvars |
| Look | GI strength, Fog density | `r_rtGIStrength`, `r_rtVolExtinction` |
| Look | Tonemap on, Exposure | `r_rtTonemap`, `r_rtTonemapExposure` |
| Look | Bloom on, Bloom strength | `r_rtBloom`, `r_rtBloomStrength` |
| Reflections | Reflection strength, Highlight ceiling | `r_rtReflectionBlend`, `r_rtReflMaxLuminance` |
| Reflections | Reflectivity scale, Reflectivity contrast | `r_rtSpecF0Scale`, `r_rtSpecF0Gamma` |
| Reflections | Half-res opaque trace, Live GUI screens | `r_rtReflOpaqueHalfRes`, `r_rtGuiReflections` |
| Upscaling | FSR mode, Quality, Sharpness | `r_fsr`, `r_fsrQuality`, `r_fsrSharpness` |
| Player model | Show own body, Third person, Third-person distance | `pm_showFirstPersonBody`, `pm_thirdPerson`, `pm_thirdPersonRange` |
| Experimental | Auto-relight | `r_rtAutoRelight` |

The Reflections group is new in this revision. F0 is the 8d/8g win — `r_rtSpecF0Scale/Gamma`
are the two knobs that set how reflective the world reads, and both are already tuned
(0.35 / 2.5, up from the audit's 0.2 / 3). `r_rtReflOpaqueHalfRes` is in the menu because it
is worth 30% of GPU frame time at no visible cost, so it is a real perf choice, not a debug
A/B.

`CVAR_ARCHIVE` still missing on `r_rtGIStrength`, `r_rtVolGain`, `r_rtGIRadius`,
`r_rtAORadius`, `r_rtVolMaxDist`, `r_rtAOTemporalAlpha` — Phase 5a fixes
this. `r_rtDenoise` (the dead "Temporal Denoising" checkbox) is resolved: the menu now
binds `r_rtReflDenoise`, and no `r_rtDenoise` exists in the renderer. §4's row is closed.

## 2. Full table

### Core RT and Vulkan feature switches

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_useRayTracing` | Core | Master RT switch | 1 | M |
| `r_rtShadows` | Core | RT shadows on/off | 1 | M |
| `r_rtAO` | Core | RT ambient occlusion on/off | 1 | M |
| `r_rtReflections` | Core | RT reflections on/off | 1 | M |
| `r_rtGI` | Core | RT global illumination on/off | 1 | M |
| `r_rtVol` | Core | RT volumetric lighting on/off | 1 | M |
| `r_rtDeformedTangents` | Core | Use deformed (skinned) tangents in RT shading | 1 | C |
| `r_rtGbufNormals` | Core | AO/GI take normals from the G-buffer | 1 | C |
| `r_rtNearDynRadius` | TLAS | Off-screen dynamic entities within this radius are forced into the TLAS | 512 | C |
| `r_rtTlasFallbackRadius` | TLAS | Pass-2 radius for off-screen static geometry (0 = auto from GI/vol/reflection ranges) | 0 | C |
| `r_vkLogTLASSlow` | TLAS | Section breakdown of TLAS CPU time on frames over N ms (pass1/2/3, materials, bindless, build) | 0 | C |
| `r_vkBLASPoolCheck` | TLAS | Validate BLAS storage/scratch/geometry pool ranges on build and refit | 0 | C |

Deleted 2026-10-18: `r_rtTlasDump`, `r_vkRTReflDataDiag`.

### Shadows

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtShadowSamples` | Shadows | Shadow rays per pixel. Preset writes 1 / 2 / 2 / 4 | 2 | C (preset) |
| `r_rtShadowBlur` | Shadows | Shadow blur radius/passes | 6 | C |
| `r_rtShadowBlurEnable` | Shadows | Blur on/off. Turn off to read debug modes 10/11 unsoftened | 1 | C |
| `r_rtShadowBlurDepthAware` | Shadows | Depth-aware blur | 1 | C |
| `r_rtShadowBlurDepthThreshold` | Shadows | Depth edge threshold for the blur | 0.003 | X |
| `r_rtShadowBlurMinRect` | Shadows | Skip blur for lights with small dispatch rects | 64 | C |
| `r_rtShadowRayBias` | Shadows | Ray origin bias against self-shadowing | 0.25 | C |
| `r_rtShadowBiasErrMargin` | Shadows | Safety multiple on the depth-reconstruction bias floor. Debug mode 9 shows the headroom | 6.0 | C |
| `r_rtShadowSoftRadiusScale` | Shadows | Light radius → soft-shadow source radius | 0.08 | C |
| `r_rtShadowSoftRadiusMin` / `…Max` | Shadows | Soft-shadow source radius clamp | 0.0 / 6.0 | C |
| `r_rtShadowMaskProjected` | Shadows | Apply RT shadow mask to projected lights | 1 | X |
| `r_rtFlashlightBias` | Shadows | Ray bias for the flashlight | 17 | C |
| `r_rtShadowDebugMode` | Shadows | The one shadow overlay: 9 bias budget, 10 player exclusion, 11 light-volume cull | 0 | C |
| `r_rtUnlockNoShadows` | Shadows | Give ACCENT noShadows lights a real RT shadow | 0 | C |
| `r_rtLightAccentMaxRadius` | Shadows | noShadows light radius below this is "accent" | 300 | C |

Deleted 2026-10-18: `r_rtShadowMinSamples`, `r_rtShadowVolumeCull` (cull now unconditional),
`r_rtShadowTemporalJitter(+MinSamples)`, `r_rtShadowStablePattern`. `r_rtShadowBackfaceCull`
was already gone before the audit — the doc had carried a phantom row for it.

### Ambient occlusion

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtAOSamples` | AO | AO rays per pixel. Preset writes 2 / 4 / 4 / 8 | 4 | C (preset) |
| `r_rtAORadius` | AO | Max AO ray length (keep below probe spacing) | 24 | C |
| `r_rtAOFalloff` | AO | Smooth distance falloff of occlusion | 1 | C |
| `r_rtAOIndirectStrength` | AO | How strongly AO darkens GI | 1.0 | C |
| `r_rtAODirectStrength` | AO | How strongly AO darkens direct light with GI on | 0.0 | C |
| `r_rtAOFadeStart` / `…FadeEnd` | AO | Where AO starts and finishes fading. Keep below `5000*(r_znear/3)` or the black-speckle artifact returns | 2000 / 3000 | C |
| `r_rtAODebug` | AO | The one AO overlay: 1 raw mask, 2 GI×AO, 3 GI without AO | 0 | C |

Deleted 2026-10-18: `r_rtAODebugGain` (hardcoded).

### Reflections and specular

Rewritten 2026-10-18. The 8d roughness, 8f GUI and 8g refactor arcs added 24 cvars this
table had never listed, and moved six defaults. Defaults below are read from source.

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtReflectionMode` | Reflections | 1 = glass only, 3 = opaque + glass (default). Mode 2 is legacy, deleted in cleanup Phase 2 | 3 | C |
| `r_rtReflectionDistance` | Reflections | Max reflection ray distance | 1000 | C |
| `r_rtReflectionBlend` | Reflections | Reflection strength. The one brightness knob | 6.0 | M |
| `r_rtReflMaxLuminance` | Reflections | Flat ceiling on incident reflected light — the single-sample GGX lobe's outlier guard. Rescaled in 8g 2b when the ceiling moved past the Fresnel multiply | 6.0 | M |
| `r_rtSpecF0Scale` | Reflections | F0 remap scale: how reflective the world reads | 0.35 | M |
| `r_rtSpecF0Gamma` | Reflections | F0 remap gamma: reflectivity contrast | 2.5 | M |
| `r_rtReflOpaqueHalfRes` | Reflections | Trace opaque reflections at half res, upsampled before the denoisers. worth −30% `gpu.total` at no visible cost, so it is a perf setting, not an A/B | 1 | M |
| `r_rtGuiReflections` | Reflections | Live GUI screens (bioscan panels, monitors) in reflections | 1 | M |
| `r_rtReflShadowPicks` | Reflections | Shadow rays per reflection hit: 0 = deterministic ≤8, 1-2 = stochastic. 1 is `gpu.Refl` −26% | 1 | C |
| `r_rtReflRoughnessScale` | Reflections | GGX lobe width multiplier on the packed Toksvig roughness | 0.6 | C |
| `r_rtReflRoughnessFloor` | Reflections | Minimum roughness — stops perfectly-smooth mirror behaviour on nominally flat panels | 0.25 | C |
| `r_rtReflRoughnessMax` | Reflections | Above this roughness an opaque pixel hands off to GI instead of tracing (debug mode 13 shows the boundary) | 0.5 | C |
| `r_rtReflRoughLumExp` | Reflections | Exponent tightening the luminance ceiling as roughness rises | — | C |
| `r_rtReflDenoise` | Reflections | 0 off, 1 temporal only, 2 temporal + roughness-gated à-trous | 2 | C |
| `r_rtReflDenoiseAlpha` | Reflections | Opaque temporal EMA alpha | 0.2 | C |
| `r_rtReflDenoiseAlphaGlass` | Reflections | Glass temporal EMA alpha (glass skips the à-trous entirely) | 0.35 | C |
| `r_rtReflAtrousIterations` | Reflections | À-trous passes | 3 | C → folded by 8g Stage 5 |
| `r_rtReflAtrousRoughnessGain` | Reflections | How much roughness widens the blur ("wash" gain) | 3.0 | C → folded by 8g Stage 5 |
| `r_rtReflAtrousFireflyClamp` | Reflections | Local firefly clamp before temporal and each à-trous pass | 2.5 | C |
| `r_rtReflAtrousSigmaPlane` | Reflections | Plane-distance edge stop (8g Stage 3; replaced `…SigmaZ`) | 0.02 | X |
| `r_rtReflAtrousNormalPower` | Reflections | Normal edge-stop exponent. Low (4) because bumps vary the shading normal | 4 | X |
| `r_rtReflectionMinWeight` | Reflections | Skip rays below this coarse (mip-4 F0) Fresnel weight — panel-scale, not texel-scale. A value tuned against per-pixel F0 reads far stricter here (8g 2b) | 0.04 | C |
| `r_rtReflVol` | Reflections | March the reflected segment through `froxelScatter` (arc 8c) | 1 | C |
| `r_rtReflVolGain` / `r_rtReflVolSteps` | Reflections | Reflected-beam gain and march steps | 1.0 / 6 | C |
| `r_rtReflGlow` / `…Gain` | Reflections | Emissive glow in reflections, and its gain | 1 / 1.0 | C |
| `r_rtReflGlowDist` | Reflections | Glow reach | 1024 | C |
| `r_rtReflGlowSmoke` / `…SmokeMinLum` | Reflections | Glow through smoke, min luminance | 1 / 0.3 | C |
| `r_rtReflFlashGlow` / `…Gain` / `…Radius` / `…Offset` | Reflections | Flashlight lamp glow in reflections — the viewmodel can never reach a reflection ray, so the lamp is drawn analytically | 1 / 1.5 / 1.5 / 0.0 | C |
| `r_rtGlassNormalMode` | Reflections | Glass normal: 0 interpolated, 1 face, 2 crease-aware (arc 8b) | 2 | C |
| `r_rtGlassCreaseRatio` | Reflections | Mode 2 threshold. Tuned once and settled | 0.25 | X |
| `r_rtGuiReflGain` | Reflections | GUI screen brightness in reflections | 0.3 | C |
| `r_rtGuiTexInterval` / `r_rtGuiTexRadius` | Reflections | GUI slot-texture refresh period (frames) and admission radius | 2 / 256 | C |
| `r_rtReflectionDebugMode` | Reflections | The one reflection overlay: 2 G-buffer normal, 3 F0, 11 denoiser history, 12 roughness, 13 GI handoff, 15 à-trous edge stops | 0 | C |

Deleted 2026-10-18: `r_rtReflAmbientScale` (nonzero leaked light through walls — a
wrong-by-design knob), `r_rtReflGlowScale`, `r_rtReflGlowSmokeCell`. Retired earlier by the
roughness arc's Stage 3: `r_rtSpecGrazingMax`/`Gain`, whose grazing-angle hack real
Schlick-GGX replaced.

`r_rtReflMaxLuminance` reads 6.0 in source, not the 8.0 that `20261017_reflection_pipeline_refactor.md`
Stage 2b says it left as the default — the eye-tuned Slow-machine value was committed. The
other three knobs from that tuning session (`r_rtSpecF0Scale 0.332`, `Gamma 2.46`,
`r_rtReflRoughLumExp 2.5`) were not, so a user `.cfg` carrying them will override the
menu (see the read-user-cfg rule). Reconcile the four before writing the presets.

### Global illumination (screen-space bounce)

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtGIStrength` | GI | Overall GI brightness | 0.20 | M |
| `r_rtGIContrast` | GI | GI contrast | 0.6 | C |
| `r_rtGIDirectScale` | GI | Direct-light scale in the bounce | 1.0 | C |
| `r_rtGIBounceScale` | GI | Per-light irradiance multiplier in the bounce | 2.0 | C |
| `r_rtGIEmissiveScale` | GI | Emissive surface contribution | 2.0 | C |
| `r_rtGIFalloffMode` | GI | Point-light falloff shape (shared with reflections + vol) | 1 | C |
| `r_rtGIFalloffReach` | GI | Falloff reach (1.0 = stop at light box) | 1.0 | C |
| `r_rtGIAlbedo` | GI | Multiply resolved GI by receiver albedo | 1 | C |
| `r_rtGIRadius` | GI | Light-collection window and TLAS radius | 128 | C |

Phase 3 removed the screen-space GI path: `r_rtGISamples`, `r_rtGICheckerboard`,
`r_rtGITemporal(+Alpha)` and `r_rtGIAtrous*` are gone. `r_rtGIRadius` stayed — it sizes
light collection (`vk_gi.cpp`) and the TLAS radius (`vk_accelstruct.cpp`), not just the
deleted per-pixel ray. `r_rtGIStochasticLights` stayed for the same reason: `gi_ray.rchit`
reads it and probe rays go through that shader.

### GI light selection

Shared by probes, reflections and volumetrics, so none of this dies with Phase 3.

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtGIMaxLights` | GI lights | Max lights collected | 128 | C |
| `r_rtGIMaxBounceLights` | GI lights | Max lights evaluated in the bounce. Preset writes 8 / 16 / 16 / 24 | 16 | C (preset) |
| `r_rtGIStochasticLights` | GI lights | Lights sampled stochastically per pixel | 2 | C |
| `r_rtGILightStratify` | GI lights | Stratified distance-tier selection | 1 | C |
| `r_rtGIAreaLights` | GI lights | Collect lights by walking the portal-area graph | 1 | C |
| `r_rtGIAreaHops` | GI lights | Portal-hop depth | 2 | C |
| `r_rtGIAreaBoundsEpsilon` | GI lights | Camera box for BFS seeding | 24 | X |
| `r_rtGIMaxLightDist` | GI lights | Absolute light distance cap | 2048 | C |
| `r_rtGIWhiteWeight` | GI lights | Importance floor for desaturated lights | 0.25 | X |
| `r_rtGIFlickerThreshold` / `…FlickerHold` | GI lights | Flicker detection threshold and hold time | 0.15 / 2.0 | C |
| `r_rtGIProbeFastBuckets` | GI lights | Separate cache bucket for flickering lights | 1 | C |
| `r_rtGILightCollectRadiusScale` | GI lights | Legacy sphere collector radius scale — superseded by the area walk | 4.0 | X |

Deleted 2026-10-18: `r_rtGILightDump`.

### GI probes

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtGIProbeSpacing` | GI probes | Probe spacing (world units). Keep `r_rtAORadius` below it | 64 | C |
| `r_rtGIProbeCountX/Y/Z` | GI probes | Grid dimensions | 32 / 32 / 16 | C |
| `r_rtGIProbeRays` | GI probes | Rays per probe update. Preset writes 64 / 128 / 128 / 192 | 128 | C (preset) |
| `r_rtGIProbeUpdatesPerFrame` | GI probes | Probes updated per frame. Preset writes 512 / 1024 / 1024 / 2048 | 1024 | C (preset) |
| `r_rtGIProbeHysteresis` | GI probes | Temporal blend (higher = slower, smoother) | 0.97 | C |
| `r_rtGIProbeNormalBias` / `…ViewBias` | GI probes | Sample biases | 8.0 / 8.0 | C |
| `r_rtGIProbeInsideThreshold` / `…OutsideThreshold` | GI probes | Buried / void classification thresholds | 0.25 / 0.9 | C |
| `r_rtGIProbeVisibility` | GI probes | Visibility (Chebyshev) weighting | 1 | C |
| `r_rtGIProbeMaxRayDist` | GI probes | Max probe ray distance | 512 | C |
| `r_rtGIProbeDistSharpness` | GI probes | Distance-weight sharpness for the visibility moments | 50 | C |
| `r_rtGIProbeDebug` | GI probes | The one probe overlay: 2 probe weights, 3 leak detector | 0 | C |

Deleted 2026-10-18: `r_rtGIProbeDebugGain`, `r_rtGIProbeDebugRadius`, `r_rtGIProbeDump`.
Removing the radius took the sphere-lattice overlay with it, which is why probe debug modes
1/4/5/6/7 are retired rather than merely unused.

### Volumetric lighting

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtVolExtinction` | Volumetrics | Fog density | 0.0006 | M |
| `r_rtVolMaxDist` | Volumetrics | Max march distance | 512 | C |
| `r_rtVolMaxLights` | Volumetrics | Max lights | 96 | C |
| `r_rtVolAlbedo` | Volumetrics | Medium albedo | 1.0 | C |
| `r_rtVolGain` / `r_rtVolAnisotropy` | Volumetrics | Point-light scatter gain and phase anisotropy | 25 / 0.4 | C |
| `r_rtVolIsotropicMix` | Volumetrics | Isotropic phase mix | 0.45 | C |
| `r_rtVolDirectedGain` / `…DirectedAnisotropy` | Volumetrics | Spot/projected light gain and anisotropy | 70 / 0.6 | C |
| `r_rtVolFlashlightGain` / `…FlashlightAnisotropy` | Volumetrics | Flashlight beam gain and anisotropy | 50 / 0.7 | C |
| `r_rtVolAttenuateBackground` | Volumetrics | Scene × transmittance, not add-only | 1 | C |
| `r_rtVolTemporal` / `…TemporalAlpha` | Volumetrics | Temporal accumulation and blend | 1 / 0.5 | C |
| `r_rtVolFroxelResX/Y/Z` | Volumetrics | Froxel grid resolution. Preset scales all three | 240 / 135 / 96 | C (preset) |
| `r_rtVolFroxelFarTransmittance` | Volumetrics | Transmittance floor that sets the froxel far plane | 0.002 | C |
| `r_rtVolProjEmitterBias` | Volumetrics | Projected-light emitter offset, so a lamp housing doesn't occlude its own beam | 24 | C |
| `r_rtVolProjFalloff` / `…ProjFalloffDist` | Volumetrics | Projected-light falloff mode and distance | 2 / 512 | C |
| `r_rtVolFroxelDebug` | Volumetrics | The one vol overlay: 2 per-cell light count, 3 grid-mapping error | 0 | C |
| `r_rtVolSamples` | Volumetrics | March samples | 8 | X |
| `r_rtVolHalfRes` | Volumetrics | Half-resolution trace (froxel forces it off) | 1 | X |
| `r_rtVolUpsampleDepthSigma` | Volumetrics | Bilateral upsample depth falloff | 24 | X |
| `r_rtVolWhiteNoiseMix` | Volumetrics | Noise mix for sampling | 0.025 | X |
| `r_rtVolBilateral` / `…BilateralSigma` | Volumetrics | Bilateral filter and sigma | 1 / 2.0 | X |
| `r_rtVolFroxel` | Volumetrics | Froxel-grid path. X after Phase 4 — froxel becomes the only path | 1 | C → X |

The X rows are march-path-only and go with Phase 4. Verify each before removing it; the
froxel path forces `r_rtVolHalfRes` off rather than ignoring it.

Deleted 2026-10-18: `r_rtVolDebugMode`, `r_rtVolDebugGain`, `r_rtVolDump`,
`r_rtVolFroxelDebugSlice`, `r_rtVolFroxelDebugGain`, `r_rtVolFroxelDump`.

### Temporal / denoise (AO + shared)

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtAOTemporal` / `…TemporalAlpha` | Temporal | AO temporal accumulation and EMA blend | 1 / 0.5 | C |
| `r_rtAtrousIterations` | Temporal | AO A-trous passes | 4 | C |
| `r_rtAtrousSigmaDepth` / `…SigmaLuminance` | Temporal | AO A-trous edge-stops | 0.01 / 0.1 | C |
| `r_rtTemporalCutPosThreshold` / `…CutAngleThreshold` | Temporal | Camera move / rotation per frame that resets every history | 6 / 4 | C |

These survive Phase 3 — AO's temporal path is separate from GI's.

### Tonemap and bloom

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtTonemap` | Tonemap | Tonemap on/off | 1 | M |
| `r_rtTonemapExposure` | Tonemap | Exposure | 1.0 | M |
| `r_rtTonemapToe` | Tonemap | Curve toe | 2.7 | C |
| `r_rtTonemapLinStart` / `…LinLen` | Tonemap | Linear section start and length | 0.3 / 0.40 | C |
| `r_rtBloom` | Bloom | Bloom on/off | 0 | M |
| `r_rtBloomStrength` | Bloom | Bloom strength. The one brightness knob | 0.15 | M |
| `r_rtBloomThreshold` / `…EmissiveThreshold` | Bloom | Brightness and emissive thresholds | 0.8 / 0.5 | C |
| `r_rtBloomKnee` | Bloom | Threshold softness | 0.5 | C |
| `r_rtBloomMips` | Bloom | Blur mip count, i.e. glow radius (not brightness) | 4 | C |
| `r_rtBloomDebug` | Bloom | HDR range bands of the finished frame. Reads the frame, not the bloom chain — useful with bloom off | 0 | C |
| `r_rtBloomLitWeight` | Bloom | Lit-surface contribution | 0 | X |

### FSR upscaling

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_fsr` | FSR | Upscale mode | 0 | M |
| `r_fsrQuality` | FSR | Quality preset (drives render scale) | 0 | M |
| `r_fsrSharpness` | FSR | Sharpening | 0.5 | M |
| `r_fsrRenderScale` | FSR | Explicit render scale | 1.0 | C |
| `r_fsrJitter` | FSR | Camera jitter | 1 | C |
| `r_fsrMotionScale` | FSR | Motion-vector scale, i.e. what saturates the debug 7 overlay | 16 | C |
| `r_fsrAutoReactive` | FSR | Auto reactive mask. Enabling it discards a hand-authored T&C mask | 1 | C |
| `r_fsrReactiveCutoff` / `r_fsrReactiveScale` | FSR | Reactive mask tuning. Tune from debug 3, not by eye | 0.05 / 1.0 | C |
| `r_fsrTcMask` | FSR | Transparency/composition mask | 1 | C |
| `r_fsrMipBias` / `r_fsrMipBiasOffset` | FSR | Texture mip bias for the lower render res | 1 / -1.0 | C |
| `r_fsrDebug` | FSR | The one FSR overlay: 3 reactive + T&C masks, 7 motion vectors | 0 | C |
| `r_vkBumpMipBias` | Textures | Extra positive mip bias on bump samplers | 0.5 | C |

### Auto-relight (experimental)

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtAutoRelight` | Auto-relight | Synthesize lights for dark areas. Experimental | 0 | M |
| `r_rtAutoRelightMax` / `…MaxPerArea` | Auto-relight | Global and per-area synthesized-light caps | 64 / 4 | C |
| `r_rtAutoRelightIntensity` | Auto-relight | Intensity | 0.5 | C |
| `r_rtAutoRelightReach` / `…Offset` | Auto-relight | Placement reach and offset from surface | 160 / 8 | C |
| `r_rtAutoRelightDedupeDist` | Auto-relight | Suppress near existing map lights | 96 | C |
| `r_rtAutoRelightDebugColor` | Auto-relight | Colour synthesized lights | 0 | C |
| `r_rtAutoRelightDebug` | Auto-relight | Debug logging level. Default should be 0, not 1 | 1 | X |

### Player exclusion (shadow/GI rays vs. the player body)

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_rtShadowPlayerExcludeDist` | Player model | Exclude player body from shadow rays when closer to a light than this | 30 | C |
| `r_rtPlayerExcludeRadius` | Player model | Cull player body from shadow/GI rays near the eye | 40 | C |

### Vulkan backend (debug / stability)

Almost all C: this is the debugging instrument set, and the engine rules say keep the
instruments. The three submit-path cvars are not substitutes for each other —
`r_vkSerializeFrames` is the only one that drops frames-in-flight to 1, which is how you
test for a CPU/GPU race on a shared buffer; `r_vkSplitSubmitMask` localises a fault within
one frame and cannot see that class at all.

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `r_vkRTProfile` | Vulkan | 0=off, 1=window summaries, 2=one row per frame | 0 | C |
| `r_vkRTProfileWindow` | Vulkan | Frames per summary window (1-256) | 30 | C |
| `r_vkRTProfileStats` | Vulkan | Summary columns: 1=mean 2=p5 4=p50 8=p95 16=max | 29 | C |
| `r_vkRTProfileGroups` | Vulkan | Console filter only — all groups are always collected and always in the CSV. Leave at 63 | 63 | C |
| `r_vkRTProfileSpikeMs` | Vulkan | Dump every metric for frames over this backend ms (0=off); ignored while a CSV is open | 0 | C |
| `r_vkRTProfileCSV` | Vulkan | Per-frame CSV path (empty=off). Suppresses the console block in favour of one heartbeat line per window | "" | C |
| `r_vkDebugLabels` | Vulkan | VK_EXT_debug_utils pass labels for Nsight/RGP/RenderDoc, including a region per light inside Interactions | 0 | C |
| `r_vkSerializeFrames` | Vulkan | Wait for the GPU between frames. Artifacts that vanish with this on are CPU/GPU races | 0 | C |
| `r_vkSplitSubmitMask` / `…SplitSubmitVerbose` | Vulkan | Extra queue submits at named stage boundaries, to localise a DEVICE_LOST | 0 / 0 | C |
| `r_vkLowPerturbationMode` | Vulkan | Master off switch for the split-submit probes. Default 1, so the mask is inert until this is 0 | 1 | C |
| `r_vkLogRT` | Vulkan | RT log level. Level 2 now also carries the FSR per-view logging | 0 | C |
| `r_vkLogSubmitInfo` / `r_vkLogTranslucent` | Vulkan | Submit and translucent-surface logging | 0 | C |
| `r_vkRTDebugInteractions` / `r_vkRTDebugLights` | Vulkan | RT interaction and light decision logging | 0 | C |
| `r_vkWeaponDepthAlways` | Vulkan | Force weapon depth-hack to ALWAYS | 0 | C |
| `r_vkPolyOffsetSlopeMin` | Vulkan | Min slope depth bias for polygonOffset | 0.0 | C |
| `r_vkShadowStableMode` | Vulkan | Force Z-fail shadow volumes | 0 | C |
| `r_vkInsideLightHysteresisEnter` / `Exit` | Vulkan | Inside-light hysteresis margins | 0.5 / 1.5 | C |

Deleted 2026-10-18: `r_vkShadowFlipOps`, `r_vkShadowFullScissor`, `r_vkLightFullScissor`,
`r_vkVcCheck`, `r_vkLogBufferGarbage`, `r_vkLogShadowBranch`, `r_vkLogShadowGeom`,
`r_vkRTReflDataDiag`, `r_vkRTDebugSurface`, `r_vkRTDebugMaxLogsPerFrame`, `r_vkRTInterLogN`,
`r_vkRTLogCaptureReset`, `r_vkRTDebugLightTextures`, `r_vkRTLightLogN`,
`r_vkRTDebugLightFilter`.

### Player model: first person, third person

| Name | Group | Purpose | Default | Verdict |
|---|---|---|---|---|
| `pm_showFirstPersonBody` | FP/TP model | Show own torso and legs in first person. Phase 0 flips the default to 1 | 0 | M |
| `pm_firstPersonBodyOffset` | FP/TP model | Body shift along its forward axis (negative = back) | -4 | C |
| `pm_firstPersonBodyMaxPitch` | FP/TP model | Max downward pitch while the body is shown | 70 | C |
| `pm_firstPersonBodyDebug` | FP/TP model | Log body create/free and pose | 0 | C |
| `pm_ladderLegAnim` | FP/TP model | Step/hang leg anim on ladders | 1 | C |
| `pm_thirdPerson` | FP/TP model | Third-person camera | 0 | M |
| `pm_thirdPersonRange` | FP/TP model | Camera distance | 80 | M |
| `pm_thirdPersonHeight` | FP/TP model | Camera height offset | 0 | C |
| `pm_thirdPersonAngle` | FP/TP model | Camera angle (0 behind, 180 front) | 0 | C |
| `pm_thirdPersonClip` | FP/TP model | Clip camera against world | 1 | C |
| `pm_thirdPersonDeath` | FP/TP model | Third person on death | 0 | C |
| `pm_modelView` | FP/TP model | Camera from the player model's POV | 0 | C |
| `g_showPlayerShadow` | FP/TP model | Player model casts shadow / appears in reflections | 1 | M |

### Debug mode census — Phase 1d done 2026-10-18

Was 44 live modes across 8 cvars, now 13 across 7. Counted by grepping the shader
`debugMode ==` branches, not the help strings — the two had drifted (the shadow help
documented no mode 12, and `r_rtShadowBackfaceCull`, which that mode visualized, no longer
exists). Retired numbers are documented no-ops, so doc and memory references keep resolving.

| Cvar | Was | Now | Retired |
|---|---|---|---|
| `r_rtReflectionDebugMode` | 9 | 2 G-buffer normal, 3 F0, 11 denoiser history, 12 roughness, 13 GI handoff, 15 à-trous edge stops | 4, 6, 14 (plus 1, 5, 7-10 already) |
| `r_rtShadowDebugMode` | 9 | 9 bias budget, 10 player exclusion, 11 light-volume cull | 1, 2, 3, 5, 7, 8 |
| `r_rtGIProbeDebug` | 8 | 2 probe weights, 3 leak detector | 1, 4, 5, 6, 7, 8 |
| `r_fsrDebug` | 7 | 3 reactive/T&C masks, 7 motion vectors | 1, 2, 4, 5, 6 |
| `r_rtVolFroxelDebug` | 4 | 2 light-count heatmap, 3 grid-mapping error | 1, 4 |
| `r_rtAODebug` | 3 | 1 raw mask, 2 GI×AO, 3 GI without AO | — |
| `r_rtBloomDebug` | 2 | 1 HDR range bands | 2 |
| `r_rtVolDebugMode` | 2 | cvar deleted | — |

Every one of the seven now routes through a `VK_RT_Valid*DebugMode()` that coerces a retired
number to 0. Without it a dead value still read as "an overlay is active" and took the
perturbed path — replace-blend composite, suppressed blur, forced `r_rtReflectionMode 3` —
to render an ordinary image.

Capability actually lost, not just renumbered: the probe sphere-lattice overlay (placement,
buried/void classification) and the FSR2 bleed and bloom source passes. The probe
lattice was already broken before this pass — its renderer went with `r_rtGIProbeDebugRadius`
in the 2026-10-18 cvar commits, so modes 1/4/5/6/7 had been rendering black.

## 3. Recommended quality preset

One new `r_rtQuality` cvar (menu combo) writes these; the individual cvars stay available in the console and Advanced node.
The numbers below are starting points, not measured.

| Cvar | Low | Medium | High (= current) | Ultra |
|---|---|---|---|---|
| `r_rtShadowSamples` | 1 | 2 | 2 | 4 |
| `r_rtAOSamples` | 2 | 4 | 4 | 8 |
| `r_rtGIProbeRays` | 64 | 128 | 128 | 192 |
| `r_rtGIProbeUpdatesPerFrame` | 512 | 1024 | 1024 | 2048 |
| `r_rtGIMaxBounceLights` | 8 | 16 | 16 | 24 |
| `r_rtVolSamples` | 4 | 8 | 8 | 12 |
| `r_rtVolHalfRes` | 1 | 1 | 1 | 0 |
| `r_rtVolFroxelRes{X,Y,Z}` | 120/68/48 | 160/90/64 | 240/135/96 | 320/180/128 |

## 4. Menu cleanup checklist

| Change | File | Check |
|---|---|---|
| Drop every C and X widget from the menu | `Dhewm3SettingsMenu.cpp` ~2749-3060 | `rtCVars.*` bindings 116 → ~30, all M rows from §1 |
| Add the new Reflections group (6 widgets, §1) | same | Sliders move the image live; F0 scale/gamma visibly change how reflective the world reads |
| Add `r_rtQuality` combo + apply function | menu + `RenderSystem_init.cpp` | Selecting a preset changes the §3 cvars in `cvarlist` |
| Add `CVAR_ARCHIVE` to the M cvars that lack it (see §1) | Vulkan `.cpp` files | Value survives a restart |
| Reconcile the four eye-tuned reflection defaults | `vk_reflections.cpp` | A fresh config and a tuned `.cfg` produce the same image |
| ~~Remove or define `r_rtDenoise`~~ | — | Done. Menu binds `r_rtReflDenoise`; no `r_rtDenoise` exists |
| Add `pm_showFirstPersonBody`, `pm_thirdPerson`, `pm_thirdPersonRange` | menu "Player model" group | Toggling in menu changes the in-game camera/body |
