# Froxel Volumetrics + Probe GI — world-space caching arc

**Closed 2026-09-19.** Both parts shipped and are the default. Volumetric marching and
per-pixel GI stay compiled as A/B references.

Moved vol and GI sampling out of screen space into world-space caches: a view-aligned
froxel grid for the medium, a DDGI-style irradiance probe grid for bounce. This deleted
most of the GI noise-fighting chain structurally rather than by tuning.

---

## What shipped

| | Part A — froxel volumetrics | Part B — probe GI |
|---|---|---|
| Default | `r_rtVolFroxel 1` | `r_rtGIProbes 1` |
| Old path | `vol_march.comp` + `vol_bilateral.comp`, kept | per-pixel rgen + temporal + a-trous, kept |
| Cost | 1.63 → 0.29 ms median (5.6×) | 4.41 → ~1.3 ms chain |
| Overlays | `r_rtVolFroxelDebug 1-4` | `r_rtGIProbeDebug 1-8` |
| Dump | `r_rtVolFroxelDump 1` | `r_rtGIProbeDump 1`, `r_rtGILightDump 1` |

### Shipped defaults

| CVar | Default | | CVar | Default |
|---|---|---|---|---|
| `r_rtVolFroxel` | `1` | | `r_rtGIProbes` | `1` |
| `r_rtVolFroxelResX/Y/Z` | `240/135/96` | | `r_rtGIProbeCountX/Y/Z` | `32/32/16` |
| `r_rtVolFroxelFarTransmittance` | `0.002` | | `r_rtGIProbeSpacing` | `64` |
| | | | `r_rtGIProbeRays` | `128` |
| | | | `r_rtGIProbeUpdatesPerFrame` | `1024` |
| | | | `r_rtGIProbeHysteresis` | `0.97` |
| | | | `r_rtGIProbeVisibility` | `1` |
| | | | `r_rtGIProbeNormalBias` / `ViewBias` | `8.0` / `8.0` |
| | | | `r_rtGIProbeMaxRayDist` | `512` |
| | | | `r_rtGIProbeInside` / `OutsideThreshold` | `0.25` / `0.9` |
| | | | `r_rtGIProbeFastBuckets` | `1` |
| | | | `r_rtGIFlickerThreshold` / `Hold` | `0.15` / `2.0` |

### Probe chain cost (Mars City, `r_vkRTProfile 1`, median GPU ms)

| Config | ProbeTrace | ProbeBlend | ProbeResolve | scratch |
|---|---|---|---|---|
| **128×1024 (shipped)** | 0.05 | 0.09 | 0.61 → **0.74** with G5b | 2 MiB ×2 |
| 256×1024 | 0.07 | 0.16 | 0.66 | 4 MiB ×2 |
| 256×4096 | 0.15 | 0.50 | 0.63 | 16 MiB ×2 |

The resolve dominates and no probe CVar affects it. G5b's second atlas bucket cost
+0.13 ms because it shares the 8 Chebyshev tap weights with the first.

---

## Chunk outcomes

| Chunk | Outcome |
|---|---|
| F0-F2 | Grid, overlays, integrate + resolve. Landed, validated |
| F3 clustered light lists | ❌ Dropped — the fill was never light-bound |
| F4 froxel temporal + rotation | ❌ Dropped — no history means no ghosting; the arc's main win |
| F6 background attenuation | Landed. Transmittance fallback must be `a = 1`, never `a = 0` |
| F7 cone penumbra + soft cookie edge | Landed |
| **F5 retire decision** | ✅ **Keep the march**, compiled behind `r_rtVolFroxel 0`. It is the only independent check on the froxel sampler and it localised the cookie hard-clip. Cost accepted: every light-model change is written into both shaders |
| G0-G3 | Payload, atlas, tracing, Chebyshev leak hardening. Landed, validated |
| G4 probe classification | Landed. Relocation dropped |
| G5 fix 1 (more rays) | ❌ Does not reach flicker at any rate — the cause was elsewhere. A door/moving-light fix only, not enabled by default |
| G5 fix 2 (adaptive hysteresis) | → backlog. Still the right answer for doors and moving lights |
| G5b flicker factorization | Landed + validated. See below |
| G6 default flip | ✅ `r_rtGIProbes 1`, decided on appearance. Per-pixel path kept |
| G6 retune | → `rt_optimization_tuning.md` T4-T6. GI/vol read over-bright; the defaults still carry the per-pixel path's tuning |

