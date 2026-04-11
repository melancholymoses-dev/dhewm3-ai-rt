# Reflection Enhancements

Tracking ideas for improving raytraced reflections beyond current state.

---

## 1. Off-Screen Entities in TLAS

Currently the TLAS only contains `viewDef->viewEntitys` — the frustum-culled entity list. Dynamic entities (enemies, NPCs, props) that are behind the player are absent from the TLAS and therefore invisible in reflections.

Static world BSP geometry is unaffected (world BLAS is persistent). The gap is dynamic entities.

### Approach

Add a second pass in `VK_RT_RebuildTLAS()` (`vk_accelstruct.cpp:1017`) after the main `viewEntitys` loop. Iterate `viewDef->renderWorld->entityDefs` (the full world entity list on `idRenderWorldLocal`, `RenderWorld_local.h:181`) and include entities where `ent->viewCount != tr.viewCount` (i.e. not visited in the main pass).

`viewDef->renderWorld` is already typed as `idRenderWorldLocal *` in `tr_local.h:394`, so no cast is needed.

### Complications

**1. Dynamic model pose**

For animated enemies (MD5, type `DM_CACHED`), `ent->dynamicModel` holds the current animation pose but is only generated when the entity is in the view frustum (`R_EntityDefDynamicModel`, `tr_light.cpp:1429`). For off-screen enemies it is either NULL (never been visible) or a stale prior-frame pose.

Two options:

- **Bind pose (easy):** Use `ent->parms.hModel` (the static reference mesh). Correct geometry, correct textures, persistent GPU-resident vertex data. Shows T-pose for characters until the player first looks at them.
- **Stale animated pose (medium):** Use `ent->dynamicModel` from the last visible frame. Looks more natural. But `dynamicModel` vertex data lives in the ring-buffer vertex cache and may be evicted. The BLAS handle itself remains valid (the BVH is separate from the source vertex data), but the material entry UV addresses (`geomVertAddrs`) will be stale and point to reused ring-buffer memory, causing wrong textures. Would need a flag in `VkMaterialEntry` to disable UV interpolation for off-screen entities to avoid corruption.
- **Force-tick animation (expensive):** Call `InstantiateDynamicModel` for off-screen entities to generate the current pose. Invasive — this is normally only done inside the view submission path — and adds CPU cost proportional to the number of off-screen animated entities.

**2. BLAS for entities never in view**

If an enemy has never been in the player's frustum, its model BLAS is not in the cache and it will be silently skipped. Acceptable for a first pass. A separate "prime the cache" pass (iterate all entities on load) could fix this later.

**3. Static TLAS signature**

The static TLAS signature hashes `ent->blas->deviceAddress` for change detection. Cache-resident BLASes have stable device addresses, so this should continue to work correctly.

### Proposed First Implementation (Bind Pose)

In `VK_RT_RebuildTLAS()`, after the `viewEntitys` loop closes, add:

```
for each ent in renderWorld->entityDefs:
    skip if ent->viewCount == tr.viewCount          // already in main pass
    skip if weaponDepthHack or suppressShadowInViewID
    skip if model == null or DM_CONTINUOUS           // particles, sprites
    look up BLAS cache (parms.hModel) — don't rebuild
    skip if no cached BLAS
    add to static instance bucket using ent->modelMatrix
    fill material entries from cached BLAS addresses (no vertex cache refresh)
```

This gives correctly positioned off-screen static props and enemies (in bind pose). No BLAS rebuilds, no vertex cache churn.

### Expected Visual Result

| Entity | Before | After (bind pose pass) |
|---|---|---|
| Props/doors behind player | Missing from reflection | Correct position and appearance |
| Enemies behind player (never seen) | Missing | Missing (no cached BLAS yet) |
| Enemies behind player (seen before) | Missing | Correct position, bind pose geometry |
| Particle effects behind player | Missing | Still missing (skipped intentionally) |

### Effort Estimate

- Bind pose pass: **low** — ~100 lines, reuses all existing BLAS cache and instance-fill logic
- Stale animated pose: **medium** — needs a UV-disable flag in `VkMaterialEntry` and shader support
- Force-tick animation: **high** — invasive to animation/render pipeline, significant CPU cost

The bind pose pass is a reasonable ship target. Animated off-screen poses are a nice-to-have for a later iteration.

### Distance Culling

No distance limit in the above. A cheap optimization: skip off-screen entities where
`(ent->parms.origin - viewDef->renderView.viewOrg).LengthFast() > r_reflectionOffScreenDist` (new cvar).
Keeps TLAS size bounded when the map has many off-screen entities.

---

## 2. Player Weapon Not Visible in Reflections (World Weapon Excluded from TLAS)

### Problem

The player's weapon doesn't appear in RT reflections. The reflected body looks like the MP marine model (empty hands), even though the full SP equipped-marine appearance is what should be shown.

### Root Cause

The weapon entity has **two render entities**:

| Entity | Purpose | Key flags |
|---|---|---|
| View weapon (`renderEntity` on `idWeapon`) | First-person screen-space model | `weaponDepthHack = true` |
| World weapon (`worldModelRenderEntity`, bound to player joint) | Third-person body + mirror model | `suppressShadowInViewID = playerViewID`, `noSelfShadow = true` |

