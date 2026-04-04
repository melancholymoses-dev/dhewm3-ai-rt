# First-Person Player Body Plan

**Date:** 2026-04-04  
**Status:** Planned  
**Scope:** Local first-person body visibility (torso/legs), while keeping normal third-person and mirror rendering behavior.

---

## Goal

Show a player body when looking down in first-person (torso/legs), but:
- Keep the normal player model behavior for third-person and mirror/subviews.
- Keep rendering/performance stable on Vulkan + RT path.
- Avoid camera-inside-mesh and clipping artifacts.

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

## Proposed Technical Approach

### 1. Keep current world player model path unchanged
- Do not alter normal third-person, multiplayer, or mirror model path.
- Keep existing suppression rules for current world model behavior.

### 2. Add a local-only first-person body render entity
- Add an additional render entity (or attachment) for local player only.
- Set visibility gates so it appears only in local first-person view:
  - `allowSurfaceInViewID = entityNumber + 1`.
- Ensure it does not leak into mirrors/subviews:
  - mirror/subviews use `viewID = 0`, so local-only gating naturally hides it.

### 3. Animation/pose sync
- Drive first-person body from same player animator state as world body.
- Preserve leg motion and torso orientation coherence with local movement.

### 4. Head/upper body handling
- Start with head hidden (or headless fp mesh) to avoid camera overlap.
- Keep a small camera-safe exclusion volume around eye point.

### 5. Shadows and RT participation (conservative first)
- Start with conservative shadow behavior for fp body to avoid artifacts.
- Enable/adjust shadows after visual validation in representative scenes.

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

Suggested cvars:
- `pm_showFirstPersonBody` (0/1)
- `pm_firstPersonBodyShadows` (0/1)
- Optional: `pm_firstPersonBodyDebug` for bounds/pose diagnostics

---

## Visual Artifact Risks and Mitigations

1. Camera clipping into body
- Mitigation: headless mesh + tuned offsets + pitch limits.

2. Weapon/body depth fighting
- Mitigation: keep weapon in existing weapon-depth-hack path; keep fp body in world depth path.

3. Self-shadow oddities in first-person
- Mitigation: conservative defaults, then incremental shadow enable with logs and captures.

4. Mirror leakage of fp-only body
- Mitigation: strict `allowSurfaceInViewID` gating and validation in mirrors/cameras.

---

## Implementation Phases

### Phase A: Spike (2-4 days)
- Add fp body entity behind a cvar.
- Local-view-only visibility gating.
- Reuse current assets to validate concept.

Exit criteria:
- Looking down shows torso/legs in first-person.
- Mirrors and third-person still show normal model behavior.

### Phase B: Production Asset + Pose Polish (3-7 days)
- Introduce dedicated fp body mesh/skin.
- Improve animation coherence and camera-safe tuning.

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
8. Save/load and map transitions preserve expected behavior.

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
- `../build_rtx/pak_assets/pak000/def/player.def`

Key declarations to know:
1. `model model_sp_marine { ... }`
- Contains the single-player marine mesh binding.
- Current mesh path is `models/md5/characters/npcs/playermoves/spplayer.md5mesh`.

2. `entityDef player_doommarine { ... }`
- Binds the player entity to `"model" "model_sp_marine"`.

Practical interpretation:
1. If replacing the normal SP body globally, change the mesh path inside `model_sp_marine`.
2. If implementing the planned first-person-only body path, keep `player_doommarine -> model_sp_marine` as-is for world/mirror views, and add a separate fp model block that code spawns only for local first-person view.

Important packaging note:
- Treat `pak_assets` as the source/reference for discovery.
- Make real gameplay overrides in your active mod/project def path so updates are intentional and maintainable.

---

## Rollout Recommendation

- Ship disabled by default first (`pm_showFirstPersonBody 0`).
- Gather screenshots and perf logs in representative levels.
- Enable by default once artifact/perf criteria are met.
