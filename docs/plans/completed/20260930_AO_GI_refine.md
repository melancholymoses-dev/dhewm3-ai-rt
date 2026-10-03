# AO / GI refinement

**Status:** ✅ Closed 2026-10-02. A1-A5 built; the two "Open checks" at the end were not run.
**Owns:** AO distance falloff, moving AO from direct light onto indirect (GI), AO/GI debug view.

Today AO is binary (any hit inside `r_rtAORadius` = fully occluded), so the dark band
grows linearly with the radius, and it multiplies the diffuse of every direct light. GI
gets no AO at all: probe GI (64-unit spacing) cannot resolve creases smaller than a
probe cell, which reads as flat indirect light.

## A1 — distance-weighted AO

| Change | Where |
|---|---|
| New closest-hit returns `t / R` (R passed in via the payload) | `glsl/ao_ray.rchit`, CMake |
| Rays drop `TerminateOnFirstHit` / `SkipClosestHit` so `t` is the nearest hit | `ao_ray.rgen` |
| Per-ray visibility = `(t/R)²` when `r_rtAOFalloff 1`, 0 on hit when `0` (legacy) | `ao_ray.rgen` |
| Hit group gets the closest-hit stage; UBO gains `aoFalloff` (112 → 128 B) | `vk_ao.cpp` |
| `r_rtAORadius` default 64 → 24 (about a third of the probe spacing) | `vk_ao.cpp` |

Check: `r_rtAODebug 1` at radius 24 vs 64 — dark band width should barely change, crease
depth should.

## A2 — AO on indirect, not direct

| Change | Where |
|---|---|
| Composite samples AO (binding 1) and multiplies GI by `mix(1, ao, r_rtAOIndirectStrength)` | `gi_composite.frag`, `vk_gi.cpp` |
| Shared "is AO valid, which view" helper | `VK_RT_GetAODescriptor` in `vk_ao.cpp` |
| Interaction UBO `useAO` int → `aoDirectStrength` float (same offset) | `interaction.frag/.vert`, `vk_pipeline.cpp`, `vk_backend.cpp` |
| Direct strength = `r_rtAODirectStrength` (default 0) while GI composites, 1.0 when GI is off | `vk_backend.cpp` |
| Menu: falloff, indirect strength, direct strength | `Dhewm3SettingsMenu.cpp` |

Check: `r_rtAODirectStrength 1; r_rtAOIndirectStrength 0` reproduces the old look
(apart from falloff). Expect a brighter scene overall; retune `r_rtGIStrength` / exposure
if needed.

## A3 — debug view

`r_rtAODebug` draws late (after shader passes, before fog) with a replace-blend variant
of the GI composite:

| Mode | Shows |
|---|---|
| 1 | raw AO mask, greyscale |
| 2 | GI × AO, scaled by `r_rtAODebugGain` |
| 3 | GI without AO, same gain (A/B against 2) |

## A4 — drop spectrum lights from GI / volumetrics

Monster teleport FX (`fx/teleporter*.fx`) spawn `lights/spectrumLight` (`spectrum 1`, 2× white,
radius 500). GL only applies a spectrum light to matching-spectrum surfaces
(`Interaction.cpp`), but GI/vol admitted it as REAL → white fog burst on every pentagram spawn.

| Change | Where |
|---|---|
| New class `RT_LIGHT_SPECTRUM` for `Spectrum() != 0`; not admitted to GI or vol | `vk_light_classify.cpp`, `vk_raytracing.h` |

Also catches the placed `lights/roundfire_spectrumLight` reveal lights (caverns1, alphalabs3, …).
`lights/squareblast` (the brief floor flash, spectrum 0) is kept.

Check: `r_rtGILightDump 1` near a spawn shows `lights/spectrumlight cls=SPECTRUM ... REJECT`,
absent from `[vol selection]`.

## A5 — projected light range from `light_end`

GI/vol took a projected light's range from `|target|`. GL uses `end - start`
(`R_SetLightProject`; `Light.cpp` defaults `end` to `target`). The sentry flashlight
(`ai_character_sentry.script`) sets `light_target "1 0 0"`, `light_end 640`, so it read as a
1-unit cone and barely reached the fog.

| Change | Where |
|---|---|
| `projReach = max(|axis·end|, |axis·target|)` drives admission radius, importance, vol cull, cone reach | `vk_gi.cpp` |
| Cone half-angle still from target vs right/up | `vk_gi.cpp` |

Check: `r_rtGILightDump 1` near a sentry with its light on (mars_city1/2, comm1):
`lights/sentrylight ... radius= 640.0 ... isProjected=1`, present in `[vol selection]` with `reach=704`.

## Open checks (not run at close)

- Viewmodel: AO at weapon pixels is computed from hacked depth; interactions skip it, the
  GI composite does not. Look for a dark gun in mode 2.
- AO cost with full closest-hit traversal vs first-hit: compare `AO=` in `r_vkRTProfile`.
