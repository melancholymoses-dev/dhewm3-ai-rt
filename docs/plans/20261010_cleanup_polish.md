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

## Phase 3 — Remove screen-space GI ✅ done (2026-10-20), not yet validated in-game

| Step | Detail |
|---|---|
| 3a/3c. Deleted | `gi_ray.rgen`, `gi_temporal_resolve.comp`, `gi_atrous.comp`; `r_rtGISamples`, `r_rtGICheckerboard`, `r_rtGITemporal(+Alpha)`, `r_rtGIAtrous*`, the `r_rtGIProbes` toggle; the GI halves of `VK_RT_Temporal*`/`VK_RT_Atrous*`; `GIParamsUBO.numSamples`/`checker`/`checkerPhase` |
| 3b. Not done | Shared light selection stays in `vk_gi.cpp`. Optional — track separately if the file is still unwieldy |
| 3d. Kept | Probe resolve, `r_rtGIStrength/Contrast/FalloffMode/Reach`, AO temporal + à-trous (`vk_temporal.cpp` is now AO-only), the albedo-mod pass and its `giScratchA/B` pair |

Struck from 3c: `r_rtGIRadius` sizes light collection and the TLAS radius, and
`r_rtGIStochasticLights` is read by `gi_ray.rchit`, which probe rays traverse. Neither was
screen-space-only.

Structural consequence: `gi_probe_trace.rgen` is now the GI pipeline's only raygen, so a
missing `gi_probe_trace.rgen.spv` disables GI outright instead of falling back, and the SBT
re-indexed to 4 groups / 6 records. `VK_RT_DispatchGI` survives as set-0 setup only — it
traces nothing, and the probe trace depends on it having run.

Check: GI screenshots identical; GI GPU ms ≤ baseline; `r_vkLogRT 1` shows `probe-rgen` in
the SBT line and no "scratch images not ready" warning; VRAM drops by the GI history and
one of the two denoise ping-pong pairs.

---

## Phase 4 — Remove non-froxel volumetrics ✅ done (2026-10-20), not yet validated in-game

| Step | Detail |
|---|---|
| 4a/4b. Deleted | `vol_march.comp`, `vol_bilateral.comp`; the march pipeline, the vol temporal EMA (`volHistory`, `volPrevCam*`, `volHistoryValid`) and the bilateral upsample (`volBlurred`); `VolParamsUBO`, `BilateralPC`, the `s_volMarchScale` machinery; `r_rtVolFroxel`, `r_rtVolSamples`, `r_rtVolHalfRes`, `r_rtVolUpsampleDepthSigma`, `r_rtVolBilateral(+Sigma)`, `r_rtVolTemporal(+Alpha)`; the `Vol`/`VolTemporal`/`VolBilateral` profile phases |
| 4c. Not done | The split stays. `vk_vol.cpp` is 711 lines (medium coefficients + `volBuffer` + composite) against `vk_vol_froxel.cpp`'s 1653; merging makes one 2.4k-line file, which is not clearer |
| 4d. Kept | `r_rtVolWhiteNoiseMix` — `vol_froxel_fill.comp` reads it, so it was never march-only. `r_rtVolMaxDist` likewise bounds the grid and the vol light selection |

Two consequences worth knowing:

- **`volReadView` has one claimant.** The froxel resolve is now the only writer of
  `volBuffer`, so `VK_RT_CompositeVolumetrics` gates on `VK_RT_VolFroxelActive()`. Without
  that, a frame where the grid stood down composites a black-cleared buffer through the
  attenuating pipeline (`dst = src.rgb + dst*src.a`) and blanks the screen.
- **The vol EMA was already dead.** It loaded `gi_temporal_resolve.comp.spv`, which Phase 3
  deleted, so it had been silently warning-and-disabling since then.

Check: volumetric hall + flashlight screenshots identical; vol GPU ms ≤ baseline (phases are
now `FroxelFill`/`FroxelIntegrate`/`FroxelResolve`/`VolComposite` only); shader list in
`CMakeLists.txt` matches files in `glsl/`.

---

## Phase 5a — Reset buttons (Done, pending check)

