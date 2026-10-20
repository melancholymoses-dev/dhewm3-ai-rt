# Cleanup and polish (Arc 8)

Delete dead code and debug scaffolding, then presets, resets and comments.
**No optimization work lives here** — that is all in `20261010_vk_core_audit.md`, including
the RT shadow dispatch that was this doc's Phase 6a. ROADMAP owns which arc runs when.

Each phase ends in a runnable build with a check. Do not start a phase until the previous
check passes. Cvar inventory and tiers: `../Dhewm3-AI-RT-Settings.md`.

**Phases are ordered by what they block, not by number** — see "Order and exit". Phase 1 is
split 1a/1b/1c/1d because its groups have different dependencies: deleting an instrument
before the work that needs it is the one mistake this arc can make cheaply and regret
expensively.

Scale as of 2026-10-18: **255 `r_rt*`/`r_vk*`/`r_fsr*` cvars, 116 menu widgets, 44 debug
modes across 8 `*Debug*` cvars.** Phase 1 assigns every cvar one of three verdicts —
**M** (menu), **C** (console, documented), **X** (deleted).

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

## Phase 1 — Cvar trim (three verdicts, four groups)

Rewritten 2026-10-18 after the reflection arcs (8d/8f/8g) landed. Every cvar gets one of
three verdicts; `../Dhewm3-AI-RT-Settings.md` is the authority on which cvar is which.

| Verdict | Means | Where |
|---|---|---|
| **M** | Menu widget. Player understands it and it changes the look or cost visibly | Dhewm3SettingsMenu |
| **C** | Console only, kept and documented. Real knob, rarely touched | Settings doc §2 |
| **X** | Deleted with its UBO field, shader branch, menu widget and CMake entry | nowhere |

Principle (rules doc, pillar 6): one overlay per feature stays. Gain / slice / radius /
dump / log-level cvars go **X**. Split by dependency — a diagnostic is only dead once the
work that would use it has shipped.

### 1a — unblocked clutter (run any time)

| X | Keep as C |
|---|---|
| `r_rtAODebugGain`, `r_rtGIProbeDebugGain/Radius/Dump`, `r_rtVolDebugGain/Dump`, `r_rtVolFroxelDebugSlice/Gain/Dump`, `r_rtGILightDump`, `r_rtTlasDump` | `r_rtAODebug`, `r_rtGIProbeDebug`, `r_rtVolFroxelDebug` (hardcode the gains) |
| `r_vkRTReflDataDiag`, `r_vkRTInterLogN`, `r_vkRTLogCaptureReset`, `r_vkRTDebugLightTextures`, `r_vkRTLightLogN`, `r_vkRTDebugLightFilter`, `r_vkRTDebugSurface`, `r_vkRTDebugMaxLogsPerFrame` | `r_vkRTProfile*`, `r_vkLogRT`, `r_vkLogSubmitInfo`, `r_vkLogTLASSlow`, `r_vkBLASPoolCheck` |
| `r_vkShadowFlipOps`, `r_vkShadowFullScissor`, `r_vkLightFullScissor`, `r_vkLogShadowBranch/Geom`, `r_vkVcCheck` | `r_vkSerializeFrames`, `r_vkSplitSubmitMask/Verbose`, `r_vkLowPerturbationMode` |

**Correction 2026-10-18:** the split-submit trio was wrongly listed as **X**. The three
answer different questions and none substitutes for another

`r_rtBloomDebug` moves to 1d (mode 1 is a general HDR-range overlay, not bloom-only).

### 1b — Shadow Cleanup (Done)

| X | Because |
|---|---|
| ~~Serial shadow path: `VK_RT_DispatchShadowRaysForLight`, `VK_RT_SHADOW_SERIAL_LAYER`, `r_rtShadowBatch`; batch width 7 → 8~~ **done 2026-10-13** | Stage S4. Check: `grep ShadowRaysForLight` clean, `shadow.breaks` = ceil(casters/8)−1, `lights.batched` pins at 8 |
| `r_rtShadowStablePattern`, `r_rtShadowVolumeCull`, `r_rtShadowMinSamples`, `r_rtShadowTemporalJitterMinSamples`, `r_rtShadowTemporalJitter` | Diagnostic switches whose defaults are the shipped behaviour. S2 replaced `r_rtShadowDistanceCutoff` with `r_rtShadowVolumeCull` (A/B switch); `r_rtShadowBackfaceCull` is **already gone** — delete its Settings-doc row and shadow debug mode 12 reference |
| `r_vkLogBufferGarbage` | Optimization E3 says use it before removing it |

`r_rtShadowDebugMode` stays (**C**), trimmed to 2 modes in 1d.

### 1c — Reflection Cleanup (Done)

8d, 8f and 8g Stages 1-4 all landed, so the reflection diagnostics are now judged against a
settled pipeline. 24 reflection/GUI cvars exist that predate the Settings doc — §2 of that
doc now lists them with verdicts. Summary:

