# Cleanup and Optimization (Arc 8)

Trim dead features and debug scaffolding, add presets, then fix CPU/memory inefficiency in the Vulkan path.
Each phase ends in a runnable build with a check. Do not start a phase until the previous check passes.
Cvar inventory and tiers: `../Dhewm3-AI-RT-Settings.md`.

**Rules for every phase**

| Rule | Check |
|---|---|
| Delete cvar + UBO field + shader code + menu widget + CMake entry together | `grep -r <cvar>` returns nothing |
| Shader UBO layouts change with the C++ struct; update both sides in one commit | validation layers clean (`vulkan_debugging.md`) |
| `neo/game` and `neo/d3xp` mirrored | diff of the two touched files shows the same hunk |
| Rewrite any comment block >3 lines in a file you touch | see Phase 7 |
| Old `config` files with removed cvars must still load | start with an old `dhewm.cfg`, no console errors |

---

## Phase 0 — Baseline + first-person default on

Will do baselining at 1440p, unscaled / no upscaling, uncapped refresh rate.  

Capture CPU/GPU and location as well as breakdowns for CPU/GPU tasks.  

**Measured 2026-10-12. Numbers in `notes/20261010_vk_core_baseline.txt`.**

Recipe (`com_fixedTic 1` matters: Doom 3 otherwise caps at 62.5 fps, so GPU cost is invisible):

```
com_fixedTic 1 / r_vkRTProfileCSV <scenario>.csv / r_vkRTProfileWindow 60 / r_vkRTProfile 1
python tools/vk_profile_report.py <scenario>.csv
```

| Scenario | What it stresses | CSV | GPU p50 | fps |
|---|---|---|---|---|
| Mars City Walkthrough | Reflections, shadows, grates. Run the hall, look at mirrors and lights, firing pistol throughout | `mchq.csv` | 9.17 ms | 109 |
| Alpha Labs Firefight | Accelerator-room geometry, many enemies, then the stairway edge for many lights and shadows | `r2r.csv` | 11.63 ms | 86 |
| Communications Fire | Particles, lights, mirrors. Circle the fires and reflections, plasma rifle for reflected particles | `commfire.csv` | 14.94 ms | 67 |

All three are GPU-bound by 4-7x (CPU busy 2.0-2.4 ms). Top GPU costs are `Interactions`
and `Shadows`, which Phase 6a shows are the same thing — see below.

| Outstanding | Files |
|---|---|
| Recapture r2r and commfire for `lights.*` (counter fix landed after them) | same CSVs |
| Screenshots: 6 fixed spots (glass, volumetric hall, flashlight, dark room, mirror, FP body) | `notes/baseline_*.png` |
| First-person body default on | `neo/game`, `neo/d3xp` |

Check: new game starts with body visible. Every later phase diffs against these CSVs with
`--baseline`.

---

## Phase 1 — Debug mode and cvar trim

Principle (pillar 6): one overlay per feature stays. Gain / slice / radius / dump / log-level cvars go.

| Remove | Keep |
|---|---|
| `r_rtAODebugGain`, `r_rtGIProbeDebugGain/Radius/Dump`, `r_rtVolDebugGain/Dump`, `r_rtVolFroxelDebugSlice/Gain/Dump`, `r_rtGILightDump`, `r_rtTlasDump`, `r_rtBloomDebug` | `r_rtAODebug`, `r_rtGIProbeDebug`, `r_rtVolDebugMode`, `r_rtVolFroxelDebug`, `r_rtShadowDebugMode` (hardcode gains) |
| `r_fsrDebug`, `r_vkRTReflDataDiag`, `r_vkRTInterLogN`, `r_vkRTLogCaptureReset`, `r_vkRTDebugLightTextures`, `r_vkRTLightLogN`, `r_vkRTDebugLightFilter`, `r_vkRTDebugSurface`, `r_vkRTDebugMaxLogsPerFrame` | `r_vkRTProfile`, `r_vkLogRT`, `r_vkLogSubmitInfo` |
| `r_vkShadowFlipOps`, `r_vkShadowFullScissor`, `r_vkLightFullScissor`, `r_vkSplitSubmitMask/Verbose`, `r_vkLowPerturbationMode`, `r_vkLogShadowBranch/Geom`, `r_vkVcCheck` | `r_vkSerializeFrames` (cheap stability bisect) |
| `r_rtReflectionDebugMode` modes 2-8 (see Phase 2) | mode 1 only if F0 survives |
| `r_rtShadowStablePattern`, `r_rtShadowDistanceCutoff`, `r_rtShadowMinSamples`, `r_rtShadowTemporalJitterMinSamples` (diagnostic switches, defaults are the shipped behaviour) | |

