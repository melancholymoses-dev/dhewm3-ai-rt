# RT Roadmap

**Status reviewed:** 2026-10-20 · **Next action: 8h Stage 1 (step 9b), 8g Stage 5 knob consolidation (step 7b), or cleanup 1a + 5a (step 4).**
Optimization Stage L (interaction overdraw) was characterized and **parked** — four candidate
causes all measured negative, and the remaining one is blocked on light falloff being a
texture. Details and dead ends: `20261010_vk_core_audit.md` → "Stage L — parked".
**This is the entry point.** If you're wondering what to work on or which plan doc
is authoritative, start here. This file owns *ordering* and *status*; detailed
designs live in the linked docs. Prior cycle: `completed/202610a_ROADMAP.md`.

Rules, pillars and settled conclusions moved to `../rt_engine_rules.md` on 2026-10-13 —
they are things to check code against, not things to schedule.


---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 8 | **Optimization** — Vulkan core (F/A), RT uploads (R/B), not-yet-audited (X), **Stage L interaction overdraw**. Stage S shadow dispatch closed | `20261010_vk_core_audit.md`, closed half in `completed/20261010_vk_core_audit_closed.md` | **Parked** — see that doc's "Current lead" section. Three wins (S2 cull, `r_rtShadowSamples` 4 → 2, BLAS pooling); 17 findings dispatched or parked by measurement.  Pausing |
| 8d | **Surface roughness in reflections** — extend past glass to opaque surfaces with a GGX lobe. Stages 1-4 | `completed/20261013_reflection_roughness.md` | **Done 2026-10-16.** Closed on live visual confirmation (sheen on floors, clear screen/emitter reflections, no fireflies or blinding pixels), not the formal numeric Stage Checks — none of those were run end to end. Three same-day addenda beyond the original plan: roughness-gated à-trous spatial filter (`r_rtReflDenoise 2`) with a tunable wash gain, a local firefly clamp (`r_rtReflAtrousFireflyClamp`), and a flat brightness ceiling (`r_rtReflMaxLuminance`). Stage 5's menu/preset/doc polish not done — see that doc's "What's left" |
| 8g | **Reflection pipeline refactor** — stochastic lights at hits, demodulation, linear-depth + normal edge stops, half-res opaque trace, knob consolidation | `20261017_reflection_pipeline_refactor.md` | **Stages 1-4 done, verified 2026-10-18.** `gpu.Refl` 3.81 → 1.45 ms p50 and `gpu.total` 7.89 → 5.52 (−30%) at Mars City HQ, no visible quality loss. The reflection stack went from 55% of RT time to 36%. Only Stage 5 (knob consolidation) is left, and it is cleanup — no image change intended |
| 8f | **Live GUI screens in reflections** — render GUIs to slot textures, admit GUI surfaces to the TLAS. Stages 0-3 | `completed/20261015_gui_reflections.md` | **Done 2026-10-16.** Bioscan screens live in the glass; gain 0.3, refresh every 2nd frame. Unchecked: pool size in the admin offices |
| 8h | **Muzzle flash sprite in reflections** — the authored `*_mflash` surface instead of a disc synthesized from the muzzle-flash light. Stages 1-4 | `20261020_muzzle_flash_sprite_reflections.md` | **Not started.** Stage 1 (emitter texture matrix) is the dependency and is useful alone — every animated additive stage currently reaches reflections at frame 0 |
| 8e | **Cleanup + polish** — cvar trim (1a/1b/1c), debug mode trim (1d), dead-path deletion, resets, presets, comment sweep | `20261010_cleanup_polish.md` | Re-scoped 2026-10-18 after the reflection arcs closed. Measured scale: **255 cvars, 116 menu widgets, 44 debug modes**; Phase 1 now assigns every cvar M (menu) / C (console) / X (delete). New Phase 1d trims debug modes 44 → 18. 1a, 1b, 1c and 5a are all unblocked; only Phase 2 and the presets still wait on 8g Stage 5 |

### Execution order

Interleaved across arcs, gaining work first, deletion last. The arc table above says *what*;
this says *when*.

| # | Step | From | Gate to clear |
|---|---|---|---|
| 1 | ~~Stage S~~ — **done 2026-10-13** | 8 | S2 + S3 landed; S2b reverted; S5-S7 dropped. Close-out in `completed/20261010_vk_core_audit_closed.md` |
| 2 | ~~Validate 8c Stages 5 + 6 in-game~~ — **done 2026-10-15** | 8c | Both validated. Stage 4's debug overlay not built. Doc in `completed/` |
| 3 | ~~8b glass crease fix~~ — **done 2026-10-15** | 8b | `r_rtGlassNormalMode 2` default; refl debug mode 9. Doc in `completed/` |
| 4 | Cleanup 1a + 5a | 8e | Unblocked; land whenever convenient |
| 4b | ~~8f GUI reflections Stages 0-3~~ — **done 2026-10-16** | 8f | Doc in `completed/` |
| 5 | Cleanup 1b (+ Stage S4 serial-path deletion) | 8e | Unblocked — Stage S closed |
| 6 | ~~Optimization Stage L~~ — **parked 2026-10-14.** L1 landed (per-light labels); L2 blocked, L3 dropped | 8 | Four candidate causes measured negative. If optimization resumes, start with the AO ray budget. |
| 7 | ~~8d roughness Stages 1-4~~ — **done 2026-10-16** | 8d | Closed on live testing, not the formal numeric Stage Checks (never run end to end). Doc in `completed/`. Stage 5 (menu/presets/docs) not done — not blocking |
| 7b | 8g reflection refactor Stage 5 (6 optional) | 8g | Stages 1-4 landed and verified. Stage 5 is knob consolidation only — cleanup, no image change |
| 8 | Cleanup 1c, then 1d | 8e | 1c unblocked 2026-10-18 — reflection cvars have M/C/X verdicts in the Settings doc. 1d (debug modes 44 → 18) wants 1a/1b/1c settled first since it edits shader branches |
| 8b | Cleanup Phase 2 (legacy reflection mode 2) | 8e | Rewritten 2026-10-17: F0 stays and becomes a menu knob; only mode 2 goes. Needs 8g Stage 5's knob fold first |
| 9 | ~~Cleanup 3, 4~~ — **done 2026-10-20** | 8e | Both landed; neither validated in-game yet. Screen-space GI and the per-pixel vol march are gone, probe GI and the froxel grid are the only paths |
| 9b | 8h muzzle flash sprite, Stages 1-3 | 8h | Unblocked. Stage 3 deletes cvars added 2026-10-20, so land it before cleanup 1c assigns them a tier |
| 10 | Cleanup 5b presets, then Phase 7 comments | 8e | Needs the final cvar set |


