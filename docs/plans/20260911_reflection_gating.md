# Reflection Gating Rework — pay only where it shows

**Date:** 2026-09-11
**Status:** Planned, not started
**Supersedes:** `rt_optimization_tuning.md` T3 (per-material F0) — absorbed here.

---

## The complaint

Reflections cost a great deal and return almost nothing. Dim, wrong-looking sheen
on metal floors everywhere; the one place the effect is actually wanted — seeing
yourself in a pane of glass — is barely visible. This is a regression against
**pillar 4** ("reflections are set dressing, gated by F0") in *both* directions at
once: too wide, and too weak where it's narrow.

## Why — the arithmetic

Screen-space F0 comes from a specular-map luminance remap
(`gbuffer.frag:76-78`, mirrored in `interaction.frag`):

```
f0 = clamp(pow(specLum, r_rtSpecF0Gamma) * r_rtSpecF0Scale)   // gamma 3, scale 0.2
```

`reflect_ray.rgen:163` then early-outs only when `f0 < 1/255`. Inverting that:

| specular-map luminance | resulting F0 | reflectance at normal incidence |
|---|---|---|
| **0.27** (the early-out cutoff) | 0.0039 | **0.4 %** |
| 0.4 | 0.0128 | 1.3 % |
| 0.6 | 0.0432 | 4.3 % |
| 0.8 | 0.1024 | 10.2 % |

So **any specular texel brighter than luminance 0.27 traces the full ray chain** —
which is essentially every painted-metal, trim, grate and floor material in Doom 3
— in order to produce a half-percent reflection that is invisible in the composite.

What that pixel pays for the privilege (`reflect_ray.rgen`, `reflect_ray.rchit`):

| | rays |
|---|---|
| glass probe (fired whenever `sceneHasGlass`) | 1 |
| reflection bounce loop | ≤ 2 |
| shadow rays per opaque hit (`REFL_MAX_SHADOW_LIGHTS`) | ≤ 8 each |
| **worst case per pixel** | **≈ 17** |

At 1080p that's a multi-million-ray budget spent almost entirely on surfaces whose
contribution rounds to zero.

### And the dim sheen you *can* see is the wrong term

`reflect_ray.rgen:252` lifts the Fresnel ceiling at grazing angles:

```
grazing = mix(params.grazingMax, 1.0, smoothstep(0.3, 0.7, f0));   // grazingMax 0.25
fresnel = f0 + (grazing - f0) * pow(1.0 - NdotV, 5.0);
```

A floor at F0 = 0.005 is 0.4 % reflective head-on but ramps to **25 %** at glancing
angles. That grazing lobe *is* the visible metal-floor sheen. It is the most
expensive and least wanted part of the feature: physically it's the term a rough
surface never reaches, and artistically Doom 3's floors should read matte.

### Two smaller bugs found on the way

1. **`sceneHasGlass` is over-broad.** `VK_MAT_FLAG_GLASS` is set for *any*
   `MC_TRANSLUCENT` material (`vk_material_table.cpp:589`), and `sceneHasGlass` is a
   scan for that flag (`vk_accelstruct.cpp:1825-1833`). But the BLAS filter's glass
   exception (`vk_accelstruct.cpp:627`) and the compositing overlay
   (`vk_backend.cpp:2183`) both correctly discriminate on `SURFTYPE_GLASS`. Result:
   P6's "skip the probe in glass-free rooms" optimisation almost never fires, and a
   non-glass translucent that clears the BLAS filter can be picked up by the probe
   and turned into a mirror.
2. **Glass F0 is hardcoded low and inconsistent** — `GLASS_F0 = 0.15` in
   `reflect_ray.rgen:240`, `F0 = 0.1` in `reflect_ray.rchit:99`. That is a physically
   correct value for a clean pane viewed head-on, and it is exactly why you can't see
   yourself. This is what T3 was written to fix and it never landed.

---

## Target behaviour

> Opaque geometry is **matte by default**. Reflections exist on glass, mirrors and
> an explicit, small opt-in set. Where they exist, they are strong enough to read.

