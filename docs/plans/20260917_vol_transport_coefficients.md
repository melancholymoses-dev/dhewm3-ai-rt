# Volumetric transport coefficients — make the medium physical

**Status:** Stage 1 written 2026-09-17, not yet in-game validated. Stage 2 (tuning) open.

Follows F6/F7 in `20260906_froxel_probe_gi.md`. Prompted by a review note on
`r_rtVolAttenuateStrength` (applying `T^k` only at composite made the two halves of
the transport equation disagree, and `k<1` implied an albedo of 25).

## The finding

Both integrators are **already structurally correct** and need no rewriting. The
march carries `stepSize`, `stepTransmittance`, phase, falloff and visibility in the
right places (`vol_march.comp`), and the froxel integrate does the same
`exp(-sigma*segLen)` with a geometric-mean midpoint. The defect is entirely in what
the coefficients *mean*.

Single-scatter transport has exactly two medium constants:

```
L = ∫ T(0,t)·σ_s·p(θ)·L_light·V dt   +   T(0,D)·L_surface
T(a,b) = exp(-σ_t·(b-a))              albedo Λ = σ_s/σ_t ≤ 1
```

Mapping the old code onto it:

- **σ_t** = `r_rtVolDensity`, used for Beer-Lambert in both paths. Already single-valued
  and medium-wide — correct.
- **σ_s** = the per-class product `density × strength` in the final combine.

Which gives the implied albedos:

| class | σ_s = density × strength | σ_t | Λ |
|---|---|---|---|
| point | 0.015 × 0.85 = 0.01275 | 0.015 | 0.85 ✓ |
| directed | 0.05 × 0.90 = 0.045 | 0.015 | **3.0** ✗ |
| flashlight | 0.05 × 0.50 = 0.025 | 0.015 | **1.67** ✗ |

Two consequences:

1. **Directed lights created 3× the energy they removed.** Cranking directed density
   to make shafts read never cost anything, because it wasn't taking the energy from
   anywhere. This is why the anisotropy/visibility tuning kept feeling like it had no
   budget.
2. **σ_t = 0.015 means T = 0.5 at 46 units** — about a metre. That is dense smoke, not
   haze, and it is why F6's attenuation blacked out a corridor. The number is that
   large because it was *also* the only knob setting point-light shaft brightness, so
   it got tuned as a scattering gain. `r_rtVolAttenuateStrength = 0.04` was a fudge of
   exactly the right magnitude to undo it (0.015 × 0.04 = 6e-4 — a sane σ_t). The
   tuning found the right answer by the wrong route.

## Stage 1 — reparameterise as (σ_t, Λ, per-class gain)

Gains multiply the **light's radiance**, not the medium. That is physically
legitimate — Doom 3 light intensities are authored, not radiometric — and it keeps
the transport equation clean. A gain on the medium coefficient is what was wrong.

| new cvar | replaces | default | meaning |
|---|---|---|---|
| `r_rtVolExtinction` | `r_rtVolDensity` | 5e-4 | σ_t. T = 0.78 @ 500u, 0.61 @ 1000u |
| `r_rtVolAlbedo` | *(new)* | 0.9 | Λ; σ_s = Λ·σ_t. 0 = black smoke, 1 = bright mist |
| `r_rtVolGain` | `r_rtVolDensity` + `r_rtVolStrength` | 28 | point-light gain |
| `r_rtVolDirectedGain` | directed density + strength | 28 | (was an effective Λ of 3.0) |
| `r_rtVolFlashlightGain` | flashlight density + strength | 28 | (was 1.67) |
| — | `r_rtVolAttenuateStrength` | **deleted** | — |

7 cvars → 6, each meaning one thing.

