# Reflection Brightness — make the surviving pixels read

**Date:** 2026-09-18
**Status:** B0 and B1 landed 2026-09-19. B2-B5 not started. **B3 is blocked** on
`20260919_dynamic_model_normals.md` — the reflected subject is lit from the wrong direction,
so nothing about reflection brightness can be judged against it until that lands.
**Follows:** `20260911_reflection_gating.md` — that doc decided *which pixels trace*
(R1/R2/R4/R6, landed 2026-09-12). This one is its unfinished R5: *what those pixels
are worth*. R5 is moved here in full and expanded; the gating doc keeps R0-R6.

---

## The complaint

Reflections are barely visible, and the player's reflection in glass carries harsh
vertical shadow bands down the body.

Two separate problems. The bands are a **bug**; the dimness is an **accumulation of
correct-in-isolation factors** that nobody multiplied out.

---

## Diagnosis

### The multiplier chain for "player seen in a glass pane"

| # | Factor | Value | Where |
|---|---|---|---|
| 1 | player albedo | ~0.3 | `rt_SampleDiffuse` |
| 2 | irradiance: `Σ lightRGB × NdotL × atten` | ~0.3 | `rt_light_eval.glsl:179` |
| 3 | **missing `r_lightScale`** | **×1, should be ×2** | `vk_gi.cpp:1382` |
| 4 | **no indirect term** — flat `REFL_AMBIENT` | **+0.01** | `player_reflect.rchit:56` |
| 5 | occluded lights | ×0.15 (`r_rtReflAmbientScale`) | `rt_light_eval.glsl:257` |
| 6 | `r_rtReflectionBlend` | ×1.0 | `reflect_ray.rgen:282` |
| 7 | **glass Schlick weight** | **×0.15 head-on** (`GLASS_F0`) | `reflect_ray.rgen:277` |
| | **radiance added to hdrScene** | **≈ 0.013** | `glass_refl_overlay.frag:55` |

### Three findings

**F1 — RT direct lighting is half the raster path's, everywhere.**
`RB_DetermineLightScale` sets `backEnd.lightScale = r_lightScale` (default **2**) with
`overBright = 1.0` (`tr_render.cpp:588-591`); `tr_render.cpp:872-874` bakes it into
`lightColor[]` and `interaction.frag:162` applies `overBright` on top. The RT light
upload takes raw `shaderParms` RGB (`vk_gi.cpp:1339-1341`) and pins
`intensity = 1.0f` (`vk_gi.cpp:1382`), so `rt_light_eval.glsl:179` computes
`lColor × 1.0 × NdotL × atten`. **Every RT hit — reflections, player reflections, GI
bounce, volumetrics — is lit at 0.5× what the raster path gives the identical surface.**
Pre-existing; it was masked while reflections were over-bright for other reasons.

**F2 — reflected surfaces receive no indirect light.**
The primary view gets direct + GI composite + vol. A surface seen *in* a reflection gets
direct only, plus a hardcoded `REFL_AMBIENT = 0.01`. GI is screen-space —
`gi_albedo_mod.comp:69` modulates by the *primary* G-buffer albedo, and reflected
geometry is not in that G-buffer. Anything shadowed in the reflection collapses to
`0.01 + contrib × 0.15`.

**F3 — the tonemap toe finishes the job, but only in dark rooms.**
`r_rtTonemapToe` defaults to **2.7** with `linearStart 0.3` (`vk_tonemap.cpp:51`).
Reflections composite additively into hdrScene *before* tonemap, so what reaches the
screen is the added radiance × the curve's local slope. Evaluating the full blended
curve (`tonemap.comp:56-79`), not the toe branch alone:

| base scene luminance | curve out | /255 | local slope | +0.013 refl shows as |
|---|---|---|---|---|
| 0.02 | 0.0005 | 0.1 | 0.064 | **0.2/255** |
| 0.05 | 0.0059 | 1.5 | 0.325 | **1.1/255** |
| 0.10 | 0.0374 | 9.5 | 0.944 | 3.1/255 |
| 0.15 | 0.0981 | 25.0 | 1.435 | 4.8/255 |
| 0.20 | 0.1742 | 44.4 | 1.535 | 5.1/255 |

