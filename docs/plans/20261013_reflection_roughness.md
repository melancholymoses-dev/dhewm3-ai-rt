# Surface roughness in reflections

Extend reflections past glass to opaque surfaces by giving them a roughness lobe instead of
a mirror. Settled conclusion (`../rt_engine_rules.md`): sharp mirror reflection is *why* opaque geometry
looked wrong, so no F0 threshold can fix it — thresholds cull from the bottom while the
worst artifacts are the highest-F0 pixels. Roughness changes the reflection's character
rather than its magnitude, which is the only lever that can work.

Two blockers sit in front of the lobe itself, and they set the stage order.

| Blocker | State |
|---|---|
| Reflections have **no temporal or spatial filter** | AO has `temporal_resolve.comp`, GI has `gi_temporal_resolve.comp`, vol reuses GI's. `reflImage` is traced and composited in the same frame, no history |
| At high roughness a specular lobe **is** the diffuse hemisphere | Probe GI already integrates that. Adding both double-counts indirect light and lifts the noise floor of dark regions (`../rt_engine_rules.md` pillar 2) |

A sharp mirror is a deterministic one-ray query — no estimator, no variance. A lobe is
importance-sampled, so one ray per pixel is a noise field with nothing downstream to resolve
it. Tuning lobe constants through un-denoised noise fits the constants to the variance.

## F0 stays

F0 is reflectance at normal incidence; roughness is lobe width. GGX needs both. Per-material
F0 (T3) was dropped because sharp mirrors made classification pointless — the condition
roughness removes. **Do not run cleanup Phase 2b/2c before this lands**: 2c reclaims the
G-buffer alpha that holds F0, and that is the one irreversible step.

---

## Stage 1 — Reflection denoiser (still glass-only)

Required regardless of how roughness is sourced, and independently useful: it lets glass
trade rays for filtering, or affords a second bounce.

| File | Change |
|---|---|
| `vk_reflections.cpp` | Per-slot `reflHistory` rgba16f at render res + `reflMoments` for variance. Follow `vk_temporal.cpp`'s AO pattern |
| `glsl/refl_temporal_resolve.comp` | **New.** Reproject by `motionVectors`, EMA with variance-driven alpha, disocclusion reject on depth + normal |
| `CMakeLists.txt` | New shader |
| `vk_backend.cpp` | Dispatch after `VK_RT_DispatchReflections`, before the composite |
| `vk_reflections.cpp` | `r_rtReflDenoise` (0 off / 1 temporal / 2 temporal+spatial), `r_rtReflDenoiseAlpha` |

Alpha must be adaptive from the start, not a fixed low value — a fixed EMA denoises the
estimator *and* low-passes the scene, and glass reflections move with the camera.

Debug mode 11: history age / rejection mask. A binary reject map rendering as a gradient
means the composite is filtering the overlay.

**Check:** `r_rtReflDenoise 0` is bit-identical to today. Mode 1 on Mars City glass shows no
added lag on a fast strafe and no ghosting on the player reflection. Cost ≤ 0.3 ms at 1440p.

**Addendum (2026-10-16): `r_rtReflDenoise 2`, the spatial half of mode 2, implemented.**
Manual testing of Stage 3/4 surfaced visible single-sample GGX grain on high-roughness
opaque surfaces (expected — see Stage 3's design, one sample per pixel, temporal-only
convergence) — worst right after a disocclusion, before the EMA has caught up. Added:

| File | Change |
|---|---|
| `glsl/refl_atrous.comp` | **New.** B3-spline À-trous spatial filter, same N-pass ping-pong shape as `gi_atrous.comp` (depth + luminance edge-stopping), run on the temporal resolve's output |
| `vk_reflections.cpp` | `VK_RT_DispatchReflAtrous`, wired into `VK_RT_InitReflections`/`ShutdownReflections`/`ResizeReflections`; `r_rtReflAtrousIterations/SigmaL/SigmaZ/RoughnessGain`; `VK_RT_GetReflReadView` prefers the à-trous output over raw `reflHistory` when it ran this frame |
| `vk_backend.cpp` | Dispatch after `VK_RT_DispatchReflTemporal`, before GI. New profiler phase `VK_RTPROF_PHASE_REFL_ATROUS` |
| `CMakeLists.txt` | New shader |