| Verdict | Cvars |
|---|---|
| **M** | `r_rtReflectionBlend` (strength), `r_rtReflMaxLuminance` (ceiling), `r_rtSpecF0Scale`, `r_rtSpecF0Gamma`, `r_rtReflOpaqueHalfRes`, `r_rtGuiReflections` |
| **C** | `r_rtReflDenoise(+Alpha/AlphaGlass)`, `r_rtReflRoughness{Scale,Floor,Max}`, `r_rtReflRoughLumExp`, `r_rtReflShadowPicks`, `r_rtReflectionDistance`, `r_rtReflectionMinWeight`, `r_rtReflVol(+Gain/Steps)`, `r_rtReflGlow*`, `r_rtReflFlashGlow*`, `r_rtGlassNormalMode`, `r_rtGuiReflGain`, `r_rtGuiTexInterval/Radius`, `r_rtReflectionDebugMode` |
| **X** | `r_rtReflAmbientScale`, `r_rtReflGlowScale`, `r_rtReflGlowSmokeCell` |

**F0 stays and is now a menu knob** — `r_rtSpecF0Scale/Gamma` drive opaque reflections
(the arc's main win). The old "remove F0" plan is dead; see Phase 2.

### 1d — debug mode trim — **done 2026-10-18**

44 live modes across 8 cvars → **13 across 7**. Own section: [Phase 1d](#phase-1d--debug-mode-trim).

Check (each group): build, load Mars City, `cvarlist r_rt` and `r_vk` shrink; baseline
screenshots identical (image diff). Also drops the matching `DebugMode` branches in
`reflect_ray.rgen`, `gi_*.comp`, `vol_*.comp`.

---

## Phase 1d — Debug mode trim — **done 2026-10-18**

A debug mode earns its keep if it answers a question no other instrument answers **against
the current pipeline**. Modes built to settle an arc went once the arc's conclusion landed in
`../rt_engine_rules.md`. **Numbers stay stable** — retired modes are documented no-ops, so
plan docs and memories keep resolving.

**44 modes across 8 cvars → 13 across 7.**

| Cvar | Was | Kept | Retired | Why |
|---|---|---|---|---|
| `r_rtReflectionDebugMode` | 9 | 2, 3, 11, 12, 13, 15 | 4, 6, 14 | 4 = prepass coverage, settled. 6 = the B2/B3/B4 "dark subject vs weak interface" measurement, arc closed. 14 (à-trous branch) subsumed by 15 |
| `r_rtShadowDebugMode` | 9 | 9, 10, 11 | 1, 2, 3, 5, 7, 8 | 1/2/3/5 are bias-direction views from the A12 arc. 7/8 (aniso cone) shipped. 9 is the bias-vs-depth-error budget, the recurring failure. **11 stays:** 1b hardcoded the cull always-on (`vk_shadows.cpp:1057`), so 11 is the only way left to check it |
| `r_rtGIProbeDebug` | 8 | 2, 3 | 1, 4, 5, 6, 7, 8 | 2 is the cheap weight view, 3 the ray-traced ground truth it approximates. 1/4/5/6/7 shared a sphere-lattice renderer that **was already deleted** with `r_rtGIProbeDebugRadius` — all five rendered black. 8 (G5b buckets) shipped |
| `r_fsrDebug` | 7 | 3, 7 | 1, 2, 4, 5, 6 | 3 = reactive/T&C masks (both are live knobs), 7 = motion vectors (the recurring FSR bug class). 2's per-view logging moved to `r_vkLogRT 2`. 1/4/6 are U0/U3 A/B switches; 5 (FSR2 bleed) was a whole pass, deleted |
| `r_rtVolFroxelDebug` | 4 | 2, 3 | 1, 4 | 2 = per-cell light count (cost attribution), 3 = grid-mapping error (the infinite-far-plane discriminator). 1 had already lost its `…DebugSlice` cvar and rendered black; 4 was a one-off |
| `r_rtAODebug` | 3 | 1, 2, 3 | — | Three distinct views, all cheap, AO still being tuned |
| `r_rtBloomDebug` | 2 | 1 | 2 | 1 is an HDR-range band overlay useful well beyond bloom and reads the frame, not the chain. 2 (raw `bloomMip[0]`) was a whole pass, deleted |
| `r_rtVolDebugMode` | 2 | — | cvar gone | Went with the per-pixel march path |

**The validator is the load-bearing part.** Every one of the seven now reads through a
`VK_RT_Valid*DebugMode()` that returns 0 for a retired number. Without it a dead value still
satisfied the `!= 0` / `>= 2 && <= MAX` tests that mean "an overlay is active", and took the
perturbed path — replace-blend composite, suppressed shadow blur, forced
`r_rtReflectionMode 3`, a probe resolve dispatched with probes off — to draw an ordinary image.
`REFL_DEBUG_MAX_MODE` is gone; the validator replaced it.

Deleted outright, with shader + pipeline + push-constant struct + CMake entry: the probe
sphere-lattice tracer, `fsr2_bleed_debug.comp`, `bloom_debug.comp`. `fsr_rcas.comp` lost its
`debugMode` push constant entirely.

Check: each kept mode still renders its documented colours at Mars City; each retired number
renders the normal image, not garbage; `cvarlist *Debug*` help matches what the shaders do.

---

## Phase 2 — Reflection legacy mode (Done)

Unblocked after the specular refleciton work (2026-10-18). The old plan (remove F0,
reclaim its G-buffer alpha) assumed glass-only reflections. **Opaque reflections are now
default (`r_rtReflectionMode 3`) and F0 is the arc's win — it drives the trace cull, the
Fresnel weight, and 8g Stage 2's coarse G-buffer channel. F0, its remap and
`r_rtSpecF0Scale/Gamma` stay, and move into the menu.** Only legacy mode 2 is dead.

Run after 8g Stage 5, which owns `r_rtReflectionBlend` and `r_rtReflectionMinWeight`.

| Step | Detail |
|---|---|
| 2a. Remove mode 2 | `r_rtReflectionMode` range 1–3 → {1, 3}. Debug modes force `reflMode = 2` (`vk_reflections.cpp`, `ubo.reflMode`); force 3 instead — rough pixels going black in a debug view is acceptable. Drop mode 2 branches in `reflect_ray.rgen` and the cvar help text. `REFL_POP_BOTH` must survive: 8g 4b kept it precisely because every debug mode needs one image to land in |
| 2b. Keep | F0 in the G-buffer (coarse in `gbufNormal.b`, fine in `.a`), `GLASS_F0`, glass Fresnel, glow/emitter cvars |

Check: modes 1 and 3 screenshots identical; `grep -rn "reflMode == 2\|ReflectionMode 2" neo` empty.

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
| `r_rtQuality` (Low / Medium / High / Custom) | Writes the cvars in `Dhewm3-AI-RT-Settings.md` §3 — re-derive **after** 1c, 2, 3 and 4, since the GI/vol screen cvars and the F0 group are gone by then. Editing any Advanced widget flips the combo to Custom |
| Menu layout | Per `Dhewm3-AI-RT-Settings.md` §1 and §4; Advanced node collapsed |
| Reflection modes | If `20261013_reflection_roughness.md` landed, mode 3 belongs on High/Ultra only (20-40% frame cost) |

Files: `neo/framework/Dhewm3SettingsMenu.cpp`, `RenderSystem_init.cpp`.
Check: pick each preset, `cvarlist` shows expected values.

---

## Phases 6a and 6 — moved out 2026-10-13 - Completed

Both were optimization and now live in `20261010_vk_core_audit.md`:

| Was | Now |
|---|---|
| Phase 6a, RT shadow dispatch, steps S0-S7 | **Stage S**, closed 2026-10-13 — now in `completed/20261010_vk_core_audit_closed.md`. Code comments in `vk_raytracing.h:971` and `vk_shadows.cpp:1465` still say "Phase 6a"; both names mean Stage S |
| Phase 6 E1-E5, Vulkan efficiency | The A/B stages owned the fixes already; E2/E3/E4 are regression guards in the audit doc. E3 is now the `vid_restart` leak hunt |


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
| 0 Regression refs + FP on | none | — | Done |
| 1a Debug clutter trim | low | 0 | Done|
| 1b Shadow diagnostics | low | optimization Stage S | Closed 2026-10-13 |
| 1c Reflection cvar verdicts | low | reflection arc | **Unblocked 2026-10-18** — 8d/8f/8g 1-4 all landed |
| 1d Debug mode trim | low | 1a, 1b, 1c | Done |
| 2 Reflection legacy mode 2 | low-med | 1c, 8g Stage 5 | Unblocked except for Stage 5's knob fold |
| 3 Screen-space GI | med | 1a | Dead paths confirmed at 0.001 ms |
| 4 Non-froxel vol | med | 1a | same. Takes `r_rtVolDebugMode` with it |
| 5a Reset buttons | low | 0 | No cvar-set dependency; can land any time |
| 5b Presets | low | 1c, 1d, 2, 3, 4 (final cvar set) | |
| 7 Comments | none | all of the above | Last by design; `git diff -w` must show no code change |

Interleaving with other arcs is ROADMAP's job, not this table's. The one remaining external
gate is 8g Stage 5 (for Phase 2 and for the presets' reflection rows).

Exit: cvar count down ≥ 40% (255 → ~150), debug modes 44 → 18, menu widgets 116 → ~30,
`vk_gi.cpp` + `vk_vol.cpp` combined line count down ≥ 40%, GPU p50 **no worse** than the
baseline on all three scenarios (this arc targets no gain), all baseline screenshots
unchanged.