Concretely: cull the wide path to a few percent of the screen, then spend a *portion*
of the savings raising quality on what's left.

---

## Stages

### R0 — Measure before changing anything *(pillar 6)*

Nothing below gets tuned by eye. Land this first and record numbers.

### R0a — Baseline measured 2026-09-11 ✅

`r_vkRTProfile 1`, Mars City, a metal+glass room then a glass-free corridor, 53 frames
with reflections on / 34 off.

| phase | median ms (refl ON) | refl OFF |
|---|---|---|
| GI | 4.41 | 4.44 |
| **Refl** | **3.24** *(range 2.18-4.46)* | 0.00 |
| Vol | 1.53 | 1.64 |
| AO | 1.17 | 1.22 |
| GIAtrous / VolBilateral / Shadows / TLAS | 0.38 / 0.26 / 0.21 | — |
| **RT GPU total** | **11.93** | **9.16** |

**Reflections are 3.2 ms — 27 % of the RT budget, second only to GI.** Worth fixing,
but the earlier 8.3 ms figure inferred from a 60→40 fps drop does not reconcile with the
2.77 ms measured total delta; see "unmeasured tail" below before claiming a frame-time
win.

**The decisive result is that `Refl` is flat.** It sat at 3.0-3.5 ms across the entire
walk and barely moved (~3.4 → ~3.0) entering a corridor with no glass. The cost does not
track scene content — not glass presence, not metal density. That is the signature of
*every pixel tracing regardless of what it is*, which is exactly the diagnosis above, and
it is the strongest evidence in this doc that R1+R2 are aimed correctly.

What the flatness does **not** resolve is the split between the glass probe, the bounce
ray, and the shadow rays — all three are per-pixel and none would vary with content.
`sceneHasGlass` was not logged this run (`r_vkLogRT` was 0), so it is still unknown
whether the corridor skipped the probe at all; the absence of a step change is weak
evidence for the R4 over-broad-flag bug. **The R0 ray counters are the instrument that
settles this**; until then, prefer R1+R2 (cheap, and correct under either split) over
R4b.

**Unmeasured tail:** the `Refl` phase brackets only `VK_RT_DispatchReflections`
(`vk_backend.cpp:4716`). `VK_RT_CompositeReflections` (`:4861`) and the per-surface glass
overlay draws (`:2266`) run outside it, the latter inside the main render pass where no
RT phase covers them. True feature cost is ≥ 3.2 ms. Bracket both before quoting a
final before/after.

- Debug mode `r_rtReflectionDebugMode 5`: colour-code each pixel by the path it took —
  **black** culled by early-out · **blue** opaque reflective · **green** glass ·
  **red** mirror · **yellow** fell back to `rt_ReconstructNormal`. Written in
  `reflect_ray.rgen` before any trace, same pattern as modes 2-4.
- An atomic counter SSBO in the reflection descriptor set: pixels surviving the
  early-out, reflection rays traced, shadow rays traced. Printed at `r_vkLogRT 1`.
  This is the before/after instrument for every stage below and the profiler
  checkpoint the froxel arc is waiting on.
- Debug mode `6`: for glass pixels, output raw `accum` **before** the Schlick weight.
  This separates the two independent causes of "can't see myself in the glass" —
  a weak interface weight vs. a dark reflected subject — which otherwise multiply
  together and can't be told apart in the composite. If mode 6 already shows a dim
  player, raising glass F0 is the wrong lever and the fix belongs in the hit shader's
  irradiance (`REFL_AMBIENT`, `reflAmbientScale`, shadow budget), not in R3/R5.

**Exit:** a logged ray count for a representative scene (suggest an Alpha Labs metal
floor + a glass office partition in one view). Expect the culled fraction to be low —
the arithmetic above predicts it, the counter proves it.

### R1 — Early-out on the *final* weight, not on raw F0 ✅ *implemented 2026-09-12*

