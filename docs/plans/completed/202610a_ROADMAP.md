# RT Roadmap

**Status reviewed:** 2026-10-13 · **Next action: optimization Stage S** (`20261010_vk_core_audit.md` Part 3)
**This is the entry point.** If you're wondering what to work on or which plan doc
is authoritative, start here. This file owns *ordering* and *status*; detailed
designs live in the linked docs. Prior cycle: `completed/202609_ROADMAP.md`.


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
| 3 | **FSR upscaling** — render resolution decoupled from display; FSR 1/2 resolve | `completed/20260918_fsr_upscaling.md` | ✅ **Closed 2026-09-30.** U0-U4 landed and validated in-game: `r_fsrRenderScale`, FSR 1 (`r_fsr 1`), motion vectors + Halton jitter, FSR 2.2.1 (`r_fsr 2`) with reactive and T&C masks, texture LOD bias. U4 denoiser retune dropped (~0.1-0.2 ms, no visible double-accumulation). 0.67 scale saves ~2.8 ms of RT at 1440p; arc 2's ~0.7 ms world-space floor caps further gains. U3b (RT texture LOD) and U5 (menu, FSR 3.1, dynamic res, skinned MVs) moved to backlog |
| 3b | **AO / GI refinement** — AO distance falloff; AO moved from direct light onto GI | `completed/20260930_AO_GI_refine.md` | ✅ Closed 2026-10-02. Two open checks not run (viewmodel AO in debug mode 2, AO cost) |
| 4 | **Reflection Emitters** - Show emitted particles in reflections | `completed/20261002_reflection_emitters.md | Completed 2026-10-6 | 
| 5 | **Bloom** - Add lighting bloom around emissive sources| `completed/20260906_bloom_plan.md` | Completed 2026-10-7. |
| 6 | **Player Body** - Add visible body from first person | `completed/20261001_see_first_person_player_model.md` | ✅ Closed 2026-10-10. Lit, animating, RT-excluded, `rtPlayer` mask verified; ladder sink fixed (body lift, ladder leg anim, foot-IK void fix). Stairs and slopes still dip slightly, accepted |
| 7 | **TLAS build defects** — (a) pass-2 distance cull holds only the view-visible set, so vol shadow rays miss off-screen occluders and in-scatter flips on a pure yaw; (b) instance mask/SBT key off `noSelfShadow` not the player, breaking breakable-glass reflections | `completed/20261009_vol_fix.md` | ✅ Closed 2026-10-10. Pass-2 bounds cull (free: TLAS build 0.12 ms), vol gain/falloff, `rtPlayer` mask bit (breakable glass fixed), vertex-cache sub-allocator. 7.4 (per-frame upload wait) conditional |




### Measured RT budget

`r_vkRTProfile 1`, Mars City, median GPU ms per phase. **CPU: Ryzen 9 5950X, GPU: RTX 4070 Ti Super, 2560×1440.**
**These are all from the fast card.** 
| | GI | Refl | Vol | AO | Shadows | TLAS | **total** |
|---|---|---|---|---|---|---|---|
| 2026-09-11 (pre-arc-1/2) | 4.41 | 3.24 | 1.53 | 1.17 | — | 0.15 | **11.93** |
| 2026-09-19 (arc 2 closed) | ~1.3 | ~0.8 | 0.29 | 1.9 | — | 0.12 | **~7-8** |
| 2026-09-23 MC2, scale 1.00 | 1.29 | ~0 | 0.52 | 1.27 | 1.80 | 0.17 | **5.05** |
| 2026-09-23 MC2, scale 0.67 | 0.54 | ~0 | 0.37 | 0.49 | 0.68 | 0.14 | **2.22** |
| 2026-10-10 MC 1440p, scale 1.00, vol_fix 1-4,6 (76 samples, `r_rtTlasFallbackRadius 0`) | 1.72 | ~0 (p90 1.8 with glass) | 0.82 | 2.05 | 2.93 | 0.46 (0.12 TLAS + 0.34 BLAS) | **~8.0** RT |
| 2026-10-12 MC entrance, 1440p scale 1.00, **A0 window p50** (17 windows x 30 frames) | 0.82 | 0.67 | 0.83 | 1.57 | 0.17 | 0.18 (incl. 0.08 BLAS) | **4.27** RT / **6.07** raster |
| **2026-10-12 baseline, `com_fixedTic 1`, three scenarios** (p50, CSV) | 0.00 | 0.00-1.11 | 0.50-1.13 | 1.69-2.51 | 1.33-4.07 | 0.17 | **5.07-9.68** RT / **3.47-6.56** raster |

Every row above the last is capped at 62.5 fps (`USERCMD_MSEC` is integer `1000/60`), so
GPU cost beyond ~16 ms was invisible. The baseline row is uncapped; numbers, method and
the three scenario routes are in `notes/20261010_vk_core_baseline.txt` and
`20261010_vk_core_audit.md`. Total GPU p50: mchq 9.17 ms (109 fps), r2r 11.63
(86), commfire 14.94 (67). All three are GPU-bound by 4-7x — CPU recording is 1.4-1.7 ms.

**The `Shadows` column understates shadows, and by more than previously thought.** Only 7
lights batch; the rest trace one at a time inside the interaction loop and are billed to
`Interactions`. Bucketing 2978 Mars City frames by `lights.serial`: `Interactions` climbs
0.08 → 10.40 ms while `Shadows` falls 2.48 → 0.31, and total GPU doubles (6.50 → 13.13).
**74% of frames overflow the 7-layer cap**, so this is the default case, not a corner.
Optimization Stage S owns it. The `lights.*` counters read zero until 2026-10-12 (the 2D GUI overlay is
a second `RC_DRAW_VIEW` and its `ShadowBatchBegin` zeroed them before the read), which is
why three earlier captures looked like they had no shadow casters at all.

**Not every remaining pass is screen-resolution** — arc 2 left a ~0.73 ms floor (TLAS,
probe trace/blend, froxel fill/integrate) that render scale cannot touch, so 0.50 saves
only 0.1 ms more than 0.67. See `completed/20260918_fsr_upscaling.md` §12.

Also recorded on a slower CPU Ryzen 5 2600x and AMD 9070XT, which had previously shown worse performance.  It is a weaker RT card, which motivated the upscale work.  
This is after fixing big CPU drops due to particle uploads.  FPS closer to 60 now. 
| | GI | Refl | Vol | AO | Shadows | TLAS | **total** |
|---|---|---|---|---|---|---|---|
| 2026-10-10 MC 1440p | 1.72 | ~0 (p90 1.8 with glass) | 0.82 | 2.05 | 2.93 | 0.46 (0.12 TLAS + 0.34 BLAS) | **~8.0** RT |




### Engine-wide rules

Each of these cost a debugging session. They apply to any new RT code.

| Rule | Because |
|---|---|
| A ray origin from `rt_ReconstructWorldPos` must floor its bias at `d²·ulp/znear` | A fixed bias is a distance-limited bias. `r_znear` is game-owned and drops to 1.0 in cinematics |
| A shadow ray's cull mask is a parameter, never a constant | `noSelfShadow` is instance mask `0x01`; `0xFF` made the player self-shadow in reflections only |
| A light's *current* value comes from `EvaluateRegisters()` + the stage's `color.registers[]` | Doom 3 puts flicker in material expressions. `shaderParms` is the constant amplitude — 74 animated materials were pinned at peak |
| A mapped buffer the CPU **reads** needs `HOST_CACHED`, and should be memcpy'd out before use | `HOST_VISIBLE\|HOST_COHERENT` alone is write-combined; scalar reads cost ~200 ns each. Worth 3.26 → 0.03 ms on the probe classifier |
| A mapped buffer (or descriptor set) the CPU **writes** while recording needs one copy per frame slot, indexed by `vk.currentFrame` | The previous frame is still on the GPU. The shared material table caused white/black triangles on the 9070 XT only (GPU-bound, CPU a frame ahead); `r_vkSerializeFrames 1` is the check |
| Doom 3 winds front faces opposite to GL/Vulkan — use the vertex normal, not `gl_HitKindEXT` | Every visible surface reports back-facing |
| A pixel-skipping pattern keys off a per-slot counter, never `tr.frameCount` | Caused the GI checkerboard ghost |
| Shared GLSL that compute shaders include must be **pipeline-agnostic** — no ray payload, no `traceRayEXT`, no TLAS. `rt_light_struct.glsl` is that home; `rt_light_eval.glsl` is not | An RT-only built-in pulled into a compute shader silently killed all volumetric lighting once |

### Standing decisions

- **Reflections are glass-only** (`r_rtReflectionMode 1`) **until 8d says otherwise.**
  Threshold tuning cannot help: the worst artifacts are the *highest*-F0 pixels while any
  threshold culls from the bottom, so only changing the reflection's *character* can open
  opaque surfaces — which is what 8d tests.
- **F0 is not dead, it is dormant.** T3 per-material F0 was dropped because sharp mirrors
  made classification pointless; a GGX lobe needs F0 *and* roughness, so 8d reinstates the
  reason for it. **Cleanup Phase 2c must not reclaim the G-buffer alpha** until 8d resolves.
- **Roughness, if it ships, comes from bump-map normal variance, not the specular map.**
  Doom 3 has no per-material gloss at all (specular is one global NdotH table), and the
  specular map is already spent on F0 — sourcing both from it collapses them onto one curve.
  Normal variance is real data the assets already carry, so this does not violate pillar 4.
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
| `20261010_vk_core_audit.md` | **All optimization.** Findings (F1-F13, R1-R10, X1-X8), stages A0-A5 / B0-B4, **Stage S** shadow dispatch, the baseline measurements, profiling tooling, and the optimization order |
| `20261010_cleanup_polish.md` | **All deletion and polish, no optimization.** Phase order 1a/1b/1c, the per-phase rules (cvar+UBO+shader+menu deleted together), presets and resets, comment sweep |
| `20261013_glass_geometric_normal.md` | Glass crease fix; the stock-map glass-patch census |
| `20261013_reflection_roughness.md` | Roughness/GGX reflections, the denoiser, and the GI handoff ceiling |
| `../Dhewm3-AI-RT-Settings.md` | Cvar inventory and tiers; the source for arc 8 phase 1 (trim) and phase 5 (presets) |
| `../vulkan_debugging.md` | Not a plan — the reference for getting Vulkan validation/GPU-AV output out of this engine. Load it before chasing any AMD-vs-NVIDIA or device-lost bug. |

---

## Completed

All in `completed/`. 

| Doc | Owns |
|---|---|
| `20261001_see_first_person_player_model.md` | **Arc 6.** First-person player body: trimmed mesh, `rtPlayer` RT exclusion, ladder and foot-IK fixes. Cvars `pm_showFirstPersonBody`, `pm_firstPersonBodyOffset`, `pm_ladderLegAnim` |
| `20261009_vol_fix.md` | **Arc 7.** TLAS pass-2 bounds cull, projected-vol gain/falloff, `rtPlayer` mask bit, vertex-cache sub-allocator + staging ring. 7.4 conditional |
| `20260906_bloom_plan.md` | Bloom: emissive-sourced, composited after FSR and before the HUD. |
| `20261002_reflection_emitters.md` | Projectile and smoke-particle glows in glass reflections via an analytic emitter list. |
| `20260930_AO_GI_refine.md` | **Arc 3b.** AO distance falloff, AO on GI instead of direct light, `r_rtAODebug`, spectrum lights out of GI/vol, projected range from `light_end` |
| `20260924_controller_gunfeel.md` | Gamepad: radial look stick + ramp fix, rumble (incl. monster steps, chainsaw rev), aim assist, D-pad weapon groups + selector. C6 wheel parked |
| `20260918_fsr_upscaling.md` | **Arc 3.** Render-resolution decoupling, FSR 1/2, motion vectors + jitter, FSR 2 masks. U5 deferred with a skinned-MV reopen trigger |
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
| Controller gun-feel leftovers | `completed/20260924_controller_gunfeel.md` | C6 radial wheel (parked), BFG/plasma rumble keys, d3xp classname/icon verification, legacy look-cvar retirement. Listed at the top of the doc |
| AO/GI open checks | `completed/20260930_AO_GI_refine.md` | Dark viewmodel in `r_rtAODebug 2`; AO cost of closest-hit vs first-hit traversal |
| FSR U5 polish | `completed/20260918_fsr_upscaling.md` U5 | `r_fsrQuality` in the video menu, FSR 3.1 evaluation, dynamic resolution. Skinned-MV double-buffering only on the reopen trigger listed in U5 |
| RT texture LOD (U3b) | `completed/20260918_fsr_upscaling.md` U3b | RT fetches sample mip 0 at any distance. Real, but not the grating artifact |
| Adaptive probe hysteresis (was G5 fix 2) | `completed/20260906_froxel_probe_gi.md` | Boost alpha when a probe's new value differs |
| Runtime emissive-state lights | `completed/20260810_auto_relight.md` | v2 of auto-relight - Skip |
| Translucent square borders over reflections | `completed/20260423_reflection_enhancements.md` | Option A (deferred glass overlays) is in `VK_RB_DrawShaderPasses`; re-check in-game, likely closable |