**Correction worth recording:** the slope exceeds 1.0 between ~0.12 and ~0.3, because the
`smoothstep` blend from toe to linear section is steeper than either. The toe therefore
*amplifies* additions in mid-dark regions and only crushes them below ~0.07. So there are
two regimes, and they need different fixes:

- **Lit rooms** (base > 0.1): the reflection lands at 3-5/255. That is the multiplier
  chain being genuinely too small. Tonemapping is not the problem here.
- **Dark rooms** (base < 0.07): the same reflection lands under 1/255. The toe is the
  problem here, and no reflection-side gain that keeps pillar 2 will fix it.

Fix the chain first. The toe is a separate, later decision.

**F4 — the player self-shadows in reflections, and nowhere else.**
Instance masks: `noSelfShadow` entities get `0x01`, world gets `0xFE`
(`vk_accelstruct.cpp:1205`, `:1580`). Every other consumer honours the split — the
primary shadow pass uses `rayCullMask = 0xFE` (`shadow_ray.rgen:367`), the glass probe
uses `0xFEu` (`reflect_ray.rgen:159`). **`rt_TraceLightShadow` hardcodes `0xFF`**
(`rt_light_eval.glsl:207`), so shadow rays fired from `player_reflect.rchit` hit the
player's own mesh.

A low-poly articulated character shaded from interpolated vertex normals but
shadow-tested against faceted geometry is the textbook shadow-terminator case; the flat
`REFL_SHADOW_BIAS = 0.5` does not cover it. The bands run along limbs and torso, which
reads as vertical lines. Doom 3 marks the player `noSelfShadow` precisely because this
model never looks right self-shadowed.

### Not the cause — checked and ruled out

- **Albedo is not double-applied.** `gi_ray.rchit:179` multiplies by the *emitter's*
  albedo; `gi_albedo_mod.comp:69` by the *receiver's*. Correct pair. It does mean GI
  carries albedo² ≈ 0.09 before `r_rtGIStrength` (0.20) × `bounceScale` (2.0), so GI is
  dim for the same reason as F1/F3 — not for a different one.
- **The gating work is sound.** R1/R2/R4/R6 decided *where* to trace and were measured.
  They did not change any radiance term. Reverting them would restore brightness only by
  restoring the artifacts and the 3.2 ms.
- **Glass probe geometry is correct.** `glass_probe.rchit` interpolates vertex normals,
  transforms by `transpose(mat3(gl_WorldToObjectEXT))`, and orients toward the incoming
  ray. Reflection origin/direction are right.

---

## Target behaviour

> A pane of glass shows you at a brightness that reads in a lit room without violating
> pillar 2 in a dark one. Nothing in the chain is a fudge factor compensating for a
> different stage's missing term.

---

## Stages

Ordered so each is independently landable and independently revertable. **One stage per
session; stop at its exit gate.** B1 and B2 both change things outside reflections and
must not be stacked before validation.

---

### B0 — Instrument first *(pillar 6)*

The R0 debug modes were specified and never built. They are the only way to tell F1/F2
(dark subject) from the glass weight (weak interface) — those two multiply, and no
amount of looking at the composite separates them.

**`reflect_ray.rgen`, alongside the existing modes 2-4** (same pattern: checked before
any trace, written straight to `reflImage`, `refl_composite.frag` displays with blending
off). Both need the full-screen launch grid, so extend the existing
`debugMode >= 2 → force full-screen` condition in `VK_RT_DispatchReflections`.

- **Mode 5 — path classification.** Colour per pixel by the branch taken, written *after*
  the glass probe and Fresnel cull but before the bounce loop:
  black = culled by early-out · blue = opaque reflective · green = glass ·
  yellow = `rt_ReconstructNormal` fallback. Tells you at a glance what the glass rect
  actually covers.
- **Mode 6 — raw `accum`, pre-Schlick.** For glass pixels only, `imageStore(accum, 1.0)`
  instead of `accum * reflBlend` with negated alpha. **This is the decisive measurement.**
  If mode 6 already shows a dim player, B2/B4 are the fix and B3 is cosmetic; if mode 6
  shows a well-lit player, B3 alone is the fix.
- **Mode 7 — `accum` scaled ×8.** Same as 6 with a fixed gain, so the subject is readable
  through the tonemap toe while judging it. Mode 6 alone inherits F3.