---

## G5b — flicker factorization

A probe re-traces every 16 frames and blends at alpha 0.03, so τ ≈ 8.9 s. A flickering
light's bounce converged to its mean and a dark room kept a glow — pillar 2.

`E_p = Σ_stable L_j·G_pj + s(t)·Σ_fast L̂_k·G_pk`. A flickering light uploads at its peak
colour `L̂` with `s = current/peak` beside it; its probe transport is cached normalised in
a second atlas bucket and the resolve reapplies the gain. Zero latency, zero extra rays.

- **One light per bucket, K = 1.** A scene-global gain is invalid for spatially disjoint
  lights — two out-of-phase lights in different rooms would each get the mean.
- **Two flag bits, not one.** `GI_LIGHT_FLAG_FAST` (rescale everywhere) is separate from
  `GI_LIGHT_FLAG_FAST_BUCKET` (cache normalised, probe path only). Merging them leaves
  every non-bucketed flickering light rendering at its **peak** in volumetrics and
  reflections.
- **Ranked by `importance × flicker depth`**, not importance. Importance alone picks the
  light that least needs factorizing — a 10 % blinker beat a strobe that goes to black.
- **Classification latches** until the light's shape hash changes or `renderView.time`
  rewinds. A fast light at `s = 1` behaves exactly like a stable one, so there is no
  transition to cross.

### The premise was wrong, and fixing it mattered more than G5b

Doom 3 writes light animation as time-driven expressions in the light material's **stage
colour registers**, not in `shaderParms`. `VK_RT_UploadGILights` read the parms — the
constant amplitude — so RT GI bounce, volumetrics and reflections had **never seen a light
flicker at all**, predating this arc. All 74 animated light materials were pinned at their
peak. Fixed by hoisting `EvaluateRegisters` and taking the colour from the chosen stage,
shared with the cookie block so colour and gobo agree.

This is why G5 fix 1 measured as "does not reach a 5 Hz flicker": no update rate tracks an
absent signal. It also means the G6 retune is owed against a light set whose average
contribution just dropped.

Engine-wide rule: when an RT feature needs a light's *current* value, take it from
`lightShader->EvaluateRegisters()` + the stage's `color.registers[]`, never from
`shaderParms`. `r_rtGILightDump` prints `parm=` beside `eval=` for exactly this check.

---

## Findings that outlive this arc

- **The probe stats readback was write-combined.** `HOST_VISIBLE | HOST_COHERENT` without
  `HOST_CACHED` gets uncached memory; the G4 classification loop read 16k entries out of it
  scalar-wise and cost **3.26 ms** of backend CPU — ~200 ns per probe for two float loads.
  Fixed with `VK_CreateBufferPreferred` plus a linear memcpy into heap before classifying:
  **0.03 ms**, a 100× drop. Any mapped buffer the CPU *reads* needs both halves of that fix.
  It hid because it is CPU time on a GPU-bound frame, and it scales with probe count rather
  than with anything on screen, so it never moved when the view did.
- **An EMA accumulator cannot live in a per-slot buffer.** The stats buffer is shared and
  the per-slot copies are read-only snapshots; the per-slot version produced a 2-frame
  oscillation across the whole grid.
- **Doom 3 winds front faces opposite to GL/Vulkan**, so `gl_HitKindEXT` calls every
  visible surface back-facing. Use the vertex normal. This silently gutted probe GI from G1.
- **Second moments overflow to +inf in an fp16 atlas.** Store them normalised.
- **Doom 3's far plane is infinite** — NDC z tops out at 0.999, so unprojecting at `ndcZ = 1`
  points the view ray backwards.
- **A temporal EMA does two jobs**: it denoises the estimator and low-passes the scene.
  Fix the estimator, then make smoothing adaptive. Never just lower alpha.
- **Per-pixel GI overlays (modes 2/3) are additive** — `VK_RT_CompositeGI` has no replace
  pipeline and interactions draw after it. Read hue ratios, not absolute colour.

## Rejected — analytic shadow-volume integration

Per light, integrate the lit intervals along each view ray: zero sampling, exact edges.
Set aside because stencil volume generation is disabled under RT shadows (re-adding the CPU
cost RT removed), the front/back-face trick yields shadowed *length* but not the
distance-attenuated phase-weighted integral, and it scales per-light per-pixel. Worth a
second look for exactly one hero light if razor-sharp shafts are ever demanded.
