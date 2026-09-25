# RT Roadmap

**Status reviewed:** 2026-09-25 · **Next work: arc 3 (FSR) — U3 plays well; U3a (texture LOD bias) blocks its exit gate.**
**This is the entry point.** If you're wondering what to work on or which plan doc
is authoritative, start here. This file owns *ordering* and *status*; detailed
designs live in the linked docs. Prior cycle: `completed/202608_ROADMAP.md`.

---

## Design pillars (the taste contract)

Every stage below serves these; anything that fights them gets cut or demoted.

1. **Shadows are the feature.** Soft, shaped, from more lights. This is where
   "dynamic lighting" actually lives.
2. **Darkness stays black.** GI may tint lit regions; it must never lift the noise
   floor of dark ones.
3. **Light the air sparingly.** Volumetrics from hero lights only.
4. **Reflections are set dressing.** Glass and mirrors, plus the odd hero surface by
   explicit opt-in.  The assets carry no PBR data; don't pretend they do.
5. **No map editing.** Engine-side rules + budgets + debug overlays. Small def-file
   mods are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.

---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 1 | **Reflection gating rework** — glass-only instead of a flat per-pixel rate over the whole screen | `completed/20260911_reflection_gating.md` | ✅ Closed 2026-09-18 |
| 1b | **Reflection brightness** — reflections dim, player self-shadowing in glass | `completed/20260918_reflection_brightness.md` | ✅ Closed 2026-09-19. B2 dropped (GI/vol were over-**bright**; settled in tuning) |
| 1c | **Stale dynamic-model normals in RT** — skinned meshes reached the TLAS with bind-pose normals | `completed/20260919_dynamic_model_normals.md` | ✅ Closed 2026-09-19. 0.16 ms with 12 characters on screen |
| 2 | **Froxel volumetrics + probe GI** — vol/GI sampling moved into world-space caches | `completed/20260906_froxel_probe_gi.md` | ✅ **Closed 2026-09-19.** Both default (`r_rtVolFroxel 1`, `r_rtGIProbes 1`); old paths kept as A/B. Vol 1.63 → 0.29 ms, GI 4.41 → ~1.3 ms |
| 3 | **FSR upscaling** — every RT pass is screen-resolution; decoupling render res is worth ~6.6 ms of 11.93 | `20260918_fsr_upscaling.md` | 🟡 **U0 landed 2026-09-23** — render-resolution decoupling + bilinear resolve, `r_fsrRenderScale`. **U1 landed 2026-09-24** — FSR 1 (EASU+RCAS) behind `r_fsr 1`, AMD headers in `neo/libs/ffx-fsr/` (MIT); exit met. **U2 landed 2026-09-24, exit met** — motion-vector attachment + Halton jitter + `r_fsrDebug 7` overlay. **U3 landed 2026-09-24** — FSR 2.2.1 compiled in (`DHEWM3_FSR2`), `r_fsr 2` dispatches it, `r_fsrQuality` presets, `r_fsrDebug 5` bleed overlay; needs three device features AMD's backend assumes (see §14 S2). Plays well 2026-09-25, but **🔴 U3a (texture LOD bias, §4) blocks the U3 gate** — samplers bake `mipLodBias` at upload and carry no render-scale term, so distant detail reads as watercolour and the quality-vs-native comparison is unfair. Perf numbers can still be taken. U4-U5 not started. Open: mirror/subview dispatch rects, glass rect, RT-total measurement — see §4 Outstanding |

- Constants are tuned and a default is selected (raster fallout, reach=1). Closed.
- U0 gated the screen-space composites on `hasRealCamera` — they had been running twice
  per frame (3D view + 2D GUI overlay view), doubling the GI/refl/vol contribution. Noted
  because it shifted the baseline the current constants sit on.

### Measured RT budget

`r_vkRTProfile 1`, Mars City, median GPU ms per phase. **RTX 4070 Ti Super, 2560×1440.**

