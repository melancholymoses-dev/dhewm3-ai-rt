# Dynamic model normals are stale in RT

**Date:** 2026-09-19
**Status:** Cause confirmed in-game 2026-09-19 (`r_useDeferredTangents 0` fixes it). Not fixed.
**Found via:** `20260918_reflection_brightness.md` B1 — the player's reflection showed a hard
vertical terminator on the wrong side, under point lights, in an open hall with no occluder.

---

## The bug

Skinned md5 models reach the TLAS with **current vertex positions and stale vertex
normals**. RT shades them from a normal field fixed to the bind pose.

**The root of it: normal derivation is gated on the decision to *draw*, and RT consumes
geometry the raster frontend decided not to draw.**

The chain, all pre-existing:

1. `vk_accelstruct.cpp:1090` — the TLAS walks `viewDef->viewEntitys`. An entity gets into
   that list by area reference; that is not the same test as "any of its surfaces survived
   frustum culling".
2. `tr_light.cpp:1762-1769` — per surface, `R_CreateAmbientCache(tri,
   shader->ReceivesLighting())` sits **inside** `if (!R_CullLocalBox(tri->bounds, ...,
   tr.viewDef->frustum))`. A culled surface gets no ambient cache — and a surface whose
   shader returns false for `ReceivesLighting()` gets one built with `needsLighting` false.
   Either way, no derivation. The interaction path's call (`Interaction.cpp:1311`) is
   likewise reached only for surfaces that made it into an interaction.
3. `tr_light.cpp:68` — that function is the **only** place deferred derivation happens:
   `if (needsLighting && !tri->tangentsCalculated) R_DeriveTangents(tri)`. It also returns
   early, without deriving, when `tri->ambientCache` already exists.
4. `Model_md5.cpp:384` — `UpdateSurface` would have derived them itself, but only when
   `!r_useDeferredTangents`, and that cvar defaults to `"1"`
   (`RenderSystem_init.cpp:109`). It instead sets `tangentsCalculated = false`
   (`Model_md5.cpp:337`).
5. `Simd_Generic.cpp:2841` — skinning writes `verts[i].xyz` **and nothing else**. Normals
   are never a product of `TransformVerts`.
6. `vk_accelstruct.cpp:468` / `:644-649` / `:1290` — the RT path reads `tri->verts` or
   `geo->ambientCache`, and `rt_InterpolateNormal` (`rt_material.glsl:177`) reads the
   `normal` field at float offset 5 of that same `idDrawVert`.

So the raster path's lazy, cull-dependent derivation is load-bearing for RT, and RT has no
way to know whether it ran.

**Observed with the experimental third-person toggle**, so this is the full player model as
an ordinary drawn entity — not the suppressed first-person body. It is in `viewEntitys` (the
TLAS has it, the mirror shows it) while the primary camera is pointed at a glass pane, which
is exactly the position where its surfaces lose the frustum test in step 2. That is the
general failure mode, not a player-specific one: anything RT wants and the rasteriser
skipped arrives with stale normals.

## Why it reads the way it does

- **Wrong side** — lighting direction is locked to the bind pose, not the animated pose.
- **Hard edge** — it is the `N·L = 0` locus of that wrong normal field, and `N·L <= 0`
  returns early in `rt_LightContribAt`, so the transition is a step, not a ramp.
- **Only the character** — static world normals are derived once at load and are correct.
- **Not a shadow** — no occluder is involved, which is why `r_rtReflAmbientScale` does
  nothing to it and why it survives in an open hall.

## Blast radius

Not a reflection bug. Every RT consumer of a dynamic model's normals is affected:
GI bounce (`gi_ray.rchit`), volumetrics, and both reflection hit shaders. Reflections are
merely where it became visible, because the player is the one model guaranteed to hit the
broken path every frame.

**Prediction worth checking, and it is a sharp one:** the same model should be *correct*
while on screen and *wrong* the moment it leaves the primary frustum but stays in the TLAS.
Point the camera at a mirror with an animated NPC beside it and pan until the NPC leaves the
frame — its reflection should break at the frustum edge. If it doesn't, this diagnosis is
incomplete.

## Confirmation

`r_useDeferredTangents 0` — forces `R_DeriveTangents` inside `UpdateSurface` before
anything else touches the mesh. Confirmed in-game 2026-09-19: the terminator corrects.

---

## Stages

### N1 — Land the proven fix

`r_useDeferredTangents 0` is already the confirmed correct behaviour, and the deferral it
disables buys much less once RT is on: nearly everything in the TLAS needs normals, so the
work being deferred is work that has to happen anyway.

Make RT depend on it explicitly rather than leaving it to a cvar the user can flip out from
under the renderer. Do **not** silently force the cvar — either gate the RT dynamic-geometry
path on it with a one-time warning when it is off, or derive in the RT path (N2).

**Measure first:** `R_DeriveTangents` per md5 mesh per frame is the cost Doom 3 added the
deferral to avoid. Record the frame-time delta with a few characters on screen before
deciding whether N2 is needed at all.

**Exit:** terminator correct in mode 7 with the cvar at its normal default; cost recorded.

### N2 — Narrow it to the RT path *(only if N1's cost justifies it)*

Derive only for surfaces actually entering the TLAS, instead of for every md5 mesh.

**The ordering constraint is the whole difficulty.** For dynamic entities the TLAS refresh
prefers `geo->ambientCache` (`vk_accelstruct.cpp:1290`) — a GPU copy. Deriving on the CPU
*after* that cache was uploaded fixes `tri->verts` and changes nothing the shader reads. So
the derivation has to happen before the cache is created, not before the BLAS is built.
Either:

- hook right after `UpdateSurface` for entities destined for the TLAS (correct ordering,
  needs the RT entity set known at that point), or
- have the RT path upload its own vertex copy for dynamic surfaces rather than aliasing
  `ambientCache` (more memory and a copy, but removes the dependency on raster frame order
  entirely — and that dependency is the actual defect here).

The second is the structurally honest one. Cost it before choosing.

**Exit:** N1's fix reproduced with the cvar back at its default and the cost recovered.

### N3 — Validate beyond the player

Run the frustum-edge test above, then re-check GI and volumetrics on animated models — both
have been consuming these normals the whole time and neither has been looked at with this in
mind.

**Exit:** the frustum-edge test shows no break, plus a before/after on one animated NPC
under GI.

---

## Not the cause — ruled out during the investigation

- **Box falloff cliff.** `rt_light_eval.glsl:135-152` does replace the raster path's
  texture-driven falloff (`interaction.frag:99`) with an L∞ box that is flat to
  `maxNorm 0.8` then drops 10× by `1.0`. Real, unfaithful to raster, and worth its own
  item — but it would cut the floor along the same plane, and the floor is clean.
- **Cone cutoff / cookie clip.** Both hard-edged (`rt_light_eval.glsl:163`,
  `rt_light_cookie.glsl:52`), both irrelevant here: these are point lights.
- **Occluder shadow.** Open hall, no geometry between light and subject.
- **The B1 cull mask.** B1 was correct and is not implicated; it removed the self-shadow
  and exposed this underneath.
