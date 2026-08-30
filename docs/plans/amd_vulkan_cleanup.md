# AMD Vulkan RT Cleanup — findings and plan

**Date:** 2026-08-26
**Status:** Live — audit complete and cross-checked against a second independent
audit. **A1 and A3 implemented** (2026-08-26) — everything above the retest line
is done. **Retest run 2026-08-29**: two symptoms reported (GI à-trous inert,
volumetrics washed light-blue) plus a new device-lost crash — see A8 below and
the retest note at the end of the Order of work section. **A1 is NOT confirmed
fixed** — the retest report covers GI/volumetrics only; shadows, AO, and
reflections were not specifically re-checked, so their pre-A1 blob behavior is
unknown, not "gone." Do not treat A1 as closed until each of the four RT effects
is individually re-verified. A2, A4-A7 still outstanding.
**Trigger:** RT build run on an AMD Radeon 9700 XT. Every RT effect (shadows, AO,
GI, reflections, volumetrics) produced large saturated white blobs. The same build
is correct on NVIDIA.

## Why AMD and not NVIDIA

Three driver behaviours account for essentially every bug in this class, and all
three are things NVIDIA hides:

1. **Fresh device memory.** NVIDIA's allocator hands back zeroed pages in practice;
   AMD's does not. Anything read before it is written gives zeros on NVIDIA and
   garbage on AMD.
2. **Shader group handles.** NVIDIA's handles degrade fairly gracefully when a
   garbage record is fetched from an SBT; AMD's are closer to raw code addresses,
   so an out-of-range fetch produces arbitrary behaviour or a fault.
3. **Layout- and bounds-strictness.** AMD honours image layout transitions
   (HTILE/DCC compression state) and buffer bounds literally. NVIDIA no-ops most
   depth layout transitions and clamps many out-of-range accesses in hardware.

Findings A1-A4 are ordered by how much of the reported symptom they explain.
A5 and A6 were found while cross-checking a second audit; A5 is a vendor-neutral
visual bug that explains nothing about the blobs but was found in the same sweep
and is worth fixing, A6 is a latent hazard.

---

## A1 — SBT hit-region overrun on player-body instances  **[critical]**

All four RT pipelines share one TLAS, and TLAS instances carry a hardcoded
hit-record offset:

```cpp
// vk_accelstruct.cpp:1207 and :1559
inst.instanceShaderBindingTableRecordOffset = ent->parms.noSelfShadow ? 2 : 0;
```

The comment above it says this selects `player_reflect.rchit`. That is correct
**for the reflection pipeline only**, whose hit region holds 4 records
(`vk_reflections.cpp:761`, `size = 4 * stride`, laid out as world-main /
world-probe / player-main / player-probe). The other three pipelines have a
**one-record hit region**:

| Pipeline | hit region | SBT buffer | offset 2 resolves to |
|---|---|---|---|
| Shadow (`vk_shadows.cpp:385`) | `base + 2*stride`, size `stride` | `3*stride` | `base + 4*stride` — **past end of buffer** |
| AO (`vk_ao.cpp:401`) | `base + 2*stride`, size `stride` | `3*stride` | `base + 4*stride` — **past end of buffer** |
| GI (`vk_gi.cpp:736`) | `base + 3*stride`, size `stride` | `4*stride` | `base + 5*stride` — **past end of buffer** |

All three raygen shaders pass SBT offset 0 and SBT stride 0
(`shadow_ray.rgen:380`, `ao_ray.rgen:149`, `gi_ray.rgen:161`), so the hit-group
index reduces to the instance's record offset — 2. The SBT buffers are small
dedicated allocations, so the read lands in unrelated or unmapped VRAM and the
fetched shader handle is garbage.

The player body and weapon viewmodel are `noSelfShadow` and are on screen
essentially always, so every RT effect breaks the moment a ray touches them.
That matches the reported symptom exactly: *any* RT effect, large blobs.

### Per-pipeline exposure (revised)

The three affected pipelines are **not** equally exposed, and an earlier draft of
this doc overstated shadow and AO:

- **GI — unambiguous.** `gi_ray.rgen:159` traces with `gl_RayFlagsNoneEXT`, so the
  closest-hit shader is invoked on every hit and the hit record is fetched every
  time a ray touches a player-body instance.
- **Shadow and AO — narrower.** Both trace with
  `gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT`, and
  world geometry is built with `VK_GEOMETRY_OPAQUE_BIT_KHR`
  (`vk_accelstruct.cpp:510`). An opaque hit invokes no shader at all, so the
  driver most likely never fetches the record. Real exposure is limited to
  *perforated / translucent* player-body geometry, which does invoke any-hit.

Shadow rays do still reach player instances in normal play: `rayCullMask` is
`0xFF` by default and only drops to `0xFE` when a light is nearer than
`playerExcludeDist` (`vk_shadows.cpp:914`). So the path is live for shadow, just
narrower than for GI.

Net: A1 remains the best single explanation for "any RT effect", but GI carries
most of the weight. If the blobs survive the A1 fix, GI-specific behaviour is not
the place to look next — A6 is.

### Fix

Widen the shadow / AO / GI hit regions to 3 records and write the same hit-group
handle into all three, so instance record offsets 0..2 all resolve to the correct
group. Record 1 is unused padding (nothing traces with `sbtRecordOffset` 1 in
these pipelines) but must exist for record 2 to be in range.