| | GI | Refl | Vol | AO | Shadows | TLAS | **total** |
|---|---|---|---|---|---|---|---|
| 2026-09-11 (pre-arc-1/2) | 4.41 | 3.24 | 1.53 | 1.17 | — | 0.15 | **11.93** |
| 2026-09-19 (arc 2 closed) | ~1.3 | ~0.8 | 0.29 | 1.9 | — | 0.12 | **~7-8** |
| 2026-09-23 MC2, scale 1.00 | 1.29 | ~0 | 0.52 | 1.27 | 1.80 | 0.17 | **5.05** |
| 2026-09-23 MC2, scale 0.67 | 0.54 | ~0 | 0.37 | 0.49 | 0.68 | 0.14 | **2.22** |

Shadows and AO are the two largest costs. **Not every remaining pass is
screen-resolution** — arc 2 left a ~0.73 ms floor (TLAS, probe trace/blend, froxel
fill/integrate) that render scale cannot touch, so 0.50 saves only 0.1 ms more than 0.67.
See `20260918_fsr_upscaling.md` §12.

**Raster is uninstrumented** — the profiler covers RT phases only, and these runs were
vsync-locked at 60, so the raster share of the frame is unknown. Wrapping the depth
prepass / interaction loop / shader passes in profiler phases is the next measurement.

**These are all from the fast card.** The 9070 XT runs the same content near 30 fps with
dips into the teens; nothing above has been re-measured there, and it is the hardware the
arc exists for.

### Engine-wide rules

Each of these cost a debugging session. They apply to any new RT code.

| Rule | Because |
|---|---|
| A ray origin from `rt_ReconstructWorldPos` must floor its bias at `d²·ulp/znear` | A fixed bias is a distance-limited bias. `r_znear` is game-owned and drops to 1.0 in cinematics |
| A shadow ray's cull mask is a parameter, never a constant | `noSelfShadow` is instance mask `0x01`; `0xFF` made the player self-shadow in reflections only |
| A light's *current* value comes from `EvaluateRegisters()` + the stage's `color.registers[]` | Doom 3 puts flicker in material expressions. `shaderParms` is the constant amplitude — 74 animated materials were pinned at peak |
| A mapped buffer the CPU **reads** needs `HOST_CACHED`, and should be memcpy'd out before use | `HOST_VISIBLE\|HOST_COHERENT` alone is write-combined; scalar reads cost ~200 ns each. Worth 3.26 → 0.03 ms on the probe classifier |
| Doom 3 winds front faces opposite to GL/Vulkan — use the vertex normal, not `gl_HitKindEXT` | Every visible surface reports back-facing |
| A pixel-skipping pattern keys off a per-slot counter, never `tr.frameCount` | Caused the GI checkerboard ghost |
| Shared GLSL that compute shaders include must be **pipeline-agnostic** — no ray payload, no `traceRayEXT`, no TLAS. `rt_light_struct.glsl` is that home; `rt_light_eval.glsl` is not | An RT-only built-in pulled into a compute shader silently killed all volumetric lighting once |

### Standing decisions

- **Reflections are glass-only** (`r_rtReflectionMode 1`). Per-material F0 (T3) is
  **dropped** — with opaque geometry never reflecting there is nothing to classify.
  Threshold tuning cannot help: the worst artifacts are the *highest*-F0 pixels while any
  threshold culls from the bottom.
- **Albedo is the volumetric lever**, not extinction and not the tonemap toe. It cuts
  in-scatter while leaving attenuation intact, so the medium darkens more than it glows and
  lit-vs-unlit contrast goes *up*. Per-class radiance gains carry punch (6:1 point:directed in play); 
- **Do not "fix" RT direct lighting being half the raster path's.** True at the source
  (`r_lightScale` = 2 is not applied to the RT upload), but the downstream gains overshot
  it — GI and vol read *over*-bright. This was a tuning matter, not a plumbing fix.
- **The tonemap toe has two regimes.** At `r_rtTonemapToe 2.7` slope is <0.33 below
  luminance 0.07 but *exceeds 1.0* between 0.12 and 0.3. Evaluate the whole curve before
  blaming tonemapping.
- **Reflected surfaces get no indirect light** — GI modulates by the *primary* G-buffer
  albedo. Structural, not a tuning miss.
- **Using same light falloff as raster** - Keep reach = 1.  Get benefit of art,
while new lighting techniques enhance without fighting too much.

---

## Live documents

| Doc | Owns |
|---|---|