Update the `r_rtReflectionDebugMode` description string in `vk_reflections.cpp:91-102`.

**Exit:** a screenshot of a glass pane in modes 5, 6 and 7 from the same viewpoint, and
a recorded verdict on which of "weak interface" / "dark subject" dominates. Every stage
below is re-validated against mode 6/7, not against the composite.

#### Landed 2026-09-19 — modes 5/6/7 in `reflect_ray.rgen`

Range checks became `2..REFL_DEBUG_MAX_MODE` (= 7, in `vk_raytracing.h`) in three places:
the full-screen decision in `VK_RT_DispatchReflections`, the replace-blend pipeline pick in
`VK_RT_CompositeReflections`, and the per-surface glass overlay in `VK_RB_DrawShaderPasses`
— that last one had been re-applying the reflection additively on top of the debug output
during modes 2-4 as well.

Two deviations from the spec: modes 6/7 write raw `accum` at the opaque store too (one
`return` before `if (isGlass)` covers both), and mode 5's yellow is unreachable — the
`rt_ReconstructNormal` fallback needs `gbuf.a <= 0`, but the early-out above it already
culls on `f0 < 1/255` and `f0` *is* `gbuf.a`. Branch kept, commented; relevant to B4.

#### Verdict, Central Access, 2026-09-19

- **Mode 5: two green panes, no blue anywhere.** Debug forces full-screen legacy tracing,
  so every opaque pixel was offered to the trace and every one was culled before it.
  Glass-only mode loses nothing in this view.
- **The player is the dominant problem, and it is F4.** Inside mode 7 alone, the floor
  beside him reads 190-240/255 while his torso and legs read 1-39/255 — adjacent pixels,
  same reflected room, ~10-100× apart. No global scalar can open that gap. Face and one
  arm survive, the rest self-occludes. → **B1.**
- **For the reflected world the interface weight binds, not the chain.** Mode 6 shows the
  world at ~1-17/255, peak 49 — dim but legible; the composite shows nothing there. That
  gap is the ×0.15 `GLASS_F0`. → **B3 is not cosmetic.**
- **B2's 2× is safe but is not the answer.** Mode 7's ×8 already saturates the world to
  white, so 2× fits; it does nothing for the player gap.

**Follow-up, not yet done:** debug modes 2-7 force `reflMode` to 2 (`vk_reflections.cpp:1105`
— `glassOnly = (mode == 1) && !reflDebugActive`), so every opaque pixel is traced and gated
only by F0/minWeight. That is what the B0 spec asked for, but it means mode 6/7 cannot show
what shipping mode 1 actually produces — raise the Fresnel knobs and metal reflections
appear that normal play never pays for. The debug modes should honour `r_rtReflectionMode`
and force full-screen only for mode 5, whose job is coverage.

Caveats: the camera moved between the mode 6 and mode 7 captures, so only within-image
comparisons are load-bearing (all of the above are). And **modes 6/7 are tonemapped** —
the debug composite writes into hdrScene before `tonemap.comp`, so those are
`tonemap(accum)`, not linear `accum`. That is why mode 7 has to exist; a later pass could
bypass the tonemap if linear numbers are ever needed.

---

### B1 — Stop the player self-shadowing in reflections *(the vertical bands)*

Smallest, most certain, zero tuning. Land it on its own.

`rt_light_eval.glsl` currently hardcodes the shadow cull mask. Thread it through as a
parameter rather than changing the constant, because the three includers want different
values:

| includer | wanted mask | why |
|---|---|---|
| `gi_ray.rchit` | `0xFF` | the player *should* occlude bounce light in the room |
| `reflect_ray.rchit` | `0xFF` | the player *should* cast shadows into the reflected world |
| `player_reflect.rchit` | **`0xFE`** | `noSelfShadow` — matches `shadow_ray.rgen:367` |

**Implementation.** Follow the file's existing "no uniform block here" contract
(`rt_light_eval.glsl:25-28`) — the value arrives as a function argument, not from a UBO:

1. Add `uint cullMask` to `rt_TraceLightShadow(...)` and pass it to `traceRayEXT` in
   place of the literal at `:207`.
2. Add `uint cullMask` to `rt_EvalDirectLighting(...)` and
   `rt_EvalDirectLightingStochastic(...)`; forward it at the three internal call sites
   (`:255`, `:335`, `:344`). Put it last so the existing argument order is untouched.