This is the right shape of fix because the offset genuinely has to be
*per-instance* — the reflection rgen cannot know at trace time which instance is
the player — so it must stay in the TLAS, and the consumers must be able to
absorb it. Reflections is already correct and must not be touched.

- [x] `vk_shadows.cpp`: `sbtSize` `3 → 5` strides; write hit handle to slots 2, 3, 4;
      `hitRegion = {base + 2*stride, stride, 3*stride}`
- [x] `vk_ao.cpp`: same shape — `sbtSize` `3 → 5`, hit handle to slots 2, 3, 4,
      `aoHitRegion = {base + 2*stride, stride, 3*stride}`
- [x] `vk_gi.cpp`: `sbtSize` `4 → 6` strides; hit handle (group 2) to slots 3, 4, 5;
      `giHitRegion = {base + 3*stride, stride, 3*stride}`
- [x] Extend the existing `r_vkLogRT >= 1` SBT log lines to print hit-region
      `size / stride` (i.e. record count) alongside the addresses, so a future
      mismatch is visible in a log rather than only on an AMD screen.
      Added `missRecords=` / `hitRecords=` to all four pipelines, including
      reflections (unchanged otherwise) so the four lines are directly comparable.

**Implemented 2026-08-26.** Each of the three sites carries a comment explaining
why the hit region is wider than the group count, so the next person to touch the
SBT does not "tidy" the padding record away. Expected log now reads
`hitRecords=3` for shadow / AO / GI and `hitRecords=4` for reflections.

Cost is a few hundred bytes of SBT per pipeline. No shader changes, no TLAS
changes.

### Alternative considered and rejected

Setting all instances to record offset 0 and selecting the player hit group via
the rgen's `sbtRecordOffset` argument. Rejected: the rgen has no per-instance
knowledge, which is precisely why the offset lives on the instance.

---

## A2 — AO mask image is never cleared and only partially written  **[high]**

`vk_ao.cpp:165-200` transitions the AO image `UNDEFINED → GENERAL` but never
clears it. This is inconsistent with the rest of the codebase: the GI, temporal,
tonemap and vol images all get a `vkCmdClearColorImage` at creation
(`vk_gi.cpp:429`, `vk_gi.cpp:2226`, `vk_temporal.cpp:1005`, `vk_tonemap.cpp:162`,
`vk_vol.cpp:407`, `vk_vol.cpp:921`), and the shadow mask gets a per-frame clear
at `vk_shadows.cpp:1293`.

Two ways uninitialized texels survive to be sampled:

- The AO rgen only writes inside the view scissor rect (`ao_ray.rgen:64-74`).
  Anything outside is never touched — relevant for subviews (mirrors, cameras,
  security monitors) and for any frame whose scissor is not full-screen.
- `VK_RT_DispatchAO` has several early-return paths that leave the image entirely
  untouched for a frame.

The interaction shader samples the mask full-screen whenever the image merely
*exists* — `vk_backend.cpp:1122` only null-checks it, it does not check that
anything was written this frame. Uninitialized VRAM sampled as an AO multiplier
saturates the lighting.

**Sharpened by A5.** Since the AO raygen currently writes a constant 1.0
everywhere it runs at all (see A5), *every* AO texel that is not uninitialized is
neutral. That makes uncleared / never-dispatched regions the **only** way the AO
path can contribute a blob today. It tightens the diagnosis rather than weakening
it: if AO is implicated in the blobs at all, it is via this finding specifically.
Note also that the fixes interact — A5 makes AO start producing real occlusion,
which is exactly when a stale or uninitialized region becomes visually obvious
rather than blending into a uniformly white mask.

The reflection image has the same missing clear at `vk_reflections.cpp:377-386`.
Its rgen is full-screen, so exposure is limited to skipped-dispatch frames, but
the fix is the same and should land together.

### Fix

- [ ] Clear the AO image to white (1.0 = unoccluded, matching the shadow mask's
      "fully lit" convention) at creation in `VK_RT_CreateAOMaskImages`.
- [ ] Clear the reflection image to black at creation in `VK_RT_CreateReflImages`.
- [ ] Add a per-frame `aoValidThisFrame` flag set by `VK_RT_DispatchAO` on the
      path that actually reaches `vkCmdTraceRaysKHR`, and gate `useAO` in
      `vk_backend.cpp:1122` on it rather than on image non-nullness. This is the
      durable fix — the clear only covers frame 0, the flag covers every
      early-return frame thereafter.

---

## A3 — Two dead guards in the AO dispatch  **[high, trivial]**

`vk_ao.cpp:498` and `vk_ao.cpp:523` use bitwise `&` where `&&` was intended:

```cpp
if (!vkRT.tlas[vk.currentFrame].isValid & (r_vkLogRT.GetInteger() >= 1))
if (ao.image == VK_NULL_HANDLE & (r_vkLogRT.GetInteger() >= 1))
```

`==` binds tighter than `&`, so these parse as `(condition) & (logging enabled)`.
With `r_vkLogRT 0` — the normal case — **both safety checks are disabled**. An
invalid TLAS or a null AO image falls straight through to `vkCmdTraceRaysKHR`.
Tracing against a stale TLAS handle is undefined behaviour and AMD will not be
forgiving about it.

These are the only two instances of the pattern in `neo/renderer/Vulkan/`.

### Fix

- [x] Split each into a real guard plus a separate log statement, matching the
      shape already used at `vk_ao.cpp:505-509` for the pipeline-null check.

