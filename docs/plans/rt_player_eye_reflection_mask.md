# Vulkan RT Player Eye Reflection Mask Plan

Date: 2026-08-30
Status: Steps 1-3 and 5 implemented 2026-08-31; Step 4 (cvar) not added, not requested.
Scope: Vulkan RT reflections for local player body and eye materials.

## Implementation Notes (2026-08-31)

- Step 1: `VK_MAT_FLAG_PLAYER_EYE`/`MAT_FLAG_PLAYER_EYE` = `0x40u` added to
  `vk_raytracing.h`/`rt_material.glsl`.
- Step 2: allow-list in `VK_RT_MakeMaterialEntry` (`vk_material_table.cpp`) —
  confirmed against `materials/char_common.mtr` that `models/characters/player/eyes`,
  `models/characters/common/{right,left}brown{,2}` are exactly the `translucent`
  `blend filter`-only, no-`SL_DIFFUSE` materials the problem summary describes (the
  default player skin's third-person head uses `rightbrown`/`leftbrown`). No blue
  variant is referenced by any player skin, so it was left off per the plan's "add
  only as encountered" guardrail.
- Step 3: `player_reflect.rchit` treats `MAT_FLAG_PLAYER_EYE` + `diffuseTexIndex == 0`
  hits as pass-through (transmittance = 1.0, straight-through continuation ray),
  reusing the same continuation mechanism `reflect_ray.rchit`'s glass branch already
  uses.
- Step 5: `VK_RT_UploadMatTableFrame`'s existing emissive-tagging log line (gated on
  `r_vkLogRT`) now also reports `player-body`/`player-eye`/`player-eye fallback`
  counts.
- Not done: Step 4's optional `r_rtReflectionPlayerEyes` cvar — skipped per the
  handoff note ("implement core steps 1 to 3 first"); add if A/B testing turns out
  to be needed.
- Also landed in the same pass (not in this plan, but the same file):
  `player_reflect.rchit`'s direct lighting was rewritten to use the shared shadowed
  `rt_light_eval.glsl` loop (previously unshadowed distance/N·L only — an unrelated,
  separately-reported brightness bug) and its shadow rays use cull mask `0xFEu` to
  exclude the player's own body from self-occlusion.

## Problem Summary

In reflections, player eye surfaces can show up as bright white floating orbs.
This is not a third-person camera setting issue. It is a reflection hit/masking and material routing issue in the Vulkan RT path.

Current behavior:
- Player body instances are tagged through noSelfShadow and participate in reflection rays.
- Main reflection rays include player instances.
- Some player eye materials are translucent/filter style and may not provide a usable SL_DIFFUSE mapping for the RT material table.
- When diffuse mapping is unavailable, RT falls back to slot 0 (white), causing white orb artifacts.

## Goals

1. Keep intended player reflections (mirror-distance body visibility remains possible).
2. Include valid player eye materials only when explicitly intended.
3. Prevent floating/random eye-like artifacts in reflections.
4. Avoid broad translucent enablement that could regress other materials.
5. Keep performance impact negligible.

## Non-Goals

1. Do not change third-person camera cvars or gameplay camera behavior.
2. Do not rewrite general material parsing for all RT paths.
3. Do not alter glass probe behavior unless needed for parity checks.

## Design Approach

Use a two-gate policy:

1. Entity gate: surface must be from player-body instance.
2. Material gate: surface must be explicitly classified as player-eye material.

This prevents accidental inclusion of unrelated eye-like or translucent surfaces elsewhere.

## Implementation Plan

### Step 1: Add a new material flag

Files:
- neo/renderer/Vulkan/vk_raytracing.h
- neo/renderer/glsl/rt_material.glsl

Action:
- Add a new bit flag for player eye surfaces, for example:
  - VK side: VK_MAT_FLAG_PLAYER_EYE
  - GLSL side: MAT_FLAG_PLAYER_EYE
- Keep bit assignments synchronized between C++ and GLSL.

Acceptance:
- Flag values match exactly in both files.

### Step 2: Classify player-eye materials during material table build

File:
- neo/renderer/Vulkan/vk_material_table.cpp

Action:
- In VK_RT_MakeMaterialEntry, classify known player eye material names and set the new flag.
- Start with a strict allow-list based on shipped content, for example:
  - models/characters/player/eyes
  - models/characters/common/rightbrown
  - models/characters/common/leftbrown
  - optional blue variants only if they are used by player skins
