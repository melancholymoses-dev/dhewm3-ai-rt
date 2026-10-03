# Projectile Glow in Reflections

**Status:** E0–E3 code landed 2026-10-02, not yet built or run. Next: E0 inventory, then the
E2/E3 checks. Supersedes AR3 / "Sprite / Particle Effects in RT Reflections" in
`completed/20260423_reflection_enhancements.md`.

### As built (differences from the stages below)

| Item | Implemented as |
|---|---|
| Emitter fill site | `VK_RT_BuildReflEmitters` in `vk_backend.cpp` right after `VK_RT_UploadGILights`, before `VK_RT_FlushBindlessTextures`. Gated to the primary 3D view, once per frame slot |
| `rtGlow` field | Last member of `renderEntity_t`, so existing offsets don't move. Cleared in `Fizzle`/`Explode` before `Hide()`; `Restore` derives it from `state`; demo reader zeroes it |
| `.prt` lookup | `declManager->FindType(DECL_PARTICLE, model->Name(), false)`; no `Model_local.h` accessor |
| Stage choice | Brightest enabled stage with blend `(ONE, ONE)` or `(SRC_ALPHA, ONE)`; `customShader`/`customSkin` honoured |
| `matTextures` in rgen | Declared directly (set 1, binding 3); `rt_material.glsl` can't be included in a raygen shader |
| Projectiles with no additive stage | Skipped and logged; light-colour fallback not built |
| Debug mode 8 | Mode 6 raw radiance with discs added on top: green in front of the hit, red behind |
| Menu | "Projectiles in Reflections" + "Projectile Glow Gain" in Reflection Settings |
**Owns:** plasma bolts, imp fireballs, rockets, BFG balls etc. appearing in glass reflections.

## Why the earlier attempts failed

| Attempt | What it did | Why it broke |
|---|---|---|
| AR3 option B | Relaxed BLAS filter to admit `MF_NOSHADOWS` translucent meshes | FPS tanked; looked wrong |
| Sprite plan (`2596ecee`, reverted `f37f071b`) | Sprite/beam entities with `customShader` → non-opaque BLAS, any-hit accumulate | Touched every RT consumer: glass probe (`CullOpaque`) sees any non-opaque geometry as glass, masks had to change in shadow/AO/GI rgens, sprite BLAS uncacheable → rebuilt per frame |
| Both | | Targeted the wrong geometry. Plasma is `plasma_bolt.lwo` with `deform sprite` (deformed per view in the front end, BLAS sees the raw quad). Imp fireball is `impfireball2.prt`, `orientation view`. Neither is a `customShader` sprite |

## Approach: analytic emitter list, traced in the rgen

Projectiles stay out of the TLAS. The CPU writes a small list of glowing points each frame;
`reflect_ray.rgen` intersects the reflection ray against them after the TLAS trace returns.

```
emitter = { vec4 posRadius; vec4 rgb + texIndex }   // 32 B, ≤ 64 per frame
t = dot(c − o, d);  if 0 < t < hitT and |o + t·d − c| < r:
    uv  = project (o + t·d − c) onto the plane ⟂ d, / r    // ray-facing billboard
    acc += rgb · tex(uv)                                   // texIndex 0 → (1 − ρ²)²
```

| Property | Result |
|---|---|
| Billboard faces the *reflection* ray | Correct orientation for free; the old plan's "thin sliver" problem is gone |
| No BLAS, no TLAS, no hit groups, no masks | Shadows, AO, GI, vol, glass probe cannot be affected |
| No raster change | Transparency and the glass overlay order cannot break |
| Source is a game-side tag on `idProjectile` | Smoke trails (`idSmokeParticles`), decals, impact fx are never in the list |
| Occlusion = TLAS `hitT` | Glow sits behind walls correctly; no self-occlusion since the projectile has no geometry |

Cost: one loop over ≤ 64 entries per traced glass pixel. Glass-only mode keeps the pixel set tiny.

## CVars

| CVar | Default | Notes |
|---|---|---|
| `r_rtReflGlow` | 0 | 1 after E3 |
| `r_rtReflGlowGain` | 1.0 | Multiplies emitter rgb |
| `r_rtReflGlowScale` | 1.0 | Multiplies emitter radius |
| `r_rtReflGlowDist` | 1024 | Max distance from view origin |
| `r_rtReflectionDebugMode 8` | — | Emitter discs only: green visible, red occluded by `hitT` |