**Implemented 2026-08-26**, with one deviation from the wording above. The
pipeline-null check logs *unconditionally*; these two do not. The sibling
dispatchers settle it — `vk_gi.cpp:1741`, `vk_reflections.cpp:939` and
`vk_shadows.cpp:1127`/`:1321` all return **silently** on invalid-TLAS and on a
null mask image, and print unconditionally only for null-pipeline, which is a
hard error rather than a transient. An invalid TLAS is normal across level-load
frames, so an unconditional print would spam every load. Landed shape: guard
unconditional, log gated on `r_vkLogRT >= 1` — the original author's evident
intent, with the operator fixed.

Also reordered: the `r_useRayTracing` / `r_rtAO` early-out now precedes the TLAS
guard. Previously the TLAS check came first, which was harmless while the guard
was dead, but with it live and logging it would report "skip — TLAS not valid"
on frames where AO is simply switched off. Pure early-out reorder, no side
effects between the two.

Verified no other instance of the `&`-on-bool pattern remains anywhere in
`neo/renderer/Vulkan/`.

**Adjacent, not fixed:** `vk_ao.cpp:513` (`!vkRT.isInitialized`) also prints
unconditionally where every sibling returns silently. Pre-existing and outside
A3's scope, but it is the remaining source of per-frame AO log noise if it ever
fires.

---

## A4 — `robustBufferAccess` never enabled  **[hardening]**

`vk_instance.cpp:330-336` enables `samplerAnisotropy`, `depthClamp`, `shaderInt64`
and `independentBlend`, but leaves `features2.features.robustBufferAccess` at
`VK_FALSE`.

The material-table SSBOs are explicitly zero-filled at creation
(`vk_material_table.cpp:193-196`) and sized to the full `VK_MAT_MAX_GEOMS`, so
`materials[matIdx]` is always in-bounds and reads zeros for unpopulated slots —
that path is genuinely safe and needs no change.

The remaining exposure is the raw `buffer_reference` reads in
`rt_material.glsl:113-116`, which index by `primId` with no bounds check against
the index buffer. `PhysicalStorageBuffer` loads are never bounds-checked by any
vendor, and the existing `maxVertex` guard (`rt_material.glsl:120-122`) validates
the *result* after the out-of-bounds read has already happened.

Not believed to be a primary cause of the blobs, but on AMD `robustBufferAccess`
is nearly free (it is the `num_records` field of the buffer descriptor) and it
converts a class of silent garbage into deterministic zeros. Worth having on
while the rest of this is being chased.

### Deliberately ranked last — do not re-promote

A second independent audit ranked this finding **first**. Recording why it is
ranked last here, so the question does not get relitigated:

- The material SSBO is allocated at the full `VK_MAT_MAX_GEOMS` size and
  explicitly zero-filled (`vk_material_table.cpp:193-196`), so `materials[matIdx]`
  is in-bounds and reads zeros — not adjacent memory — for unpopulated slots.
- The address tables have the same treatment.
- The genuinely unprotected reads are the `buffer_reference` ones, which
  `robustBufferAccess` **does not cover on any vendor**.

So the feature's live exposure in this codebase is close to zero. Enable it, but
it is insurance against future code, not a fix for anything currently known.

### Fix

- [ ] Enable `robustBufferAccess` when supported, alongside the other feature
      requests in `VKimp_CreateLogicalDevice`.

---

## A5 — RT AO is inert: payload initialized to the wrong value  **[high, vendor-neutral]**

Not a blob cause — a separate, long-standing visual bug found while cross-checking
the payload-initialization question. **RT AO has never done anything.**

`ao_ray.rgen:144` initializes the payload to the *unoccluded* value before every
trace:

```glsl
// Miss shader sets aoPayload = 1.0 (unoccluded)
// Any-hit shader sets aoPayload = 0.0 (occluded) and terminates
aoPayload = 1.0;
```

The second comment line is stale. `ao_ray.rahit` calls `ignoreIntersectionEXT`
unconditionally and never writes the payload, and the AO pipeline has **no
closest-hit shader at all** (`vk_ao.cpp:341`, `closestHitShader =
VK_SHADER_UNUSED_KHR`). So:

| outcome | who writes the payload | value |
|---|---|---|
| miss | `ao_ray.rmiss` | 1.0 |
| opaque hit | nobody — skip-closest-hit, and opaque bypasses any-hit | 1.0 (the initial value) |
| non-opaque hit | `rahit` ignores it, ray continues | resolves to one of the above |

Nothing can ever write 0.0. `ao = unoccluded / N` is therefore always exactly 1.0,
and `imageStore(aoImage, coord, vec4(ao))` writes pure white for every pixel the
raygen touches. AO darkens nothing.

`shadow_ray.rgen:375` gets the identical pattern **right**, and its comment spells
out the reasoning that AO's is missing:

```glsl
// Initialize to 0.0 (shadowed). Opaque geometry bypasses the any-hit shader
// via VK_GEOMETRY_OPAQUE_BIT_KHR so the payload stays 0.0 = shadowed.
shadowFactor = 0.0;
```

The likely history: `ao_ray.rahit` was changed to ignore-all (to stop translucent
steam casting blocky AO blobs — its current comment explains that intent) and the
raygen's initial value was never flipped to compensate.

