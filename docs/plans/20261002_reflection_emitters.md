# Projectile Glow in Reflections

**Status:** E0–E4 built and working in-game 2026-10-02 (plasma, rockets, lost souls). Remaining:
the last weapons/monsters in the E3 check list, then flip `r_rtReflGlow` to default 1. Explosions
are out of scope until E5. Supersedes AR3 / "Sprite / Particle Effects in RT Reflections" in
`completed/20260423_reflection_enhancements.md`.
**Owns:** projectile and smoke-system glows (plasma, fireballs, rocket exhaust, lost-soul flames,
trails) in glass reflections.

## Why the earlier attempts failed

| Attempt | What it did | Why it broke |
|---|---|---|
| AR3 option B | Relaxed BLAS filter to admit `MF_NOSHADOWS` translucent meshes | FPS tanked; looked wrong |
| Sprite plan (`2596ecee`, reverted `f37f071b`) | Sprite/beam entities with `customShader` → non-opaque BLAS, any-hit accumulate | Touched every RT consumer: glass probe (`CullOpaque`) sees any non-opaque geometry as glass, masks had to change in shadow/AO/GI rgens, sprite BLAS uncacheable → rebuilt per frame |
| Both | | Targeted the wrong geometry. Plasma is `plasma_bolt.lwo` with `deform sprite`; imp fireball is `impfireball2.prt`; rocket exhaust is a `deform particle2` surface. None is a `customShader` sprite |

## Approach: analytic emitter list, traced in the rgen

Nothing enters the TLAS. The CPU writes up to 64 glowing points per frame; `reflect_ray.rgen`
intersects the reflection ray against them after the TLAS trace returns.

```
emitter = { vec3 pos; float radius; vec3 rgb; uint texIndex }   // 32 B, std430
t = dot(c − o, d);  if 0 < t < hitT and |o + t·d − c| < r:
    uv  = project (o + t·d − c) onto the plane ⟂ d, / r    // ray-facing billboard
    acc += rgb · tex(uv)                                   // texIndex 0 → (1 − ρ²)²
```

| Property | Result |
|---|---|
| Disc faces the *reflection* ray | Correct orientation in any mirror angle |
| No BLAS, TLAS, hit groups or masks | Shadows, AO, GI, vol, glass probe unaffected |
| No raster change | Transparency and glass overlay order untouched |
| Occlusion = TLAS `hitT` (new `ReflPayload.hitT`) | Glow hides behind walls; no self-occlusion |
| Added to `accum` before the Fresnel/glass weight | Glows scale with `r_rtReflectionBlend` like the rest of the reflection |

## CVars

| CVar | Default | Notes |
|---|---|---|
| `r_rtReflGlow` | 0 | Master switch. Flip to 1 after the remaining checks |
| `r_rtReflGlowGain` | 1.0 | Multiplies emitter rgb (menu) |
| `r_rtReflGlowScale` | 1.0 | Multiplies emitter radius |
| `r_rtReflGlowDist` | 1024 | Max distance from the view origin |
| `r_rtReflGlowSmoke` | 1 | Include smoke-system particles (menu); needs `r_rtReflGlow` |
| `r_rtReflGlowSmokeMinLum` | 0.3 | Quads dimmer than this (vertex colour × stage rgb) are dropped |
| `r_rtReflGlowSmokeCell` | 32 | Grid size for merging smoke quads into one disc |
| `r_rtReflectionDebugMode 8` | — | Mode 6 radiance + discs: green in front of the hit, red behind. Collects emitters even with `r_rtReflGlow 0` |

`r_rtReflectionBlend` default raised 4 → 6 in the same change (clamp and slider ceiling 5 → 10).

---

## As built