| Item | Landed |
|---|---|
| Default accessor | `idCVar::SetString(nullptr)` falls back to `resetString` and raises the modified flags. The console `reset` does not raise them, so it is not used |
| Lists | `FindTabCVar` in `InitRTOptionsMenu` adds each cvar to `rtTabCVars` or `upscalingTabCVars` as it is looked up. `r_useRayTracing` is excluded |
| Buttons | "Reset Ray Tracing to Defaults" (RT tab), "Reset Upscaling and Filtering to Defaults" (Upscaling tab), each behind a confirm popup |
| Persistence | `CVAR_ARCHIVE` added to the 29 menu cvars that lacked it (AO, temporal, refl distance/blend, GI, vol, auto-relight, probe hysteresis). Left unarchived: `r_rtAutoRelightDebugColor`, and the off-menu `r_rtGIEmissiveScale`, `r_rtGIStochasticLights`, `r_rtGIContrast` |
| Menu fixes | Dropped the dead "Temporal Denoising" checkbox (`r_rtDenoise` was deleted in b4e7694b). Added the missing lookups for `r_rtAutoRelightMax` and `r_rtAutoRelightIntensity`, whose sliders never drew |

Check: change several RT and Upscaling values, press Reset, then compare `cvarlist r_rt*` with a
fresh config. Change a GI slider, quit, relaunch, and confirm the value is still set.

---|---|
| "Reset ray tracing to defaults" | Loop over a static list of RT cvars, set each to its default string. Needs a default accessor: check `idCVar`/`idCVarSystem` for one, else store defaults in the menu table |
| "Reset graphics to defaults" | Same for the non-RT graphics group |
| Persistence | Add `CVAR_ARCHIVE` to cvars that stay in the menu without it |

Iterating a list of cvars does not care which cvars exist, so this does not wait on 1c/2/3/4.

Check: Reset returns every RT cvar to default (diff against a fresh config); quit and
relaunch keeps settings.

---

## Phase 5b — Presets (landed 2026-10-20, values not yet measured)

Three presets plus Custom, each tied to a frame-rate target. No `r_rtQuality` cvar: the
combo shows whichever preset the live values match, so console edits read as Custom.

| Preset | Target | Main levers |
|---|---|---|
| Low | 60 fps on an RTX 2070 | FSR 2 Balanced, `r_rtReflectionMode 1` (glass only), lowest ray counts |
| High | steady 60 fps at 1440p on the Fast machine | FSR 2 Quality, current RT defaults |
| Ultra | none, "for lol" | native resolution, full-res opaque reflections, highest ray counts |

Values live in `rtQualityRows` (`Dhewm3SettingsMenu.cpp`) and in `Dhewm3-AI-RT-Settings.md` §3.
Menu: Quality / Features / Look / Reflections on the RT tab, with everything else under a
collapsed Advanced node.

Check, per preset, using routes mchq and commfire with `r_vkRTProfileCSV`:

| Preset | Pass condition |
|---|---|
| Low | Fast machine at 1920x1080: `frame.backend` p95 ≤ ~5.5 ms. This stands in for a 2070, assuming the 2070 is about 3x slower on RT |
| High | Fast machine at 1440p: `frame.backend` p95 ≤ 16.6 ms on commfire, the worst route |
| Ultra | Runs without device loss, and the froxel grid allocates at 320x180x128 |

Not done here: the `pm_*` Player model group (§4), and reconciling the four eye-tuned
reflection defaults. Neither touches a preset row.

---|---|
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

## Phase 7 — Comment sweep (Done)

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
| 4 Non-froxel vol | med | 1a | Done 2026-10-20, not yet validated in-game |
| 5a Reset buttons | low | 0 | Landed 2026-10-20, pending in-game check |
| 5b Presets | low | 1c, 1d, 2, 3, 4 (final cvar set) | Landed 2026-10-20. No preset row touches a Phase 2/3 cvar. Values need measuring |
| 7 Comments | none | all of the above | Last by design; `git diff -w` must show no code change |

Interleaving with other arcs is ROADMAP's job, not this table's. The one remaining external
gate is 8g Stage 5 (for Phase 2 and for the presets' reflection rows).

Exit: cvar count down ≥ 40% (255 → ~150), debug modes 44 → 18, menu widgets 116 → ~30,
`vk_gi.cpp` + `vk_vol.cpp` combined line count down ≥ 40%, GPU p50 **no worse** than the
baseline on all three scenarios (this arc targets no gain), all baseline screenshots
unchanged.
