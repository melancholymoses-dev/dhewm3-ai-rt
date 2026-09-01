# RT Parallel ("Sun") Lights — GI/Volumetrics/Reflections Admission

**Date:** 2026-08-31
**Status:** Spec only — not started.
**Motivates:** Mars-surface sun visible through windows/skylights from inside the
base (rare, but present — e.g. mars_city1-style exterior shots) getting GI bounce,
volumetric shafts, and reflections, matching what direct lighting already does.

---

## The surprise: this is a much smaller fix than it looks

`vk_gi.cpp`'s `considerLight` rejects every parallel light outright:

```cpp
// Parallel (directional/sky) lights are infinite — no volume boundary or falloff,
// so they cannot contribute meaningful single-scatter volumetrics.
if (p.parallel)
    return;
```

That premise doesn't hold up against the GL reference path. A Doom 3 "parallel"
light is **not** an infinite/unbounded directional light — it's an ordinary
box-bounded point light (`renderLight->pointLight` is true for it too; see
`Light.cpp:98-130` — `parallel` only gets checked at all when `light_target` was
never given, i.e. the same spawn path as any point light) that fakes a directional
look for shading purposes only. `R_DeriveLightData`
(`tr_lightrun.cpp:456-473`) shows the whole trick:

```cpp
// adjust global light origin for off center projections and parallel projections
// we are just faking parallel by making it a very far off center for now
if (light->parms.parallel)
{
    idVec3 dir = light->parms.lightCenter;
    if (!dir.Normalize())
        dir[2] = 1;                                    // straight up if unspecified
    light->globalLightOrigin = light->parms.origin + dir * 100000;
}
else
{
    light->globalLightOrigin = light->parms.origin + light->parms.axis * light->parms.lightCenter;
}
```

Everything else — `lightProject` (still the point-light box setup,
`tr_lightrun.cpp:424-435`), the falloff image, the `lightRadius` AABB volume — is
identical to a normal point light. The only difference is where `globalLightOrigin`
(the point N·L/specular/shadow-ray math is actually computed from) sits: 100,000
units away along `light_center`'s direction instead of `origin + axis*lightCenter`.
At that distance, direction varies by ~1% across a room-sized volume — indистin­
guishable from true parallel light, by design.

**Direct lighting and RT shadows already use exactly this value today** —
`vk_shadows.cpp:810` reads `vLight->lightDef->globalLightOrigin` unconditionally for
every visible light's shadow ray, with no parallel-light exclusion anywhere in that
file. A sun fixture already lights and self-shadows correctly through a window right
now. The only things that read `globalLightOrigin` and *don't* see parallel lights
are GI bounce, reflections, and volumetrics — because all three pull from
`vk_gi.cpp`'s admitted-candidate list, and `considerLight` throws them away before
`globalLightOrigin` is even copied into `GILightEntry.emitPos`.

So the fix is not a new containment model, a bounding slab, or new attenuation math
— all of that already exists and already works for these lights along the direct
path. It's: stop rejecting them, and copy `globalLightOrigin` into `emitPos`
(`vk_gi.cpp:1340-1343` already does this correctly — it just never runs for a
parallel light today).

---

## What actually needs doing

### CPU (`vk_gi.cpp::considerLight`)

1. Delete the `if (p.parallel) return;` early-out.
2. `isProjected` is already `(!p.pointLight && !p.parallel)` — for a well-formed
   parallel light `p.pointLight` is always true (Light.cpp only sets `parallel` on
   the no-`light_target` / point-light spawn path), so this already evaluates
   `false` and the light falls into the existing point/box branch unchanged. No
   change needed here — just confirm with an assert or dump line that a
   `parallel && !pointLight` light (a malformed map entity) doesn't sneak through;
   original engine doesn't support that combination either, treat as out of scope.
3. `emitPos` (`vk_gi.cpp:1340-1343`) already reads `lightLocal->globalLightOrigin`,
   which `R_DeriveLightData` already computed the parallel-correct way. Nothing to
   change.