Free and exact. Every input to the Fresnel term — `f0`, `NdotV`, `grazingMax` — is
already known in the rgen **before** the trace; the computation just happens to sit
at line 251, *after* it. Hoist it above the bounce loop and cull on the result:

```
if (glassProbe.hitT <= 0.01 && fresnel * params.reflBlend < params.minWeight) { store 0; return; }
```

New cvar `r_rtReflectionMinWeight`, default **0.04**. This culls precisely the pixels
whose radiance would have been multiplied to nothing — no approximation, no new
heuristic, and at `minWeight = 1/255` it is bit-identical to today's behaviour, so the
cvar doubles as the A/B control.

Note the ordering constraint that survives: the glass probe still has to run first,
because glass reflections don't depend on surface F0. R4 shrinks that cost.

### R2 — Make low-F0 surfaces matte at grazing angles ✅ *implemented 2026-09-12*

Replace the grazing lift so the ceiling scales *with* F0 instead of being a flat 0.25
floor for everything:

```
grazing = min(f0 * r_rtSpecGrazingGain, 1.0);   // gain ~4, so F0 0.005 -> ceiling 0.02
grazing = mix(grazing, 1.0, smoothstep(0.3, 0.7, f0));   // true metal still reaches 1.0
```

A 0.5 %-reflective floor now tops out at 2 % instead of 25 % — the sheen goes away —
while genuinely high-F0 surfaces keep their full grazing response. Combined with R1
this is what actually deletes the metal-floor workload: those pixels stop clearing the
weight threshold at *any* view angle. Keep `r_rtSpecGrazingMax` as a hard ceiling for
compatibility with recorded tuning values.

**Validate:** mode 5 before/after — the metal floor should go from blue to black
across the whole viewing range, not just head-on.

**As landed (R1+R2 together, `reflect_ray.rgen` + `vk_reflections.cpp`):** the Fresnel
block moved above the bounce loop, guarded by `if (!isGlass)`, with the cull immediately
after it; the tail `imageStore` reuses the hoisted `fresnel`. The pre-existing
`f0 < 1/255` test is kept ahead of it as a trivial reject — it avoids the
`rt_ReconstructNormal` depth fetches that computing `NdotV` would otherwise force on
genuinely zero-F0 pixels. `ReflParamsUBO` grew 96 → 112 bytes (two new floats plus 8
bytes of pad: std140 rounds the block to 112 and `uboInfo.range` is `sizeof(...)`, which
must not undershoot it).

**Measured 2026-09-12** (Mars City, corridor → glass room → corridor → glass):

| | before (R0a) | after R1+R2 |
|---|---|---|
| `Refl` median | 3.24 ms | **1.95 ms** |
| `Refl` range | 2.18 – 4.46 (flat) | **0.61 – 2.79** |
| corridor segment | ~3.0 ms | **0.62 ms** |
| glass room | ~3.4 ms | ~2.0 – 2.8 ms |

**The cost is now scene-dependent, which was the real prediction.** The corridor fell 5×
while the glass room fell only ~35 %, so the pass finally tracks what is actually in view
instead of charging a flat per-pixel rate. Overall median down 40 %.

Two things follow. The **0.62 ms corridor floor** is the residual full-screen cost —
dispatch plus, if `sceneHasGlass` is true there, the probe. The **glass room's remaining
~2 ms** is work R1/R2 by construction cannot touch: glass pixels bypass the weight cull,
and the probe is full-screen. Both point at R4/R4b as the next perf move, not R3.

Still unresolved: `sceneHasGlass` was not logged (`r_vkLogRT` 0), so we don't know whether
the corridor skipped the probe. Two zero-code experiments discriminate:
`r_rtReflectionMinWeight 1.0` culls every opaque pixel, leaving probe + glass + dispatch
as the residual; `r_rtReflectionMinWeight 0.004` restores pre-R1 behaviour for a clean
same-route A/B.

