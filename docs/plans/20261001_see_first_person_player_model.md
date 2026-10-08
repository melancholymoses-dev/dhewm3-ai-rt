# First-Person Player Body Plan

**Date:** 2026-04-04  
**Status:** Visible and animating in-game. Stages 2-3 landed plus `pm_firstPersonBodyOffset`.
Open issues below; stage 4 (RT exclusion) still pending, so the fp body *is* in the TLAS.  
**Scope:** Local first-person body visibility (torso/legs), while keeping normal third-person and mirror rendering behavior.

---

## Goal

Show a player body when looking down in first-person (torso/legs), but:
- Keep the normal player model behavior for third-person and mirror/subviews.
- Keep rendering/performance stable on Vulkan + RT path.
- Avoid camera-inside-mesh and clipping artifacts.

---

## Gotchas / Current State

Each was established by inspection, not assumption. If one turns out false, the stage it
supports is wrong.

| Fact | Why it matters | Detail |
|---|---|---|
| `g_showPlayerShadow` defaults to **1** here (stock Doom 3 ships `0`) | It is the reason the world body is in TLAS pass 1 at all, i.e. the reason RT lighting "is handled by the third-person model" | [RT Participation](#current-rt-participation-of-the-world-body) |
| Pass 1's `suppressShadowInViewID` skip does **not** stamp `blasFrameCount`, so pass 3 re-adds the entity | The fp body must be excluded in both passes *with* the stamp, or pass 3 silently puts it back | [RT Participation](#current-rt-participation-of-the-world-body) |
| The world body's `ModelCallback` never fires in the local view (dropped from `viewEntitys` by `suppressSurfaceInViewID`) | The animator joint array goes stale unless `CreateFrame` is called explicitly | [Stage 3](#3-share-the-players-joint-array) |
| `allowSurfaceInViewID` is set only by the view weapon today | Makes it safe to overload as the RT-exclusion predicate | [Stage 4](#4-exclude-from-the-tlas) |

---

## Feasibility

Feasible with current architecture.

The engine already separates visibility by `viewID`:
- Local first-person uses `viewID = entityNumber + 1`.
- Player world body is currently suppressed in that local view.
- Mirror/subviews reset to `viewID = 0` so normal body appears there.

This means we can add a local-only first-person body without changing mirror behavior logic.

---

## Do We Need a Separate Player Model?

Recommended: yes (for production quality).

Options:
1. **Prototype path (fast):** reuse existing body model in local view.
2. **Production path (preferred):** dedicated first-person body mesh/skin (headless or reduced upper body).

Why separate model is preferred:
- Reduces camera clipping into neck/chest.
- Avoids ugly self-intersection at high pitch/FOV.
- Makes per-view material/shadow tuning simpler and safer.

---

## FP Mesh Asset (Done)

`base/models/sp_player.md5mesh` — engine path `models/sp_player.md5mesh`.

| Property | Value |
|---|---|
| Source | `spplayer.md5mesh`, trimmed in Blender above Z≈60 (below `Shoulders` at 61.9) |
| Joints | 75, names/order/parents/bind pose identical to source — all 19 stock `.md5anim` remain valid |
| Meshes | 1 (`models/characters/player/body`); `soldier` and `marine2` arm mesh removed |
| Verts / tris | 1071 / 2082 |
| Weighted joints | 19: legs, `Hips`, `Waist`, `Belly`, `Chest`, `Lrib`/`Rrib`, `Lshldr`/`Rshldr` |
| Unweighted but present | 56, incl. `Head`, `Loneck`, arms, fingers, all `*_ATTACHER` |
| Export settings | `Reorient: False; Scale: 1.0` — reuse verbatim for any re-export |
| Blender source | `base/models/doom3_sp.blend` |

Eye is at Z=68 (`pm_normalviewheight`), i.e. at `Loneck` — hence the shoulder-height cut.
Crouched eye is Z=32 (`pm_crouchviewheight`), the worst case for camera/chest overlap.

**The shoulder cut must be left open — do not cap it.** A cap face at Z≈60 sits below the
standing eye at Z=68, so it occludes the abs and legs entirely while standing (only the
crouched camera gets below it). It also renders near-black: MD5 carries no normals, and a
cap built by filling the rim inherits the rim's outward-facing vertex normals, which are
perpendicular to the cap surface. Leaving it open costs nothing — `noShadow` keeps the fp
entity out of the stencil shadow-volume path, and backface culling hides the torso interior,
so looking down reads as a hollow shell with the legs visible below. A concave "scooped"
mesh is unnecessary for the same reason.

---

## Current RT Participation of the World Body

Established by inspection, and the basis for keeping the fp body out of RT entirely:

| Pass | Site | World body included? |
|---|---|---|
| TLAS pass 1 (viewEntitys) | [vk_accelstruct.cpp:1164](../../neo/renderer/Vulkan/vk_accelstruct.cpp#L1164) | Yes — `g_showPlayerShadow` defaults to **1**, so `suppressShadowInViewID` is 0 |
| TLAS pass 2 (cached static) | [vk_accelstruct.cpp:1474](../../neo/renderer/Vulkan/vk_accelstruct.cpp#L1474) | No — skips `dynamicModel` |
| TLAS pass 3 (nearby dynamic) | [vk_accelstruct.cpp:1570](../../neo/renderer/Vulkan/vk_accelstruct.cpp#L1570) | Yes — only `weaponDepthHack` is excluded |

Pass 1's `suppressShadowInViewID` skip is a bare `continue` that does **not** set
`blasFrameCount`, so pass 3 re-adds the body even with `g_showPlayerShadow 0`. That is why
the body reaches reflections at all, and why the fp body needs excluding in both passes.

---

## Implementation: Hook-Up and Animation

### 1. Keep the world player model path unchanged
Do not alter third-person, multiplayer, or mirror model paths, or existing suppression rules.

### 2. Own a bare render entity on idPlayer
Pattern after [PlayerIcon.cpp:165-185](../../neo/game/PlayerIcon.cpp#L165) — a `renderEntity_t`
plus `qhandle_t`, not an `idAnimatedEntity`. A second entity would need a second animator and
could desync from the world pose.

| Field | Value | Reason |
|---|---|---|
| `hModel` | `FindModel("models/sp_player.md5mesh")` | |
| `allowSurfaceInViewID` | `entityNumber + 1` | local view only; mirrors/subviews use viewID 0. Same trick as [Weapon.cpp:2148](../../neo/game/Weapon.cpp#L2148) |
| `origin` / `axis` | copied from `renderEntity` after `Present()` | inherits the `offset ( 0 0 1 )` from `model_sp_marine` without re-deriving it |
| `callback` | `NULL` | see stage 3 |
| `noShadow`, `noSelfShadow` | `true` | keep out of the stencil path |
| `weaponDepthHack` | `false` | plan requires world depth, not view depth |

Lifecycle: `AddEntityDef` on spawn, `UpdateEntityDef` each `Think`, `FreeEntityDef` on
destructor / `pm_showFirstPersonBody 0`. Do not save the handle — recreate it in `Restore`.

### 3. Share the player's joint array
`sp_player.md5mesh` has the same 75 joints in the same order, so the animator's output is
directly usable: no second animation evaluation, and pose desync is impossible by construction.

In `idPlayer::Think`, after the existing `UpdateAnimation(); Present();`
([Player.cpp:7919](../../neo/game/Player.cpp#L7919)):

```
animator.CreateFrame( gameLocal.time, false );            // no-op if already current
animator.GetJoints( &fpBody.numJoints, &fpBody.joints );  // re-fetch; array can realloc
fpBody.origin = renderEntity.origin;
fpBody.axis   = renderEntity.axis;
gameRenderWorld->UpdateEntityDef( fpBodyHandle, &fpBody );
```

`CreateFrame` must be explicit. The world body is dropped from `viewEntitys` by
`suppressSurfaceInViewID` ([RenderWorld_portals.cpp:682](../../neo/renderer/RenderWorld_portals.cpp#L682)),
so its `ModelCallback` never fires in the local view and the joint array would otherwise go
stale. Leaving `fpBody.callback` NULL avoids `ModelCallback`'s `renderEntity->entityNum`
lookup, which would resolve to the world entity on a game-unowned render entity.

If `numJoints != 75`, [Model_md5.cpp:836](../../neo/renderer/Model_md5.cpp#L836) prints a
joint-count mismatch and returns no model — that is the first thing to check if nothing draws.

### Stages 2-3: landed

Stages 2 and 3 are one chunk — stage 2 alone cannot be tested, because an MD5 render entity
with NULL joints is rejected at [Model_md5.cpp:828](../../neo/renderer/Model_md5.cpp#L828).

| File | Change |
|---|---|
| `gamesys/SysCvar.{h,cpp}` ×2 | `pm_showFirstPersonBody` (archived, default 0), `pm_firstPersonBodyDebug` |
| `Player.h` ×2 | `fpBodyRenderEnt`, `fpBodyHandle`, `fpBodyJointsWarned`; `UpdateFirstPersonBody`/`FreeFirstPersonBody` |
| `Player.cpp` ×2 | both functions; ctor init; `FreeFirstPersonBody()` in dtor; `UpdateFirstPersonBody()` in `Think` after the `g_stopTime` block; `renderer/ModelManager.h` include |
| `framework/Dhewm3SettingsMenu.cpp` | "Show Own Body in First Person" under Game → Visual |
| `neo/CMakeLists.txt` | `deploy_base_overrides` now copies `base/models/sp_player.md5mesh` |
| `.gitignore` | `!/base/models/sp_player.md5mesh` negation |

`.gitignore` blanket-ignores `*.md5mesh`/`*.md5anim`/`*.blend*` so extracted pak assets stay
out of the repo. Only the fp mesh is un-ignored; the Blender source and the stock `.md5anim`
working copies remain local-only, and `deploy_base_overrides` lists models file-by-file rather
than `copy_directory` for the same reason.

Nothing was added to `Save`/`Restore` — the handle is rebuilt by the next `Think`, so the
savegame format is unchanged.

Not saved to the def: no `model` block was added, per the Asset Touchpoints note below.

### 4. Exclude from the TLAS — landed

`if ( ent->parms.allowSurfaceInViewID ) continue;` added to all three passes in
[vk_accelstruct.cpp](../../neo/renderer/Vulkan/vk_accelstruct.cpp) — lines 1178, 1488, 1611.
Pass 1 also stamps `ent->blasFrameCount = tr.frameCount`, matching the `weaponDepthHack`
branch above it, so pass 3's dedup check skips the entity as well.

Pass 2 technically did not need it: it already rejects anything with `ent->dynamicModel` set,
and the fp body is a dynamic MD5. The check is there anyway so the invariant holds uniformly
across all three passes rather than resting on an incidental property of pass 2's filter.

`allowSurfaceInViewID` is set only by the view weapon and the fp body. The weapon was already
excluded via `weaponDepthHack`, so this is a strict superset of prior behaviour.

**Why this stopped being a pure perf item.** At offset 0 the fp body sat exactly inside the
world body and its duplicate shadow was indistinguishable from the real one. Once
`pm_firstPersonBodyOffset` moves it, the duplicate separates and casts a visibly displaced
second shadow. The exclusion is now a visual fix as well as a saved per-frame BLAS rebuild.

Check: BLAS frame stats instance count must not rise when `pm_showFirstPersonBody` toggles on.

### 5. Head/upper body handling
The mesh is already cut at the shoulders, so no runtime head hiding is needed. Retain pitch
and offset tuning as a fallback if crouch still clips.

### First-response table

| Symptom | Check first | Fix |
|---|---|---|
| Nothing draws | Console for `renderEntity has different number of joints` ([Model_md5.cpp:836](../../neo/renderer/Model_md5.cpp#L836)) | `numJoints` must be 75; re-fetch from `animator.GetJoints` each frame, don't cache the pointer |
| Nothing draws, no console output | `r_skipSuppress 1` — if the body appears, gating is the cause | `allowSurfaceInViewID` must equal `renderView.viewID`, i.e. `entityNumber + 1` |
| Body frozen in bind pose | Is `animator.CreateFrame` called explicitly in `Think`? | See stage 3 — the world body's callback does not fire in the local view |
| Body one frame behind | `UpdateEntityDef` ordering vs `Present()` | Update after `Present()`, so `renderEntity.origin`/`axis` are current |
| Torso solid black, world shadows correct | `r_rtShadowDebugMode` | Risk 3 — `r_rtShadowPlayerExcludeDist` / `rayCullMask` |
| Body visible in a mirror | `allowSurfaceInViewID` is non-zero and mirror `viewID` is 0 | If it still leaks, the subview is not resetting `viewID`; check [Player.cpp:8917](../../neo/game/Player.cpp#L8917) |
| TLAS instance count rises when cvar toggles on | Stage 4 predicate present in **both** pass 1 and pass 3? | Pass 1 must also stamp `blasFrameCount` |
| Body lit flat, detached from the floor | Risk 5 — it should receive screen-space GI | Exclusion is TLAS-only; it must stay in the G-buffer |

---

## Interaction With Mirrors, Third Person, and Remote Views

Desired behavior should be automatic with existing rules:
- **Local first-person view:** fp body visible, world self model suppressed.
- **Third person:** normal player world model visible.
- **Mirror/subview/remote camera:** normal player world model visible (no fp-only body).

No special-case mirror code is expected if viewID gates are correctly set.

---

## Special Case: Entities Inside Player Space (e.g., spiders at feet)

This remains possible in Doom 3 collision/gameplay behavior.

Expected visual impacts with fp body:
- Overlap around feet/legs can still happen.
- Risk of clipping is mainly upper-body/camera overlap, not gameplay overlap.

Mitigations:
- Keep fp body camera-safe (headless/reduced upper torso).
- Tune body offsets and pitch-driven visibility limits.

---

## Performance Plan

### Cost profile
- Adds one extra local animated mesh draw path.
- In RT mode, may add BLAS/TLAS update cost for local fp body.

### Guardrails
- Prefer simplified fp body mesh/surfaces.
- Start with reduced shadow participation.
- Add cvar toggles for controlled rollout and profiling.
- Validate in heavy scenes before default enable.

Cvars (implemented):
- `pm_showFirstPersonBody` (0/1, archived, default 0)
- `pm_firstPersonBodyOffset` (float, archived, default **-4**) — see below
- `pm_firstPersonBodyMaxPitch` (float, archived, default **70**) — clamps downward pitch, but
  only while the body is shown, so vanilla `pm_maxviewpitch` 89 is untouched when it is off
- `pm_firstPersonBodyDebug` (0/1) — create/free and pose diagnostics

### Why the body needs a horizontal offset

`pm_normalviewheight 68` puts the view at `Loneck` — the base of the neck, centred
front-to-back on the spine. Real eyes sit ~8-12 units forward of the spine, so the engine
hangs the body symmetrically around the camera and the pelvis reads as "looking at your own
backside": it sits ~10 back and ~23 down, only ~23° off straight-down and well inside the
view cone at pitch.

`pm_firstPersonBodyOffset` shifts the fp entity along `renderEntity.axis[0]` (body forward,
so it tracks yaw). ~-12 to -15 pushes the rear past ~45° off vertical, out of frame, while
the thighs stay visible. The feet land 12-15 units behind true position but `pm_bboxwidth` is
32 (half-width 16), so they stay inside the collision hull.

Horizontal only, deliberately. The camera is also ~5 units below the model's eye height, but
shifting the body down sinks the feet through the floor, and raising `pm_normalviewheight`
would change crouch, collision and the game's framing. That mismatch is accepted.

Distinct from the run-cycle lean: the lean is a *rotation* about the waist, which no constant
offset can cancel. See the torso-channel note under Open Issues.

---

## Visual Artifact Risks and Mitigations

1. Camera clipping into body
- Mitigation: headless mesh + tuned offsets + pitch limits.

2. Weapon/body depth fighting
- Mitigation: keep weapon in existing weapon-depth-hack path; keep fp body in world depth path.

3. **Predicted first artifact: fp body shadowed black by the world body.** The fp body now
   writes depth in the local view, and the world body sits in the TLAS at the same place.
   Shadow rays traced from fp-body pixels hit the world body's chest almost immediately.
- Mitigation: the knob already exists — `r_rtShadowPlayerExcludeDist`
  ([vk_shadows.cpp:1007](../../neo/renderer/Vulkan/vk_shadows.cpp#L1007)) forces
  `rayCullMask = 0xFE`, dropping player instances (`inst.mask = 0x01`). Currently applied only
  when the light is nearer than that distance; may need to key off the fp-body pixel instead.

4. Mirror leakage of fp-only body
- Mitigation: strict `allowSurfaceInViewID` gating and validation in mirrors/cameras.

5. GI/reflections double-counting the body
- The fp body is excluded from the TLAS, so it contributes nothing — but it still *receives*
  screen-space GI and shadow-mask lookups, which is intended. Verify the received lighting
  looks continuous with the floor rather than flat-lit.
- The fp body does appear in the G-buffer, so FSR motion vectors pick it up automatically.

---

## Open Issues (in-game testing, 2026-10-07)

| # | Finding | Status |
|---|---|---|
| 1 | Capped shoulder rim rendered black and occluded everything below it | Fixed — cap deleted, mesh re-exported (2042 tris) |
| 2 | Run cycle leans the torso into view; you see your own back | Fix by cutting the mesh to the hip line (Z≈47-48) — see below |
| 3 | Body renders **pitch black** under a light, not merely shadowed | Fixed — `r_rtPlayerExcludeRadius`, see below. Untested in-game |
| 6 | Pitch to 89° shows the backs of your own legs | Fixed — `pm_firstPersonBodyMaxPitch` 70 |
| 4 | Pelvis visible when looking down ("seeing your own backside") | Prefer trimming the mesh's rear over offsetting — see below |
| 5 | Offset body cast a second, displaced shadow | Fixed — stage 4 landed, fp body out of the TLAS |

**On #4 — trim the backside rather than shift the body.** `pm_firstPersonBodyOffset` works but
costs alignment: the world body stays at the true position, so its shadow sits ahead of the fp
body's feet. Shifting the world body to match is not an option — one `renderEntity.origin`
serves every view, so it would displace the body in mirrors, in RT reflections (which pass 3
includes during first-person play) and for other clients in multiplayer.

Deleting the rear of the pelvis from the fp mesh keeps the body at its true position, so the
shadow, reflections and collision hull all stay correct, and the offset can return to 0. The
fp mesh is only ever viewed by its own player from above; the backside has no job. Do it in
the same Blender pass as the hip-line cut.

**On #2 — the asset fix replaces the code fix.** `player.def` splits the rig
`channel torso ( *Waist )` / `channel legs ( *origin -*Waist ... )`, and the entire lean lives
in the torso channel. Cutting below `Waist` (Z=45.7) leaves only `Hips`- and leg-driven
geometry, making the lean structurally impossible rather than merely suppressed. This cancels
the planned torso-channel joint lock, which would have required the fp entity to own a copy of
the joint array instead of sharing the animator's pointer.

Joint heights for the cut: `Chest` 54.1, `Belly` 47.4, `Waist`/`SPINNER` 45.7, `Hips` 45.0
(legs channel), `Lupleg`/`Rupleg` 43.2. Pass criterion: `Chest` and `Belly` drive zero
vertices, `Waist` near zero.

Current mesh (930 verts / 1762 tris) does **not** meet it — `Chest` 13, `Belly` 20,
`Waist` 136 weights remain, so the cut landed at the waist rather than below it. The lean is
roughly 90% reduced (`Chest` was 140) but not structurally gone. Cut below Z=45.7 if the run
cycle still shows it.

**On #3 — direct *and* indirect are killed by the same occluder.** The fp body sits inside the
world body, which must stay in the TLAS because it carries the RT lighting. Every ray leaving
an fp-body surface hits the world body within a couple of units:

| Ray | Cull mask | Player instances (`mask 0x01`) |
|---|---|---|
| Shadow ([vk_shadows.cpp:1012](../../neo/renderer/Vulkan/vk_shadows.cpp#L1012)) | `0xFE` only when light-to-camera < `r_rtShadowPlayerExcludeDist` | usually **included** |
| GI ([gi_ray.rgen:164](../../neo/renderer/glsl/gi_ray.rgen#L164)) | `0xFF` | **included** |
| Probe GI ([gi_probe_trace.rgen:97](../../neo/renderer/glsl/gi_probe_trace.rgen#L97)) | `0xFF` | **included** |

So direct light is shadowed to zero and the bounce that would otherwise lift it is occluded
too. That is why it reads pitch black rather than dim — it is not tonemapping, and the body
is not excluded from GI; GI reaches it and returns nothing.

`r_rtShadowPlayerExcludeDist` is not the fix. It keys off **light-to-camera** distance, so a
ceiling light 80-150 units up never engages it however high the value — only the 400 test
value was large enough to cover everything, and raising the default that far also loses the
world body's shadow cast on the floor.

**Fix as built — key the mask on ray-origin distance, not a G-buffer flag.** There is no free
G-buffer channel (`gbufNormal.a` is F0, `gbufAlbedo.a` is `1 - tcClass`, motion vectors are
R16G16), so a per-pixel flag would have meant a new attachment plus an extra draw. The
requirement restates as *rays leaving a surface inside the player's own volume must not hit
the player's own body* — a property of the ray origin, which every rgen already has.

`r_rtPlayerExcludeRadius` (archived, default 40), defined in `vk_shadows.cpp` and shared:

| Shader | Change |
|---|---|
| [shadow_ray.rgen](../../neo/renderer/glsl/shadow_ray.rgen#L303) | per-pixel `cullMask`; reuses the existing `camDist` |
| [gi_ray.rgen](../../neo/renderer/glsl/gi_ray.rgen#L130) | per-pixel `giCullMask`; reuses the existing `camPos` |
| [gi_probe_trace.rgen](../../neo/renderer/glsl/gi_probe_trace.rgen#L100) | `0xFE` **unconditionally** |

Neither rgen needed a new camera-position uniform — both already derive `camPos` from
`invViewProj`. `ShadowParamsUBO` absorbed the radius into `_pad1[0]` (still 192 bytes);
`GIParamsUBO` grew 128 → 144. `gi_ray.rchit` needs no edit: it already declares a block that
stops at `stochasticLights`, so appending at the end leaves every earlier offset intact.

Probe GI is unconditional on purpose. Probes are a static world grid; letting a walking player
occlude them bakes a moving actor into the grid and pops as they pass.

Radius 40 covers thighs and pelvis (25-30 units from the eye) and deliberately leaves boots
(~65) and the floor at the feet (~68) alone, so the body still casts its contact shadow. Dark
boots are accepted — read as black leather.

`r_rtShadowPlayerExcludeDist` stays at 30 and keeps its original, separate job: stop an
*invisible* body casting a shadow from nowhere. That job still exists whenever
`pm_showFirstPersonBody` is 0, which is the default.

**The fp body has no collision.** `AddEntityDef` creates a render entity only; the player's
clip model is still `idPhysics_Player`'s bbox plus `SetCombatModel()`. Enabling the body
cannot change what can occupy the player's volume.

---

## Implementation Phases

### Phase 0: FP asset — **done**
`base/models/sp_player.md5mesh` exported and verified against the stock skeleton.

### Phase A: Spike
Stages 2-4 above, behind `pm_showFirstPersonBody`.

Exit criteria:
- Looking down shows torso/legs in first-person.
- Mirrors and third-person still show normal model behavior.
- TLAS instance count unchanged when the cvar is toggled.

### Phase B: Pose Polish
- Camera-safe tuning at crouch and extreme pitch.
- Resolve risk 3 (world-body self-shadowing) and risk 5 (received lighting continuity).

Exit criteria:
- Minimal clipping in normal gameplay movement and look ranges.

### Phase C: RT/Shadow + Perf Stabilization (2-4 days)
- Tune shadow participation and RT cost.
- Run scene-based profiling and artifact pass.

Exit criteria:
- No major regressions in frame time.
- No obvious first-person artifact regressions.

---

## Estimated Difficulty

- **Development difficulty:** Medium for prototype, Medium-High for polished production result.
- **Maintenance burden:** Medium if isolated in a dedicated fp body path; High if mixed into many special-case world-model rules.

Recommendation: isolate as a clear fp-body feature path with explicit cvars and minimal coupling to existing world-model code.

---

## Validation Checklist

1. First-person look-down shows body in idle/walk/run/crouch.
2. Third-person unchanged.
3. Mirror view unchanged (normal model only).
4. Remote camera/subview unchanged.
5. Weapon render remains stable (no new z artifacts).
6. No severe clipping at extreme pitch angles.
7. Performance check in AI-heavy and light-heavy scenes.
8. Save/load and map transitions preserve expected behavior (handle recreated, not saved).
9. TLAS instance count identical with `pm_showFirstPersonBody` 0 vs 1.
10. `g_showPlayerShadow 0` — fp body behaviour must not change, since that cvar only moves the
    *world* body between TLAS passes 1 and 3.
11. Mirror check must be done while standing in front of one: the fp body must be absent and the
    world body present, both in the same frame.

---

## Asset Workflow (Blender Manual Procedure)

### Tool choice
- **Minimum required:** Blender only.
- **Recommended:** Blender + Noesis.

Use Blender for authoring/export. Use Noesis as a fast verifier for MD5 skeleton, weights, and animation playback before game-side testing.

### Step-by-step (manual)

1. Prepare source assets
- Extract/locate the current player `md5mesh` and representative `md5anim` clips (idle, walk, run, crouch).
- Keep an untouched copy of original source assets for diff and rollback.

2. Import to Blender
- Import the existing player mesh + armature using the same MD5 import/export add-on for the entire project.
- Confirm armature orientation, scale, and rest pose on first import.

3. Build the first-person variant mesh
- Duplicate the body mesh and create an fp variant (typically remove head/neck and trim upper torso).
- Preserve material slots/surface naming where possible to reduce downstream def/script churn.

4. Preserve rig compatibility
- Do **not** rename bones.
- Do **not** change hierarchy/parenting.
- Do **not** reorder bones in export path.
- Do **not** change bind/rest pose transforms.
- Keep bone local axes/orientations intact.

5. Reweight only where needed
- Repaint weights around edited regions only.
- Keep leg/hip/torso weighting behavior close to original so existing animations remain valid.

6. Validate in Blender before export
- Play stock clips (idle/walk/run/crouch) against the fp mesh.
- Check for collapse/twist at pelvis, spine, and upper leg joints.
- Verify no accidental non-uniform bone scale.

7. Export MD5
- Export with project-standard axis/scale options and stick to those settings for all future updates.
- Export as a new fp mesh path (do not overwrite original world mesh during initial integration).

8. Optional Noesis verification (recommended)
- Open exported `md5mesh` and existing `md5anim` clips in Noesis.
- Confirm joint count/names, bind pose, and obvious deformation issues.
- If Noesis looks wrong, fix in Blender before engine integration.

9. Engine integration and visual pass
- Hook fp mesh into first-person-body path only.
- Run look-down, movement, mirror, and third-person checks.

### Rig compatibility quick checklist
- Joint names unchanged.
- Joint hierarchy unchanged.
- Bind/rest pose unchanged.
- Export scale/axis unchanged.
- Mesh deforms correctly under original locomotion clips.

---

## Asset Touchpoints (From pak_assets)

Primary reference file in extracted assets:
- `../build_rt/pak_assets/pak000/def/player.def`

Key declarations to know:
1. `model model_sp_marine { ... }`
- Contains the single-player marine mesh binding.
- Current mesh path is `models/md5/characters/npcs/playermoves/spplayer.md5mesh`.

2. `entityDef player_doommarine { ... }`
- Binds the player entity to `"model" "model_sp_marine"`.

Practical interpretation:
1. If replacing the normal SP body globally, change the mesh path inside `model_sp_marine`.
2. For the fp path, keep `player_doommarine -> model_sp_marine` untouched.

**No fp `model` def block is needed.** Because the fp entity borrows the player animator's joint
array (stage 3), it needs no anim bindings, no channel setup and no skin — just the raw
`idRenderModel` from `renderModelManager->FindModel()`. A def block would only add a second
`idDeclModelDef` whose anims never play.

Important packaging note:
- Treat `pak_assets` as the source/reference for discovery.
- Make real gameplay overrides in your active mod/project def path so updates are intentional and maintainable.

---

## Rollout Recommendation

- Ship disabled by default first (`pm_showFirstPersonBody 0`).
- Gather screenshots and perf logs in representative levels.
- Enable by default once artifact/perf criteria are met.

---

## Ray-Traced Reflection Approaches for Player Body

This section records approaches evaluated for showing the player body in RT reflections
without causing blank/dark blotch artifacts from rays passing through or originating inside
the player geometry.

### Problem Statement

The reflection pipeline has two ray passes per pixel:

1. **Glass probe ray** — fired from the camera toward the visible depth surface, using
   `gl_RayFlagsCullOpaqueEXT`. It finds any non-opaque (glass/translucent) geometry between
   the camera and the surface. If hit, that glass surface becomes the reflection origin so
   the mirror shows the room the player is standing in rather than what is behind the glass.

2. **Reflection ray** — fired outward from the reflection origin (glass surface or depth
   surface) in the mirror-reflected view direction to pick up the colour to display.

**The blotch problem** is primarily in step 1. When the player stands in front of a mirror
or window, the glass probe ray must travel through the player's body to reach it. Player
geometry that has non-opaque BLAS entries (e.g. MC_PERFORATED arm/weapon surfaces) is
visible to `gl_RayFlagsCullOpaqueEXT`. The probe hits the player body first, misidentifies
it as the glass/reflective surface, and sets the reflection origin to the player's body
surface. The reflection ray then fires from the wrong point and returns a dark or incorrect
colour — producing a large dark silhouette-shaped blotch on the mirror wherever the player's
body overlaps it.

A secondary issue: the main reflection ray (step 2, mask 0xFF) can also hit the player body
from close range when fired from a reflective floor or wall surface nearby, returning the
player's dark texture instead of the intended room colour.

---

### Approach 1: tMin Distance Cutoff (in rgen)

Set a minimum ray distance (`tMin`) large enough that rays fired from floor/wall surfaces
skip past the player body entirely.

**How it works:** `traceRayEXT(..., tMin, ...)` — any hit closer than `tMin` is ignored by
the driver. If `tMin` exceeds the player's physical extent, floor-bounce rays cannot hit
the body.

**Drawback:** Blunt instrument. A single global tMin cannot distinguish "floor looking up
at player" from "floor looking at a far mirror that happens to have the player in front of
it". Also breaks reflections on surfaces very close to the player's feet. Not adopted.

---

### Approach 2: MAT_FLAG_PLAYER_BODY Check in reflect_ray.rchit

Add a `MAT_FLAG_PLAYER_BODY` (0x08) material flag to player geometry, and check it in the
existing `reflect_ray.rchit` shader to do a distance-based pass-through.

**How it works:** The rchit shader reads `mat.flags & MAT_FLAG_PLAYER_BODY`. If set and
`gl_HitTEXT < threshold`, it sets `transmittance = 1.0` and `nextOrigin/Dir` to continue
the ray past the player body.

**Drawback:** Mixes player-specific logic into the world geometry shader. The edit was
rejected in favour of keeping world and player shaders separate. Not adopted.

---

### Approach 3: Per-Instance SBT Routing with a Dedicated Player Shader (Adopted)

Route player body TLAS instances to a dedicated closest-hit shader by setting
`instanceShaderBindingTableRecordOffset = 2` on noSelfShadow entities.

**SBT layout (7 entries):**

| Slot | Group | Shader | Purpose |
|------|-------|--------|---------|
| 0 | rgen | reflect_ray.rgen | reflection ray generation |
| 1 | miss | reflect_ray.rmiss | main miss (sky colour) |
| 2 | miss | reflect_ray.rmiss | glass probe miss |
| 3 | hit  | reflect_ray.rchit + reflect_ray.rahit | world geometry, main ray |
| 4 | hit  | reflect_ray.rchit + glass_probe.rahit | world geometry, glass probe |
| 5 | hit  | player_reflect.rchit (opaque) | player body, main ray |
| 6 | hit  | placeholder (never fires) | player body, glass probe |

**player_reflect.rchit behaviour (as built, 2026-10-07):**
- Always opaque: samples the player diffuse texture, applies shadowed irradiance through the
  shared `rt_light_eval.glsl` loop with self-shadow mask `0xFE`, and sets `transmittance = 0.0`.
- There is **no distance pass-through**. An earlier revision of this section described an
  80-unit threshold; the shader has never contained one. Floor-bounce rays are kept off the
  body by the instance mask instead.

**Glass probe exclusion via instance mask:**
The glass probe `traceRayEXT` call uses cull mask `0xFE`. Player TLAS instances have
`inst.mask = 0x01`. Because `0x01 & 0xFE == 0`, the player is invisible to glass probe
rays entirely. Slot 6 (player glass probe) exists only to keep SBT index arithmetic
valid; it never fires.

**Material flag:**
`VK_MAT_FLAG_PLAYER_BODY (0x08)` is set in the material table for noSelfShadow entity
geometry. This is readable in shaders but is not needed for the routing to work — the
routing is purely SBT-based.

**Trade-offs:**
- Cleanest separation: world shaders have no player-specific branches.
- Extra pipeline size: 7 groups vs 5. Minor cost.
- Because the body is opaque to the main reflection ray, a *second* co-located player
  instance (the fp body, before stage 4 lands) is visually indistinguishable from the first.

---