Check: build, load Mars City, `cvarlist r_rt` and `r_vk` shrink by ~45; baseline screenshots identical (image diff).
Also drops the matching `DebugMode` branches in `reflect_ray.rgen`, `gi_*.comp`, `vol_*.comp`.

---

## Phase 2 — Reflection path (Fresnel / F0 / legacy mode)

Roadmap standing decision: reflections are glass-only, per-material F0 dropped. F0 is still written to the G-buffer alpha and read in `reflect_ray.rgen` (`f0 < 1/255` early-out at ~line 263).

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

## Phase 5 — Presets and reset buttons

Yes to both. Replace ~85 sliders with a preset and a small Advanced node; the player needs "make it faster / prettier", not `r_rtGIProbeHysteresis`.

| Item | Detail |
|---|---|
| `r_rtQuality` (Low / Medium / High / Ultra / Custom) | Writes the cvars in `Dhewm3-AI-RT-Settings.md` §3 (re-derive after Phases 3-4: GI/vol screen cvars are gone). Editing any Advanced widget flips the combo to Custom |
| "Reset ray tracing to defaults" | Loop over a static list of RT cvars, set each to its default string. Needs a default accessor: check `idCVar`/`idCVarSystem` for one, else store defaults in the menu table |
| "Reset graphics to defaults" | Same for the non-RT graphics group |
| Menu layout | Per `Dhewm3-AI-RT-Settings.md` §1 and §4; Advanced node collapsed |
| Persistence | Add `CVAR_ARCHIVE` to cvars that stay in the menu without it |

Files: `neo/framework/Dhewm3SettingsMenu.cpp`, `RenderSystem_init.cpp`.
Check: pick each preset, `cvarlist` shows expected values; Reset returns every RT cvar to default (diff against a fresh config); quit and relaunch keeps the chosen preset.

---

## Phase 6a — RT shadow dispatch (one chunk)

Top GPU item. First seen in the 2026-10-11 capture (`r_vkLogRT 1`, Mars City 2 corner,
2560x1440, 25 view lights, 21 shadow-casting); **confirmed 2026-10-12 across 2978 frames**
of the Mars City Walkthrough with `lights.serial` in the CSV:

| `lights.serial` | frames | `gpu.Interactions` | `gpu.Shadows` | casters |
|---|---|---|---|---|
| 0 | 780 | 0.08 | 2.48 | 6 |
| 5 | 229 | 5.03 | 2.18 | 12 |
| 10 | 92 | 7.56 | 1.08 | 17 |
| 15 | 40 | 10.40 | 0.31 | 22 |
| 20 | 31 | 7.49 | 0.10 | 27 |

`Interactions` +7.41 ms while `Shadows` −2.38 ms: work moves out of the batched dispatch
and reappears in the interaction loop. Total GPU 6.50 → 13.13 ms. `lights.batched` pins
to exactly 7 and **74% of frames overflow the cap**, so this is not a corner case. The
`lights.*` counters were zeroed by a GUI-view bug until 2026-10-12; `python
tools/vk_profile_report.py mchq.csv` reproduces the table.

