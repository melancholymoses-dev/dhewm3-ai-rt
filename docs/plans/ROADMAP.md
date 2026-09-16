# RT Roadmap

**Status reviewed:** 2026-09-11
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
   explicit opt-in. Gated by *surface type*, not by a value derived from specular maps —
   that was tried and failed (2026-09-12): Doom 3's specular maps encode a Blinn-Phong
   highlight concentrated on panel seams and trim, so an F0 remap puts sharp mirror
   specks on exactly the wrong geometry. The assets carry no PBR data; don't pretend
   they do.
5. **No map editing.** Engine-side rules + budgets + debug overlays. Small def-file
   mods are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.

---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 1 | **Reflection gating rework** — reflections were charging a flat per-pixel rate over the whole screen for sub-1% radiance. Now glass-only. | `20260911_reflection_gating.md` | 🟡 **R1/R2/R6 landed 2026-09-12**, awaiting in-game validation |
| 2 | **Froxel volumetrics + probe GI** — move vol/GI sampling out of screen space into world-space caches; deletes most of the GI noise-fighting chain structurally. | `20260906_froxel_probe_gi.md` | 🟡 **Part A F0-F2 landed 2026-09-13, in-game validated** — vol 1.63 → 0.29 ms median (5.6×), and visually better. F3/F4 dropped (payoff gone); **F6 written 2026-09-16, awaiting in-game A/B** (`r_rtVolAttenuateBackground`); F5 open behind it. **Part B G0/G1 landed 2026-09-13, in-game validated** — probes trace and converge, per-pixel GI still the default (`r_rtGIProbes 0`). Next: G2 resolve switch. |

### Measured RT budget (Mars City, 2026-09-11)

`r_vkRTProfile 1`, median ms per phase. This is the checkpoint the froxel arc was
waiting on, and it sets the ordering.

| GI | Refl (before) | Vol | AO | denoise chain | TLAS | **RT total** |
|---|---|---|---|---|---|---|
| 4.41 | 3.24 | 1.53 | 1.17 | ~0.65 | 0.15 | **11.93** |

**GI is the largest single cost**, which is why the froxel/probe arc is next and why
it is the bigger perf prize (GI + Vol + denoise ≈ 6.6 ms). Reflections were second at
27 % of the budget.

### Decisions taken 2026-09-12

- **Reflections are glass-only.** `r_rtReflectionMode 1` (default) dispatches rays only
  over the union screen rect of the view's `SURFTYPE_GLASS` surfaces, and skips
  `vkCmdTraceRaysKHR` entirely when the view holds no glass. Mode 2 keeps the old
  full-screen path for A/B.
- **Per-material F0 (tuning item T3) is dropped, not deferred.** With opaque geometry
  never reflecting there is nothing for a material F0 table to classify. If a specific
  hero surface is wanted later it returns as a small opt-in list.
- **Threshold tuning cannot fix reflection looks.** Max achievable F0 under the spec-map
  remap is 0.2, and the worst artifacts (mirror specks on rack edges, unlit fixture
  housings) are the *highest*-F0 pixels in the scene while any weight threshold culls
  from the bottom. There is no setting between "all on with artifacts" and "all off".
- **Grazing-angle tuning has a narrow reach.** The Schlick tail `pow(1-NdotV,5)` is
  ≤ 0.0007 until ~40° off-normal, so grazing knobs only affect near-silhouette pixels —
  useful for floors viewed along, a no-op on wall panels.
- **Engine-wide rule: any ray origin built from `rt_ReconstructWorldPos` must floor its
  bias at `d^2*ulp/znear`.** A fixed bias is a distance-limited bias. This caused A12 in
  both AO and shadows; GI and volumetrics share the same reconstruction and the same
  latent exposure. Check the bias before reaching for a new G-buffer target — and note
  `r_znear` is game-owned and drops to 1.0 in cinematics, cutting every safe distance by
  sqrt(3).

---

## Live documents

| Doc | Owns |
|---|---|
| `rt_optimization_tuning.md` | Perf items P1-P10, light-list L1, tuning items T1-T6. Waves 2-4 done; **T3-T6 not started** (T3 is absorbed into the reflection rework above). |
| `20260906_froxel_probe_gi.md` | World-space caching arc (arc #2). |
| `20260906_bloom_plan.md` | Bloom post-process — unimplemented; the tonemapped HDR pipeline it needs now exists. |
| `see_first_person_player_model.md` | First-person player body; orthogonal to the lighting arc. |
| `../vulkan_debugging.md` | Not a plan — the reference for getting Vulkan validation/GPU-AV output out of this engine. Load it before chasing any AMD-vs-NVIDIA or device-lost bug. |

---

## Completed

All in `completed/`. Waves 1-7 of the original roadmap are done.

| Doc | Owns |
|---|---|
| `20260826_amd_vulkan_cleanup.md` | AMD-vs-NVIDIA RT correctness. A1/A3/A5/A8/A11 landed; **A12 (far-field shadow flicker) has one zero-code experiment left** — `r_rtShadowSoftRadiusScale 0` picks between a cheap fix and a reversed-Z projection change. A2/A4/A6/A7 minor/latent. |
| `20260831_rt_temporal_cut_detection.md` | Camera-cut detection rewritten to test camera position/orientation instead of ill-conditioned matrix elements; also fixed the GUI/HUD overlay's degenerate second `RC_DRAW_VIEW` re-running AO/Refl/GI/Vol every frame. |
| `20260905_rt_projected_light_cookies.md` | Projected-light cookie/gobo textures in direct lighting, reflections, GI and volumetrics. All 4 stages, in-game validated. |
| `20260810_auto_relight.md` | Synthesized shadow-casting lights from emissive panels. |
| `20260808_gbuffer_normal_pass.md` | G-buffer normal/F0 prepass. |
| `20260816_portal_area_lights.md` | Stage 2 (transition blend) was shelved on the temporal bug, now unblocked — not currently scheduled. |
| `20260831_rt_parallel_sun_lights.md` | Sun/parallel lights in GI/vol/reflections. **Not pursuing.** |

---

## Backlog (not in the current arc, not dead)

| Item | Doc | Note |
|---|---|---|
| Tuning items T4-T6 | `rt_optimization_tuning.md` | Falloff-mode A/B, emissive floor, final constants pass. Stable base; polish. **T3 is dropped** — see decisions above. |
| ~~A12 far-field flicker~~ | `completed/20260826_amd_vulkan_cleanup.md` | ✅ **Fixed 2026-09-12, in-game validated.** Ray-origin bias was swamped by depth-reconstruction error (`d^2*ulp/znear`) past d~2739 in shadows and d~5000 in AO — worse in cut-scenes, where the game drops `r_znear` to 1.0. Shadow bias now floors at the error term; AO fades out, band scaled by `sqrt(znear/3)`. Linear-depth G-buffer **not needed** and deferred. |
| Projectiles in reflections | `completed/20260423_reflection_enhancements.md` AR3 | Sprite attempt reverted (`f37f071b`); needs a new approach. |
| Roughness-blurred reflections | — | Now the *only* route to reflective non-glass surfaces: sharp mirror reflection is why opaque geometry looks wrong, so "dimmer" can't fix it. Affordable for the first time now the traced pixel set is tiny. Not scheduled. |
| Runtime emissive-state lights | `completed/20260810_auto_relight.md` | v2 of auto-relight. |
| Translucent square borders over reflections | `completed/20260423_reflection_enhancements.md` | Polish. |
