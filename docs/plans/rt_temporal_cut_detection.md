# RT Temporal Cut Detection — fix the false-positive-every-frame bug

**Date:** 2026-08-31
**Status:** ✅ Implemented 2026-08-31, in two parts — see "Second bug" below, found
via this fix's own diagnostic logging. Not yet re-validated in-game.
**Motivates:** GI/AO/volumetric temporal accumulation (EMA) is effectively inert —
camera-cut detection is firing on ordinary mouselook, not just real cuts, so history
gets discarded (or nearly discarded) far more often than intended, undermining the
whole point of the temporal pass. This has been a known "unfixed" line item in
`completed/202608_ROADMAP.md`'s Wave 5 status and `portal_area_lights.md`'s Stage 2
(built then reverted because it depended on this) since at least 2026-08-19.

---

## The bug

All three temporal passes (`vk_temporal.cpp`'s AO dispatch, `vk_temporal.cpp`'s GI
dispatch, `vk_vol.cpp`'s volumetric dispatch) carry an identical, independently
copy-pasted block:

```cpp
// build invViewProj from projectionMatrix * modelViewMatrix, then invert
idMat4 inv = vpMat.Inverse();
memcpy(invVP, inv.ToFloatPtr(), 16 * sizeof(float));
...
float maxDiff = 0.0f;
for (int i = 0; i < 16; i++)
{
    float d = fabsf(invVP[i] - prevInvVP[i]);
    if (d > maxDiff) maxDiff = d;
}
if (maxDiff <= r_rtAOTemporalCutThreshold.GetFloat() /* default 0.5 */)
    effectiveAlpha = temporalAlpha;   // accumulate
else
    /* treat as a cut, reset history */
```

This compares raw elements of the **inverse** view-projection matrix with a fixed
absolute epsilon (0.5). Two things make that the wrong test:

1. **The inverse view-projection matrix is not uniformly scaled.** Its elements map
   clip-space coordinates back to world space, so entries associated with distant
   geometry (near the far plane) are far more sensitive to small camera rotation
   than entries associated with near geometry. A one-degree mouselook turn can move
   a far-plane unprojection point by hundreds of world units — which shows up as a
   correspondingly huge swing in specific `invVP[i]` entries, even though nothing
   resembling a "cut" happened.
2. **Doom 3's infinite-far-Z projection matrix makes this worse, not just present.**
   `amd_vulkan_cleanup.md`'s A12 already diagnosed this exact projection
   (`projectionMatrix[10] = -0.999f`, non-reversed) as having world-position error
   per depth ULP that grows as `d²/zNear` — i.e. the matrix is close to
   ill-conditioned at the far end by construction. Inverting an ill-conditioned
   matrix amplifies whatever perturbation was already there. A12 found this
   destabilizes *shadow-ray reconstruction*; this is the same root cause showing up
   a second time, in the *cut-detection* math instead of a shading ray.

Net effect: `maxDiff` routinely blows past a fixed `0.5` threshold from nothing more
than normal mouselook, so the "is this a cut" test reads as "yes" far more often
than a real cut occurs, and temporal accumulation gets reset (or degraded toward
`effectiveAlpha = 1.0`, i.e. no history blend at all) on an ordinary frame. This
matches the user-visible report precisely: camera cuts are detected "every frame."

## Confirmed (2026-08-31, in-game log capture)

`r_vkLogRT 1` was captured during ordinary play — standing still and moving/turning
slowly, no teleport, no level transition. Every "camera cut" line fired with
`maxDiff` around **472** (`dhewm3log.txt`, frame 570 shown below), roughly 1000x
the `0.5` threshold, on effectively every frame including while stationary:

```
[22440] VK RT Temporal: camera cut detected slot=1 maxDiff=472.0678 — resetting history
[22442] VK RT GI Temporal: camera cut slot=1 maxDiff=472.0678 — resetting history
[22443] VK RT Vol Temporal: camera cut slot=1 maxDiff=472.0678 — resetting history
```

All three passes report the identical `472.0678` in the same frame — expected,
since all three independently build `invVP` from the same
`projectionMatrix`/`modelViewMatrix` for that frame, confirming this isn't
per-pass drift, it's the shared math.

"Even standing still" is not a contradiction of the theory above, it's further
confirmation of it: standing still doesn't mean zero motion — idle view sway,
weapon-bob-at-rest, and ordinary float noise still perturb the camera by a
fraction of a degree every frame. The bug is precisely that the inverse
far-Z-projection matrix turns that imperceptible nudge into a huge element-wise
delta. A direct position/angle-delta test would read that same real-world nudge as
the near-zero motion it actually is. This is the number to beat: after the fix,
the same stand-still/slow-turn capture should show `posDelta`/`angleDelta` near
zero and no cut lines at all, with cuts reappearing only across an actual
teleport/respawn/level-load.

## The fix

Stop testing the ill-conditioned inverse matrix at all. Test the two physical
quantities a "cut" actually is: **the camera teleported (position jumped)** or
**the camera snapped to a new orientation (view jumped)** — both of which are
already available on the CPU with no matrix build or inversion required:

```cpp
const idVec3 &pos = viewDef->renderView.vieworg;
const idVec3 &fwd = viewDef->renderView.viewaxis[0]; // forward, already unit length

// vs. the previous frame's stored pos/fwd for this frame-in-flight slot:
float posDelta   = (pos - prevPos).Length();          // world units moved this frame
float cosAngle   = fwd * prevFwd;                     // dot product, forward vectors
bool  isCut = (posDelta > r_rtTemporalCutPosThreshold.GetFloat()) ||
              (cosAngle < cosThresholdFromDegrees);    // precompute cos() from a degrees cvar
```

This is strictly better on every axis that mattered for the bug:
- **Numerically robust regardless of projection conditioning** — position and
  forward direction are read directly from `renderView`, never multiplied through
  the far-Z-sensitive projection/inverse-projection matrices at all.
- **Physically interpretable thresholds.** "Reset history if the camera moved more
  than N world units in one frame" and "...or rotated more than M degrees" are
  numbers that can be reasoned about and tuned directly, unlike an L-inf epsilon
  over 16 matrix entries of mixed units. Suggested starting points: position ~4-8
  units/frame (a fast player sprint is a few units/frame at 60fps; a teleporter or
  respawn jumps hundreds+), angle ~3-5 degrees/frame (ordinary mouselook at typical
  sensitivity stays well under this per-frame; a cinematic cut or `noclip` snap
  exceeds it easily).
- **Cheaper.** Building `vp` and calling `idMat4::Inverse()` every frame per pass
  (three times, once per pass, all discarding the result immediately after the
  diff) goes away entirely for the cut check. (The AO/GI/vol shaders may still need
  an inverse-view-projection for their own reconstruction math elsewhere — this
  only removes the redundant one built solely for the cut test.)

### Consolidate the three copies

All three passes want the identical test. Factor it into one function (e.g.
`VK_RT_DetectCameraCut(const viewDef_t *viewDef, idVec3 &prevPos, idVec3 &prevFwd)`
in `vk_temporal.cpp`, called from both its own AO/GI dispatch functions and
exported for `vk_vol.cpp` to call) instead of three independently-maintained
copies. This is exactly the kind of triplication the project already has a
precedent for fixing (`rt_light_eval.glsl`'s P5 extraction) — and it's also *why*
this bug likely persisted: `portal_area_lights.md`'s Stage 2 notes the fix was
attempted and reverted once already, but per-pass copies mean a fix to one doesn't
propagate to the other two.

### CVars

Replace the single, misleadingly-named (`AO`-prefixed but shared by all three)
`r_rtAOTemporalCutThreshold` with two shared cvars used by all three passes:

- `r_rtTemporalCutPosThreshold` (world units/frame, default ~6)
- `r_rtTemporalCutAngleThreshold` (degrees/frame, default ~4)

Keep the old cvar name around as a no-op alias only if something external depends
on it (unlikely — it's an internal tuning knob); otherwise a clean rename is fine
since this is pre-release engine work, not a shipped user-facing setting.

## Validation plan

1. Re-run the same "stand still, mouselook only" log capture from the confirm step.
   Expected: zero (or near-zero) cut detections during that window, for all three
   passes.
2. Explicitly re-test real cuts still fire: noclip teleport across the map,
   respawn after death, a cinematic skip, and a level transition. Expected: a cut
   fires exactly once at the transition, not a steady stream before/after it.
3. Visual check: GI/AO/volumetric noise should visibly smooth out over a couple of
   frames of ordinary walking (the EMA finally getting to run), with no new
   ghosting/smearing artifacts introduced by the pos/angle thresholds being too
   loose. If ghosting appears on fast turns, the angle threshold is too high — tune
   from the debug log's reported delta values, not by eye on the final composite
   (project pillar 6).
4. ✅ Done — the log line prints `posDelta`/`angleDelta` instead of the old
   `maxDiff`. A `r_vkLogRT 2` raw-vector dump (`VK RT CutDbg`) also exists for
   when the numbers alone aren't enough to tell what's wrong — this is what
   caught the second bug below.
5. Re-check GPU cost: AO in particular was doing a full duplicate ray trace every
   frame (no dedup guard at all, unlike GI/Refl/Vol) against the degenerate GUI
   view. A profiler capture before/after should show AO's phase cost drop by
   roughly half.

## Why this matters beyond smoothing

This has been silently capping quality on every temporal consumer:
- GI/AO noise reduction from the EMA never actually accumulates across ordinary
  camera motion — every pass has effectively been running closer to
  `r_rtGISamples`/`r_rtAOSamples`-per-frame quality than the multi-frame-accumulated
  quality the temporal pass was built to deliver.
- `portal_area_lights.md`'s Stage 2 (light-set transition blending across a
  hop-boundary pop) was shelved specifically because it "depended on a still-broken
  GI/Vol temporal camera-cut fix" — fixing this reopens that as a real option, not
  a required follow-up.

## Second bug, found via this fix's own diagnostics (2026-08-31)

After the position/angle fix landed, a fresh capture showed `posDelta`/`angleDelta`
identical to several decimal places on **every single call for an entire session**,
on both frame-in-flight slots — impossible for real camera motion (which varies
continuously), and a strictly stronger signal than the original matrix-diff ever
gave, since it's now physically interpretable. A `r_vkLogRT 2` diagnostic added to
`VK_RT_DetectCameraCut` (dumps the raw vectors, not just the delta) showed exactly
why:

```
pos=(1152.84 -1294.90 68.25) prevPos=(0.00 0.00 0.00) fwd=(-0.612 -0.786 -0.090) prevFwd=(0.000 0.000 0.000)
pos=(0.00 0.00 0.00) prevPos=(1152.84 -1294.90 68.25) fwd=(0.000 0.000 0.000) prevFwd=(-0.612 -0.786 -0.090)
```

`prevPos`/`prevFwd` were correctly persisting the previous call's values (ruling out
a storage/reference bug) — the calls themselves alternate between the real player
camera and a fully zeroed one. Cross-referencing frame/slot numbers in the same log
showed both calls landing in the *same* `tr.frameCount`/slot, and
`vk_backend.cpp:4248`'s own comment explains why: **"Doom 3 submits multiple
RC_DRAW_VIEW commands per EndFrame (e.g. the 3D world view followed by the 2D
GUI/menu overlay)."** The existing guard around the whole AO/Refl/GI/Vol block
(`if (!viewDef->isSubview && !viewDef->isMirror)`) excludes mirrors and portal
subviews, but the GUI/HUD overlay is neither of those — it sailed straight through
with a `renderView` that was never populated (no 3D camera makes sense for a 2D
overlay), corrupting the "previous frame" state for whichever real pass ran next.

**First attempt (wrong, reverted same day):** the engine has a comment that looks
like exactly the right signal (`tr_local.h:436-439`):

```cpp
struct viewEntity_s *viewEntitys; // chain of all viewEntities effecting view...
// we use viewEntities as a check to see if a given view consists solely
// of 2D rendering, which we can optimize in certain ways.  A 2D view will
// not have any viewEntities
```

Gating on `viewEntitys != NULL` immediately caused GI ghosting at disoccluded
screen edges during camera motion (visible in-game; AO/Vol far less visibly, same
cause) — because `viewEntitys` only chains actual game **entities**
(`R_SetEntityDefViewEntity`, `tr_light.cpp:441`); static world/brush geometry
renders through the separate `worldSpace` viewEntity and never touches that list.
Any ordinary corridor with zero monsters/items on screen — extremely common —
also has `viewEntitys == NULL` and was being wrongly skipped by the same guard.
The engine's comment describes a valid optimization for whatever the *original*
GL-renderer consumer of that field needed; it is not a safe "is this a real 3D
view" test for a full-screen effect that must run on empty rooms too.

**Actual fix:** test the camera itself, using the exact degenerate signature the
`CutDbg` dump showed directly — the GUI overlay's `viewaxis` is the zero vector,
which no real camera (with or without visible entities) ever has:

```cpp
const bool hasRealCamera = backEnd.viewDef->renderView.viewaxis[0].LengthSqr() > 0.0001f;
if (!backEnd.viewDef->isSubview && !backEnd.viewDef->isMirror && hasRealCamera)
```

This is not merely a cut-detection fix — GI and Reflections
happened to have their own independent per-`tr.frameCount` dedup guards
(`s_lastGIDispatchFrame`/`s_lastReflDispatchFrame`) that silently absorbed the
duplicate call for their *ray dispatch* specifically, but nothing downstream of
that (GI Temporal, à-trous, albedo mod, Vol Temporal, bilateral, and all three
composite draws) had any such guard and re-ran unconditionally every frame. Worse,
**AO's dispatch has no dedup guard at all** (`vk_ao.cpp` — unlike GI/Refl/Vol) and
was genuinely re-running its full ray trace a second time, every frame, against
the degenerate view — real wasted GPU cost the whole time, not just a logging
artifact. The `viewEntitys` guard fixes all of this at the source, upstream of
every one of those individually-inconsistent guards.

## Files touched

- `neo/renderer/Vulkan/vk_raytracing.h` — `vkRTCameraCutResult_t` +
  `VK_RT_DetectCameraCut` declaration; `{ao,gi,vol}PrevInvViewProj[16]` replaced
  by `{ao,gi,vol}PrevCamPos`/`PrevCamFwd` (idVec3, 6 floats instead of 16 per slot)
- `neo/renderer/Vulkan/vk_temporal.cpp` — `VK_RT_DetectCameraCut` implementation;
  `r_rtAOTemporalCutThreshold` replaced by `r_rtTemporalCutPosThreshold` (world
  units, default 6) and `r_rtTemporalCutAngleThreshold` (degrees, default 4); AO
  and GI dispatch functions call the shared helper instead of building/inverting
  a matrix themselves
- `neo/renderer/Vulkan/vk_vol.cpp` — volumetric dispatch calls the same shared
  helper; dropped the now-unused `extern idCVar r_rtAOTemporalCutThreshold`
- `neo/renderer/Vulkan/vk_backend.cpp` — added a `hasRealCamera` (non-degenerate
  `viewaxis[0]`) check to the AO/Refl/GI/Vol dispatch guard (the second, deeper
  bug above)

No shader changes — this is CPU-side cut detection only, upstream of the
`effectiveAlpha` value already passed into each EMA compute dispatch.