| Finding | Evidence | Where |
|---|---|---|
| Only 7 lights are batched; the rest trace one at a time inside the interaction loop, each with a render-pass break, depth round-trip and its own blur | 7 batched, 14 serial | `vk_backend.cpp:4107`, `vk_shadows.cpp:1248` |
| Serial shadow time is reported as `Interactions` (raster), so `Shadows` reads near zero when the cost is highest | `Shadows=0.16 Interactions=12.97` | span at `vk_backend.cpp:5419` |
| Layers go out in list order; the batch got 1.9 M pixels, the serial path 31.9 M (8.6 screens, 128 M rays at 4 samples) | `VK RT TRACE dims=` lines | `VK_RT_ShadowBatchAddLight` |
| Dispatch rect is the light scissor, full screen when the camera is inside the light volume. Four full-screen lights touch 2-4 surfaces each | lights 20-23 | `VK_ComputeLightScissor` |
| No per-pixel volume test. The only cull is a sphere of the box diagonal, point lights only, and off by default (`r_rtShadowDistanceCutoff 0`) | every pixel in the rect traces | `shadow_ray.rgen:173`, `vk_shadows.cpp:982` |
| Stencil is cleared per light although no stencil volumes are drawn with RT shadows | — | `vk_backend.cpp:4126` |

No leftover GI "reach" extension was found in this path; the full-screen rects are stock scissor behaviour plus the missing volume test.

| Step | Change | Check |
|---|---|---|
| S0 Measure | Mars City Walkthrough: `r_rtShadowSamples 1`, then `r_rtShadowBlurEnable 0`. Also sweep `r_vkRTShadowBatchMax` if it is a cvar (else raise `VK_RT_SHADOW_BATCH_MAX` and rebuild) — the cheapest possible win if break overhead dominates | `--baseline mchq.csv` per variant; notes record ray vs blur vs break overhead |
| S1 Label | Close the `Interactions` span around any mid-loop shadow dispatch and open a `Shadows` span for it | `Shadows` + `Interactions` sum unchanged; `Interactions` under ~1.5 ms in the corner |
| S2 Pixel cull | In `shadow_ray.rgen`, before tracing: point light = test world pos against the light's oriented box; projected = against the frustum planes. Outside stores 1.0 and returns. Replaces `r_rtShadowDistanceCutoff` | Debug mode tinting culled pixels; shadows identical inside volumes; `Shadows` ms drops |
| S3 Looped batches | Assign shadow lights to batch `k = n / W`. Batch 0 traces where it does today. On reaching the first light of batch k>0: end pass, clear used layers, trace + blur the batch, resume. Lights are additive, so order within the loop is free | 21 lights = 2 breaks at W=8 (was 14); log line per batch |
| S4 Delete serial path | Remove `VK_RT_DispatchShadowRaysForLight`, `VK_RT_SHADOW_SERIAL_LAYER`, `r_rtShadowBatch`, overflow log; the scratch layer becomes a batch layer (W=8) | `grep -r ShadowBatch\b` clean; settings doc row removed |
| S5 Rect shrink | Dispatch rect = light scissor ∩ screen rect of (lit surface bounds ∩ light bounds). Blur and interaction scissor use the same rect | Traced pixels per frame logged; corner drops well below 32 M |
| S6 Stencil clear | Skip the per-light stencil clear when RT shadows are on, after confirming the pass starts with stencil 128 | Screenshots identical; stencil-shadow fallback (`r_rtShadows 0`) unchanged |
| S7 Layer count (optional) | W=16 only if S0/S3 show break overhead still matters. First share one mask + blur-temp pair across frame slots (mask has no history), which makes 16 layers VRAM-neutral | VRAM logged before/after; no flicker with 2 frames in flight |

Order: S0, S1, S2, S3+S4, S5, S6, S7. One commit each, numbers in `notes/20261011_shadow_dispatch.txt`.