| Area | Files | What |
|---|---|---|
| Tag | `RenderWorld.h` | `bool rtGlow`, last member of `renderEntity_t` so existing offsets don't move. Demo reader zeroes it |
| Projectiles | `game/`, `d3xp/` `Projectile.cpp` | Set in `Create`; cleared in `Fizzle`/`Explode` before `Hide()`; `Restore` derives it from `state` (not in the savegame) |
| Smoke | `game/`, `d3xp/` `SmokeParticles.cpp` | `Init` tags the shared smoke entity. `UpdateRenderEntity` now only empties the model on a non-view callback or when regenerating (see findings) |
| Emitter list | `vk_rt_emitters.cpp/.h` | `VK_RT_BuildReflEmitters`, called in `vk_backend.cpp` after `VK_RT_UploadGILights`, before `VK_RT_FlushBindlessTextures`. Primary 3D view only, once per frame slot. SSBO per frame slot, binding 6 |
| Stage choice | same | Brightest enabled stage with blend `(ONE, ONE)` or `(SRC_ALPHA, ONE)`, colour from `EvaluateRegisters`. `customShader`/`customSkin` honoured |
| Particle decls | same | `.prt` models and `deform particle`/`particle2` surfaces use the decl's brightest additive stage, radius = stage size. Deform surfaces are placed at their own bounds (rocket tail) |
| Smoke clusters | same | Quads of the smoke entity's additive surfaces, luminance floor, skip within 32 units of the camera, merged per (surface, grid cell) with area-weighted rgb. Projectiles first, smoke fills the rest nearest-first |
| Shader | `reflect_ray.rgen`, `rt_emitter.glsl`, `reflect_payload.glsl`, both rchits, rmiss | `hitT` in the payload; emitter loop; `matTextures` declared directly (`rt_material.glsl` can't be included in raygen) |
| Plumbing | `vk_reflections.cpp`, `vk_raytracing.h`, `CMakeLists.txt` | Binding 6, `emitterCount`/`glowGain` in `ReflParams` padding, `REFL_DEBUG_MAX_MODE` 8 |
| Menu | `Dhewm3SettingsMenu.cpp` | Projectiles in Reflections, Projectile Glow Gain, Include Smoke Particles |

## Findings

| Finding | Fix |
|---|---|
| Smoke read `surfaces=0` every frame. `idSmokeParticles::UpdateRenderEntity` emptied the model before its "already current" check, so a second callback in a frame left it empty (raster unaffected: tri frees are deferred; screenshots unaffected: `forceUpdate`) | Empty only on a non-view callback (then force a regenerate) or when regenerating. Mirrors may now show smoke too |
| Rocket drew a flat white disc: winning stage was `models/weapons/rocketlauncher/zoom` (`kentest.mtr`), a `deform particle2 rocketblast` emitter whose own image doesn't exist | Deform-particle surfaces use their particle decl → orange `boomboom2` at the tail |
| Rocket body never reflects | Pre-existing: `rocket` material is `noshadows`, which the BLAS filter drops for opaque surfaces. Not addressed here |

## Remaining checks

| Check | Pass condition |
|---|---|
| BFG, chaingun/shotgun impacts, grenade; revenant, mancubus, cacodemon, hell knight, sentry; d3xp weapons | Each glowing projectile shows; `VK RT Emitter: skip ... no additive stage` lines reviewed |
| Firefight, mode 8 | Gun smoke and impact puffs don't haze the glass; raise `r_rtReflGlowSmokeMinLum` or move to an allow-list if they do |
| 10+ emitters | `refl` phase +≤ 0.05 ms on the 9070 XT |
| `r_rtReflGlow 0` | Identical to before the change |

Then set `r_rtReflGlow 1` and record tuned values here.

## E5 — Explosions (proposed, not started)

Explosions are intentionally absent: `Explode` swaps the projectile's model to `model_detonate`
(e.g. `rocketExplosion.prt`) and clears `rtGlow`. A disc has no notion of particle age, so a
kept tag would leave a constant glow until the entity is removed (often seconds after the fireball).

| Change | Where |
|---|---|
| Keep `rtGlow` through `Explode` | `Projectile.cpp` (both) |
| For `.prt` emitters, age = view time + `SHADERPARM_TIMEOFFSET`; scale rgb by the chosen stage's fadeIn/fadeOut envelope over `particleLife`, drop after `particleLife + timeOffset` | `vk_rt_emitters.cpp` |

Check: rocket explosion in glass flashes and fades with the raster fireball; no lingering disc.

## Cut

| Item | Reason |
|---|---|
| Lighting the reflected glow | Raster draws these `blend add`, unlit |
| Emitters in GI / vol | Projectile `renderLight`s already reach both |
| Per-particle billboards | One disc per emitter / cluster reads fine at reflection scale |
| Light-colour fallback for projectiles with no additive stage | Not needed so far; revisit if the remaining checks find one |