The gain default is not arbitrary: `0.01275 / (0.9 × 5e-4) = 28.3`, the factor that
preserves today's point-light brightness. Using it for all three classes puts them on
one honest basis. **Directed shafts land ~3.5× dimmer than before** — that is the
energy creation being removed, and raising `r_rtVolDirectedGain` is then an explicit
"spots punch harder" decision rather than a hidden Λ = 3.

Composite drops `attenuation()`/`pow` entirely; `.a` is the true transmittance and the
blend becomes `dst = airlight + T·dst`.

### Tuning model after this

Three near-orthogonal knobs, which is the point of the exercise:

- **σ_t** — how far you can see; how much distance washes out.
- **Λ** — whether the medium glows or just absorbs.
- **gain** — per-class punch.

σ_t and brightness stay coupled through σ_s = Λ·σ_t, which is correct physics (less
medium ⇒ less to scatter off). "Thin but bright" is a gain increase.

## Resource usage — zero change, marginally positive

- No new images, buffers, dispatches or descriptors.
- **No UBO growth; both shrink in use.** `VolParamsUBO` stays 176 B and
  `VolFroxelParamsUBO` stays 304 B, so neither size assertion moves. All repurposed
  fields are 4-byte scalars in a contiguous run, so the slots are permuted in place:
  - march: `density`→`extinction`, `strength`→`scatterPoint`,
    `flashlightDensity`→`scatterFlash`, `flashlightStrength`→`albedo` (dump only),
    `directedDensity`→`scatterDirected`, `directedStrength`→`_pad`.
  - froxel: `densities.x`→σ_t (what `alphaOut` already writes), `strengths.xyz`→the
    three precomputed `σ_s × gain` scales. `densities.yz` freed.
- **Fewer ALU ops**: composite loses a per-pixel `pow`; the march loses 3 multiplies
  per pixel by folding the class scale CPU-side.
- Perf delta unmeasurable.

## Structural consequence — the froxel grid gets longer

`dFar = -ln(r_rtVolFroxelFarTransmittance)/σ_t` = ~12,400 units at the new σ_t, so it
clamps to `r_rtVolMaxDist` and the grid covers the **full 512** rather than today's
414. This fixes the standing "shafts only appear near the camera" complaint for free.

Cost: far cells grow ~33 → ~40 units deep, so wall-straddle gets marginally worse.
`r_rtVolFroxelFarTransmittance` becomes inert at the default — keep it as a clamp for
anyone who raises σ_t, do not delete it.

## Validation

`r_rtVolDebugMode 1` is the direct before/after. It should read near-white across a
room and visibly grey down a long corridor. Today it is near-black a few units out —
that image *is* the bug.

Both dumps print σ_t, Λ, and the per-class **effective albedo** `σ_s·gain/σ_t`, so
energy creation shows as a printed number > 1 instead of a look to second-guess.

## Risks

| Risk | Signal | Mitigation |
|---|---|---|
| Whole look changes scale; no reference left | Everything reads wrong at once | Gains default to the brightness-preserving 28, so the first build should land near today for point lights |
| Directed/flashlight deliberately dimmer | "The shafts got weaker" | Correct and intended; raise the per-class gain, and note it is now visible as albedo > 1 in the dump |
| Renamed cvars break saved configs / autoexec | Silent revert to defaults | Old names are gone, not aliased — `r_rtVol*Density`/`*Strength` in any cfg will warn as unknown. Intentional: silently mapping an old 0.015 onto the new σ_t would be 30× too thick |
| A missed call site keeps reading a deleted cvar | Link error, not a silent bug | Full audit done: `vk_vol.cpp`, `vk_vol_froxel.cpp`, `vol_march.comp`, `vol_froxel_fill.comp`, `vol_composite.frag`, `Dhewm3SettingsMenu.cpp`. `vk_gi.cpp` reads none of them |

## Stage 2 — tune from the new basis

Open. Pick σ_t from desired visibility distance, Λ from how much the haze should glow,
then per-class gain for punch. No tuning pass has happened yet, so there is nothing to
preserve.