| Risk | Guard |
|---|---|
| S2 box test disagrees with the light's falloff texture at the edges, giving a lit rim | Pad the box by a few units; compare against the tinted debug view |
| S3 layer reused before the previous batch's draws executed | Barrier fragment-read → RT-write at each batch start; sync validation |
| S5 rect too tight when surface bounds are stale (animated models) | Use the same bounds the frontend culled with; fall back to light scissor when unsure |
| S7 shared mask across slots | Only after sync validation is clean on S3 |

Exit: on `mchq.csv`'s route, `SHADOW DISPATCH` shows `Interactions` flat against
`lights.serial` instead of climbing, `Interactions + Shadows` p50 at serial≥10 at most
half of today's 8.6-10.7 ms, and GPU p50 under 9.17 ms.

---

## Phase 6 — Vulkan efficiency pass

Detailed findings and stages A0-A5: `20261010_vk_core_audit.md`. A0 is done; that doc's
"Dispatched by measurement" table and re-ordered stage list supersede the steps below.

**Most of this phase is parked.** All three Phase 0 scenarios are GPU-bound by 4-7x, with
total CPU recording at 1.4-1.7 ms, so A2/A4/A5 have no measurable target. What survives is
GPU-side (B1, B2, B4) plus the BLAS rebuild burst. E2-E4 below are still worth doing as
audits because they are cheap and protect against regressions, not because a number demands
them.

Steps are lettered E1-E5 to avoid collision with Phase 6a above.

| Step | Detail |
|---|---|
| E1. Instrument | Audit stage A0 (substages A0a-A0e) owns this. Do not restate the counter list here |
| E2. Audit sync | `vkQueueWaitIdle` at `vk_ao.cpp:257`; `vkWaitForFences` in `vk_bloom.cpp:340`, `vk_gbuffer.cpp:185`, `vk_gi.cpp:831/3187`, `vk_gi_probe.cpp:545`; `vkDeviceWaitIdle` in resize/cvar-change paths. Confirm none run per frame — A0's tagged wait wrapper answers this directly |
| E3. Audit memory | Run 3 map loads + 10 min play; log buffer/image/AS create vs destroy counts at shutdown (`r_vkLogBufferGarbage` already exists: use before removing it). Validation layer leak report at exit |
| E4. Audit uploads | Full-buffer re-uploads of unchanged data (light tables, material table, instance data); write-combined reads (rule in ROADMAP); host-visible buffers not per-frame-slotted; BLAS rebuild vs update for dynamic models |
| E5. Fix | One change per commit, each with before/after numbers in the notes file |

Check per fix: frame CPU ms and uploaded bytes/frame drop; RSS and VRAM flat across map reloads (±2%); validation clean; screenshots identical.

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

| Phase | Risk | Depends on | Status |
|---|---|---|---|
| 0 Baseline + FP on | none | Audit A0 | Baseline measured 2026-10-12; FP default and screenshots outstanding |
| 6a Shadow dispatch | med | 0 | **Next.** Confirmed on 2978 frames; largest measured GPU item |
| 1 Debug trim | low | 0 | |
| 2 Reflections | low-med | 1 | |
| 3 Screen-space GI | med | 1 | Dead paths confirmed: `gpu.GI`/`GITemporal`/`Vol`/`VolTemporal`/`VolBilateral` all read 0.001 ms |
| 4 Non-froxel vol | med | 1 | same |
| 5 Presets/reset | low | 2, 3, 4 (final cvar set) | |
| 6 Efficiency | med | 3, 4 (smaller surface) | Mostly parked — GPU-bound, see the phase note |
| 7 Comments | none | 6 | |

6a moved ahead of 1-5: it is the only item with a measured double-digit GPU cost, and the
cvar trimming in 1-5 cannot change a GPU-bound frame.

Exit: cvar count down ≥ 40%, `vk_gi.cpp` + `vk_vol.cpp` combined line count down ≥ 40%, RSS/VRAM flat over map reloads, GPU p50 below the Phase 0 baseline on all three scenarios, all baseline screenshots unchanged.
