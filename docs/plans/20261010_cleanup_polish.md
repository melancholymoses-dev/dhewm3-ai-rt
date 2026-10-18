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
| `r_vkShadowFlipOps`, `r_vkShadowFullScissor`, `r_vkLightFullScissor`, `r_vkSplitSubmitMask/Verbose`, `r_vkLowPerturbationMode`, `r_vkLogShadowBranch/Geom`, `r_vkVcCheck` | `r_vkSerializeFrames` (cheap stability bisect) |

`r_rtBloomDebug` moves to 1d (mode 1 is a general HDR-range overlay, not bloom-only).

### 1b — after optimization Stage S closes (closed 2026-10-13)

| X | Because |
|---|---|
| ~~Serial shadow path: `VK_RT_DispatchShadowRaysForLight`, `VK_RT_SHADOW_SERIAL_LAYER`, `r_rtShadowBatch`; batch width 7 → 8~~ **done 2026-10-13** | Stage S4. Check: `grep ShadowRaysForLight` clean, `shadow.breaks` = ceil(casters/8)−1, `lights.batched` pins at 8 |
| `r_rtShadowStablePattern`, `r_rtShadowVolumeCull`, `r_rtShadowMinSamples`, `r_rtShadowTemporalJitterMinSamples`, `r_rtShadowTemporalJitter` | Diagnostic switches whose defaults are the shipped behaviour. S2 replaced `r_rtShadowDistanceCutoff` with `r_rtShadowVolumeCull` (A/B switch); `r_rtShadowBackfaceCull` is **already gone** — delete its Settings-doc row and shadow debug mode 12 reference |
| `r_vkLogBufferGarbage` | Optimization E3 says use it before removing it |

`r_rtShadowDebugMode` stays (**C**), trimmed to 2 modes in 1d.

### 1c — reflection arc closed 2026-10-18 (unblocked)

8d, 8f and 8g Stages 1-4 all landed, so the reflection diagnostics are now judged against a
settled pipeline. 24 reflection/GUI cvars exist that predate the Settings doc — §2 of that
doc now lists them with verdicts. Summary:

| Verdict | Cvars |
|---|---|
| **M** | `r_rtReflectionBlend` (strength), `r_rtReflMaxLuminance` (ceiling), `r_rtSpecF0Scale`, `r_rtSpecF0Gamma`, `r_rtReflOpaqueHalfRes`, `r_rtGuiReflections` |
| **C** | `r_rtReflDenoise(+Alpha/AlphaGlass)`, `r_rtReflRoughness{Scale,Floor,Max}`, `r_rtReflRoughLumExp`, `r_rtReflShadowPicks`, `r_rtReflectionDistance`, `r_rtReflectionMinWeight`, `r_rtReflVol(+Gain/Steps)`, `r_rtReflGlow*`, `r_rtReflFlashGlow*`, `r_rtGlassNormalMode`, `r_rtGuiReflGain`, `r_rtGuiTexInterval/Radius`, `r_rtReflectionDebugMode` |
| **X** | `r_rtReflAtrousSigmaPlane`, `r_rtReflAtrousNormalPower`, `r_rtReflAmbientScale`, `r_rtGlassCreaseRatio`, `r_rtReflGlowScale`, `r_rtReflGlowSmokeCell`, plus whatever 8g Stage 5 folds together |