### Measured RT budget

Orientation only — current numbers so the arc table has a scale. Method, history and
per-phase detail: `20261010_vk_core_audit.md` and `notes/20261010_vk_core_baseline.txt`.

**Test machines**

| | CPU | GPU | Notes |
|---|---|---|---|
| Fast | Ryzen 9 5950X | RTX 4070 Ti Super | Default for all captures unless stated |
| Slow | Ryzen 5 2600 | AMD 9070 XT | Weaker RT card — this is what motivated the upscaling work. 49-64 fps p50 at 1440p (2026-10-13) |

**Current baseline** — Fast 2026-10-12, Slow 2026-10-13. `com_fixedTic 1` (uncapped), 1440p,
no upscaling, three scenarios (mchq / r2r / commfire). The per-pass table lives in
`20261010_vk_core_audit.md`; only the totals are repeated here.

| Total GPU p50 (fps) | mchq | r2r | commfire |
|---|---|---|---|
| Fast | 9.17 (109) | 11.63 (86) | 14.94 (67) |
| Slow | 14.31 (64) | 15.12 (61) | 19.66 (49) |

**Both machines are GPU-bound in every scenario** — 4-7x on Fast (CPU recording 1.4-1.7 ms),
4-5x on Slow (CPU busy 3.6-4.0 ms). No CPU-side work can show up as frame time, so
optimization parks every CPU stage and Stage S runs first. Serial shadow dispatch reproduces
on Slow (0.50-0.59 ms per serial light). commfire's p95 is ~34 ms (29 fps) on both machines,
driven by gpu.Interactions.

`com_fixedTic 1` matters: Doom 3 otherwise caps at 62.5 fps (`USERCMD_MSEC` is integer
`1000/60`), which hid all GPU cost beyond ~16 ms in every capture before this one.

## Live documents

| Doc | Owns |
|---|---|
| `20261010_vk_core_audit.md` | **All optimization.** Open findings, open stages (B2 remainder, B3, B4, A3, E2-E4), the X verdicts, the baseline numbers and the next levers. Closed work is in `completed/20261010_vk_core_audit_closed.md`; the profiler reference is in `../vulkan_debugging.md` |
| `20261010_cleanup_polish.md` | **All deletion and polish, no optimization.** Phase order 1a/1b/1c, the per-phase rules (cvar+UBO+shader+menu deleted together), presets and resets, comment sweep |


**Reference Documents**
| Doc | Owns|
|----|-----|
| `../Dhewm3-AI-RT-Settings.md` | Cvar inventory and tiers; the source for arc 8 phase 1 (trim) and phase 5 (presets) |
| `../rt_engine_rules.md` | **Not a plan.** Taste contract, correctness rules, settled conclusions, debugging posture. Check new RT code against it |
| `../vulkan_debugging.md` | Not a plan — the reference for getting Vulkan validation/GPU-AV output out of this engine, **and the profiler reference** (capture recipe, cvars, metric groups, CSV, `vk_profile_report.py`, external tools). Load it before chasing any AMD-vs-NVIDIA or device-lost bug, or before taking a measurement. |

---

## Completed

All in `completed/`. 

| Doc | Owns |
|---|---|
| `20261013_reflection_roughness.md` | **Arc 8d.** Opaque reflections get a GGX lobe sourced from bump-map (Toksvig) roughness instead of a mirror; reflection temporal + roughness-gated à-trous denoiser; firefly clamp and flat brightness ceiling for the single-sample lobe's worst outliers |
| `20261013_reflection_volumetrics.md` | **Arc 8c.** Flashlight beam in glass: reflected segment marched through `froxelScatter` (`r_rtReflVol*`), mean-direction phase grid, GL projected-light falloff image in the shared light eval |
| `20261013_glass_geometric_normal.md` | **Arc 8b.** Glass patch crease normals: per-triangle symmetry test (`r_rtGlassNormalMode 2`, default), refl debug mode 9, the stock-map glass-patch census. Stage 3 map tour skipped — only the `mars_city1` cafeteria was checked |
| `20261010_vk_core_audit_closed.md` | **Arc 8, closed half.** Stage S shadow dispatch in full, the closed B stages and their A/Bs (BLAS pooling, static flags, BAR placement, frame serialization), the 10 findings dispatched by measurement, the parked stages, and the three measurement defects |
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
