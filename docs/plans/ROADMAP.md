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
4. **Reflections are set dressing.** Mirrors, glass, screens, the odd hero surface —
   gated by F0, never a global material property. The assets carry no PBR data;
   don't pretend they do.
5. **No map editing.** Engine-side rules + budgets + debug overlays. Small def-file
   mods are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.

---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 1 | **Reflection gating rework** — reflections cost ~17 rays/pixel across most of the screen to produce sub-1% radiance. Cull to glass/mirrors, make those actually look like reflections. Pillar 4 is currently violated in both directions. | `20260911_reflection_gating.md` | ⬜ Next |
| 2 | **Froxel volumetrics + probe GI** — move vol/GI sampling out of screen space into world-space caches; deletes most of the GI noise-fighting chain structurally. | `20260906_froxel_probe_gi.md` | ⬜ Queued behind #1 |

Reflection gating goes first: it's small, it subsumes the never-started tuning
item T3 (per-material F0), and it frees GPU budget that the froxel/probe arc will
want. It also needs a profiler checkpoint, which the froxel doc lists as its own
precondition — one measurement pass serves both.

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
| Tuning items T4-T6 | `rt_optimization_tuning.md` | Falloff-mode A/B, emissive floor, final constants pass. Stable base; polish. |
| A12 far-field shadow flicker | `completed/20260826_amd_vulkan_cleanup.md` | One experiment away from a decision. |
| Projectiles in reflections | `completed/20260423_reflection_enhancements.md` AR3 | Sprite attempt reverted (`f37f071b`); needs a new approach. |
| Roughness-blurred reflections | — | Reconsider *after* the gating rework — a small, high-F0 pixel set makes this affordable for the first time. |
| Runtime emissive-state lights | `completed/20260810_auto_relight.md` | v2 of auto-relight. |
| Translucent square borders over reflections | `completed/20260423_reflection_enhancements.md` | Polish. |