**Corroborated in-game.** The reported experience of RT AO has been that it is
"useless / barely noticeable". That is the exact predicted symptom of a mask that
is uniformly 1.0, and it is the strongest evidence available that this analysis is
right — this is a real observation matching a prediction, not just a code reading.

**How it slipped through.** The original Phase 5 acceptance criterion was
`r_rtAO 1` → "ambient surfaces show contact darkening"
(`completed/rtx_refactor_plan.md:244`). That is precisely the thing the code
cannot do, and the phase was signed off anyway — the effect was presumably read as
working-but-subtle rather than inert. Worth remembering when writing acceptance
criteria for the rest of this doc: "looks subtle" and "does nothing" are
indistinguishable by eye, which is why A5's validation step below specifies a
high-contrast test position rather than a general impression.

### Fix

- [ ] `ao_ray.rgen`: initialize `aoPayload = 0.0` before `traceRayEXT`, matching
      `shadow_ray.rgen`. The miss shader already supplies 1.0.
- [ ] Update the two stale comment lines above it to describe what the rahit
      actually does (ignore all non-opaque hits), mirroring `shadow_ray.rgen`'s
      wording so the invariant is written down at both sites.
- [ ] Re-tune `r_rtAORadius` / `r_rtAOSamples` afterwards. Any value previously
      settled on was tuned against a no-op and means nothing.

---

## A6 — Stale vertex/index device addresses in the static material table  **[latent]**

Raised by a second independent audit; verified here as a real structural hazard,
though not well matched to the reported symptom.

Three facts combine:

1. BLASes hold **non-owning** references to vertex-cache buffers —
   `vk_accelstruct.cpp:432-437` explicitly documents "Do not own/destroy these
   buffers from BLAS lifetime management".
2. `VK_VertexCache_Free` (`vk_buffer.cpp:186`) retires those buffers on its own
   deferred-garbage schedule, independent of BLAS lifetime.
3. The static half of the address table is uploaded **only when `rewriteStatic`
   is true** (`vk_material_table.cpp:647`), and `staticSignature` hashes the
   entity pointer, the BLAS device address and the transform
   (`vk_accelstruct.cpp:1461-1466`) — **not** the vertex/index buffer addresses.

So if a cache buffer is freed and reallocated underneath a BLAS that itself
survives with an unchanged device address, the GPU keeps a stale VA and nothing
in the signature notices. `rt_material.glsl`'s only defences are a non-null
address check and the `maxVertex` guard, and neither protects a
`buffer_reference` read that is merely *wrong* rather than null — those loads are
unbounded on every vendor.