**F0 stays and is now a menu knob** — `r_rtSpecF0Scale/Gamma` drive opaque reflections
(the arc's main win). The old "remove F0" plan is dead; see Phase 2.

8g Stage 5 (knob consolidation) still owns folding `r_rtReflAtrousIterations` +
`r_rtReflAtrousRoughnessGain` into one wash scale. Do that before writing the presets.

### 1d — debug mode trim

44 live modes across 8 cvars; target 18. Own section: [Phase 1d](#phase-1d--debug-mode-trim).

Check (each group): build, load Mars City, `cvarlist r_rt` and `r_vk` shrink; baseline
screenshots identical (image diff). Also drops the matching `DebugMode` branches in
`reflect_ray.rgen`, `gi_*.comp`, `vol_*.comp`.

---

## Phase 1d — Debug mode trim

A debug mode earns its keep if it answers a question no other instrument answers **against
the current pipeline**. Modes built to settle an arc go when the arc's conclusion lands in
`../rt_engine_rules.md`. **Numbers stay stable** — retired modes become documented no-ops,
as `r_rtReflectionDebugMode` already does, so plan docs and memories keep resolving.

| Cvar | Live now | Keep | Retire | Why those |
|---|---|---|---|---|
| `r_rtReflectionDebugMode` | 2,3,4,6,11,12,13,14,15 (9) | 2,3,11,12,13,15 | 4, 6, 14 | 4 = prepass coverage, settled. 6 = the B2/B3/B4 "dark subject vs weak interface" measurement, arc closed. 14 (à-trous branch) is subsumed by 15 (edge-stop weight) |
| `r_rtShadowDebugMode` | 1,2,3,5,7,8,9,10,11 (9) | 9, 10 | 1,2,3,5,7,8,11 | 1/2/3/5 are bias-direction views from the A12 arc — settled. 7/8 (aniso cone) shipped. 9 is the bias-vs-depth-error budget, the recurring failure. 11 goes with `r_rtShadowVolumeCull` (1b) |
| `r_rtGIProbeDebug` | 1-8 (8) | 1, 3, 4 | 2, 5, 6, 7, 8 | 1 = is there GI here, 3 = leak detector, 4 = probe classification. 2 is subsumed by 3/4. 6/7 existed to validate 4's verdict; 8 (G5b buckets) shipped. **Move mode 5's "impossible second moment" magenta into mode 4** before deleting 5 — that check is an earned rule |
| `r_fsrDebug` | 1-7 (7) | 3, 7 | 1, 2, 4, 5, 6 | 3 = reactive/T&C mask overlay (both masks are live knobs), 7 = motion vectors (the recurring FSR bug class). 2 is verbose view logging → fold into `r_vkLogRT`. 1/4/5/6 are U0/U3 A-B switches from closed stages |
| `r_rtVolFroxelDebug` | 1-4 (4) | 1, 2, 3 | 4 | 1 = slice, 2 = per-cell light count (cost attribution), 3 = grid-mapping error (the infinite-far-plane discriminator). 4 (blocked cone hits) is a one-off |
| `r_rtAODebug` | 1-3 (3) | 1, 2, 3 | — | Three distinct views, all cheap, AO is still being tuned |
| `r_rtVolDebugMode` | 1,2 (2) | — | both | Goes with the per-pixel march path in Phase 4 |
| `r_rtBloomDebug` | 1,2 (2) | 1 | 2 | Mode 1 is an HDR-range band overlay useful well beyond bloom; rename it out of the bloom group. 2 (raw extract) is one-off |

Totals: 44 → **18**, cvars 8 → 7 (`r_rtVolDebugMode` goes entirely).

| Step | Detail |
|---|---|
| 1d-a | Shaders first: delete the retired branches in `reflect_ray.rgen`, `refl_atrous.comp`, `shadow_ray.rgen`, `gi_probe_resolve.comp`, `fsr_rcas.comp`, `vol_froxel_fill.comp`. Lower `REFL_DEBUG_MAX_MODE` only if 15 is ever retired — it is not |
| 1d-b | C++ clamps and the dispatch guards that mention specific modes (`vk_reflections.cpp:3161`, `vk_backend.cpp:1943`, `vk_vol_froxel.cpp:565/1095`) |
| 1d-c | Rewrite each cvar help string as a one-line-per-live-mode list, ending with "retired no-ops: N, N, N" |
| 1d-d | Probe mode 5 → 4 magenta merge (do before deleting 5) |

Check: each kept mode still renders its documented colours at Mars City; each retired number
renders the normal image, not garbage; `cvarlist *Debug*` help text matches what the shaders do.

---

## Phase 2 — Reflection legacy mode

Rewritten 2026-10-17, unblocked 2026-10-18 (8g Stages 1-4 landed). The old plan (remove F0,
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
| 0 Regression refs + FP on | none | — | Screenshots and FP default outstanding |
| 1a Debug clutter trim | low | 0 | Blocks nothing. **Runs first in this arc** |
| 1b Shadow diagnostics | low | optimization Stage S | Unblocked — Stage S closed 2026-10-13 |
| 5a Reset buttons | low | 0 | No cvar-set dependency; can land any time |
| 1c Reflection cvar verdicts | low | reflection arc | **Unblocked 2026-10-18** — 8d/8f/8g 1-4 all landed |
| 1d Debug mode trim | low | 1a, 1b, 1c | Shader branches, so it wants the cvar verdicts settled first |
| 2 Reflection legacy mode 2 | low-med | 1c, 8g Stage 5 | Unblocked except for Stage 5's knob fold |
| 3 Screen-space GI | med | 1a | Dead paths confirmed at 0.001 ms |
| 4 Non-froxel vol | med | 1a | same. Takes `r_rtVolDebugMode` with it |
| 5b Presets | low | 1c, 1d, 2, 3, 4 (final cvar set) | |
| 7 Comments | none | all of the above | Last by design; `git diff -w` must show no code change |

Interleaving with other arcs is ROADMAP's job, not this table's. The one remaining external
gate is 8g Stage 5 (for Phase 2 and for the presets' reflection rows).

Exit: cvar count down ≥ 40% (255 → ~150), debug modes 44 → 18, menu widgets 116 → ~30,
`vk_gi.cpp` + `vk_vol.cpp` combined line count down ≥ 40%, GPU p50 **no worse** than the
baseline on all three scenarios (this arc targets no gain), all baseline screenshots
unchanged.