3. Callers: `0xFFu` from `gi_ray.rchit:158`/`:166` and `reflect_ray.rchit:137`;
   **`0xFEu`** from `player_reflect.rchit:79`.
4. `#define REFL_SELF_SHADOW_MASK 0xFEu` in `player_reflect.rchit` next to the other
   `REFL_*` constants, with a one-line note pointing at `vk_accelstruct.cpp:1205`.

**If bands persist after the mask change** it is the shadow-terminator term, not
self-occlusion, and the follow-on is a slope-scaled bias in `rt_TraceLightShadow`
(`shadowBias / max(dot(hitNorm, lightDir), 0.1)`) rather than the flat 0.5. Do not do
both at once — the mask change alone should be conclusive, and doing both hides which
one worked.

**Validate:** stand at a glass pane, sweep the view so the body faces several lights.
Bands gone, silhouette unchanged, world geometry reflected in the same pane unchanged
(proves `0xFF` survived on the other two paths). `r_rtReflAmbientScale 0` makes any
residual self-shadow maximally obvious — use it as the stress setting.

**Exit:** bands gone in-game; GI and world reflections visually unchanged.

#### Landed 2026-09-19 — awaiting in-game check

`cullMask` threaded through as a trailing argument on `rt_TraceLightShadow`,
`rt_EvalDirectLighting` and `rt_EvalDirectLightingStochastic`, per the file's no-UBO
contract. `0xFFu` from `gi_ray.rchit` (both paths) and `reflect_ray.rchit`;
`REFL_SELF_SHADOW_MASK` (`0xFEu`) from `player_reflect.rchit`. All five includers of
`rt_light_eval.glsl` compile; `gi_probe_trace.rgen` and `vol_march.comp` include it for
the light SSBO only and call none of these.

**Check against mode 7, not the composite** — B0 showed the composite is in F3's
dark-room regime at Central Access. Expect the player's body to come up toward the
190-240/255 the floor beside him already reads. If it doesn't, it's the shadow-terminator
term, not self-occlusion — see the slope-scaled bias note above, and do not stack it.

---

### B2 — Restore `r_lightScale` in the RT light upload

Fixes F1 at the source. **Blast radius: GI, volumetrics and reflections simultaneously.**
Expect the scene to look wrong afterwards until retuned — that is the tuning-absorbs-error
pattern the froxel fog retune already went through, not a regression.

**The channel already exists.** `GILightEntry::colorIntensity[3]` is documented
(`vk_gi.cpp:1379-1381`) as reserved for exactly this: *"kept as an explicit 1.0 rather
than deleted ... for a future source to drive deliberately. The rule is only that it must
not silently inherit parm3."* No struct change, no descriptor change, no shader change —
`rt_light_eval.glsl:179`, `vol_march.comp:334` and `vol_froxel_fill.comp:147` all already
multiply by it.

**Implementation** (`vk_gi.cpp`, the `intensity` constant at `:1382`):

```
// F1: the raster path multiplies every light colour by r_lightScale (tr_render.cpp:590)
// and again by overBright (interaction.frag:162). RT read the raw parms, so every RT hit
// was lit at half the raster path's level. Drive the reserved intensity channel with it.
const float intensity = r_rtLightScaleMatch.GetBool()
                      ? r_lightScale.GetFloat() * backEnd.overBright : 1.0f;
```

- New cvar `r_rtLightScaleMatch`, `"1"`, `CVAR_RENDERER | CVAR_BOOL`, described as
  "match the raster path's r_lightScale/overBright in RT light evaluation; 0 = legacy".
  It is the A/B control and the bisect handle if GI or vol regress.
- `r_lightScale` is declared `extern` in `tr_local.h:873`; `backEnd.overBright` is
  set by `RB_DetermineLightScale` before the backend runs. Confirm the GI light upload
  happens after that in the frame, or use `r_lightScale` alone and note the omission —
  `overBright` is 1.0 unless a light's scaled registers exceed
  `tr.backEndRendererMaxLight`, which is the uncommon case.