- Use exact or prefix matching that is tight and deterministic.

Important:
- Keep this allow-list narrow. Do not classify monster eye materials as player-eye.

Acceptance:
- Only intended player eye shaders get the new flag.
- No broad translucent classes are flagged.

### Step 3: Add a safe fallback policy in player reflection hit shader

File:
- neo/renderer/glsl/player_reflect.rchit

Action:
- On player-body hit:
  - If MAT_FLAG_PLAYER_EYE is set, allow eye shading only when diffuseTexIndex is valid and not the white fallback slot.
  - If diffuseTexIndex is fallback or invalid, treat as pass-through (transmittance path) instead of returning shaded white.
  - If material is player-body but not player-eye, keep existing body behavior unchanged.

Rationale:
- This directly removes white-orb outcomes without disabling player reflections globally.

Acceptance:
- White eye orbs no longer appear.
- Body reflection behavior remains stable.

### Step 4: Optional cvar for fast A/B testing

Files:
- neo/renderer/Vulkan/vk_reflections.cpp
- neo/renderer/glsl/player_reflect.rchit (if UBO wiring is chosen)

Action:
- Add an optional renderer cvar, for example r_rtReflectionPlayerEyes:
  - 0: disable eye contribution in player_reflect path
  - 1: enable filtered eye contribution (default)
- This is optional but useful for QA and bisecting visual issues.

Acceptance:
- Cvar toggles behavior with no crashes and no shader compile regressions.

### Step 5: Logging for diagnosis

Files:
- neo/renderer/Vulkan/vk_accelstruct.cpp
- neo/renderer/Vulkan/vk_material_table.cpp

Action:
- Under existing RT log cvar gates, add lightweight counters:
  - number of MAT_FLAG_PLAYER_BODY entries
  - number of MAT_FLAG_PLAYER_EYE entries
  - number of eye entries with diffuseTexIndex fallback
- Keep logs rate-limited and non-spammy.

Acceptance:
- Logs confirm classification and fallback rates in live scenes.

## Exact Code Touchpoints

Primary:
- neo/renderer/Vulkan/vk_raytracing.h
- neo/renderer/glsl/rt_material.glsl
- neo/renderer/Vulkan/vk_material_table.cpp
- neo/renderer/glsl/player_reflect.rchit

Optional:
- neo/renderer/Vulkan/vk_reflections.cpp
- neo/renderer/Vulkan/vk_accelstruct.cpp

Related context files:
- neo/renderer/glsl/reflect_ray.rgen
- neo/renderer/glsl/glass_probe.rahit
- neo/game/Player.cpp
- neo/d3xp/Player.cpp

## Suggested Matching Policy

Order:

1. Check entity is player body (MAT_FLAG_PLAYER_BODY).
2. Check shader name is in strict allow-list.
3. Set MAT_FLAG_PLAYER_EYE only when both checks conceptually align.

Guardrails:
- Never infer player-eye from generic words like eye or brown alone.
- Require full known material path matches where possible.

## Validation Checklist

Visual:
1. Stand in front of mirrors and reflective floors with player visible.
2. Confirm no bright white floating eye orbs.
3. Confirm player body still appears at intended reflection distances.
4. Confirm no regression in glass reflections.

Functional:
1. Toggle first-person and third-person camera modes.
2. Validate behavior in both base game and d3xp content.
3. Confirm no shader compile/runtime errors.

Diagnostics:
1. Enable RT logs and verify eye classification counters.
2. Verify fallback diffuse eye count is zero or handled by pass-through.

## Risks and Mitigations

Risk: Overly strict allow-list misses some player skins.
Mitigation: Add explicit entries as encountered, do not widen pattern loosely.

Risk: Overly broad match pulls in monster/NPC eyes.
Mitigation: Exact path matching and MAT_FLAG_PLAYER_BODY gate.

Risk: Visible change in artistic look of player eyes in mirrors.
Mitigation: Optional cvar for quick A/B and tuning.

## Definition of Done

1. White eye-orb artifacts are gone in reflection-heavy scenes.
2. Player reflections remain conceptually correct.
3. No broad translucency regressions.
4. Logging confirms expected classification behavior.

## Handoff Notes For Next LLM

1. Implement core steps 1 to 3 first before optional cvar work.
2. Keep changes minimal and localized to Vulkan RT material and player reflection paths.
3. Preserve existing mirror-distance behavior for player body.
4. Prefer strict correctness over broad heuristics for material classification.