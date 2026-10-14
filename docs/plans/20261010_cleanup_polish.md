# Cleanup and polish (Arc 8)

Delete dead code and debug scaffolding, then presets, resets and comments.
**No optimization work lives here** — that is all in `20261010_vk_core_audit.md`, including
the RT shadow dispatch that was this doc's Phase 6a. ROADMAP owns which arc runs when.

Each phase ends in a runnable build with a check. Do not start a phase until the previous
check passes. Cvar inventory and tiers: `../Dhewm3-AI-RT-Settings.md`.

**Phases are ordered by what they block, not by number** — see "Order and exit". Phase 1 is
split 1a/1b/1c because its three groups have different dependencies: deleting an instrument
before the work that needs it is the one mistake this arc can make cheaply and regret
expensively.

**Rules for every phase**

| Rule | Check |
|---|---|
| Delete cvar + UBO field + shader code + menu widget + CMake entry together | `grep -r <cvar>` returns nothing |
| Shader UBO layouts change with the C++ struct; update both sides in one commit | validation layers clean (`vulkan_debugging.md`) |
| `neo/game` and `neo/d3xp` mirrored | diff of the two touched files shows the same hunk |
| Rewrite any comment block >3 lines in a file you touch | see Phase 7 |
| Old `config` files with removed cvars must still load | start with an old `dhewm.cfg`, no console errors |

---

## Phase 0 — Regression references + first-person default on

Perf baseline (scenarios, CSVs, GPU p50, the `com_fixedTic 1` recipe) belongs to
`20261010_vk_core_audit.md` and `notes/20261010_vk_core_baseline.txt`. **Measured
2026-10-12.** What this arc needs from it: every deletion phase must diff against those CSVs
with `--baseline`, and nothing here may move a number.

Deletion is judged on images, not milliseconds, so the screenshots are this arc's real gate.

| Outstanding | Files |
|---|---|
| Screenshots: 6 fixed spots (glass, volumetric hall, flashlight, dark room, mirror, FP body) | `notes/baseline_*.png` |
| First-person body default on | `neo/game`, `neo/d3xp` (mirror both) |

Check: new game starts with body visible; the 6 screenshots exist and are reproducible from
recorded positions (`getviewpos`).

---

## Phase 1 — Debug mode and cvar trim (three groups)

Principle (rules doc, pillar 6): one overlay per feature stays. Gain / slice / radius /
dump / log-level cvars go. Split by dependency — a diagnostic is only dead once the work that would use it has
shipped.

### 1a — unblocked clutter (run any time)

| Remove | Keep |
|---|---|
| `r_rtAODebugGain`, `r_rtGIProbeDebugGain/Radius/Dump`, `r_rtVolDebugGain/Dump`, `r_rtVolFroxelDebugSlice/Gain/Dump`, `r_rtGILightDump`, `r_rtTlasDump`, `r_rtBloomDebug` | `r_rtAODebug`, `r_rtGIProbeDebug`, `r_rtVolDebugMode`, `r_rtVolFroxelDebug` (hardcode gains) |
| `r_fsrDebug`, `r_vkRTReflDataDiag`, `r_vkRTInterLogN`, `r_vkRTLogCaptureReset`, `r_vkRTDebugLightTextures`, `r_vkRTLightLogN`, `r_vkRTDebugLightFilter`, `r_vkRTDebugSurface`, `r_vkRTDebugMaxLogsPerFrame` | `r_vkRTProfile`, `r_vkLogRT`, `r_vkLogSubmitInfo` |
| `r_vkShadowFlipOps`, `r_vkShadowFullScissor`, `r_vkLightFullScissor`, `r_vkSplitSubmitMask/Verbose`, `r_vkLowPerturbationMode`, `r_vkLogShadowBranch/Geom`, `r_vkVcCheck` | `r_vkSerializeFrames` (cheap stability bisect) |

### 1b — after optimization Stage S closes (closed 2026-10-13)

| Remove | Because |
|---|---|
| Serial shadow path: `VK_RT_DispatchShadowRaysForLight`, `VK_RT_SHADOW_SERIAL_LAYER`, `r_rtShadowBatch`; batch width 7 → 8 | Stage S4, moved here. Only `r_rtShadowBatch 0` uses it since S3. Check: `grep ShadowRaysForLight` clean, `shadow.breaks` = ceil(casters/8)−1 |
| `r_rtShadowStablePattern`, `r_rtShadowVolumeCull`, `r_rtShadowMinSamples`, `r_rtShadowTemporalJitterMinSamples` | Diagnostic switches whose defaults are the shipped behaviour — but they are the knobs Stage S tunes against. S2 replaced `r_rtShadowDistanceCutoff` with `r_rtShadowVolumeCull` (A/B switch). `r_rtShadowDebugMode` stays |
| `r_vkLogBufferGarbage` | Optimization E3 says use it before removing it |

### 1c — after the reflection arc closes

| Remove | Because |
|---|---|
| `r_rtReflectionDebugMode` modes 2-8 | The instrument for the glass fix and the roughness work, which *add* modes 9-13. Delete-then-add is churn |
| F0 remap cvars | See Phase 2 — blocked on `20261013_reflection_roughness.md` |