The filter is **roughness-gated per pixel**, not a flat blur: `refl_atrous.comp` mixes the
filtered and unfiltered colour by Stage 2's packed roughness (`mix(cCenter, filtered,
clamp(roughness * roughnessGain, 0, 1))`), so a roughness-0 pixel — glass-adjacent trim,
GUI screens, anything sharp — is written back byte-for-byte unchanged, and only genuinely
rough surfaces pay for the blur. This was the point: a flat spatial filter would have cost
the sharp reflections' crispness to fix noise that only exists on the rough ones.
Glass (negative alpha sentinel) and untraced/non-reflective pixels (alpha ≤ 1/255, the same
threshold `reflect_ray.rgen`'s own F0 early-out uses) are excluded both as filter centres
and as neighbour samples — letting either contaminate a neighbour's blend would paint a
reflection onto, or blur the wrong surface into, a pixel that was deliberately excluded.

**2026-10-16, same day:** raw `roughness` as the mix factor washed out too little on
surfaces with low-but-nonzero Toksvig/floor roughness — the user wanted more wash, not
more passes. Added `roughnessGain` (`r_rtReflAtrousRoughnessGain`, default 3.0, menu
"Spatial Filter Wash") multiplying roughness before the clamp, so moderate roughness
reaches a full blend sooner. Roughness 0 is excluded before the gain is ever applied, so
glass and screens stay exactly as sharp regardless of the gain setting.

**2026-10-16, later same day:** more smoothing didn't fix the real complaint — bright
single-pixel flashes at texture creases, there and gone as the view rotates. Root cause:
Stage 3's GGX lobe is one sample per pixel with no light importance sampling, so a wide
lobe (which is exactly what high roughness gives it) occasionally points straight at
something far brighter than its surroundings. The luminance edge-stop in the blend can't
remove this — an outlier has near-zero weight against normal neighbours, so it survives
unblurred as its own centre value regardless of iteration count, which is why cranking
`r_rtReflAtrousRoughnessGain`/iterations did nothing for it.

Added a local firefly clamp in `refl_atrous.comp`, run before the edge-stopped blend:
each of the 9 taps (and the centre) is scaled down if its luminance exceeds its own 3x3
neighbours' mean by more than `r_rtReflAtrousFireflyClamp` (default 3.0, menu "Firefly
Clamp", 0 disables). The reference mean excludes the centre tap itself so a firefly can't
raise its own ceiling. This is local and relative, not a flat brightness cap — a surface
that is uniformly bright (a real reflected screen, a sunlit wall) is unaffected, since its
whole neighbourhood is bright together; only a texel that disagrees sharply with its
immediate surroundings gets pulled down.

Push constants grew from 40 to 44 bytes (`AtrousReflPC`/`PC` in `vk_reflections.cpp`/
`refl_atrous.comp`) for the new `fireflyClamp` field.

Also confirmed for the user: the reflection buffer stores the *hit surface's* own shaded
colour (`reflect_ray.rchit`: `diffuse.rgb * irradiance` at the hit point, or raw emissive
for a live screen) weighted by the *reflecting* surface's Schlick Fresnel — it is not
multiplying the reflecting surface's own albedo into the result. `roughnessGain` only
changes how much of the spatial blend applies at a given roughness; it has no effect on
radiance magnitude, which is why it couldn't have capped the bright flashes.

**2026-10-16, third addendum:** the firefly clamp helped, but the user wanted a second,
separate knob — a flat ceiling on overall reflection brightness, not a per-pixel outlier
test. Added `r_rtReflMaxLuminance` (default 0, unbounded; menu "Reflection Max
Luminance"): applied in `reflect_ray.rgen` to the final opaque radiance (after reflBlend
and Fresnel, so it reads the same units as what gets composited), via a new
`ReflClampLuminance` helper. Glass is unaffected.

| File | Change |
|---|---|
| `vk_reflections.cpp` | `r_rtReflMaxLuminance` cvar; `ReflParamsUBO.maxLuminance` (reused the struct's last pad float — offset 268, block still 272 bytes, no `static_assert` change); fill in `VK_RT_DispatchReflections` |
| `reflect_ray.rgen` | `ReflParams.maxLuminance`; `ReflClampLuminance()`; applied at the opaque `imageStore` |
| `Dhewm3SettingsMenu.cpp` | "Reflection Max Luminance" slider, next to Roughness Scale (not gated by denoise mode — this runs in the raygen, independent of `r_rtReflDenoise`) |

Deliberately a *different* mechanism from the à-trous firefly clamp: that one is local
(a texel is only pulled down if it disagrees with its own neighbours, so a uniformly
bright reflected screen or sunlit wall is untouched); this one is absolute (anything
over the ceiling gets capped regardless of its neighbours). The user wanted both —
local for the sampling spikes, absolute for tuning the reflection layer's overall
brightness down independent of that.

Mode 11 (history age/rejection) always reads the raw temporal output, never the à-trous
one — `VK_RT_GetReflReadView` special-cases it, since the debug tint's alpha is forced to
1.0 and would otherwise be blurred by the filter, muddying the red/green boundary the mode
exists to show precisely.

**Addendum check:** `r_rtReflDenoise 1` vs `2` on the same rough opaque surface (needs
`r_rtReflectionMode 2` or `3` to have any opaque surface to look at) — `2` should show
visibly less single-sample speckle in the first ~0.25s after a disocclusion, while a
roughness-0 surface (glass trim, a GUI screen) in the same shot stays pixel-identical
between the two settings.

---

## Stage 2 — Roughness source

Doom 3 has **zero** per-material gloss: specular is a global NdotH lookup table
(`interaction.frag:35`), the same curve for concrete, steel and glass. The specular map's
RGB is already fully consumed synthesizing F0, so sourcing roughness from it collapses F0
and roughness onto one curve — polished dark metal and rough aluminium become
inexpressible.

Use **bump-map normal variance** instead (Toksvig/LEAN): the shortening of the filtered
normal in a bumpmap's mip chain measures sub-pixel normal variance, which converts directly
to roughness. Doom 3 ships bumpmaps for nearly everything, so this is per-pixel, spatially
varying, and independent of F0. It also fixes specular aliasing rather than causing it —
high-frequency normal detail under a delta BRDF is the pathological case.

Channel: both G-buffers are `R8G8B8A8_UNORM`, and both alphas are taken (`gbufNormal.a` =
F0, `gbufAlbedo.a` = 3-level surface class).

| Option | Cost | Verdict |
|---|---|---|
| Pack class (2 bits) + roughness (6 bits) into `gbufAlbedo.a` | free; 2 consumers to migrate (`rt_player_exclude.glsl`, `fsr_tc_mask.comp`) | **Recommended.** 64 roughness levels is ample for a lobe width |
| Split `gbufNormal.a` 4+4 F0/roughness | free | F0 drops to 16 levels and shifts the `f0 < 1/255` early-out |
| New `R8_UNORM` attachment | ~10 pipeline sites | Last resort; the project has paid this before |
| Widen `gbufNormal` to RGBA16 | bandwidth on GPU-bound frames | No. Roughness smears the 8-bit normal error anyway |

The class migration changes "threshold the alpha" to "extract the top 2 bits" — mechanical,
but it invalidates the current `never 1 - alpha` rule, so update it with the code.

**Roughness floor.** Toksvig is a specular-AA signal, not authored material data: it only
measures the bump map's own normal variance, so a clean/flat bump map reads as roughness 0
regardless of how matte the material is meant to look — confirmed in mode 12, where large
flat panels stayed near-black between the grates/rivets. `r_rtReflRoughnessFloor` (default
0.2) is a flat minimum added in `gbuffer.frag`/`gbuffer_clip.frag` so no opaque surface's
G-buffer entry is ever a perfect mirror once Stage 3's lobe reads it; Toksvig's variance
still rides on top. Does not touch glass — Stage 3 keeps glass hardcoded to roughness 0
independent of the G-buffer.

**Fixed-mip second sample.** The base Toksvig sample uses the screen-footprint LOD, which
only reveals a bump map's detail once minification has blended texels together — up close,
even a heavily-detailed map (grates, rivets, rust) still measures as sharp. Both prepasses
now also sample the same bump map via `textureLod` at a fixed mip (4), independent of
camera distance, and take whichever of the two samples shows more variance (the shorter raw
length). A flat/no-bump material decodes to the same length at every mip, so this adds no
roughness there — only genuinely detailed textures are affected, and only at close range
where the footprint sample would otherwise miss them.

**Check:** debug mode 12 — roughness as greyscale. Rough concrete bright, polished trim and
glass dark, and the gradient tracks visible surface detail rather than the diffuse texture.
With the floor, "dark" means `r_rtReflRoughnessFloor`, not 0 — sweep it to 0 to see the raw
Toksvig signal underneath when judging the gradient.

---

## Stage 3 — GGX lobe, glass still sharp — done 2026-10-16, unbuilt/unverified

| File | Change |
|---|---|
| `glsl/rt_ggx.glsl` | **New.** `rt_SampleGGXReflection`: isotropic GGX VNDF importance sampling (Heitz 2018). Pipeline-agnostic, no bindings — reusable outside this one rgen |
| `glsl/reflect_ray.rgen` | Opaque branch (`!isGlass`) samples `rt_SampleGGXReflection(reflNormal, viewDir, alpha, u1, u2)` in place of the plain mirror `reflDir`; `alpha = (roughness * r_rtReflRoughnessScale)^2` from Stage 2's packed roughness (`gbufAlbedoSampler`, binding 9). One sample per pixel, world-anchored seed (`rt_WorldAnchoredSeed`/`rt_PixelPhiOffset`, same as `gi_ray.rgen`) — Stage 1's temporal denoiser resolves the variance, not a sample-count loop here |
| `glsl/reflect_ray.rgen` | Replaced the `grazingGain`/`grazingMax`/smoothstep stack with plain Schlick (`f0 + (1-f0)*(1-NdotV)^5`), still evaluated against the macro normal (not `H`) so R1's pre-trace cull stays exact and pre-sampling |
| `vk_reflections.cpp` | Added `r_rtReflRoughnessScale` (default 1.0); retired `r_rtSpecGrazingMax/Gain` (cvars, UBO fields, settings-menu entries all removed — `ReflParamsUBO` stays 272 bytes, an explicit `_padBeforeViewProj` float replaces the implicit std140 alignment pad GLSL now inserts in the same spot) |

Glass takes roughness 0 and keeps its current path — a delta BRDF is physically right for a
pane, and it is the cheap case. Enforced structurally, not by a zero roughness value: the
`isGlass` branch never calls `rt_SampleGGXReflection` at all, so nothing can make glass pick
up a lobe by accident.

**Check:** glass screenshots identical to Stage 1's output (structurally guaranteed above, not
just a zero-roughness coincidence — worth confirming anyway). For the lobe-widening sweep,
glass itself can't be forced to pick it up (see above), so use `r_rtReflectionMode 2` on a
shiny opaque surface (F0 > 0): the exact same `!isGlass` code path reflections already go
through in mode 1 for any non-glass reflective geometry near a glass rect, just without the
mode-1 launch-grid restriction. Sweep `r_rtReflRoughnessScale` there and confirm the lobe
widens smoothly with no energy change at grazing angles.

---

## Stage 4 — Open to opaque surfaces — done 2026-10-16, unbuilt/unverified

| Item | Detail |
|---|---|
| `r_rtReflectionMode 3` | New: glass + opaque-with-roughness. Mode 1 (glass-only) and mode 2 (legacy, no ceiling, predates roughness) both stay as-is; mode 3 is purely additive |
| Launch grid | Full-screen, same as mode 2 — `glassOnly` in `vk_reflections.cpp` only ever triggers on mode 1, so 3 inherits the existing full-screen path with no new code |
| **Roughness ceiling** | `r_rtReflRoughnessMax` (default 0.5), mode 3 only. `reflect_ray.rgen` folds it into the existing f0 early-out: `glassProbe.hitT <= 0.01 && (f0 < 1/255 \|\| tooRoughForMode3)`. Uses `>=`, not `>`, so a ceiling of exactly 0 disqualifies every opaque pixel even if `r_rtReflRoughnessFloor` is also 0 — structurally identical to mode 1, not just numerically close |
| GI handoff | No code change needed in `gi_albedo_mod.comp` — it already runs unconditionally on every opaque pixel regardless of reflMode, so "GI only" above the ceiling was already true by construction. Debug mode 13 added to `reflect_ray.rgen`: white = glass (always reflects), blue = reflection + GI (both, roughness under the ceiling), green = GI only (over the ceiling, or f0 too low). Mode 13 forces `reflMode = 3` in the UBO fill regardless of the real cvar, so the boundary is visible without having to set mode 3 first |

**Check:** `r_rtReflRoughnessMax 0` reproduces mode 1 exactly (confirmed structurally by the
`>=` comparison above, not just empirically). Sweeping it up, total scene brightness must stay
flat — any rise is double-counting. Dark corridors must not gain a noise floor; compare against
the GI-only capture at the same spot. Mode 13 is the fastest way to see where the boundary
currently sits before doing either comparison.

---

## Stage 5 — Retune and ship

`reflBlend`, `minWeight` and the F0 remap were all fitted with glass as the sole consumer, so
the first all-surface build will look worse than glass-only even where the math is better —
tuned constants absorb the old estimator's error. Retune from the debug overlays, not the
composite.

Then: menu entries (mode combo, roughness scale, ceiling, denoise), `r_rtQuality` preset
rows, `Dhewm3-AI-RT-Settings.md`, and cleanup Phase 1c becomes safe to run.

---

## Cost

| | Refl p50 |
|---|---|
| Now (glass-only, rect-limited) | 0.00-1.11 ms |
| 2026-09-11 full-screen, pre-arc-1 | 3.24 ms |

3.24 ms understates mode 3: per-ray cost has grown since with emitter discs and arc 8c's
volumetric marching per reflected segment. Plus Stage 1's denoiser and its history images.
Against a total GPU p50 of 9.17-14.94 ms, all three scenarios GPU-bound by 4-7x, budget
20-40% frame time for mode 3 at scale 1.0. Mode 3 should not be a default on the low presets.

## Gates

| Stage | Exit |
|---|---|
| 1 | `r_rtReflDenoise 0` bit-identical; no ghosting on glass; ≤0.3 ms |
| 2 | Mode 12 roughness tracks surface detail; class migration leaves FSR T&C and player-exclude unchanged |
| 3 | Glass identical to Stage 1; grazing energy flat |
| 4 | `r_rtReflRoughnessMax 0` ≡ mode 1; scene brightness flat under the sweep; no noise floor in dark corridors |
| 5 | Presets include mode 3 only at High/Ultra |

## Not doing

Per-material authored roughness. No source data, and `.mtr` edits at that scale are map
editing by another name (rules doc, pillar 5).

Roughness on the glass path. A pane is a delta BRDF; widening it would be wrong and would
cost the one reflection case that is currently cheap and correct.
