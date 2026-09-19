# Dynamic model normals are stale in RT

**Date:** 2026-09-19
**Status:** ✅ **Closed 2026-09-19.** N1 landed + measured (0.16 ms/frame, 12 characters).
N2 dropped — the cost doesn't justify it. N3 dropped — the fix is unconditional, so there is
no frustum-edge case left to test.
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

### N1 — Land the proven fix — *landed 2026-09-19, cost measured*

The deferral is overridden for deformed surfaces whenever the TLAS is live, independently of
`r_useDeferredTangents`. `r_useDeferredTangents` keeps its meaning for the raster path.

- `R_DeriveDeformedTangents` (`tr_trisurf.cpp`) owns the decision and the instrumentation;
  `Model_md5.cpp` and `Model_liquid.cpp` call it in place of their `if
  (!r_useDeferredTangents)` blocks.
- `VK_RT_TLASActive()` (`vk_accelstruct.cpp`) is the predicate, now also the single source of
  truth for `vk_backend.cpp`'s TLAS gate.
- `r_rtDeformedTangents` (default 1) restores the deferral for A/B measurement, with a
  one-time warning that normals will be bind-pose.
- `r_showDynamic` gained `rtTang:<n> (<ms>)` — count and CPU time of the forced derivations.

**Cost is smaller than the plan assumed.** md5 meshes have `dominantTris`, so derivation goes
through `R_DeriveUnsmoothedTangents`, which early-returns on `tangentsCalculated`. For a mesh
the raster path would have derived anyway the work only moves earlier and costs nothing; the
real added cost is one unsmoothed derivation per *culled* md5 mesh per frame.

**Measured 2026-09-19:** `rtTang:48 (0.16 ms)` steady, 12 characters on screen
(`md5:12`, ~4 meshes each), range 0.160-0.200 ms with no drift. Against an RT budget near
12 ms this is noise.

**Exit met.** Cost recorded; terminator corrects with `r_useDeferredTangents` at its
default 1.

### N2 — Narrow it to the RT path — **dropped 2026-09-19**

N1's 0.16 ms does not justify the surgery below. Kept for the record, and because the second
option is the one to reach for if the RT path ever needs to stop aliasing `ambientCache` for
another reason.

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

### N3 — Validate beyond the player — **dropped 2026-09-19**

The frustum-edge test existed to find cases N1 might have missed. N1 derives for *every*
deformed surface whenever the TLAS is live, with no dependence on culling, view, entity or
consumer — so there is no edge left to sit on. GI and volumetrics read the same
`idDrawVert.normal` the reflections do and are fixed by the same change.

Reopen only if a dynamic model shows bind-pose lighting in GI or vol despite `rtTang` being
non-zero in `r_showDynamic`; that would mean a deformed model type reaching the TLAS through
neither `Model_md5.cpp` nor `Model_liquid.cpp`.

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