- **Do not** apply it in `gi_ray.rchit` or the reflection hit shaders instead. Putting it
  on the light makes it conserve energy by construction and keeps one source of truth —
  the same argument the volumetrics doc settled on 2026-09-18 ("a gain on the light is
  legitimate; a gain on a medium coefficient is not").

**Retuning that follows, in this order:** `r_rtGIStrength` (0.20) and
`r_rtGIBounceScale` (2.0) both sit downstream and will now over-deliver — halve one, not
both, and record which. Volumetric per-class radiance gains were tuned in play on
2026-09-18 against the un-scaled light and will need the same halving.

**Validate:** mode 6/7 before/after on the same pane — the reflected subject should be
exactly 2× brighter, nothing else about it changed. Then `r_rtGI 0 / r_rtVol 0` and
confirm direct raster lighting is untouched (it must be — this path never feeds the
raster interaction).

**Exit:** 2× confirmed in mode 6; GI and vol retuned back to their pre-B2 look with the
new constants recorded in this doc.

---

### B3 — Glass F0 from the material entry *(carry-over of R5)*

Two hardcoded, mutually inconsistent constants: `GLASS_F0 = 0.15` (`reflect_ray.rgen:277`,
the camera-facing interface) and `F0 = 0.1` (`reflect_ray.rchit:99`, secondary panes).
0.15 is physically right for a clean pane head-on and is *why you can't see yourself* —
the gating doc said so and R5 never landed.

**Implementation:**

1. `VkMaterialEntry` (`vk_raytracing.h:71-85`) gains `float reflF0;` — 36 → 40 bytes.
   Bump `static_assert(sizeof(VkMaterialEntry) == 36)` at `:86` to 40 and mirror the
   field in `MaterialEntry` (`rt_material.glsl:28-40`). std430 on a struct of 4-byte
   scalars needs no padding, but re-check the SSBO stride after the change.
2. `vk_material_table.cpp`, beside the existing flag classification: `MC_TRANSLUCENT &&
   SURFTYPE_GLASS` → **0.4**; `SS_SUBVIEW` (mirror) → 0.9; everything else **0.0**.
   0.4 is not physical for glass — it is the deliberate "set dressing reads" number from
   pillar 4, and this doc is where that is recorded.
3. `GlassProbePayload` (`glass_probe_payload.glsl`) gains `float f0;` in place of `pad`
   — the slot is already there, so the payload size does not change.
   `glass_probe.rchit` writes `materials[matIdx].reflF0` where it currently writes
   `hitNormal` (it has `matIdx` in hand at `:44` and already validates `REAL_GLASS`).
4. `reflect_ray.rgen:277` uses `glassProbe.f0` instead of the `GLASS_F0` constant;
   `reflect_ray.rchit:99` uses `mat.reflF0` instead of its own `0.1`, keeping
   `transmit = 1.0 - F0`.
5. Opaque geometry never reads `reflF0` in mode 1 — it exists for mode 2 and for a
   future hero-surface opt-in. Leave it 0.0 and do not re-add a spec-map path; pillar 4
   and the 2026-09-12 decisions settled that.

**Risk:** 0.4 makes glass noticeably mirror-like. If it reads as a mirror rather than a
pane, the number is the knob, not the mechanism — expose it as `r_rtGlassF0` before
arguing about it.

**Validate:** mode 6 (raw `accum`) must be *identical* before and after — B3 changes only
the interface weight. The composite gets ~2.7× brighter glass. If mode 6 moved, something
else changed too.

**Exit:** in-game screenshot of the same pane at F0 0.15 / 0.4 / 0.6, and a recorded
choice.

---

### B4 — An indirect term at reflection hits *(fixes F2)*

`REFL_AMBIENT = 0.01` is a placeholder standing in for everything GI and volumetrics
give the primary view. This is the structurally correct fix and the largest piece of
work; do not start it before B0-B3 are measured, because they may make it unnecessary.

**Cheap version — scale the existing floor.** Replace the literal with a cvar
(`r_rtReflAmbient`, default 0.01) in both hit shaders, driven through the existing
`RTLightBuf` header — `GILightBuffer` has `float pad[3]` (`vk_gi.cpp:227`) with two slots
free after this, so no struct-size change and no descriptor churn. **This is a fudge and
must be labelled one:** a constant floor violates pillar 2 by lifting dark reflections
uniformly. Useful as a bisect step to size the real term, not as a shipped answer.

**Real version — sample the probe GI field at the reflected hit point.** G2 already built
the machinery: `gip_SampleIrradiance(GIProbeSite, out vec3, out float, out float)`
currently lives in `gi_probe_resolve.comp:373`, not the shared header.

1. Move `gip_SampleIrradiance` and the `GIProbeSite` construction into
   `gi_probe_common.glsl`, parameterised on the atlas samplers rather than reading the
   resolve's binding-0/4 declarations directly.
2. Bind the irradiance atlas, distance atlas, `GIProbeParams` UBO and probe-state SSBO
   into the **reflection** pipeline's set 0 at new bindings (6-9 are free; 0-5 are TLAS,
   reflImage, depth, params, lights, gbufNormal). All four must be added to the hit
   stages, not just raygen — the same `VK_SHADER_STAGE_*` omission that silently killed
   all volumetric lighting during the cookie work.