Check (each group): build, load Mars City, `cvarlist r_rt` and `r_vk` shrink; baseline
screenshots identical (image diff). Also drops the matching `DebugMode` branches in
`reflect_ray.rgen`, `gi_*.comp`, `vol_*.comp`.

---

## Phase 2 — Reflection path (Fresnel / F0 / legacy mode) — **BLOCKED**

**Do not run 2b or 2c until `20261013_reflection_roughness.md` lands or is abandoned.**

F0 is reflectance at normal incidence; roughness is lobe width. GGX needs both, so a
roughness path reinstates F0 rather than replacing it — per-material F0 was dropped only
because sharp mirrors made classification pointless, which is the condition roughness
removes. Step **2c is the irreversible one**: it reclaims the G-buffer alpha that holds F0,
and the roughness plan's Stage 2 needs that byte budget. 2b is cheap to redo; 2c is not.

Current state: reflections are glass-only, per-material F0 dropped. F0 is still written to the G-buffer alpha and read in `reflect_ray.rgen`
(`f0 < 1/255` early-out at ~line 263).

| Step | Detail |
|---|---|
| 2a. Prove | Log counts of pixels where `f0 >= 1/255` and no glass probe hit, in mode 1. If 0 reflection rays trace, F0 is dead |
| 2b. Remove | `r_rtReflectionMode` (2 = legacy), `r_rtReflectionMinWeight`, `r_rtSpecF0Scale/Gamma`, `r_rtSpecGrazingMax/Gain`, `r_rtReflectionBlend` if unused for glass; F0 remap in `gbuffer.frag`, `gbuffer_clip.frag`, `interaction.frag`; UBO fields in `vk_backend.cpp:1422/3036`, `vk_pipeline.cpp:58`, `vk_reflections.cpp:113-143` |
| 2c. Reclaim | G-buffer alpha channel (F0) freed; decide reuse (e.g. material class) or shrink the format |
| 2d. Keep | `GLASS_F0` constant, glass Fresnel, glow/emitter cvars |

Check: glass, mirror and breakable-glass screenshots identical; reflection GPU ms ≤ baseline; `grep -r SpecF0 neo` empty.

---

## Phase 3 — Remove screen-space GI

Probes own GI (`VK_RT_GIProbeActive()`): `vk_temporal.cpp:1261` and the A-trous at `vk_gi.cpp:3401` already return early when probes are on.

| Step | Detail |
|---|---|
| 3a. Map | List what `vk_gi.cpp` (3840 lines) provides to probes (light collection, area walk, stratify, upload) vs screen-space only (per-pixel bounce, checkerboard, A-trous, GI temporal) |
| 3b. Split | Move shared light selection into `vk_gi_lights.cpp`; add to `CMakeLists.txt` |
| 3c. Delete | `r_rtGISamples`, `r_rtGICheckerboard`, `r_rtGITemporal(+Alpha)`, `r_rtGIAtrous*`, `r_rtGIRadius` (if screen-only), `r_rtGIStochasticLights`, `r_rtGIProbes` toggle (probes always on), shaders for per-pixel GI + GI denoise, GI parts of `VK_RT_Temporal*` |
| 3d. Keep | Probe resolve, `r_rtGIStrength/Contrast/FalloffMode/Reach`, AO temporal (separate path) |

Check: GI screenshots identical (probes already active); GI GPU ms ≤ baseline; GPU memory drops by the GI history/denoise images (log VRAM before/after).

---

## Phase 4 — Remove non-froxel volumetrics

Froxel is default (`r_rtVolFroxel 1`, `vk_vol_froxel.cpp:1023`). `vk_vol.cpp` is 2237 lines and may still hold shared composite / upsample code.

| Step | Detail |
|---|---|
| 4a. Map | Which `vk_vol.cpp` functions the froxel path calls (composite, upsample, light list) |
| 4b. Delete | per-pixel ray-march pipeline + shader, `r_rtVolFroxel` toggle, `r_rtVolSamples`, `r_rtVolHalfRes`, `r_rtVolUpsampleDepthSigma`, `r_rtVolBilateral*`, `r_rtVolWhiteNoiseMix` (verify each is march-only) |
| 4c. Rename | Remaining froxel + composite code into `vk_vol.cpp`; drop `vk_vol_froxel.cpp` name split if one file is clearer |

Check: volumetric hall + flashlight screenshots identical; vol GPU ms ≤ baseline; shader list in `CMakeLists.txt` matches files in `glsl/`.

---

## Phase 5a — Reset buttons (no cvar-set dependency)

| Item | Detail |
|---|---|
| "Reset ray tracing to defaults" | Loop over a static list of RT cvars, set each to its default string. Needs a default accessor: check `idCVar`/`idCVarSystem` for one, else store defaults in the menu table |
| "Reset graphics to defaults" | Same for the non-RT graphics group |
| Persistence | Add `CVAR_ARCHIVE` to cvars that stay in the menu without it |

Iterating a list of cvars does not care which cvars exist, so this does not wait on 1c/2/3/4.