---

## E0 — Tag and inventory

| Change | Where |
|---|---|
| `bool rtGlow` on `renderEntity_t` (after `weaponDepthHack`) | `neo/renderer/RenderWorld.h` |
| Set `renderEntity.rtGlow = true` in `idProjectile::Create`, and again at the end of `Restore` (not added to the savegame format) | `neo/game/Projectile.cpp`, `neo/d3xp/Projectile.cpp` |
| `r_vkLogRT 1`: one line per tagged entity per second: model, material, first additive stage image, bounds radius, evaluated rgb | new `vk_rt_emitters.cpp` |

Check: fire plasma, rockets, BFG; let imps, revenants, mancubus, cacodemon, sentry fire. Record
the table in this doc. Every visible projectile must have an additive stage with a non-zero colour.
Fill-in rule for any that do not (e.g. `rocket.lwo` is opaque): fall back to the projectile's own
`renderLight` colour, passed via `shaderParms`, or skip it.

## E1 — Emitter buffer

| Change | Where |
|---|---|
| `VK_RT_BuildEmitters(viewDef)`: walk `renderWorld->entityDefs`, keep `rtGlow` within `r_rtReflGlowDist`, cap 64 (nearest first) | `vk_rt_emitters.cpp/.h` |
| Position: `modelMatrix · bounds centre`. Radius: bounds radius × scale; for `.prt` use the chosen stage's `max(size.from, size.to)` | same |
| Colour: `material->EvaluateRegisters(...)` with entity `shaderParms` + view time, then the stage's `color.registers[]`; `.prt` also × `idParticleStage::color` | same; needs an `idRenderModelPrt::ParticleSystem()` accessor in `Model_local.h` |
| Texture: stage image → bindless index, same lookup `vk_material_table.cpp` uses for `diffuseTexIndex` | same |
| SSBO per frame slot, `HOST_VISIBLE`, indexed by `vk.currentFrame`; binding 6, `RAYGEN` | `vk_reflections.cpp` |
| `vk_rt_emitters.cpp` added to sources | `CMakeLists.txt` |

Check: E0's log matches what the buffer holds (log count and first entry from the fill site).
`r_vkSerializeFrames 1` changes nothing.

## E2 — Shader

| Change | Where |
|---|---|
| `float hitT` in `ReflPayload`; rchit writes `gl_HitTEXT`, miss writes `maxDist` | `reflect_payload.glsl`, `reflect_ray.rchit`, `player_reflect.rchit`, `reflect_ray.rmiss` |
| Emitter loop after the first trace segment, before `imageStore`; adds into `accum` so the glass Fresnel weight and `reflBlend` apply unchanged | `reflect_ray.rgen` (declares `matTextures` directly; set 1 already has `RAYGEN`) |
| `emitterCount`, `glowGain` in `ReflParams`; `REFL_DEBUG_MAX_MODE` 7 → 8 | `reflect_ray.rgen`, `vk_reflections.cpp`, `vk_raytracing.h` |
| New include `rt_emitter.glsl` (struct + intersect) in `GLSL_INCLUDES` | `CMakeLists.txt` |

Check: debug mode 8 in front of the Mars City glass: discs track bolts fired behind the player,
turn red behind pillars. Mode 0: plasma reads blue and round in glass at any wall angle.
`r_rtReflGlow 0` is bit-identical to today.

## E3 — Menu and tune

| Change | Where |
|---|---|
| Toggle + gain under the reflections block | `Dhewm3SettingsMenu.cpp` (`RTCVars` + draw) |

| Check | Pass condition |
|---|---|
| Plasma/fireball in glass vs the same bolt seen directly | Similar hue; glow no brighter than the direct bolt |
| Bolt in front of glass in the primary view | No change to its raster look (no doubling) |
| 10+ projectiles on screen (BFG, imp swarm) | `refl` phase +≤ 0.05 ms on 9070 XT |
| Smoke trails, impact puffs, scorch decals | Absent from reflections |

Then default `r_rtReflGlow 1` and record tuned values here.

## Cut

| Item | Reason |
|---|---|
| Lighting the reflected projectile | Raster draws these `blend add`, unlit |
| Emitters in GI / vol | Projectile `renderLight`s already reach both |
| Per-particle billboards for `.prt` | One disc per emitter reads fine at reflection scale |
| Muzzle flash / explosions | Not `idProjectile`; add a tag later if wanted |
