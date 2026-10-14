# RT Roadmap

**Status reviewed:** 2026-10-13 · **Next action: optimization Stage S** (`20261010_vk_core_audit.md` Part 3)
**This is the entry point.** If you're wondering what to work on or which plan doc
is authoritative, start here. This file owns *ordering* and *status*; detailed
designs live in the linked docs. Prior cycle: `completed/202610a_ROADMAP.md`.

Rules, pillars and settled conclusions moved to `../rt_engine_rules.md` on 2026-10-13 —
they are things to check code against, not things to schedule.


---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 8 | **Optimization** — Vulkan core (F1-F13/A0-A5), RT uploads (R1-R10/B0-B4), and **Stage S** RT shadow dispatch (was cleanup Phase 6a) | `20261010_vk_core_audit.md` | **Current.** Owns all perf work. Stage S runs first: 74% of frames overflow the 7-light batch, up to 10 ms. 9 findings dispatched by measurement; A2/A4/A5 parked — all three scenarios are GPU-bound by 4-7x |
| 8b | **Glass reflection normals** — Mars City windows focus and invert; the panes are piecewise-flat Bezier patches whose 26.57° crease `GenerateNormals` smooths into a cylindrical mirror. Stages 1-3 | `20261013_glass_geometric_normal.md` | Unstarted. Small and bounded. Cause confirmed by hand calculation + a census of all 27 stock maps (718 glass patches: 6 `CREASE`, 21 real `ARC`, 153 `MIXED`) |
| 8c | **Volumetrics in reflections** — the flashlight beam is absent from glass; march the reflected segment through `froxelScatter`. Stages 1-4 | `20261013_reflection_volumetrics.md` | Stages 1-3 landed 2026-10-13, **not yet validated in-game**. Stage 4 is the debug overlay + menu entries |
| 8d | **Surface roughness in reflections** — extend past glass to opaque surfaces with a GGX lobe. Blocked on a reflection denoiser (the only RT pass without one) and a roughness ceiling to avoid double-counting probe GI. Stages 1-5 | `20261013_reflection_roughness.md` | Unstarted. Budget 20-40% frame time for the all-surface mode; **gates cleanup Phase 2b/2c**, which would otherwise reclaim the G-buffer alpha that F0 needs |
| 8e | **Cleanup + polish** — debug/cvar trim (1a/1b/1c), dead-path deletion, resets, presets, comment sweep | `20261010_cleanup_polish.md` | Reordered 2026-10-13: deletion runs *after* the gaining work, since the dead GI/vol paths measure 0.001 ms. 1a and 5a are unblocked and can land any time |

### Execution order

Interleaved across arcs, gaining work first, deletion last. The arc table above says *what*;
this says *when*.

| # | Step | From | Gate to clear |
|---|---|---|---|
| 1 | **Stage S** — shadow dispatch S2-S7 | 8 | Only double-digit measured GPU item (up to 10 ms, 74% of frames). S0/S1 dropped 2026-10-13: steps read live off a `shadow.pixels` counter instead of a capture each |
| 2 | 8c Stage 4 + validate 1-3 in-game | 8c | Already landed, unvalidated. Finish before stacking more reflection work |
| 3 | 8b glass crease fix | 8b | Small, bounded; adds refl debug modes 9/10 |
| 4 | Cleanup 1a + 5a | 8e | Unblocked; land whenever convenient |
| 5 | Cleanup 1b | 8e | Needs Stage S closed |
| 6 | Optimization B0/B2/B1, BLAS rebuild burst | 8 | GPU-side; everything CPU-side is parked |
| 7 | 8d roughness Stages 1-5 | 8d | Denoiser first. Decide go/no-go on the 20-40% frame cost before Stage 4 |
| 8 | Cleanup 1c + Phase 2 | 8e | Needs the reflection arc resolved — **2c is irreversible** |
| 9 | Cleanup 3, 4 | 8e | Biggest surgery, 0.001 ms gain. Last of the code changes |
| 10 | Cleanup 5b presets, then Phase 7 comments | 8e | Needs the final cvar set |

If 8d is abandoned, step 8 unblocks immediately and Phase 2 can delete the F0 path outright.




### Measured RT budget

Orientation only — current numbers so the arc table has a scale. Method, history and
per-phase detail: `20261010_vk_core_audit.md` and `notes/20261010_vk_core_baseline.txt`.

**Test machines**

| | CPU | GPU | Notes |
|---|---|---|---|
| Fast | Ryzen 9 5950X | RTX 4070 Ti Super | Default for all captures unless stated |
| Slow | Ryzen 5 2600X | AMD 9070 XT | Weaker RT card — this is what motivated the upscaling work. ~60 fps after the particle-upload CPU fix |

**Current baseline** — 2026-10-12, `com_fixedTic 1` (uncapped), 1440p, no upscaling, three
scenarios, p50 GPU ms from CSV:

| | GI | Refl | Vol | AO | Shadows | TLAS | RT total | raster |
|---|---|---|---|---|---|---|---|---|
| mchq / r2r / commfire | 0.00 | 0.00-1.11 | 0.50-1.13 | 1.69-2.51 | 1.33-4.07 | 0.17 | **5.07-9.68** | 3.47-6.56 |

Total GPU p50: mchq 9.17 ms (109 fps), r2r 11.63 (86), commfire 14.94 (67). **All three are
GPU-bound by 4-7x** — CPU recording is 1.4-1.7 ms, so no CPU-side work can show up as frame
time. This is why optimization parks every CPU stage and Stage S runs first.

`com_fixedTic 1` matters: Doom 3 otherwise caps at 62.5 fps (`USERCMD_MSEC` is integer
`1000/60`), which hid all GPU cost beyond ~16 ms in every capture before this one.

## Live documents

| Doc | Owns |
|---|---|
| `20261010_vk_core_audit.md` | **All optimization.** Findings (F1-F13, R1-R10, X1-X8), stages A0-A5 / B0-B4, **Stage S** shadow dispatch, the baseline measurements, profiling tooling, and the optimization order |
| `20261010_cleanup_polish.md` | **All deletion and polish, no optimization.** Phase order 1a/1b/1c, the per-phase rules (cvar+UBO+shader+menu deleted together), presets and resets, comment sweep |
| `20261013_glass_geometric_normal.md` | Glass crease fix; the stock-map glass-patch census |
| `20261013_reflection_roughness.md` | Roughness/GGX reflections, the denoiser, and the GI handoff ceiling |


**Reference Documents**
| Doc | Owns|
|----|-----|
| `../Dhewm3-AI-RT-Settings.md` | Cvar inventory and tiers; the source for arc 8 phase 1 (trim) and phase 5 (presets) |
| `../rt_engine_rules.md` | **Not a plan.** Taste contract, correctness rules, settled conclusions, debugging posture. Check new RT code against it |
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