| `20260918_fsr_upscaling.md` | Render-resolution decoupling and FSR upscaling (arc #3). Also owns motion vectors and jitter, which any future upscaler/TAA would share. |
| `20260906_bloom_plan.md` | Bloom post-process — unimplemented; the tonemapped HDR pipeline it needs now exists. |
| `see_first_person_player_model.md` | First-person player body; orthogonal to the lighting arc. |
| `20260924_controller_gunfeel.md` | Gamepad aim response, rumble, aim assist (C1-C4). Orthogonal to the lighting arc. |
| `../vulkan_debugging.md` | Not a plan — the reference for getting Vulkan validation/GPU-AV output out of this engine. Load it before chasing any AMD-vs-NVIDIA or device-lost bug. |

---

## Completed

All in `completed/`. Waves 1-7 of the original roadmap are done.

| Doc | Owns |
|---|---|
| `20260906_froxel_probe_gi.md` | **Arc 2.** Froxel volumetrics + probe GI, both shipped as default. Includes G5b flicker factorization and the animated-light premise correction |
| `20260919_dynamic_model_normals.md` | Skinned md5 meshes reaching the TLAS with bind-pose normals |
| `20260918_reflection_brightness.md` | Reflection brightness + player self-shadowing in glass |
| `20260911_reflection_gating.md` | Reflections narrowed to glass-only |
| `20260917_vol_transport_coefficients.md` | Volumetric medium split into σ_t + albedo + per-class radiance gains |
| `20260905_rt_projected_light_cookies.md` | Projected-light cookie/gobo textures across all four consumers |
| `20260831_rt_temporal_cut_detection.md` | Camera-cut detection; also the degenerate GUI `RC_DRAW_VIEW` re-running every RT pass |
| `20260826_amd_vulkan_cleanup.md` | AMD-vs-NVIDIA RT correctness. A1/A3/A5/A8/A11/A12 landed; A2/A4/A6/A7 minor/latent |
| `20260810_auto_relight.md` | Shadow-casting lights synthesized from emissive panels |
| `20260808_gbuffer_normal_pass.md` | G-buffer normal/F0 prepass |
| `20260816_portal_area_lights.md` | Stage 2 (transition blend) shelved, now unblocked — not scheduled |
| `202608_rt_optimization_tuning.md` | Perf items P1-P10, light-list L1, tuning items T1-T6. Waves 2-4 done; T3 dropped, T5 won't-fix, All completed 2026-9-21 (`r_rtGIFalloffMode` 1 chosen, with reach=1) |
| `20260831_rt_parallel_sun_lights.md` | Sun/parallel lights. **Not pursuing** |

---

## Backlog (not in the current arc, not dead)

| Item | Doc | Note |
|---|---|---|
| Controller gun-feel | `20260924_controller_gunfeel.md` | C1 (radial deadzone + curve) and C2 (accel) are framework-only and independent of the RT arc — can run any time. C3 rumble, C4 aim assist follow. Not scheduled. |
| Adaptive probe hysteresis (was G5 fix 2) | `completed/20260906_froxel_probe_gi.md` | Boost alpha when a probe's new value differs sharply from `prev`. The answer for **doors and moving lights** — G5b handles flicker and explicitly cannot help here. Needs a lower-variance estimator first (`r_rtGIProbeRays 256`), so it costs ~+0.5 ms before it starts - Skip|
| Probe relocation / per-area isolation | `completed/20260906_froxel_probe_gi.md` | Dropped from G4. Revisit only if leaks reappear on a map where Chebyshev isn't enough - Skip|
| Projectiles in reflections | `completed/20260423_reflection_enhancements.md` AR3 | Sprite attempt reverted (`f37f071b`); needs a new approach. |
| Roughness-blurred reflections | — | Now the *only* route to reflective non-glass surfaces: sharp mirror reflection is why opaque geometry looks wrong, so "dimmer" can't fix it. Affordable for the first time now the traced pixel set is tiny. Not scheduled. |
| Runtime emissive-state lights | `completed/20260810_auto_relight.md` | v2 of auto-relight - Skip |
| Translucent square borders over reflections | `completed/20260423_reflection_enhancements.md` | Polish. |
