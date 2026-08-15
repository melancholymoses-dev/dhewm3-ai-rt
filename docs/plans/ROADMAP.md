# RT Roadmap — Master Plan

**Date:** 2026-08-10
**This is the entry point.** If you (human or LLM) are wondering what to work on or
which plan doc is authoritative, start here. Detailed designs live in the three live
docs below; this file owns the *ordering* and the *status*.

---

## Design pillars (the taste contract)

Decided after review of the shipped RT features against Doom 3's art direction.
Every stage below serves these; anything that fights them gets cut or demoted.

1. **Shadows are the feature.** Soft, shaped, from more lights (including dynamic
   weapon/projectile lights). This is where "dynamic lighting" actually lives.
2. **Darkness stays black.** GI may tint lit regions (color bleed, corner-spill from
   bright sources); it must never lift the noise floor of dark ones.
3. **Light the air sparingly.** Volumetrics from hero lights only — flashlight plus a
   budgeted handful of fixtures/panels.
4. **Reflections are set dressing.** Mirrors, glass, screens, the odd hero surface —
   gated by F0, never a global material property. The assets carry no PBR data;
   don't pretend they do.
5. **No map editing.** Everything is engine-side rules + budgets + debug overlays.
   Small def-file mods (weapon/projectile lights) are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.

## Live documents

| Doc | Owns | Status |
|---|---|---|
| `gbuffer_normal_pass.md` | Normal/F0 G-buffer in the depth prepass; reflection composite pass; re-enabling reflections | **Next up** (Wave 1) |
| `rt_optimization_tuning.md` | Perf items P1-P10, light-list L1, tuning items T1-T6, profiler checkpoints | Live (Waves 2-4, 6) |
| `auto_relight.md` | Synthesized shadow-casting lights from emissive panels; noShadows unlock; zombie-vs-LED-wall shot | Live (Wave 5) |

Completed / superseded docs are in `completed/` — notably
`completed/lighting_shadows_refinement.md` (Phase 9 record: shape-aware soft shadows
and Fresnel normalization, both landed; its Stages 3/3.5/4a/4b were redesigned into
the live docs above).

---

## Order of operations

Rationale in brief: the G-buffer is both the reflection-correctness fix *and* the
biggest reflection perf win; quick perf items buy the sample-count headroom that
kills GI noise (1→4 samples was a huge visual win — Wave 3 makes 8+ affordable);
invasive restructures come next; new lights (auto-relight) land on the fast base;
constants get tuned **last**, because Waves 1/3/5 each change what the constants mean.

```
Wave 1 — G-buffer + reflection composite         [gbuffer_normal_pass.md]
  1a  Commit 1: independentBlend, image, render pass/framebuffer/pipeline plumbing,
      prepass writes normal+F0, debug modes 2-4.  NO visible change; validate via overlays.
  1b  Commit 2: rgen reads G-buffer, F0 early-out, refl_composite pass, delete
      interaction.frag reflection block, re-enable reflections.
      → fixes: grate-mirror bug, N×-per-light reflection brightness, grazing blowout
        (T1/T2 land here), most of reflection GPU cost (P2).

Wave 2 — Perf quick wins (independent, small)     [rt_optimization_tuning.md]
  P7   TLAS PREFER_FAST_BUILD                  (~1 line)
  P6   sceneHasGlass probe skip                (~20 lines)
  P4   shadow-ray threshold in reflect rchit   (~5 lines)
  P1a  per-light blur skip for small rects
  L1   stratified light tiers + importance sort + hysteresis  (CPU only)
  P5   shared rt_light_eval.glsl               (prep for P3)

Wave 3 — GI quality-per-ray                       [rt_optimization_tuning.md]
  P3   stochastic light selection at bounce hits (1-2 lights, importance-weighted)
       ✅ implemented 2026-08-15 (r_rtGIStochasticLights, default 2)
  →    then RAISE r_rtGISamples with the freed budget — the noise-reduction payoff
       (deliberately deferred: validate P3 at samples=4 first)
  P9   AO/GI rgen read G-buffer normals (G-buffer commit 3)  ← deferred by request

Wave 4 — Structural perf                          [rt_optimization_tuning.md]
  P1b  batched shadow masks (4 lights/RGBA8 or texture array; zero render-pass
       breaks in the interaction loop)  ← enabler for Wave 5's extra lights
  P8   half-res volumetric march + bilateral upsample

Wave 5 — Auto-relight                             [auto_relight.md]
  Synthesized real lights from emissive panels (cluster → score → dedupe →
  AddLightDef), noShadows unlock rule, weapon/projectile def patches.
  Delivers: panels casting shadowed light, zombie silhouettes in volumetric glow.
  (Can be tried earlier with r_rtAutoRelightMax 6 if impatient — P1b makes it cheap.)

Wave 6 — Tuning pass                              [rt_optimization_tuning.md T3-T6]
  T3 per-material F0 (mirrors ~0.9)  ·  T4 RT-vs-raster falloff match
  T5 emissive floor fix  ·  T6 final constants (record values in the doc)
```

After Wave 6: reassess against the pillars. Candidate next arc: **sun/sky-light path**
(parallel lights are skipped everywhere today; directional sky radiance in miss
shaders; volumetric sun shafts) — valuable for Doom 3's Mars-surface areas and the
prerequisite tech for any future outdoor-heavy game (Quake 4 ambitions).

## Backlog (not in the current arc, not dead)

| Item | Doc | Note |
|---|---|---|
| Bloom post-process | `bloom_plan.md` | Unimplemented; revisit after Wave 6 — tonemapped HDR pipeline it needs now exists |
| First-person player body | `see_first_person_player_model.md` | Orthogonal to lighting arc |
| Projectiles in reflections | `completed/reflection_enhancements.md` §3 | Sprite attempt was reverted (f37f071b); needs a new approach |
| Translucent square borders over reflections | `completed/reflection_enhancements.md` (last section) | Polish |
| Roughness-blurred reflections | — | Explicitly out of scope until G-buffer + composite prove out; interim is T2's grazing clamp + F0 gating |
| Runtime emissive-state lights (scripted screens turning off) | `auto_relight.md` limitations | v2 of auto-relight |

## Status tracking

Update this table as waves land (and move fully-finished docs to `completed/`).

| Wave | Status | Landed in commit(s) | Profiler checkpoint taken? |
|---|---|---|---|
| 1a | landed | 811d6ca7, 29dac019, d4449803 (gbuffer branch) | no |
| 1b | landed | 928f1a8f, 7154481e (gbuffer branch) | no |
| 2 | implemented, uncommitted (2026-08-14) | gbuffer branch working tree | no |
| 3 | P3 implemented, untested (2026-08-15); r_rtGISamples raise + P9 pending | rt_perf2_b working tree | no |
| 4 | not started | | |
| 5 | not started | | |
| 6 | not started | | |
