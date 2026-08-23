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
| `rt_optimization_tuning.md` | Perf items P1-P10, light-list L1, tuning items T1-T6, profiler checkpoints | Live (Waves 2-4, 6) |
| `auto_relight.md` | Synthesized shadow-casting lights from emissive panels; noShadows unlock; zombie-vs-LED-wall shot | Live (Wave 5) |
| `portal_area_lights.md` | Topology-aware GI/vol light gathering via id's portal-area lightRefs; per-room curation sidecar | Live (Wave 5, alongside §0) |
| `gi_albedo_target.md` | Receiver-albedo G-buffer target + `gi × albedo` composite — fixes the "noticeable GI looks worse" wash (bodies underlit yellow, 2026-08-16) | Designed (Wave 5 push) |

Completed / superseded docs are in `completed/` — notably
`completed/gbuffer_normal_pass.md` (Waves 1a/1b landed: normal/F0 G-buffer in the
depth prepass, reflection composite; `gi_albedo_target.md` extends its G-buffer
contract) and `completed/lighting_shadows_refinement.md` (Phase 9 record:
shape-aware soft shadows and Fresnel normalization, both landed; its Stages
3/3.5/4a/4b were redesigned into the live docs above).

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
  P9   AO/GI rgen read G-buffer normals (G-buffer commit 3)
       ✅ implemented 2026-08-15 (r_rtGbufNormals, default 1)

Wave 4 — Structural perf                          [rt_optimization_tuning.md]
  P1b  batched shadow masks (4 lights/RGBA8 or texture array; zero render-pass
       breaks in the interaction loop)  ← enabler for Wave 5's extra lights
       ✅ implemented 2026-08-15 (R8 array, 7 batched layers + 1 serial;
          r_rtShadowBatch, default 1).  Untested; profiler capture still owed.
  P8   half-res volumetric march + bilateral upsample
       ✅ implemented 2026-08-15 (r_rtVolHalfRes, default 1; march + temporal EMA
          at half res, joint bilateral upsample resolves to full).  Untested.
       ✅ IGN dither striping fixed 2026-08-22 (rt_optimization_tuning.md P8
          follow-up): march-step jitter blends a small amount of white noise back
          into the IGN pattern (`r_rtVolWhiteNoiseMix`, tuned in-game to 0.025 —
          0.25 was too strong, caused visible flickering) to break up the IGN
          grid's diagonal striping without reintroducing the beam-decorrelation
          problem IGN was originally added to fix. Tested in-game, working.