**Visual (mode 3, Sec Ops Junction, 2026-09-12): the F0 mask is edge tracery, not broad
surfaces.** What lights up is panel seams, trim, grate slats and vent louvers — Doom 3's
specular maps concentrate their energy on high-frequency edge detail. This retro-explains
the old flat 3.2 ms: edge detail is uniformly distributed, so every room cost the same,
and scattered single-pixel rays are the worst case for warp coherence.

It also inverts the obvious tuning move. `minWeight` culls lowest-F0 first, so the bright
tracery is the *last* thing it removes; raising it erases broad subtle washes and leaves
the wireframe look. The surgical knob for a broad-panel grazing wash is
`r_rtSpecGrazingGain 1.0` (flattens `fresnel` to `f0`), not `minWeight`.

Admission thresholds against the current `pow(specLum,3)*0.2` remap:

| minWeight | reflects face-on if specLum > | at grazing if specLum > |
|---|---|---|
| 0.04 *(default)* | 0.59 | 0.37 |
| 0.08 | 0.74 | 0.46 |
| 0.12 | 0.84 | 0.53 |
| 0.20 | never | 0.63 |
| 0.25 | never | 0.68 |

At `minWeight >= 0.20` nothing reflects head-on; only glancing hits survive. Because glass
is exempt from the weight test, **`minWeight 0.25` already approximates R3's end state**
("glass and mirrors only") without any material classification. If that look is accepted,
R3 shrinks to raising glass quality and the per-material F0 table becomes optional.

### R2's real reach — correction, 2026-09-12

In-game, `r_rtSpecGrazingGain` changed nothing on the offending surfaces (bright mirror
specks on CPU-rack edges and unlit light-fixture housings, Sec Ops Processing). That is
expected once the numbers are checked, and it corrects an overclaim above:

| angle off-normal | `pow(1-NdotV,5)` | fresnel at f0=0.2 |
|---|---|---|
| 20° | 0.00000 | 0.2000 |
| 40° | 0.00070 | 0.2000 |
| 60° | 0.03125 | 0.2016 |
| 85° | 0.63385 | 0.2310 |
| 89° | 0.91573 | 0.2458 |

**The Schlick tail only matters within ~20° of the silhouette.** R2 therefore helps
surfaces viewed *along* (floors) and is a no-op on wall panels and rack faces at normal
incidence, whose weight is pure `f0`. The earlier claim that R2 makes metal matte "from
every angle" was wrong.

**Consequence — this is the empirical case for R3.** Max achievable `f0` is 0.2
(`scale` 0.2 × `specLum` ≤ 1.0), and max `fresnel` is 0.246. The worst artifacts are the
*highest*-F0 pixels in the scene, while `minWeight` culls from the bottom. So no threshold
separates good opaque reflections from bad ones — the only settings are "all on, with
20 % mirror specks on rack edges" or "all off". Spec-map-derived F0 cannot be salvaged by
tuning, which promotes R3 from polish to the actual fix and argues for opaque default
F0 = 0 with no spec-map path at all.

**Also: mode 3 is not a linear read of F0.** Max f0 is 0.2, but the debug output lands in
the HDR scene target and is tonemapped downstream, so near-white strips are ~0.2, not ~1.0.
Useful for *where*, misleading for *how much*. Worth scaling by `1/scale` if the view is
kept.

### R3 — Per-material F0: matte by default, reflective by classification *(was T3)*

Stop inferring reflectance from specular-map luminance on the wide path. Doom 3's
specular maps encode a Blinn-Phong highlight, not a Fresnel F0 — reading them as PBR
data is the root cause of the whole problem, and pillar 4 says not to pretend.

CPU-side, in `vk_material_table.cpp` alongside the existing flag classification:

| material | F0 |
|---|---|
| mirror (`SS_SUBVIEW` sort) | 0.9 |
| `SURFTYPE_GLASS` | 0.4 *(up from 0.1/0.15 — see R5)* |
| matches `r_rtReflectiveMaterials` opt-in patterns | 0.5 |
| everything else | **0.0** |

- Add `float f0` to `VkMaterialEntry` / GLSL `MaterialEntry` (pad space exists; bump
  the `static_assert`s). This is also what the hit shaders need in R5.
