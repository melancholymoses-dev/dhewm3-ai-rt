# Muzzle flash sprite in reflections

Show the game's authored muzzle flash in RT reflections instead of a synthesized
disc derived from the muzzle-flash light.

The flash is real geometry: `models/md5/weapons/pistol_world/worldpistol.md5mesh`
carries `pistol_mflash` (`deform sprite`) and `pistol_mflash2` (`deform tube`),
and every other weapon world model carries the equivalent. Three things keep it
out of reflections:

| Blocker | Detail |
|---|---|
| No view-independent geometry | `R_DeformDrawSurf` builds the billboard at draw time from `tr.viewDef`'s camera axes. Nothing to put in a BLAS, and a baked quad would face the primary camera, not the ray |
| World model not tagged | `idWeapon::worldModel` is a distinct `idAnimatedEntity`, not `weaponDepthHack`, but the emitter walk only inspects `renderEntity.rtGlow` entities. `CollectGlowSurfaces` also requires `DM_STATIC` |
| **Emitters cannot address a filmstrip** | `pistol_mflash_strip.tga` is 12 frames; the live frame comes from `scale 1/12, 1` + `scroll table12[(time+parm4)*8], 0` + `rotate rotate90[parm5]`. `rt_EmitterDisc` samples raw `[0,1]²`, so the image reads as all 12 frames at once |

A ray-facing disc is the correct generalization of `deform sprite` — the sprite
faces the viewer, and in a reflection the viewer is the ray. The missing piece is
the texture transform, which is useful on its own: every scrolling or animated
additive stage currently reaches reflections at frame 0.

Supersedes the muzzle half of the 2026-10-20 flash split (`r_rtReflMuzzleGlow*`).
The flashlight lamp keeps its synthetic disc — that one is on the view model and
has no world geometry at all.

---

## Stage 1 — Affine texture transform on emitters

Emitters and glow triangles sample the stage's texture matrix instead of raw uv.

| File | Change |
|---|---|
| `vk_rt_emitters.h` | `vkRTEmitter_t` gains `float texMat[2][4]` (two `vec4` rows, Doom 3's `m00 m01 _ m02` / `m10 m11 _ m12` layout); 32 → 64 bytes. Same two rows on `vkRTGlowTri_t` |
| `rt_emitter.glsl` | `RtEmitter`/`RtGlowTri` mirror the rows; new `rt_EmitterTexUV(uv, texMat)` applies them |
| `reflect_ray.rgen` | Both `textureLod` calls go through `rt_EmitterTexUV` |
| `vk_rt_emitters.cpp` | New `EmitterTexMatrix(stage, regs, out)`: identity when `!stage->texture.hasMatrix`, else the six registers read the same way as `draw_common.cpp:1239-1249`. Called wherever `EmitterTexIndex` is |

`PickAdditiveStage` currently discards `regs` — return it, or re-evaluate, so the
matrix and the colour come from one evaluation.

**Check:** a scrolling additive stage already in the emitter path (lost-soul flame
trails, `r_rtReflGlowSmoke 1`) animates in glass rather than sitting static.
Identity matrix on every other emitter leaves the current image unchanged.

---

## Stage 2 — The flash surface as an emitter

| File | Change |
|---|---|
| `neo/game/Weapon.cpp` | `worldModel->GetRenderEntity()->rtGlow = true` where the world model is set up; refresh on `UpdateVisuals` |
| `neo/d3xp/Weapon.cpp` | Mirror it |
| `vk_rt_emitters.cpp` | `EvalEmitter`: accept `DFRM_SPRITE` like the existing `DFRM_PARTICLE` branch — radius from the surface bounds, colour/image/matrix from the winning stage. Position from the surface bounds centre through `modelMatrix`, which is already what the non-particle path does |

The flash's own timing needs no work: `PickAdditiveStage` tests
`regs[stage->conditionRegister]`, and `if (time + parm4) * 8 < 1.0` makes the
surface appear and vanish on its own. `w_pistol` is opaque and has no additive
stage, so tagging the weapon admits only the flash surfaces.

**Check:** `r_vkLogRT 1` prints an emitter with `mtr='models/weapons/pistol/pistol_mflash'`
and `img='textures/particles/pistol_mflash_strip'` for the frames a shot is live,
and nothing between shots. Fire at a window: a flash that changes shape across its
12 frames, not a steady blob.

---

## Stage 3 — Retire the synthetic muzzle disc

| File | Change |
|---|---|
| `vk_rt_emitters.cpp` | Delete `r_rtReflMuzzleGlow`, `…Gain`, `…Radius` and the muzzle branch of `CollectViewFlashEmitters`. Keep the `pointLight` test as the lamp's admission condition, and keep `flashImage` unused-for-lamp (it reverts to `texIndex = 0`) |
| `Dhewm3SettingsMenu.cpp` | Drop the two muzzle widgets; keep "Flashlight Lamp in Reflections" |
| `docs/Dhewm3-AI-RT-Settings.md` | Drop the `r_rtReflMuzzleGlow*` row |

**Check:** flashlight-out reflections identical to before this doc. Firing any
weapon shows the sprite from Stage 2 and no disc.

---

## Stage 4 — `deform tube` (optional)

`*_mflash2` is a tube along the barrel, not a billboard, so a disc is the wrong
primitive. Defer until Stage 2 is on screen and judge whether the sprite alone
reads right. If it doesn't, the glow-triangle path is the closer fit — the tube's
authored verts are a real ring, and `rt_GlowTriHit` already handles arbitrary
triangles with uv.

---

## Risks

| Risk | Note |
|---|---|
| Sprite bounds are a degenerate placeholder | `deform sprite` surfaces may author a zero-extent quad, since the real size comes from the deform. If `radius` lands at 0, take it from the stage's `deform sprite` width or a cvar |
| The flash lasts ~0.12 s | Shorter than the reflection temporal EMA's response. It may read as a dim smear rather than a flash; reflections have no disocclusion signal for an emitter that appears and vanishes |
| 64-byte emitters | Buffer goes 2 KB → 4 KB per frame slot. Immaterial, but `VK_RT_MAX_EMITTERS` is mirrored in three places |