3. `reflect_ray.rchit` and `player_reflect.rchit` replace `vec3(REFL_AMBIENT)` with the
   sampled irradiance × the hit albedo, falling back to the constant when the probe grid
   is invalid or `r_rtGIProbes 0`.

**Ordering constraint.** Probe GI is not the default (`r_rtGIProbes 0`, pending G6 in
`20260906_froxel_probe_gi.md`). B4's real version is therefore gated on that arc, and
until then reflections get the cheap version or nothing. **This is the reason B4 is last
and the reason it may be deferred entirely** — say so explicitly rather than half-landing it.

**Validate:** a reflected surface in shadow should pick up room colour, not grey.
`r_rtGIProbes 1` vs `0` must change the reflection, which is also the proof the binding
actually reached the hit stages.

**Exit:** deferred-with-reason, or landed with a before/after on a shadowed reflected wall.

---

### B5 — The dark-room regime *(decide; do not tune blind)*

F3's second regime. Below base luminance ~0.07 the tonemap slope is under 0.33 and no
reflection-side gain that respects pillar 2 will make the reflection visible.

Three mutually exclusive positions. **Pick one and record it; do not tune toward a
compromise.**

1. **Accept it.** Glass in a dark room shows nothing. Consistent with pillar 2 and with
   Doom 3's art direction. Costs nothing.
2. **Lower `r_rtTonemapToe`** (2.7 → ~2.0). Global. Also lifts real shadow detail, which
   the 2026-09-18 volumetrics decision already warned against reaching for
   ("albedo is the lever, not the toe").
3. **Bloom.** `20260906_bloom_plan.md` is unimplemented and its prerequisite (the
   tonemapped HDR pipeline) now exists. A bright reflection in a dark room is exactly
   what bloom is for, and it lifts the highlight without lifting the floor — the only
   one of the three that does not fight pillar 2.

**Recommendation:** 1 now, 3 later as part of the bloom arc. Do not do 2.

**Exit:** a decision line added to `ROADMAP.md`.

---

## Zero-code experiments — run these before writing any of the above

Three of the four causes are cvar-reachable. Each isolates one term.

| Setting | If the reflection brightens | Conclusion |
|---|---|---|
| `r_rtReflectionBlend 4` | yes | it is a pure scalar shortfall — B2+B3 sized right |
| `r_rtTonemapToe 1.5` | yes, only in dark rooms | confirms F3's two-regime split; B5 |
| `r_rtReflAmbientScale 0.6` | bands soften into a wash | confirms F4 is the shadow term, not geometry — B1 |
| `r_rtGIStrength 0.6` | GI brightens proportionally | GI is dim for the same reason (F1), not a separate one |

`r_rtReflectionMode 2` restores full-screen legacy reflections for comparison; it will
re-introduce the mirror specks that R6 removed, so it is a diagnostic, not a fallback.

---

## Summary of the priority

| | Fixes | Confidence | Blast radius |
|---|---|---|---|
| **B1** | vertical bands on the player | **high** — straight inconsistency with two other call sites | reflections only |
| **B2** | F1, half-brightness everywhere | **high** — provably missing vs. raster | GI + vol + refl; needs retune |
| **B3** | the glass interface weight | **high** — already specified as R5 | glass only |
| **B4** | F2, no indirect at reflected hits | medium | needs probe GI arc |
| **B5** | F3, dark-room regime | decision, not a fix | global |

B1 and B3 are small and independent. B2 is small but changes three subsystems and must
land alone.