Why it was ranked below A1: this failure is intermittent and scene-dependent
rather than "any RT effect, immediately." **Promoted 2026-08-29**: three separate
device-lost crashes on AMD (see A8's crash notes) all landed at or immediately
after `RT_GI` — the one pipeline whose `.rchit` unconditionally fetches these
`buffer_reference` addresses on every hit — reproduced by ordinary movement in a
large map (cache churn). This is now the leading crash theory, ahead of "generic
GPU hang."

### Fix

- [x] Fold the geometry vtx/idx device addresses into `staticSignature` so any
      change forces a static rewrite. **Implemented 2026-08-29**, both
      construction sites (`vk_accelstruct.cpp`, the cache-hit reuse path and the
      fresh-BLAS path). This is the minimal correct fix and closes the
      invalidation gap directly. Not yet re-tested on the 9700 XT.
- [ ] Log when a vertex-cache block is freed while its device address is still
      resident in a live frame's address table. Skipped for now — doing this
      right means cross-referencing every free against the material table's
      resident addresses across all frames-in-flight, which is either a
      non-trivial lookup structure or an O(n) scan on every free. The signature
      fix above closes the actual gap; this would only be corroborating evidence
      if the crash recurs.
- [ ] Longer term, reconsider the non-owning-reference design — a refcount or a
      generation counter on cache blocks would make this class of bug
      structurally impossible rather than signature-dependent.

---

## A7 — `VK_MAT_MAX_GEOMS` overflow clamps the counter but not the instances  **[latent]**

Also raised by the second audit. At `vk_accelstruct.cpp:1710-1714`, when
`staticGeomCount + dynamicGeomCount` exceeds the 16384 cap, only
`dynamicGeomCount` is truncated. The TLAS instance list is left intact, so
surviving instances can resolve `instanceCustomIndex + gl_GeometryIndexEXT` to
geometry slots that were never written this frame.

One nuance worth recording: because the SSBO is zero-filled, a never-written slot
yields `diffuseTexIndex == 0`, which is **the white fallback texture**. The
failure mode of this path is literally white geometry — so while it needs a
16k-geometry scene to trigger and is very unlikely to be the reported bug, its
symptom is in the right family and it should not be dismissed on symptom alone.

### Fix

- [ ] Drop whole trailing *instances* rather than clamping the geometry counter,
      so no surviving instance can reference an unwritten slot.

---

## A8 — GI/vol temporal EMA has no NaN/Inf guard; a poisoned texel never recovers  **[critical, retest finding]**

Found during the 2026-08-29 AMD retest, after A1+A3. Two symptoms reported: GI's
à-trous pass reads as doing nothing (still dotty), and volumetrics washes the
screen light-blue, both persisting for the rest of the play session. Note this is
NOT evidence that A1 fixed shadows/AO/reflections — those weren't specifically
re-checked this pass, see the Status line at the top of this doc. Root-caused by
reading, not yet re-confirmed on hardware — see Validation below for the two
zero-code experiments that would confirm it.

`gi_temporal_resolve.comp` is shared verbatim by both the GI temporal stage and
the volumetric temporal stage (`vk_vol.cpp` reuses the same `.spv`, see its own
comment at `vk_vol.cpp:776`). It is read-modify-write on `historyImage` every
frame:

```glsl
vec4 hist    = imageLoad(historyImage, coord);
vec4 blended = mix(hist, current, alpha);
imageStore(historyImage, coord, blended);
```

`alpha` is small in ordinary gameplay (`r_rtVolTemporalAlpha` default 0.15; GI's
equivalent is comparable) — history dominates, current is a small nudge. That's
fine for finite values, but `mix(hist, NaN, alpha)` is `NaN`, and `mix(hist, Inf,
alpha)` saturates towards `Inf` — once either lands in `current`, the texel is
poisoned in `historyImage` **forever**, not just for one frame: every subsequent
frame reads back its own poisoned value as `hist` and re-blends against it. A
plain large-but-finite outlier would at least decay geometrically (`0.85^n`); a
non-finite one does not decay at all. The only reset path is a detected camera
cut (`effectiveAlpha` snaps to 1.0, bypassing the history read at
`vk_vol.cpp:1181-1197`, mirrored for GI) — which won't help if the same
screen-space condition recurs on the next frame.

Two consequences, matching the two reported symptoms from one shared shader:

- **GI**: a poisoned/huge texel sits upstream of `gi_atrous.comp`. À-trous is an
  *edge-preserving* filter by design — `wD`/`wL` correctly read a NaN or
  multi-order-of-magnitude neighbour as a hard edge and refuse to blend across
  it. So à-trous isn't broken; it's doing exactly what an edge-stopping filter
  should do when fed a genuine outlier, which reads to the eye as "the filter
  does nothing, still dots."
- **Volumetrics**: `vol_composite.frag` additively blends `volReadView` (the
  temporal history) onto the framebuffer. A poisoned or saturated texel reads as
  a blown-out wash; a growing number of them over a play session (different
  screen-space conditions triggering the upstream spike at different times) reads
  as the reported light-blue wash across most of the screen. The specific hue
  (light-blue, not white) is not yet explained by this theory alone — an Inf/NaN
  additively blended per-channel should saturate toward whatever the scene's own
  color balance is, not a consistent tint. Worth checking whether one channel is
  saturating faster than the others (e.g. blue channel intensity/anisotropy
  constants differ from red/green) before assuming A8 is the whole story here.

**Plausible upstream trigger, not the only possible one:** `vol_march.comp`'s
`HenyeyGreenstein()` phase function is a true singularity as the forward-scatter
angle and anisotropy both approach 1 (an ordinary gameplay moment — staring
straight down a fog beam at a light). The pre-existing `max(denom, 1e-4)` floor
already let this spike into the thousands under normal play; `volBuf` is
`rgba16f` (max representable ~65504), so a bad frame's accumulated contribution
overflowing to `Inf` on `imageStore` is well within reach, and doesn't need to be
identical across vendors to explain an AMD-only report — different RT hit
ordering/jitter timing only needs to nudge one frame over the fp16 ceiling where
NVIDIA's happened not to.

### Fix

- [x] `gi_temporal_resolve.comp`: reject a non-finite `current` outright (keep
      existing `hist` rather than blend it in), and scrub `hist` itself if it was
      already poisoned before this guard existed. Never write a non-finite value
      into `historyImage` again.
- [x] `vol_march.comp`: raise `HenyeyGreenstein`'s denominator floor `1e-4 → 1e-2`
      (caps the peak ~8x lower, still visually a dramatic forward-scatter look)
      and clamp the final per-pixel `result` to `4096.0` before `imageStore` as a
      hard backstop independent of which upstream term caused the spike.

**Implemented 2026-08-29. Retested 2026-08-29, same session, and it did NOT fix
either symptom** — dots are still present in GI, and the volumetric wash is back
to being reported as white (not light-blue, contradicting the previous report; no
explanation yet for the color inconsistency between the two sessions either).
**A8's diagnosis is likely wrong, or incomplete.** Leaving the fix in place (it's
a correct hardening regardless — a temporal EMA should never propagate NaN/Inf
forever, independent of whether it's this bug's cause), but it is not the
explanation for the reported dots/blob. Do not spend more time on the
temporal-EMA theory without new evidence pointing back at it. Two things not yet
tried that would actually narrow this down:
- `r_useRayTracing 0` in the same spot — if the white blob is present with RT
  fully disabled, it's not an RT bug at all (asset/lighting/fog issue, possibly
  present on NVIDIA too and simply not noticed). This test costs nothing and
  hasn't been run.
- The zero-code experiments below were never actually run to confirm the
  mechanism before the fix went in — this whole finding was diagnosed from
  reading, not from an isolated repro. That ordering should not repeat: next
  hypothesis gets validated with a cvar toggle *before* a shader edit.

### Validation

Two zero-code experiments to run *before* trusting the fix, straight from a
retest build or even the pre-fix one — per the project's debug-over-theory rule:

- Set `r_rtVolTemporal 0` (disables only the volumetric temporal stage). If the
  white blob disappears, that confirms the temporal-EMA-poisoning mechanism for
  volumetrics specifically, independent of whether the fix above is correct.