Wave 5 — Auto-relight                             [auto_relight.md]
  §0   shared light classifier (REAL/ACCENT/AMBIENT_FILL/FOG_BLEND) + layered GI/vol
       admission + saturation-weighted importance + r_rtGILightDump.  Lands FIRST —
       no dependency on synthesis; fixes the live "uniform white GI lift" bug (the
       GI/vol collector filters entity noShadows only, so ambient washes are IN and
       colored accents are OUT — inverted from intent; see auto_relight.md §0).
       ✅ implemented 2026-08-15; validated in-game 2026-08-15/16 via dump.
  AREA portal-area light gathering                 [portal_area_lights.md]
       Replace the camera-sphere light cull with a walk of id's live per-area
       lightRefs (BFS over portals, door-aware).  Fixes "big room goes dark" and
       through-wall slot waste observed 2026-08-16; Stage 3 adds the per-room
       pin/ban curation sidecar.  Independent of synthesis; feeds §0 + L1 unchanged.
       ✅ Stage 1 implemented 2026-08-16; doorway validation passed in-game
          (closed door blocks far room's lights, open admits them).
       ✅ Stage 1.5 implemented 2026-08-19 (view-flood union — fixes stationary
          "looking across the room at an out-of-hop-range light" pop, confirmed
          via dump comparison to be a hop-boundary cliff, not an adjacency bug),
          untested in-game. Stage 2 (transition blend) built then reverted —
          depended on a still-broken GI/Vol temporal camera-cut fix and was
          superseded in priority by Stage 1.5's broader coverage.
       ✅ Stage 1.75 implemented 2026-08-19 (BFS seeds from every area touching a
          box around the camera via BoundsInAreas, not the single hard
          PointInArea pick — fixes hop numbers flipping wholesale when standing
          at a threshold/boundary and stepping slightly left/right while facing
          straight at the dividing wall).
       ✅ In-game dump validation 2026-08-22: Stage 1.75 confirmed working as designed
          (admission set stable across a real hallway threshold); Stage 1.5 confirmed
          actually firing at a zig-zag corridor (view-flood union nearly doubling the
          candidate pool when a hub area becomes visible down the hall). Root cause of
          the reported volumetric on/off pop wasn't either stage directly — it was
          `vol_march.comp` sharing GI's light buffer/ranking, so a Stage-1.5-admitted
          light that can never appear in fog (too far for `r_rtVolMaxDist`) could evict
          a genuinely nearby, march-relevant one purely by winning a shared slot.
       ✅ Vol-specific light selection implemented 2026-08-22 (portal_area_lights.md):
          volumetrics now reads a dedicated, distance-filtered selection (new
          `vkRT.volLightSsbo`) built from the same admitted-candidate pool as GI, not
          a raw prefix of GI's own buffer. GI/reflections' own selection is untouched.
          `r_rtVolMaxLights` 32→96 along the way (interim mitigation, now mostly
          redundant). **Validated in-game 2026-08-22 — pop confirmed fixed.**
          Portal-area gathering (Stages 1/1.5/1.75) + dedicated vol selection now
          considered closed. Stage 3 (per-room pin/ban curation sidecar) not
          started, not currently blocking anything.
  ALB  GI receiver-albedo modulation               [gi_albedo_target.md]
       gbufAlbedo target in the G-buffer prepass + a gi_albedo_mod.comp pass
       (post à-trous) multiplying denoised GI by it before composite.
       Fixes the "noticeable GI looks worse" wash (bodies underlit yellow,
       2026-08-16) — serves pillar 2 directly.  Do BEFORE §1-5: synthesized
       panel lights would otherwise amplify the wash, and GI constants retune
       after this lands anyway.
       ✅ implemented 2026-08-16, **validation step 2 passed in-game 2026-08-22**
          (bodies/corpses now read as dark cloth with a subtle tint, not a color
          wash — the original motivating bug is fixed). Steps 3/4 surfaced a new
          finding rather than closing clean: with volumetrics now contributing too,
          overall scene luminance is elevated and blacks are lifted (pillar 2,
          "darkness stays black," not yet holding) — **currently being addressed via
          tonemapping retune** (`completed/tonemapping.md`/`tonemapping2.md` cover the
          existing pipeline; this is tuning within it, not new architecture, unless
          that changes). `r_rtGIBounceScale`/`r_rtGIStrength` retune (step 4) is
          entangled with the tonemapping work — don't retune GI in isolation until
          tonemapping settles, or the two will chase each other.
  §1-5 Synthesized real lights from emissive panels (cluster → score → dedupe →
       AddLightDef), §6 noShadows unlock rule, §7 weapon/projectile def patches.
  Delivers: panels casting shadowed light, zombie silhouettes in volumetric glow.
  (§1-5 can be tried earlier with r_rtAutoRelightMax 6 if impatient — P1b makes it cheap.)

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
| 3 | P3 running; P9 implemented, untested (2026-08-15). Remaining: raise r_rtGISamples | rt_perf2_b working tree | no |
| 4 | P1b + P8 implemented (2026-08-15); P1b runs, P8 untested | rt_perf2_b working tree | no — new `Shadows` / `VolBilateral` phases exist for it |
| 5 | §0 implemented + validated in-game via r_rtGILightDump (2026-08-15/16); GI contrast formula fixed (gi_ray.rgen, energy-neutral extrapolation); portal-area gathering Stage 1 implemented + doorway validation passed in-game (2026-08-16); Stage 1.5 + 1.75 implemented 2026-08-19, **in-game dump validation 2026-08-22** — 1.75 confirmed working as designed, 1.5 confirmed firing at a zig-zag corridor and identified (not itself, but via shared-buffer slot competition) as the cause of a reported volumetric on/off pop; Stage 2 built then reverted (blocked on GI/Vol temporal cut-detection bug, superseded by Stage 1.5); Stage 3 not started, not blocking; **volumetrics given its own dedicated distance-filtered light selection 2026-08-22** (fixes the pop's actual mechanism — separate SSBO, GI's own selection untouched), **validated in-game 2026-08-22 — pop confirmed fixed**; **IGN dither striping fixed + tested in-game 2026-08-22** (`r_rtVolWhiteNoiseMix`, tuned to 0.025); GI albedo modulation implemented 2026-08-16, **partially validated 2026-08-22** — corpses/bodies confirmed reading correctly (was the original motivating bug), but surfaced a new open item: volumetric contribution is raising overall scene luminance and lifting blacks, tonemapping being retuned to compensate (pillar 2 compliance not yet fully confirmed — see ALB row above); `r_rtGIAutoDirectScale` added 2026-08-19 (GI/direct-light scale coupling, tuning UX); GI/Vol temporal camera-cut detection reverted to the original raw-matrix-diff metric (still likely false-positives on ordinary camera motion — unfixed); §1-7 not started, **in scope for this release per 2026-08-22 direction (finish all current plans, nothing deferred)** | auto-relight working tree | no |
| 6 | not started | | |