- `r_rtReflectiveMaterials` is a semicolon-separated substring list over material
  names, default `mirror;glass;`. This is the sanctioned escape hatch for the "odd
  hero surface" in pillar 4 — a wet floor in a specific room — without map editing.
- The raster prepass needs the same number. `vk_backend.cpp` already pushes
  `u_SpecF0Scale` / `u_SpecF0Gamma` per draw (`:1167`, `:2670`); add
  `u_ReflF0Override` on the same path, `< 0` meaning "use the spec-map remap".
  `gbuffer.frag` and `gbuffer_clip.frag` branch on it.
- Keep the legacy behaviour reachable: `r_rtReflectionMode` — `0` off, `1` classified
  (new default), `2` legacy spec-map-wide. Cheap insurance and a clean A/B.

**Validate:** debug mode 3 (F0 greyscale) should go near-uniformly black except glass
panes and mirrors. If a whole room lights up, a classification rule is too loose.

### R4 — Fix the glass probe's scope

- Add `VK_MAT_FLAG_REAL_GLASS`, set only for `MC_TRANSLUCENT && SURFTYPE_GLASS` —
  the same discriminator the BLAS filter and the compositing overlay already use.
  Keep `VK_MAT_FLAG_GLASS` for the existing coverage semantics.
- `sceneHasGlass` scans for `REAL_GLASS`. Glass-free rooms now genuinely skip the
  fullscreen probe, which is what P6 was supposed to buy.
- `glass_probe.rchit` accepts a hit only if the material carries `REAL_GLASS`, so a
  stray translucent can't become a mirror.

### R6 — Glass-only reflections ✅ *implemented 2026-09-12* (supersedes R3, subsumes R4b)

Per user direction after the artifacts above: *"just do reflections on glass, it generally
looks awful elsewhere."* This also answers the perf question R1/R2 left open — at
`minWeight 0.25` every non-sky pixel was still firing a glass probe before being culled,
which is precisely the 0.62 ms corridor floor. Gating harder never removes that; only not
dispatching does.

`r_rtReflectionMode`: **1 = glass only (default)**, 2 = legacy full-screen.

- CPU (`VK_RT_GlassScreenRect`) unions the frontend's per-surface `scissorRect`
  (`drawSurf_t`) over every `SURFTYPE_GLASS` surface in the view — no projection math,
  the frontend already computed it — and converts to Vulkan Y-down pixels using the same
  convention as `VK_ComputeDrawSurfScissor`. The reflection buffer is swapchain-sized, so
  the two share a coordinate space.
- **No glass in view → `vkCmdTraceRaysKHR` is not called at all.** Not a cheaper dispatch,
  no dispatch.
- Glass in view → the launch grid is just the glass rect; the rgen adds `rectOrigin` to
  `gl_LaunchIDEXT`. Rect padded 2 px because `glass_refl_overlay.frag` samples bilinearly.
- The rgen returns immediately on any pixel without a glass hit — no G-buffer F0, no
  Fresnel, no bounce ray. The probe no longer consults `sceneHasGlass` in this mode: the
  rect is derived from real `SURFTYPE_GLASS` surfaces, a stricter test than that flag's
  any-`MC_TRANSLUCENT` semantics, so letting it veto would silently drop glass.
- `refl_composite.frag` is skipped entirely in mode 1 — opaque pixels are never traced,
  glass is composited per-surface, and running it would read stale texels outside the rect.
- Debug modes 2-4 force the full-screen grid so the G-buffer views still work.

Consequence: **R3 (per-material F0) is no longer needed for culling** — with opaque
geometry never reflecting there is nothing to classify. If specific hero surfaces are
wanted later, R3 returns as a small opt-in list rather than a whole material table.
`minWeight` / `grazingGain` / the spec-map F0 remap now only apply in legacy mode 2.

Not yet validated in-game.

### R4b — Bound the glass probe in screen space *(superseded by R6)*