4. Recommended (not required): give parallel lights their own `lightType` (4 — 0
   point, 1 directed/spot, 2 flashlight, 3 reserved/unused check first, so use
   whatever's next free) instead of silently reusing `lightType 0`. Reasons:
   - Matches the existing pattern — directed (spot) and flashlight already get
     independent `r_rtVolDirectedDensity/Strength/Anisotropy` and
     `r_rtVolFlashlightDensity/Strength/Anisotropy` knobs distinct from ordinary
     point lights. A sun beam through a window is a wide, strong, hero shaft —
     pillar 3 ("light the air sparingly... a budgeted handful") means it should be
     independently tunable, not fight point-light volumetric budget/knobs.
   - Free label for `r_rtGILightDump` (print `SUN` instead of `POINT`), which
     matters for the tuning risk below.
   - Zero shader-side behavior change required beyond the density/strength/aniso
     lookup — containment, falloff, shadow-ray targeting are all unchanged from the
     point-light path.

### GPU

No required changes — the existing point-light box containment
(`rt_LightContribAt` in `rt_light_eval.glsl`, the `lt == 0u` branch in
`vol_march.comp`) already does the right thing once `emitPos` is 100,000 units out:
- Direction (`toLight`/`lightDir`) becomes a near-constant vector across the whole
  box — correct parallel-shaft look in volumetrics, falls out for free.
- The near-emitter singularity fade in `vol_march.comp`
  (`coreFade = smoothstep(0.0, sphereRad * 0.15, dist)`) always evaluates near 1.0
  since `dist` is always ~100,000 — harmless, no visible effect either way.
- Sphere pre-cull and the box `atten` calc measure from `posRadius.xyz` (the real
  `origin`), never `emitPos` — unaffected by the distance trick.

If the `lightType` split above is taken: add the `lt == 3u` (or whatever's free)
branch to `vol_march.comp`'s per-light loop, identical to the `lt == 0u` box path,
just reading `r_rtVolSunDensity/Strength/Anisotropy` instead of the global
`density`/`anisotropy`/`strength`. New CVars, same shape as the directed-light ones
already in `vk_vol.cpp`.

---

## Risks / things to actually check before calling this done

- **Why was it excluded in the first place?** Worth a `git log -p` /
  `git blame` on that line before touching it — if there's a real remembered bug
  (rather than the stated "infinite, no falloff" reasoning, which doesn't match the
  GL code), find it first. Best guess given the timeline: this landed before A11
  (`amd_vulkan_cleanup.md` — `shaderParm3` misread as an intensity multiplier, fixed
  2026-08-26) and before the AR0 light classifier (2026-08-15) existed to separate
  real fixtures from ambient washes. A parallel light with a bad `parm3` or no
  classifier to catch an oversized ambient one could plausibly have produced the
  "blew the volumetric march into a full-screen wash" symptom A11's own writeup
  describes for an unrelated light — both fixes are already in place now, which is
  exactly why this is worth re-trying today. Confirm with the dump (below) rather
  than assuming.
- **Light-selection budget dominance.** A sun fixture's `lightRadius` box is
  typically large (it's meant to fill a big room or skylight shaft), and the
  existing importance formula
  (`baseImportance = intensity·lum·radius²/max(dSq, radius²)`) approaches
  `intensity·lum` alone once the camera is well inside the box — a bright sun could
  crowd out genuinely nearby practical fixtures for GI/vol upload slots the same
  way the mars_city1 `parm3` light did pre-A11. Check with `r_rtGILightDump`
  (`r_rtAutoRelightDebug`-style table) before/after enabling, at the one spot where
  the sun is actually visible from indoors. If it dominates, the hysteresis and
  L1-tier machinery already in `considerLight` should handle it the same as any
  other bright light — no new logic anticipated, just verify.
- **Shadow ray distance ~100,000 units.** `vk_shadows.cpp` already fires this ray
  for direct lighting today with no reported issue, so this is a "should be fine,
  confirm it stays fine" item for the GI-bounce/reflection/volumetric shadow rays
  (`rt_TraceLightShadow` in `rt_light_eval.glsl`, `vol_march.comp`'s per-step
  query) reusing the same distance — not a new code path, just a new caller of an
  existing one.
- **`amd_vulkan_cleanup.md` A12 (far-field shadow instability)** is about the
  *shading point* being far from the camera (depth-precision-driven world-position
  jitter), not about a long ray to a distant light — should be unrelated, but if a
  sun-lit exterior view also happens to be a long-distance view (plausible for Mars
  terrain seen through a window), the two bugs could show up in the same shot.
  Don't misattribute one for the other; check A12's status first if shadows flicker
  in the same scene.

## Validation plan

Per the project's debug-before-theory rule:
1. Find (or place, if none exists retail) a parallel light visible from an interior
   space — a mars_city1-style window/skylight is the obvious candidate. Confirm via
   `r_rtGILightDump` that it now appears in the admitted list at all (currently:
   absent, unconditionally).
2. Toggle GI (`r_rtGI`), volumetrics (`r_rtVol`), and reflections independently
   with the light admitted, screenshot each, compare against the direct-lit-only
   baseline (which should be unchanged, since that path never touched this code).
3. Color-code overlay: tint any GI/vol/reflection sample whose dominant contributor
   is the sun light a distinct color (reuse the existing per-light debug-tint
   convention if `r_rtGILightDump`/dump tooling already has one) so its contribution
   is visually separable from practical fixtures while tuning.
4. If the dedicated `lightType`/CVars route is taken, tune `r_rtVolSunDensity/
   Strength/Anisotropy` from that overlay, not by eye on the final composite.

## Files touched (expected)

- `neo/renderer/Vulkan/vk_gi.cpp` — remove the early-out; optional `lightType`
  branch; dump label
- `neo/renderer/Vulkan/vk_vol.cpp` — optional new CVars +
  `directedDensity`-style UBO fields, if the dedicated-type route is taken
  (skip entirely if reusing `lightType 0` for a v1)
- `neo/renderer/glsl/vol_march.comp` — optional new `lt == N` branch (skip if
  reusing `lightType 0`)

## Out of scope

- **Sky/miss-shader ambient radiance** (a uniform sky-color term added to rays that
  miss all geometry) — a genuinely separate feature (ambient fill for the sky dome
  itself, not a specific mapped light entity) that the ROADMAP's earlier "sun/sky-
  light path" note conflated with this one. Worth its own doc if the flat black sky
  in RT miss shaders becomes a visible problem; not needed for the "sun light
  admitted to GI/vol/reflections" goal here.
- **True unbounded/infinite directional lights.** Doesn't exist in Doom 3's format
  at all per the above — nothing to build.