Check: Reset returns every RT cvar to default (diff against a fresh config); quit and
relaunch keeps settings.

---

## Phase 5b — Presets (needs the final cvar set)

Replace ~85 sliders with a preset and a small Advanced node; the player needs "make it
faster / prettier", not `r_rtGIProbeHysteresis`.

| Item | Detail |
|---|---|
| `r_rtQuality` (Low / Medium / High / Ultra / Custom) | Writes the cvars in `Dhewm3-AI-RT-Settings.md` §3 — re-derive **after** 1c, 2, 3 and 4, since the GI/vol screen cvars and the F0 group are gone by then. Editing any Advanced widget flips the combo to Custom |
| Menu layout | Per `Dhewm3-AI-RT-Settings.md` §1 and §4; Advanced node collapsed |
| Reflection modes | If `20261013_reflection_roughness.md` landed, mode 3 belongs on High/Ultra only (20-40% frame cost) |

Files: `neo/framework/Dhewm3SettingsMenu.cpp`, `RenderSystem_init.cpp`.
Check: pick each preset, `cvarlist` shows expected values.

---

## Phases 6a and 6 — moved out 2026-10-13

Both were optimization and now live in `20261010_vk_core_audit.md`:

| Was | Now |
|---|---|
| Phase 6a, RT shadow dispatch, steps S0-S7 | Part 3, **Stage S** (S0/S1 since dropped; S2-S7 remain). Code comments in `vk_raytracing.h:971` and `vk_shadows.cpp:1465` still say "Phase 6a" — both names mean Stage S |
| Phase 6 E1-E5, Vulkan efficiency | Stages A0-A5 / B0-B4 owned the fixes already; the E2/E3/E4 audits moved to that doc's order table as regression guards |

Nothing was dropped. This section is a forwarding note; delete it once no reader is looking
for the old numbers.

---

## Phase 7 — Comment sweep

Policy (`.claude/rules/RULES.md`): inline comments 1-2 sentences of rationale; long motivation goes to file-top or function header, or to memory/docs.

| Step | Detail |
|---|---|
| 7a. Find | Script: runs of ≥6 consecutive `//` lines and `/* */` blocks >8 lines, per file. Current counts of such runs: `vk_gi.cpp` 268, `vk_backend.cpp` 139, `vk_reflections.cpp` 80, `vk_gi_probe.cpp` 76, `vk_auto_relight.cpp` 69, `vk_vol.cpp` 67, `vk_vol_froxel.cpp` 62, `vk_shadows.cpp` 57 (counts are lines inside runs, pre-cleanup) |
| 7b. Triage | Delete: plan/stage references to completed docs ("Stage 3.5", "R2", "G5b"), history ("was X before"), restated code. Move: durable design reasoning to `docs/plans/notes/` or a memory file. Keep: non-obvious invariants, one or two sentences |
| 7c. Cvar help strings | Cut to one sentence; drop plan-doc references |
| 7d. Shaders | Same rule for `.glsl/.rgen/.comp` header blocks (`reflect_ray.rgen` top comment) |

Do it one file per commit, comments only, so `git diff -w` shows no code change.
Check: build output byte-identical for touched `.cpp` files if possible (compare object hashes) or at minimum a clean build and one smoke run; run counts from 7a again, target <10 per file.

---

## Order and exit

Reordered 2026-10-13. Two principles, both from measurement rather than taste:

- **Deletion buys no frame time.** `gpu.GI`, `GITemporal`, `Vol`, `VolTemporal` and
  `VolBilateral` all read **0.001 ms**. Phases 3 and 4 are the largest surgery in the arc
  (`vk_gi.cpp` 3840 lines, `vk_vol.cpp` 2237) at med risk for zero measured gain, so they
  run after the work that does gain something.
- **Never delete an instrument before the work that uses it.** Hence 1a/1b/1c.

| Phase | Risk | Depends on | Status |
|---|---|---|---|
| 0 Regression refs + FP on | none | — | Screenshots and FP default outstanding |
| 1a Debug clutter trim | low | 0 | Blocks nothing. **Runs first in this arc** |
| 1b Shadow diagnostics | low | optimization Stage S | |
| 5a Reset buttons | low | 0 | No cvar-set dependency; can land any time |
| 1c Reflection debug + F0 | low | reflection arc | |
| 2 Reflection F0 path | low-med | 1c | **Blocked** — see the phase note |
| 3 Screen-space GI | med | 1a | Dead paths confirmed at 0.001 ms |
| 4 Non-froxel vol | med | 1a | same |
| 5b Presets | low | 1c, 2, 3, 4 (final cvar set) | |
| 7 Comments | none | all of the above | Last by design; `git diff -w` must show no code change |

Interleaving with other arcs is ROADMAP's job, not this table's. The two external gates are
optimization Stage S (for 1b) and the reflection arc (for 1c and 2).

Exit: cvar count down ≥ 40%, `vk_gi.cpp` + `vk_vol.cpp` combined line count down ≥ 40%,
GPU p50 **no worse** than the baseline on all three scenarios (this arc targets no gain),
all baseline screenshots unchanged.