**R1-R3 do not reduce the probe at all.** It fires at `reflect_ray.rgen:146`, *before*
the early-out, and has to — glass reflections don't depend on surface F0. So in any
room containing real glass, every non-sky pixel still pays one full-screen probe ray
no matter how aggressively the opaque path is culled. R4 only helps glass-*free* rooms.

Fix: the backend already collects the frame's glass surfaces into `pendingGlass`
(`vk_backend.cpp:2190`) for the compositing overlay. Project those surfaces' bounds to
screen space, union them into one rect, and pass it in `ReflParams`; the rgen skips the
probe outside it. Glass is nearly always a small part of the frame, so this turns the
probe from a full-screen cost into a local one.

Conservative and cheap: a union rect over-covers, which is fine — it only ever probes
*more* than necessary, never less. Degenerate case (glass filling the screen) is
today's behaviour.

### R5 — Spend a little of the savings on the surfaces that matter

Only after R0-R4 are measured. The surviving pixel set should be a few percent of
screen, so per-pixel quality is affordable for the first time:

- Glass/mirror F0 from the material entry rather than the two hardcoded constants —
  `glass_probe.rchit` returns the hit `matIdx` so the rgen's `glassWeight` Schlick uses
  the real value. This, plus R3's 0.4, is what makes you visible in the glass.
- `REFL_MAX_SHADOW_LIGHTS` 8 → 16 for the reflection and player hit shaders. Reflected
  rooms currently drop shadows for lights beyond the budget; with far fewer pixels
  tracing, the budget can double and still cost less than today.
- Re-check `r_rtReflectionDistance` (2500) — for a pixel set this small, extending it
  is cheap and helps large reflective panes.

Do **not** add roughness-blurred reflections here; that stays in the backlog and is
reassessed once these numbers exist.

---

## Risks

- **Over-culling something wanted.** The classification list is small and manual by
  design, so a genuinely reflective hero surface can get missed. Mitigated by
  `r_rtReflectiveMaterials` (no rebuild needed to add one) and `r_rtReflectionMode 2`.
- **Glass F0 0.4 reads as too mirror-like** on thin panes where you should mostly see
  through. Tune from mode 5 with a real office partition in view, not from a guess.
- **R1's hoist changes evaluation order** around the glass-probe branch. The glass
  path must keep bypassing the weight test entirely — it has no meaningful surface F0.
- **Reflections stay mirror-sharp.** Nothing here adds roughness blur, so any surface
  given a non-zero F0 reads as a *clean mirror*, which is wrong for almost all of
  Doom 3's geometry. This is the reason the plan defaults opaque to matte rather than
  trying to make metals look good at reduced cost: with a sharp-only reflection model,
  "dimmer" doesn't make a metal floor correct, it just makes it a faint mirror. Giving
  metal a plausible response is a separate arc that needs the blur first.

## Exit criteria

1. Reflection ray + shadow ray counts down by a large factor on the R0 scene, logged
   before and after.
2. `Refl` drops from its 3.24 ms baseline to a small fraction of it, and — unlike today —
   *varies with scene content*, dropping near zero in the glass-free corridor.
2. Debug mode 3 near-black except glass and mirrors.
3. Metal floors read matte from every angle (mode 5 black across the sweep).
4. The player is clearly visible in a glass partition.
5. Values for every new cvar recorded in this doc before it moves to `completed/`.

## New cvars

| cvar | default | purpose |
|---|---|---|
| `r_rtReflectionMode` | 1 | 0 off · 1 classified · 2 legacy spec-map-wide |
| `r_rtReflectionMinWeight` | 0.04 | R1 final-Fresnel cull threshold |
| `r_rtSpecGrazingGain` | 4.0 | R2 grazing ceiling as a multiple of F0 |
| `r_rtReflectiveMaterials` | `mirror;glass;` | R3 opt-in substring list |
| `r_rtReflectionDebugMode 5` | — | R0 path-classification overlay |

---

*This file is a new addition with dhewm3-rt. It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL
Source Code.*

*It is distributed under the same modified GNU General Public License Version 3 of the
original Doom 3 GPL Source Code release.*
