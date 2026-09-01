# RT Roadmap — 2026SepRoadmap

**Date:** 2026-08-10 (Waves 1-6 planned); **status reviewed 2026-08-31**
**This is the entry point.** If you (human or LLM) are wondering what to work on or
which plan doc is authoritative, start here. Detailed designs live in the live docs
below; this file owns the *ordering* and the *status*.
See completed/202608_ROADMAP.md for the priorplanning.  

- **Wave 6 tuning (T3-T6)** in `rt_optimization_tuning.md` was never started (no
  per-material F0, no falloff-mode A/B, no final constants pass recorded). Low
  urgency — the base it tunes is stable — but it's the one item from the *original*
  six waves that isn't actually done, despite the "mostly done" framing above.
- **`amd_vulkan_cleanup.md`** still has A12 (far-field shadow flicker) waiting on one
  more zero-code experiment (`r_rtShadowSoftRadiusScale 0`) to pick between a cheap
  fix and a reversed-Z projection change, and its own status header is stale — A2/A5
  landed in commit `398095ee` without the doc text being updated to say so.

When the next roadmap cycle starts (post-Wave-7), this file should be moved to
`completed/` and a fresh entry-point doc started at this same path.

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
| `rt_optimization_tuning.md` | Perf items P1-P10, light-list L1, tuning items T1-T6, profiler checkpoints | Waves 2-4 done; **Wave 6 (T3-T6) not started** |
| `completed/auto_relight.md` | Synthesized shadow-casting lights from emissive panels; noShadows unlock; zombie-vs-LED-wall shot | **Done** (Wave 5) — moved to `completed/` |
| `amd_vulkan_cleanup.md` | AMD-vs-NVIDIA RT correctness: SBT hit-region overrun, image init, dead guards, stale geometry VAs, `parm3`-as-timescale, far-field shadow flicker | Nearly done — A1/A3/A5/A8/A11 landed; **A12 has one experiment left** to pick its fix; A2/A4/A6/A7 minor/latent, not blocking |
| `rt_projected_light_cookies.md` | Projected-light material textures (fan blades, grates, window blinds) sampled in direct lighting + volumetrics — spec only, not started | **Wave 7** |
| `rt_parallel_sun_lights.md` | Admit parallel ("sun") lights to GI/vol/reflections — currently rejected outright in `considerLight` on a premise that turned out to be false; direct lighting/shadows already support them — spec only, not started | **Wave 7** |

`../vulkan_debugging.md` (one level up, not a plan) is the reference for actually
getting Vulkan validation/GPU-AV output out of this engine — layer settings file,
env vars, what each VUID class means, this project's own diagnostic cvars. Load it
before chasing any AMD-vs-NVIDIA or device-lost bug.



Wave 6 — Tuning pass                              [rt_optimization_tuning.md T3-T6]
  T3 per-material F0 (mirrors ~0.9)  ·  T4 RT-vs-raster falloff match
  T5 emissive floor fix  ·  T6 final constants (record values in the doc)
  ⬜ NOT STARTED (2026-08-31 review) — no per-material F0 field on VkMaterialEntry,
     no falloff-mode A/B, no final constants recorded. Lower urgency than Wave 7:
     the base it tunes is stable and this is optimization/polish, not a missing
     feature. Pick up whenever a tuning pass is wanted; nothing below depends on it.

Wave 7 — Remaining light coverage                 [rt_projected_light_cookies.md, new]
  This is the actual open arc as of 2026-08-31 — everything above has landed.
  COOKIE  Projected light cookie/gobo textures — the classic Doom 3 fan-blade-shadow-
          in-a-light-shaft effect is a rotating material texture on a spot light
          (`lights/fanlightgrate`), not geometry; our RT path already admits these
          lights but renders them as a smooth cone with the texture silently
          dropped. See `rt_projected_light_cookies.md` for the full spec — data
          needed (light projection planes, animated texture matrix, bindless image)
          already exists in the shared frontend and material table, this is wiring,
          not new math. ⬜ NOT STARTED.
  SUN     Parallel/directional ("sun") lights for GI/volumetrics/reflections —
          seen rarely from inside the Mars base (windows/skylights). Smaller than
          COOKIE, and smaller than originally scoped here: `considerLight` rejects
          them on a stated premise ("infinite, no volume boundary") that doesn't
          match the GL reference path — a Doom 3 parallel light is a normal
          box-bounded point light that fakes direction via a 100,000-unit-distant
          `globalLightOrigin`, and direct lighting/RT shadows already use that value
          today with no exclusion. The fix is closer to "stop rejecting them" than
          "build a new containment model" — see `rt_parallel_sun_lights.md` for the
          full trace through `tr_lightrun.cpp`/`vk_shadows.cpp`. ⬜ NOT STARTED.

After Wave 7: reassess against the pillars. Candidate next arc:
- **world-space caching** (`froxel_probe_gi.md`, designed 2026-08-23) — froxel-grid
  volumetrics + DDGI-style probe GI: move the expensive sampling out of screen
  space into cached world-space structures; big perf win, deletes most of the
  GI noise-fighting chain (and its open camera-cut bug) structurally.

## Backlog (not in the current arc, not dead)

| Item | Doc | Note |
|---|---|---|
| Bloom post-process | `bloom_plan.md` | Unimplemented; revisit after Wave 6 — tonemapped HDR pipeline it needs now exists |
| First-person player body | `see_first_person_player_model.md` | Orthogonal to lighting arc |
| Projectiles in reflections | `completed/reflection_enhancements.md` AR3 | Sprite attempt was reverted (f37f071b); needs a new approach |
| Translucent square borders over reflections | `completed/reflection_enhancements.md` (last section) | Polish |
| Roughness-blurred reflections | — | Explicitly out of scope until G-buffer + composite prove out; interim is T2's grazing clamp + F0 gating |
| Runtime emissive-state lights (scripted screens turning off) | `completed/auto_relight.md` limitations | v2 of auto-relight |
| Froxel volumetrics + probe GI | `froxel_probe_gi.md` | Post-Wave-7 arc; needs the owed profiler checkpoints first |

## Status tracking

Update this table as waves land (and move fully-finished docs to `completed/`).

| Wave | Status | Landed in commit(s) | Profiler checkpoint taken? |
| 6 | **not started** (2026-08-31 review confirms — no per-material F0 field exists on `VkMaterialEntry`) | | |
| 7 | **not started** — new arc as of 2026-08-31; see `rt_projected_light_cookies.md` (COOKIE) and this table's Wave 7 entry (SUN, not yet speced) | | |
