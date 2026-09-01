# RT Temporal Cut Detection — fix the false-positive-every-frame bug

**Date:** 2026-08-31
**Status:** Spec only — not started.
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

## Confirm before fixing (debug-first, per project convention)

Each of the three sites already has a gated log line
(`r_vkLogRT >= 1`, `"... camera cut ... maxDiff=%.4f"`). Before writing any fix:

1. Set `r_vkLogRT 1`, stand still, then do nothing but mouselook (no teleport, no
   level transition) for ~10 seconds.
2. Grep the resulting log for "camera cut" across all three passes (AO/GI/Vol) and
   count hits vs. total frames in that window, and note the reported `maxDiff`
   values against the `0.5` threshold.
3. Expected finding: cuts firing on most/all frames during rotation, with `maxDiff`
   values well above 0.5 (likely by one or more orders of magnitude) purely from
   turning the camera — this is the number to have in hand before/after the fix,
   not a theoretical argument.

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
4. Extend the existing log line to print `posDelta`/`angleDelta` instead of (or
   alongside) the old `maxDiff`, so future tuning has real numbers to look at.

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

## Files touched (expected)

- `neo/renderer/Vulkan/vk_temporal.cpp` — AO + GI cut-detection blocks replaced by
  the shared helper; new cvars
- `neo/renderer/Vulkan/vk_vol.cpp` — Vol cut-detection block replaced by a call to
  the shared helper
- `neo/renderer/Vulkan/vk_raytracing.h` — replace `{ao,gi,vol}PrevInvViewProj[16]`
  storage with `{ao,gi,vol}PrevCamPos`/`PrevCamFwd` (smaller, too — 6 floats
  instead of 16 per slot)