- Temporarily set `r_rtGIAtrousSigmaL`/`r_rtGIAtrousSigmaZ` very large (e.g. 100)
  to defeat edge-stopping entirely. If the GI dots disappear (i.e. à-trous starts
  visibly blurring), that confirms à-trous was never the broken part — it was
  correctly refusing to blur real outliers — and points back at whatever's
  upstream feeding it those outliers.
- After the fix: `r_vkLogRT 1` during `VK RT Vol Temporal: camera cut ...` and
  `VK RT GI Atrous: N passes ...` lines won't show anything new (this bug doesn't
  log), so the observable here is purely visual — the blob/dots should stop
  building up over a long play session rather than staying finite-but-visible.

---

## A9 — `rayQuery` device feature/extension never enabled  **[critical]**

Found 2026-08-29 from an actual GPU-AV capture (first one that worked — see the
log-capture note at the end of this doc). Confirmed directly by the validation
layer, not inferred:

```
Validation Error: [ VUID-VkShaderModuleCreateInfo-pCode-08740 ]
vkCreateShaderModule(): SPIR-V Capability RayQueryKHR was declared, but
VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery is required.

Validation Error: [ VUID-VkShaderModuleCreateInfo-pCode-08742 ]
vkCreateShaderModule(): SPIR-V Extension SPV_KHR_ray_query was declared, but
VK_KHR_ray_query is required.
```

`vol_march.comp` is the only shader in the codebase using `GL_EXT_ray_query` /
`rayQueryEXT` (everything else uses the full ray-tracing pipeline,
`traceRayEXT`) — grepped to confirm. `vk_instance.cpp` never added
`VK_KHR_ray_query` to `rtDeviceExtensions[]`, and never queried or enabled
`VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery`. So every frame, that one
shader module was created in a state the spec calls undefined — the exact
"AMD is strict, NVIDIA quietly tolerates it" pattern this whole doc is about,
just in the one spot (ray *queries*, not the RT pipeline) nobody had checked.
Leading theory for the volumetric white/light-blue wash reported after A8
didn't fix it.

### Fix

- [x] `vk_instance.cpp`: added `VK_KHR_RAY_QUERY_EXTENSION_NAME` to
      `rtDeviceExtensions[]`; threaded `VkPhysicalDeviceRayQueryFeaturesKHR`
      through both the capability-query chain and the device-creation feature
      chain, same shape as the existing AS/RT-pipeline feature structs.
      **Implemented 2026-08-29.** Not yet retested.

### Validation

The fix is directly confirmable from the same log: `vklog.txt` should no longer
show `VUID-VkShaderModuleCreateInfo-pCode-08740`/`-08742`. Whether it also
explains the volumetric wash and/or the device-lost crashes is a separate,
open question — retest both.

**Note on getting GPU-AV output at all:** PowerShell's `*>` stdout redirection
does not reliably work for this GUI-subsystem exe — it produced an empty file
both for the user and independently for a same-machine sanity check. What
worked: a `vk_layer_settings.txt` next to the exe with
`khronos_validation.log_filename` pointing at an absolute path — the validation
layer writes there directly, bypassing the app's own stdio entirely. Use this,
not stream redirection, for any future capture.

---

## Audited and found clean

Recorded so this ground is not re-covered. Each of these was specifically
suspected and specifically checked.

- **BLAS → TLAS barrier.** Present and correct (`vk_accelstruct.cpp:1605-1614`),
  `ACCELERATION_STRUCTURE_WRITE → ACCELERATION_STRUCTURE_READ` across
  `ACCELERATION_STRUCTURE_BUILD` on both sides.
- **TLAS → ray-tracing barrier.** Present and correct
  (`vk_accelstruct.cpp:1933-1937`). The host-write → AS-build barrier for the
  mapped instance buffer is also there (`vk_accelstruct.cpp:1846-1853`).
- **Scratch buffer sizing.** Correct, and `vk_accelstruct.cpp:953-962` already
  takes `max(buildScratchSize, updateScratchSize)` — the subtle case most code
  gets wrong. Alignment against `minAccelerationStructureScratchOffsetAlignment`
  is not explicitly asserted, but every scratch buffer gets a dedicated
  `vkAllocateMemory` bound at offset 0, so the address is page-aligned in
  practice. An assert would be cheap insurance; not a bug.
- **SPIR-V / shader layout.** `nonuniformEXT` is applied at every bindless index
  (`rt_material.glsl:199`, `:216`, and all four `.rahit` shaders).
  `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND` are set on the material set layout with
  the matching device features correctly gated on support
  (`vk_material_table.cpp:247-260`, `vk_instance.cpp:348-353`).
  `buffer_reference_align = 4` matches the float-array access pattern for the
  60-byte `idDrawVert`. This area is in good shape.
- **Zero initialization of GPU-visible tables.** Material, vertex-address and
  index-address SSBOs are all `memset` to zero at creation
  (`vk_material_table.cpp:193-196`).
- **Ray payload initialization — *presence* only.** Raygens do re-initialize
  payloads before each `traceRayEXT` (e.g. `reflect_ray.rgen:207-208`), and all
  six miss shaders write the fields their consumers actually read. A second audit
  independently confirmed this, refuting a sub-agent claim that `aoPayload` /
  `shadowFactor` were read before being written — that claim is false.
  **But this check is not sufficient**, and both audits initially stopped here:
  asking "is the payload written?" passes, while asking "is the written *value*
  correct given this pipeline's ray flags and hit-group composition?" fails for
  AO. See A5. Any future audit of this area must ask the second question.