The world weapon is the one that should be visible in reflections — it's the one bound to the player's hand joint and suppressed only in first-person (`suppressSurfaceInViewID`).

The TLAS builder in `vk_accelstruct.cpp:1082-1083` has this guard:

```cpp
if (ent->parms.suppressShadowInViewID &&
    ent->parms.suppressShadowInViewID == viewDef->renderView.viewID)
    continue;
```

In single-player, the primary viewDef has `viewID = entityNumber + 1`. The world weapon has `suppressShadowInViewID = entityNumber + 1` (`Weapon.cpp:803`). The condition is true → **the world weapon is skipped entirely from the TLAS** → it can't appear in RT reflections.

The player body does NOT have `suppressShadowInViewID` set, which is why the body does appear (with empty hands, hence the "MP model" look).

### Why the Current Check Is Overly Aggressive

`suppressShadowInViewID` was designed for the classic GL shadow-volume system to prevent the weapon from casting a stencil shadow on the player body in first-person. In the RT path, this job is already done by the **instance cull mask** system:

- `noSelfShadow = true` → `inst.mask = 0x01`, SBT offset 2 → `player_reflect.rchit`
- Shadow rays use `params.rayCullMask = 0xFE` (confirmed in `shadow_ray.rgen:38`)
- `0x01 & 0xFE = 0` → shadow rays **already skip** noSelfShadow instances

Skipping the world weapon from the TLAS entirely is redundant (shadow behavior already handled by mask) and has the unwanted side-effect of making it invisible to reflection rays too.

### Fix

**File:** [neo/renderer/Vulkan/vk_accelstruct.cpp](neo/renderer/Vulkan/vk_accelstruct.cpp), line 1082

Remove the `suppressShadowInViewID` check from the TLAS entity loop. It is no longer needed — shadow exclusion is covered by `inst.mask = 0x01` + shadow ray cull mask `0xFE`.

```cpp
// REMOVE these lines:
if (ent->parms.suppressShadowInViewID &&
    ent->parms.suppressShadowInViewID == viewDef->renderView.viewID)
    continue;
```

With this removed, the world weapon flows through to the existing `noSelfShadow` handling:
- Gets `inst.mask = 0x01` → shadow rays (cull mask `0xFE`) skip it → no self-shadow cast
- Gets SBT offset 2 → routed to `player_reflect.rchit`
- `player_reflect.rchit` applies the 80-unit pass-through for close-range hits → prevents floor-bounce rays from hitting the weapon arms from below
- Reflection rays (mask `0xFF`) hit it → weapon appears in mirror

### What Prevents the View Weapon from Leaking

The view weapon (`renderEntity.weaponDepthHack = true`) is still excluded at line 1084 — that check is untouched. The view weapon has depth-hack applied and is positioned at a non-world-space depth, so it must never appear in the TLAS regardless.

### Effort: Low

Single line removal in `vk_accelstruct.cpp`. The routing infrastructure (`player_reflect.rchit`, SBT slot 2, mask `0x01`) is already in place and will handle the world weapon correctly with no shader changes.

---

## 3. Projectiles Not Visible in Reflections

### Problem

Projectiles (plasma balls, rockets, fireballs) do not appear in RT reflections. The BLAS builder in `vk_accelstruct.cpp:619-628` filters out:

- `MF_POLYGONOFFSET` — coplanar decals
- `MF_NOSHADOWS` on non-translucent surfaces — particles, flares, coronas
- `DFRM_PARTICLE`, `DFRM_PARTICLE2`, `DFRM_SPRITE`, `DFRM_FLARE` deforms — billboards

Projectile visuals almost certainly use these flags. The glow/corona effect around a plasma ball is a sprite or particle; the rocket exhaust is a particle emitter. Even if the projectile body has a physical 3D mesh, its main visible material is likely `MF_NOSHADOWS` for the glow.

### Why the Current Exclusions Exist

- **Sprites and particles are view-facing billboards.** Their geometry is oriented toward the primary camera. In a reflection, the reflected camera direction is different — the billboard would face the wrong way and look like a thin sliver.
- **Glow/corona materials** with `MF_NOSHADOWS` are intentionally excluded from shadow and AO casting. Including them in the TLAS would make glowing effects cast hard shadows, which looks wrong.

### Fix Options

**A. Do nothing for billboards (correct)**

Sprites, particles, and billboard deforms should stay excluded — they'd look wrong in reflections due to incorrect facing. Acceptable trade-off.

**B. Include 3D projectile mesh geometry with additive pass-through (medium)**

If a projectile entity has actual 3D mesh surfaces (not billboards) with `MC_TRANSLUCENT` coverage and `MF_NOSHADOWS`, relax the BLAS filter to include them. Introduce a `MAT_FLAG_ADDITIVE` material flag alongside the existing `MAT_FLAG_GLASS`. In `reflect_ray.rchit`:

