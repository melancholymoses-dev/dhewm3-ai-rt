# RT Roadmap

**Status reviewed:** 2026-09-29 · 
**This is the entry point.** If you're wondering what to work on or which plan doc
is authoritative, start here. This file owns *ordering* and *status*; detailed
designs live in the linked docs. Prior cycle: `completed/202609_ROADMAP.md`.

September handled controller, upscaling, improved volumetrics.

---

## Design pillars (the taste contract)

Every stage below serves these; anything that fights them gets cut or demoted.

1. **Shadows are the feature.** Soft, shaped, from more lights. This is where
   "dynamic lighting" actually lives.
2. **Darkness stays black.** GI may tint lit regions; it must never lift the noise
   floor of dark ones.
3. **Light the air sparingly.** Volumetrics from hero lights only.
4. **Reflections are set dressing.** Glass and mirrors, plus the odd hero surface by
   explicit opt-in.  The assets carry no PBR data; don't pretend they do.
5. **No map editing.** Engine-side rules + budgets + debug overlays. Small def-file
   mods are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.

---

## Current arc

| # | Item | Doc | Status |
|---|---|---|---|
| 1 | Projectiles in reflections | `20261002_reflection_enhancements.md`  | Try to handle reflections of particles. | In progress |
| 2 | `20260906_bloom_plan.md` | Bloom: emissive-sourced, composited after FSR and before the HUD. Unimplemented; revised 2026-09-30. | Not started |
| 3| `see_first_person_player_model.md` | First-person player body; orthogonal to the lighting arc. | Modeling started | 
-----------------------
### Measured RT budget

`r_vkRTProfile 1`, Mars City, median GPU ms per phase. **RTX 4070 Ti Super, 2560×1440.**

| | GI | Refl | Vol | AO | Shadows | TLAS | **total** |
|---|---|---|---|---|---|---|---|
| 2026-09-11 (pre-arc-1/2) | 4.41 | 3.24 | 1.53 | 1.17 | — | 0.15 | **11.93** |
| 2026-09-19 (arc 2 closed) | ~1.3 | ~0.8 | 0.29 | 1.9 | — | 0.12 | **~7-8** |
| 2026-09-23 MC2, scale 1.00 | 1.29 | ~0 | 0.52 | 1.27 | 1.80 | 0.17 | **5.05** |
| 2026-09-23 MC2, scale 0.67 | 0.54 | ~0 | 0.37 | 0.49 | 0.68 | 0.14 | **2.22** |

Shadows and AO are the two largest costs. **Not every remaining pass is
screen-resolution** — arc 2 left a ~0.73 ms floor (TLAS, probe trace/blend, froxel
fill/integrate) that render scale cannot touch, so 0.50 saves only 0.1 ms more than 0.67.
See `completed/20260918_fsr_upscaling.md` §12.

**Raster is now instrumented** — `r_vkRTProfile 1` reports `raster=` (depth prepass,
interactions, shader passes, fog lights, upscale, tonemap) plus `VK FRAME PROFILE` / `VK CPU PROFILE`
lines. The rows above were vsync-locked RT-only runs; re-measure uncapped before quoting a raster share.

**These are all from the fast card.** The 9070 XT runs the same content near 30 fps with
dips into the teens; nothing above has been re-measured there, and it is the hardware the
arc exists for.

### Engine-wide rules

Each of these cost a debugging session. They apply to any new RT code.

| Rule | Because |
|---|---|
| A ray origin from `rt_ReconstructWorldPos` must floor its bias at `d²·ulp/znear` | A fixed bias is a distance-limited bias. `r_znear` is game-owned and drops to 1.0 in cinematics |
| A shadow ray's cull mask is a parameter, never a constant | `noSelfShadow` is instance mask `0x01`; `0xFF` made the player self-shadow in reflections only |
| A light's *current* value comes from `EvaluateRegisters()` + the stage's `color.registers[]` | Doom 3 puts flicker in material expressions. `shaderParms` is the constant amplitude — 74 animated materials were pinned at peak |
| A mapped buffer the CPU **reads** needs `HOST_CACHED`, and should be memcpy'd out before use | `HOST_VISIBLE\|HOST_COHERENT` alone is write-combined; scalar reads cost ~200 ns each. Worth 3.26 → 0.03 ms on the probe classifier |
| A mapped buffer (or descriptor set) the CPU **writes** while recording needs one copy per frame slot, indexed by `vk.currentFrame` | The previous frame is still on the GPU. The shared material table caused white/black triangles on the 9070 XT only (GPU-bound, CPU a frame ahead); `r_vkSerializeFrames 1` is the check |
| Doom 3 winds front faces opposite to GL/Vulkan — use the vertex normal, not `gl_HitKindEXT` | Every visible surface reports back-facing |
| A pixel-skipping pattern keys off a per-slot counter, never `tr.frameCount` | Caused the GI checkerboard ghost |
| Shared GLSL that compute shaders include must be **pipeline-agnostic** — no ray payload, no `traceRayEXT`, no TLAS. `rt_light_struct.glsl` is that home; `rt_light_eval.glsl` is not | An RT-only built-in pulled into a compute shader silently killed all volumetric lighting once |

### Standing decisions

- **Reflections are glass-only** (`r_rtReflectionMode 1`). Per-material F0 (T3) is
  **dropped** — with opaque geometry never reflecting there is nothing to classify.
  Threshold tuning cannot help: the worst artifacts are the *highest*-F0 pixels while any
  threshold culls from the bottom.
- **The tonemap toe has two regimes.** At `r_rtTonemapToe 2.7` slope is <0.33 below
  luminance 0.07 but *exceeds 1.0* between 0.12 and 0.3. Evaluate the whole curve before
  blaming tonemapping.
- **Reflected surfaces get no indirect light** — GI modulates by the *primary* G-buffer
  albedo. Structural, not a tuning miss.
- **Using same light falloff as raster** - Keep reach = 1.  Get benefit of art,
while new lighting techniques enhance without fighting too much.

---

## Live documents

| Doc | Owns |
|---|---|

| `20260924_controller_gunfeel.md` | Gamepad aim response, rumble, aim assist (C1-C4). Orthogonal to the lighting arc. |
| `../vulkan_debugging.md` | Not a plan — the reference for getting Vulkan validation/GPU-AV output out of this engine. Load it before chasing any AMD-vs-NVIDIA or device-lost bug. |

---

## Completed

| Name | Doc | Owns |
|---|---|-----|
| Controller gun-feel | `20260924_controller_gunfeel.md` | C1 (radial deadzone + curve) and C2 (accel) are framework-only and independent of the RT arc — can run any time. C3 rumble, C4 aim assist follow. Added weapon select and rumble. |
| FSR U5 polish | `completed/20260918_fsr_upscaling.md` U5 | `r_fsrQuality` in the video menu, FSR 3.1 evaluation, dynamic resolution. Skinned-MV double-buffering only on the reopen trigger listed in U5 |
| RT texture LOD (U3b) | `completed/20260918_fsr_upscaling.md` U3b | RT fetches sample mip 0 at any distance. Real, but not the grating artifact |
| Adaptive probe hysteresis (was G5 fix 2) | `completed/20260906_froxel_probe_gi.md` | Boost alpha when a probe's new value differs sharply from `prev`. The answer for **doors and moving lights** — G5b handles flicker and explicitly cannot help here. Needs a lower-variance estimator first (`r_rtGIProbeRays 256`), so it costs ~+0.5 ms before it starts - Skip|

---

## Backlog (not in the current arc, not dead)

| Item | Doc | Note |
|---|---|---|