- **Depth image layout discipline.** Every `ATTACHMENT → READ_ONLY` transition is
  balanced by a restore on all paths, *including* the NaN early-outs
  (`vk_gi.cpp:1810-1826`, `vk_vol.cpp:1826-1841`, `vk_ao.cpp:781-791`). A stale
  depth layout is the other classic AMD-only corruption source and was searched
  for specifically; there is no unbalanced pair.
- **TLAS instance construction.** `memset` to zero before field assignment at all
  three construction sites (`vk_accelstruct.cpp:1181`, `:1417`, `:1541`), so
  `mask`, `flags` and the transform are never garbage.
- **SBT base alignment.** Handle/base alignment is queried from
  `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` and applied in all four
  pipelines rather than assumed.

---

## A10 — `primId` indexes the index buffer with no bounds check  **[critical, confirmed]**

Found 2026-08-29 from a real GPU-AV capture (the first one that actually
produced output — see A9). This is the thing A4 predicted but didn't chase:

```
Validation Error: [ VUID-RuntimeSpirv-PhysicalStorageBuffer64-11819 ]
vkCmdTraceRaysKHR(): Out of bounds: trying to read 4 bytes at
[0x2a4080020, 0x2a4080023) but no buffer device address was found at this range.
Nearest below: VkBuffer size 24 bytes, range [0x2a4080000, 0x2a4080018)
Stage = Closest Hit.
```

24 bytes = 6 × uint32 = 2 triangles. `rt_InterpolateUV`/`rt_InterpolateNormal`
(`rt_material.glsl`) and `glass_probe.rchit` all compute `base = primId * 3`
and read `iBuf.idx[base+0..2]` straight from a `buffer_reference` with **no
check that `primId` is within range** — `maxVertex` only validates the vertex
indices *read back*, after the index-buffer read already happened out of
bounds. A4 wrote this down as a known gap; this is it firing on real geometry
(a 2-triangle object) on AMD, which enforces `buffer_reference` bounds where
NVIDIA apparently doesn't fault on it.

### Fix

- [x] Added `maxPrimId` (numTriangles-1, sentinel `0xFFFFFFFF`) to
      `VkMaterialEntry`/`MaterialEntry` (`vk_raytracing.h`, `rt_material.glsl`;
      struct grew 32→36 bytes) and populated it from `geomIdxSizes[g]` at all
      four TLAS construction sites in `vk_accelstruct.cpp`, mirroring the
      existing `maxVertex` population exactly.
- [x] `rt_InterpolateUV`/`rt_InterpolateNormal`: check `primId <= maxPrimId`
      **before** touching `IdxBuf`, not after.
- [x] `glass_probe.rchit`: same `primId` check added (it had none at all,
      GLSL SSBO), plus a `maxVertex` guard on `i0/i1/i2` it was also missing.

**Implemented 2026-08-29.** Not yet retested.

---

## Order of work

A3 first (two-line change, removes a confound). Then A1, which is the one that
plausibly accounts for the entire symptom on its own. Re-test on the 9700 XT
before doing anything else — if the blobs are gone **on all four RT effects**,
everything below the retest line becomes ordinary correctness work rather than
urgent. (The 2026-08-29 retest only exercised GI/volumetrics — see A8 — so A1 is
still open pending shadows/AO/reflections being checked too.)

A5 sits below the retest line deliberately: it is vendor-neutral and changing AO
from "always white" to "actually occluding" mid-diagnosis would move the goalposts
on what the AMD retest is measuring. Fix the blobs first, then fix AO, then tune.

```
1.  A3  dead guards                      ~2 lines                   [DONE 2026-08-26]
2.  A1  SBT hit-region widening          ~15 lines across 3 files   [DONE 2026-08-26]
    → RETEST ON AMD HERE — everything above is blob-directed. Both landed;
      the retest is the next action and nothing below should start before it.
      → 2026-08-29 retest covered GI/volumetrics only (NOT shadows/AO/
        reflections — A1 unconfirmed for those): GI à-trous inert, volumetrics
        washed light-blue, plus a new VK_ERROR_DEVICE_LOST at frame 2854 during
        split-submit. A8 below addresses the first two; the crash is not yet
        root-caused — see note below the list. Shadows/AO/reflections on AMD
        still need their own explicit check.
3.  A8  temporal EMA NaN/Inf guard        ~30 lines, 2 shader files  [DONE 2026-08-29]
    → RETEST ON AMD AGAIN — confirm the à-trous/blob symptoms are gone before
      continuing down this list.
4.  A5  AO payload init 1.0 → 0.0        ~1 line + comments, then re-tune AO
5.  A2  image clears + aoValidThisFrame  ~40 lines  (do after A5: A5 is what makes
                                          a stale AO region visible at all)
6.  A6  vtx/idx addrs into staticSignature + free-while-resident logging
7.  A4  robustBufferAccess               ~1 line
8.  A7  drop trailing instances on geom overflow
```

**On the device-lost crash (2026-08-29 retest):** not yet root-caused — it needs
data this session didn't have (the full log, not just the tail around the crash).
Breadcrumbs show a normal frame (2854) executing the full RT chain start-to-finish
before `SplitSubmit_AfterRTResume`'s `vkQueueSubmit` comes back
`VK_ERROR_DEVICE_LOST`, i.e. the fault happened *during* GPU execution of that
batch, not at record time. It surfaced deep into a play session on `mars_city1`
(30447 interactions per the log — a large map), which is exactly the shape of
scene A6 (stale vertex/index device addresses surviving a freed-and-reallocated
vertex-cache buffer) is most likely to actually trigger.