```glsl
if ((mat.flags & MAT_FLAG_ADDITIVE) != 0u) {
    vec4 emissive = rt_SampleDiffuse(matIdx, gl_PrimitiveID, baryCoord);
    reflPayload.colour += weight * emissive.rgb;  // accumulate glow
    reflPayload.transmittance = 1.0;              // ray continues through
    // nextOrigin/Dir: same direction, advance past surface
    return;
}
```

This lets the reflection ray pick up projectile glow and continue to whatever is behind, preserving the scene reflection with the projectile blended in.

**Requires:**
1. New `MAT_FLAG_ADDITIVE` bit in `VkMaterialEntry.flags` (`vk_raytracing.h`)
2. New BLAS inclusion condition: include `MC_TRANSLUCENT` + `MF_NOSHADOWS` non-deform surfaces
3. New branch in `reflect_ray.rchit`
4. BLAS builder marks these surfaces as non-opaque (no `VK_GEOMETRY_OPAQUE_BIT_KHR`)

**Effort: medium** — BLAS builder, material entry, one shader branch. Risk: could include unintended surfaces; need careful material flag selection.

**C. Investigate actual projectile assets first**

Before building the above, check `def/projectile.def` and the plasma/rocket model materials in `pak000` to confirm the actual flags. It's possible projectile body meshes are entirely particle/sprite based, making option B irrelevant. This is a one-time investigation, not a code change.

---

## 3. Translucent Effects Showing Square Borders Over Reflections

### Actual Problem

When translucent effects (shotgun smoke puffs, plasma hit impacts) appear near a glass/reflective surface, a **rectangular hole** — the full sprite quad boundary — becomes visible in the reflection, as if the reflection was erased in the footprint of the effect. The transparent edges of the smoke texture do not fade gracefully; instead the whole quad's rectangular border is visible against the reflection.

### Root Cause

Doom3 particle and effect materials typically use **two rendering stages**:

1. A **masking stage** — `blend gl_dst_color, gl_zero` (or similar multiplicative darkening). This is a full-quad draw that blacks out/modulates the rectangular sprite region to prevent additive glow from stacking on bright backgrounds. It operates on the *entire geometry rectangle*, not just the opaque texel area.
2. A **color/glow stage** — additive or src_alpha blend that renders the actual visible smoke/fire.

The masking stage writes over the *entire rectangular quad* with a darkening multiply. When this fires after the glass reflection overlay (which used ONE/ONE additive blend to deposit the reflection), the mask stage erases the reflection contribution in the full sprite rectangle — leaving a visible square shadow/hole.

### How the Compositing Order Works

1. RT dispatch → writes `reflImage` (before any raster)
2. Back-to-front translucent loop (`vk_backend.cpp:3176`)
3. Per-surface: material stages drawn, then glass overlay fires if `SURFTYPE_GLASS` (`vk_backend.cpp:1804`)
4. Any surface drawn *after* the glass in back-to-front order can clobber the reflection

Since smoke/particles are closer to the camera than the glass, they sort *after* it and render on top of the completed reflection.

### Fix Options

**A. Move the glass overlay to a final post-translucent pass (medium, recommended)**

Instead of drawing the glass overlay inside `RB_CreateSingleDrawInteractions` mid-loop, defer it: collect glass draw surfaces during the loop, then execute all glass overlays in a separate mini-pass *after* the entire translucent loop completes. The reflection overlay is then the last thing drawn and cannot be clobbered by particle mask stages.

**Requires:**
- A small deferred-glass list (e.g. `static const drawSurf_t *s_pendingGlassOverlays[...]` filled during the translucent loop)
- A new function `VK_RB_DrawDeferredGlassOverlays()` called after the translucent loop in `vk_backend.cpp`
- Remove the overlay draw from inside `RB_CreateSingleDrawInteractions`

**Effect:** Rectangular particle-mask holes in reflections disappear. The smoke/glow itself may still partially composite over the glass in an additive way (which is fine), but the dark mask rectangle will no longer eat the reflection.

**Limitations:** Smoke is rendered in primary view but not in the reflection image (it's excluded from the BLAS). So the mirror shows a clean scene behind the player, while the primary view shows smoke floating — a slight inconsistency, but far less jarring than the square holes.

**B. Separate reflection pass entirely (high)**

Render the glass overlay in a dedicated final full-screen pass or as a post-process after all translucent geometry, keyed by stencil or a separate render target. More isolated from the interaction pipeline but more involved to implement.

**C. Do nothing / accept (poor UX)**

The square holes are quite noticeable. Option A is low-risk and low-effort relative to the visual improvement.

### Summary Recommendation

Option A is the right fix. It is a localized change to `vk_backend.cpp` — move one code block and add a small deferred draw list. The core rendering logic is unchanged.

### Related: Smoke/Effects Not Visible IN Reflections

A secondary issue is that smoke/plasma effects fired toward the camera (visible in reflected direction) are absent from the reflection image — the mirror shows a clean room, but the primary view has visible effects. This is a harder problem (BLAS inclusion of billboard geometry is incorrect due to facing direction) and is lower priority than fixing the rectangle artifacts.


### 4. Include Irradiance in reflections
Right now all reflections are treated equally.  We should take into account their source illumination/albedo. 