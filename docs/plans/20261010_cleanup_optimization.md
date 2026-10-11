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

| Change | Files |
|---|---|
| `pm_showFirstPersonBody` default 0 → 1 | `neo/game/gamesys/SysCvar.cpp`, `neo/d3xp/gamesys/SysCvar.cpp` |
| Capture baseline: `r_vkRTProfile 1` GPU ms + `VK CPU PROFILE` on Mars City, 1440p, scale 1.0, uncapped | log saved to `docs/plans/notes/20261010_baseline.txt` |
| Screenshots: 6 fixed spots (glass, volumetric hall, flashlight, dark room, mirror, FP body) | `notes/baseline_*.png` |
| Record RSS + VRAM after 5 min play and after 3 map loads | same notes file |

Check: new game starts with body visible; the baseline files exist. Every later phase is compared against them.

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

Top GPU item from the 2026-10-11 capture (`r_vkLogRT 1`, Mars City 2 corner, 2560x1440, 25 view lights, 21 shadow-casting).

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
| S0 Measure | In the capture corner: `r_rtShadowSamples 1`, then `r_rtShadowBlurEnable 0`, with `r_vkRTProfile 1` | Notes file records ray cost vs blur cost vs remainder (break overhead) |
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

Exit: corner capture shows `Interactions` ≤ 1.5 ms, total shadow ms at least halved, 60 fps held with default settings.

---

## Phase 6 — Vulkan efficiency pass

Detailed findings and stages A0-A5: `20261010_vk_core_audit.md`. Run A0 first; the audit's ordering supersedes the generic steps below where they overlap.

Evidence so far: per-frame sync and allocation sites, roadmap item 7.4 (per-frame upload wait, conditional), cache/upload thrash reported by the user. Measure first; fix only what the numbers show.

| Step | Detail |
|---|---|
| 6a. Instrument | Per-frame counters under `r_vkRTProfile`: bytes uploaded per buffer, `vkMap`/alloc calls, descriptor writes, fence waits and ms, TLAS/BLAS rebuild count. CPU time by function (VS profiler on `vk_backend.cpp` `DrawView`) |
| 6b. Audit sync | `vkQueueWaitIdle` at `vk_ao.cpp:257`; `vkWaitForFences` in `vk_bloom.cpp:340`, `vk_gbuffer.cpp:185`, `vk_gi.cpp:831/3187`, `vk_gi_probe.cpp:545`; `vkDeviceWaitIdle` in resize/cvar-change paths. Confirm none run per frame |
| 6c. Audit memory | Run 3 map loads + 10 min play; log buffer/image/AS create vs destroy counts at shutdown (`r_vkLogBufferGarbage` already exists: use before removing it). Validation layer leak report at exit |
| 6d. Audit uploads | Full-buffer re-uploads of unchanged data (light tables, material table, instance data); write-combined reads (rule in ROADMAP); host-visible buffers not per-frame-slotted; BLAS rebuild vs update for dynamic models |
| 6e. Fix | One change per commit, each with before/after numbers in the notes file |

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

| Phase | Risk | Depends on |
|---|---|---|
| 0 Baseline + FP on | none | — |
| 1 Debug trim | low | 0 |
| 2 Reflections | low-med | 1 |
| 3 Screen-space GI | med | 1 |
| 4 Non-froxel vol | med | 1 |
| 5 Presets/reset | low | 2, 3, 4 (final cvar set) |
| 6a Shadow dispatch | med | 0 (independent of 1-5; can run first) |
| 6 Efficiency | med | 3, 4 (smaller surface) |
| 7 Comments | none | 6 |

Exit: cvar count down ≥ 40%, `vk_gi.cpp` + `vk_vol.cpp` combined line count down ≥ 40%, RSS/VRAM flat over map reloads, uploaded bytes/frame and CPU frame time ≤ baseline, all baseline screenshots unchanged.