`"combined geom count overflow"` was greped for across the full session log on
2026-08-29 and **not found** — A7 is ruled out as the cause of this crash. A6
remains the live candidate.

**Second crash captured, same session, with GPU-AV env vars set (`VK_INSTANCE_LAYERS`
/ `VK_LAYER_ENABLES`, ran at ~2fps consistent with GPU-AV actually being active):**
this time it faulted at `SplitSubmit_AfterGI` (frame 2189), not
`SplitSubmit_AfterRTResume` (frame 2854, the first crash). Different frame,
different exact breadcrumb, but **`RT_GI` is the stage immediately preceding the
fault window both times** — the first crash's batch also contains RT_GI upstream
of where it got to. GI's `.rchit` is the one shader among the four RT pipelines
that unconditionally invokes closest-hit (and therefore fetches the
`buffer_reference` material/vertex/index addresses A6 is about) on every ray hit,
including opaque geometry — shadow/AO skip closest-hit entirely via
`gl_RayFlagsSkipClosestHitShaderEXT`. Two device-losses both shadowing GI is
exactly the pattern A6 predicts and is now the leading theory for the crash,
ahead of "generic GPU hang."

**However: no VUID / GPU-AV validation error text has been seen yet.** The pasted
log excerpts are from the engine's own `dhewm3log.txt`, not captured stdout from
the Vulkan loader/validation layer — those are two different output streams. If
the validation layer produced any messages, they went to the console window,
which likely closed with the process on crash before they could be read. **This
needs to be re-run with stdout actually redirected** (`& "...\dhewm3.exe" *>
vklog.txt` from PowerShell, not just watching the console) to capture whatever
GPU-AV had to say. Until that capture exists, GPU-AV has not actually been
checked yet, only run.

## Validation

Per the project's debug-over-theory rule, each step gets an observable rather than
an argument:

- **A1** — the extended SBT log line should show `hitRecords=3` for shadow/AO/GI
  and `hitRecords=4` for reflections. Then, in-game: put the weapon viewmodel
  against a lit wall and confirm the blob is gone. A useful narrowing test *before*
  fixing: temporarily force `instanceShaderBindingTableRecordOffset = 0`
  unconditionally at `vk_accelstruct.cpp:1207`/`:1559`. If the blobs disappear on
  AMD (at the cost of wrong player reflections), A1 is confirmed as the cause.
  That one-line experiment is the fastest way to validate the diagnosis and is
  worth running first.
- **A2** — set `r_rtAO 1` and run a mirror/security-monitor subview, where the
  scissor is not full-screen. Uncleared regions should show as saturated patches
  before the fix and be clean after.
- **A3** — set `r_vkLogRT 1` and confirm the "skip" messages now appear on frames
  where the TLAS is genuinely invalid (during a level load transition), which they
  currently never do at `r_vkLogRT 0` because the guard is dead.
- **A5** — the observable is blunt: AO should stop being a no-op. Set
  `r_rtAO 1` and stand in a corner or under a desk; before the fix the AO mask is
  uniformly white, after it there should be visible contact darkening. If it still
  looks "barely noticeable" afterwards, that is a *tuning* result to chase with
  `r_rtAORadius` / `r_rtAOSamples` — a meaningful distinction that could not be
  drawn before, because the effect was inert regardless of the constants.
- **A6** — the free-while-resident log should be silent in normal play. If it
  fires, capture the surrounding frames; that is the smoking gun for the whole
  dangling-VA theory and turns a latent concern into a reproducible bug.
- **A8** — see the two zero-code experiments (`r_rtVolTemporal 0`,
  oversized `r_rtGIAtrousSigmaL`/`Z`) in A8's own Validation subsection above;
  run those on the pre-fix build to confirm the mechanism, then confirm the
  fixed build shows neither symptom over a long play session.
- **A4** — no visible change expected; this is insurance. Confirm no perf
  regression in the existing profiler phases.
- **A7** — needs a synthetic 16k-geometry scene to exercise; not worth building
  one. Verify by reading, and rely on the existing overflow `common->Warning`.

### Instrumentation to run before touching code

See `docs/vulkan_debugging.md` for the full how-to (layer settings file, env
vars, what each VUID class means, this project's own cvars). Summary:

- **`r_vkRTReflDataDiag 2`** on the AMD machine. This CVar
  (`vk_accelstruct.cpp:1718`) already validates material-table addresses, texture
  slots and base indices and logs bad slots. Its checks exist for exactly the A6
  class of bug and may surface a stale address directly.
- **`VK_LAYER_KHRONOS_validation` with GPU-Assisted Validation.** Plain validation
  catches A1 (`VUID-vkCmdTraceRaysKHR`-class: SBT index outside the region's
  `size`) and A3's null-image path. **GPU-AV is the only thing that catches
  `buffer_reference` out-of-bounds** — normal validation cannot see those at
  all. This is how A9 and A10 were actually found. Setup and what to look for:
  `docs/vulkan_debugging.md`.

## Open question

Whether the AMD 9700 XT run was made with validation layers enabled. If they were
off, a validation-enabled run on either vendor is likely to surface A1 and A3 plus
anything both audits missed, and is worth doing before or alongside step 1.
