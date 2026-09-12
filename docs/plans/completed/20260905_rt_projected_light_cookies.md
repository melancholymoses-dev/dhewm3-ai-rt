# RT Projected Light Cookies (fan blades, grates, window blinds)

**Date:** 2026-08-31
**Status:** Stage 1 (CPU plumbing + dump validation) implemented and in-game validated
2026-09-08 (`r_rtGILightDump 1` in mars_city1: `lights/fanblade3` correctly resolves
`stages=1 passing=1 image=lights/fanblade3`). Stage 2 (direct lighting) implemented
2026-09-10, validated in-game the same day via `RT_LIGHT_COOKIE_DEBUG` on a real
`lights/fanblade3` fixture in mars_city1 (reflections only — see Stage 2 note; the
debug tint's first iteration had its own bug, also fixed same day, see project memory
`feedback_cookie_debug_tint_bug`). Stage 3 (volumetrics) implemented 2026-09-11 and
**in-game validated 2026-09-12** — volumetric light shafts visibly follow the rotating
fan blades. Two real bugs found and fixed en route (a compute-shader-incompatible
`#include`, and a missing `VK_SHADER_STAGE_COMPUTE_BIT` on the shared bindless-texture
binding that silently killed all volumetric lighting, not just cookies — see project
memory `project_light_cookie_stage3`). Stage 4 (cleanup) done 2026-09-12 — debug-tint
scaffolding removed. All four stages complete. Linked from ROADMAP.md Wave 7 (COOKIE).
**Motivates:** visible fan-blade shadows in volumetric light shafts, plus every other
patterned projected light in the retail maps (window blinds, grates, cage lights).

---

## The actual effect, and why it's currently invisible

Doom 3 does not shadow fan blades with geometry. `lights/fanlightgrate` and
`lights/fanblade3` (`build_rt/pak_assets/pak000/materials/lights.mtr:1089-1153`) are
**projected light materials** — a stage with a `fanblade.tga` cutout and a texture-matrix
expression `rotate time * -1`. Every projected/spot light in idTech4 draws through a
projective S/T/Q texgen built from the light's frustum (`R_SetLightProject`,
`tr_lightrun.cpp:246`, called from `tr_lightrun.cpp:421`) and samples its own material's
stage(s) through it (`RB_BlendLight`, `draw_common.cpp:1919`). The rotating blade shape
*is* the light — there's no mesh to occlude.

Our RT path already admits these lights (`lightType==1`, "directed", both
`rt_light_eval.glsl`'s direct-lighting loop and `vol_march.comp`'s march), but both
model them as a smooth geometric cone (`coneDir` + `cosHalf`) with quadratic/linear
falloff — no texture is ever sampled. So a fanlightgrate light currently renders as a
plain soft cone: correctly positioned and shadowed against world geometry, but with the
blade pattern silently discarded. This is the actual gap, not light admission.

Confirmed separately: a real spinning *mesh* fan (if any map uses one instead of the
texture trick) would already animate correctly in both direct shadows and volumetric
occlusion — `vk_accelstruct.cpp:1185` rebuilds the TLAS instance transform from
`ent->modelMatrix` every frame independent of BLAS caching, and `dynamicModel` entities
get a full per-frame BLAS rebuild. Nothing to fix there; no further work item below
covers it.

---

## What already exists to build on

- `idRenderLightLocal::lightProject[4]` (`tr_local.h`) — the 4 idPlanes (S, T, Q,
  falloff), computed once per light in `R_DeriveLightData` → `R_SetLightProject`
  (`tr_lightrun.cpp:421`) and rotated to global space at `tr_lightrun.cpp:452`. This is
  frontend code shared by both renderers — **not GL-specific**. We do not need to
  re-derive the projection math; we just need to read it.
- `idRenderLightLocal::shaderRegisters` (evaluated every frame in `R_AddLightSurfaces`,
  `tr_light.cpp:1169-1170`, again backend-agnostic) — the light material's expression
  registers, including the `rotate(time*-1)` texture-matrix terms. `RB_GetShaderTextureMatrix`
  (`tr_local.h:1348`) turns registers + a `textureStage_t` into the 2×3 affine matrix
  already, with the rotation pivot baked in by the material compiler — no pivot math to
  reimplement.
- `vk_material_table.cpp`'s bindless texture array (`GetOrAssignTexIndex`, currently
  `static`) — the existing mechanism every RT hit shader already uses to sample material
  textures by index. Needs a public wrapper; the registration logic itself is reusable
  as-is.
- `GILight.flags` (`rt_light_eval.glsl` / `vk_gi.cpp`) already has a free bit pattern
  (`GI_LIGHT_FLAG_SELF_SHADOW` is bit 0) — a `GI_LIGHT_FLAG_HAS_COOKIE` bit fits the same
  field.

None of this needs new frontend math. The work is: read data that already exists,
resolve one image into the existing bindless table, and add one texture sample + a
plane-projection + a 2×3 matrix multiply to two shaders.

---

## Data plumbing

### CPU (`vk_gi.cpp::considerLight`)

For every admitted **projected** light (`isProjected == true`, i.e. `lightType` 1 or 2 —
scene spot lights and the flashlight both qualify, they use the same S/T/Q mechanism):

1. Walk `lightLocal->lightShader->GetNumStages()`, exactly mirroring `RB_BlendLight`'s
   loop (`draw_common.cpp:1952-1988`): for each stage, test
   `lightLocal->shaderRegisters[stage->conditionRegister]`, take the first stage that
   passes with a real bound image. (`fanlightgrate` has two condition-gated pairs keyed
   on `global0`; picking the first passing stage is a correct v1 — accumulating multiple
   simultaneously-active stages is a stretch goal, not needed for the fan/blinds case
   since the pairs are mutually exclusive in practice.)
   - `lightLocal->shaderRegisters` may be stale/null for a light admitted only via the
     portal-area BFS (never got surfaces this frame — see `portal_area_lights.md`). Fall
     back to `lightShader->EvaluateRegisters(...)` ourselves in that case, same call
     `RenderWorld_portals.cpp:119` already makes.
2. If a stage is found: register its image in the bindless table (new public wrapper
   around `GetOrAssignTexIndex`), compute its texture matrix via
   `RB_GetShaderTextureMatrix(regs, &stage->texture, matrix)`, and set
   `GI_LIGHT_FLAG_HAS_COOKIE` on the candidate.
3. Copy `lightLocal->lightProject[0..3]` (already global-space) into a new side buffer,
   **not** the hot `GILight` struct — most lights have no cookie and shouldn't pay for
   4 extra vec4s. New SSBO `GILightCookieBuf`, indexed 1:1 with `GILightBuf.lights[]`:

```c
struct GILightCookie {
    vec4 planeS, planeT, planeQ;  // world-space projection planes (Q for perspective divide)
    vec4 texMatRow0, texMatRow1;  // 2x3 affine: xyz used, w unused (std430 padding)
    uint imageIndex;              // bindless slot, vk_material_table.cpp
    uint pad0, pad1, pad2;
};
```

   Only entries for lights with `GI_LIGHT_FLAG_HAS_COOKIE` set are meaningful; others can
   be left zeroed (imageIndex 0 = white fallback is harmless if ever misread).

### GPU — shared helper (new: `rt_light_cookie.glsl`)

```glsl
// Returns vec3(1.0) for no-cookie lights (safe multiply-in default).
vec3 rt_SampleLightCookie(GILightCookie c, vec3 worldPos)
{
    vec4 p = vec4(worldPos, 1.0);
    float s = dot(p, c.planeS);
    float t = dot(p, c.planeT);
    float q = dot(p, c.planeQ);
    if (q <= 0.0) return vec3(0.0);   // behind the light's near plane
    vec2 uv = vec2(s, t) / q;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return vec3(0.0);             // zeroclamp semantics — outside frustum = dark
    uv = vec2(dot(vec3(uv, 1.0), c.texMatRow0.xyz),
              dot(vec3(uv, 1.0), c.texMatRow1.xyz));
    return texture(bindlessTextures[nonuniformEXT(c.imageIndex)], uv).rgb;
}
```

Multiply this into the light's contribution alongside the existing atten/phase terms —
in `rt_LightContribAt` (direct) and the per-step light loop in `vol_march.comp`
(volumetric). Gate the sample behind `(flags & GI_LIGHT_FLAG_HAS_COOKIE) != 0` so
non-cookie lights (the vast majority) don't pay for it.

---

## Phasing

**Stage 1 — CPU plumbing + dump validation. ✅ Implemented.**
Admission-time stage walk and bindless registration are in `vk_gi.cpp::considerLight`;
`r_rtGILightDump` now prints a `cookie:` line per admitted projected light (stages
passed/chosen/image). No shader touched yet, as planned. Implementation deviated from
this doc in a few ways worth knowing before Stage 2/3 — see the project memory
`project_light_cookie_stage1` for full detail:

- No standalone `GILightCookieBuf` SSBO — `GILightCookie cookies[VK_GI_MAX_LIGHTS]` was
  added directly inside `GILightBuffer` instead (after `lights[]`), giving free 1:1 index
  alignment with each of GI's and Vol's independently-ordered final selections. GLSL
  mirrors don't declare it yet; the SSBO range already covers the bytes.
- `GILightCookie` stores 3 world-space planes (S/T/Q) with the stage's texture matrix
  already folded in, not a separate 2×3 matrix — sampling is 2 dot products + a divide,
  no matrix multiply needed at hit time.
- Registers are evaluated fresh every time (`EvaluateRegisters`), not read from a cached
  viewLight — `idRenderLightLocal` has no `shaderRegisters` of its own, only its
  per-frame `viewLight` does, and most GI/vol-admitted lights aren't real view-lights
  that frame.
- **Known gap, not yet fixed**: `lights/fanlightgrate` (unlike `fanlightgrateSC`) has two
  *always-simultaneously-active* stages (blade + grate), and real Doom 3 draws both as
  separate additive passes. This doc's "first passing stage" v1 assumption only holds for
  the `SC` variant's mutually-exclusive `global0` pairs. Dump logging now reports
  `passing=N` so this can be confirmed on real data before Stage 2 locks in a
  single-image-per-light shader model; if it's a real problem the fix is a small fixed
  stage cap (2), not a redesign.
- **Scope correction found via first in-game dump**: retail `lights/fanblade3` fixtures
  (e.g. mars_city1 `light_5268`) have no `light_target` key — they're authored as *point*
  lights, shaped entirely by the material's `rotate` stage. This doc's title/framing
  ("projected light cookies") is misleading: cookie detection must NOT be gated on
  `isProjected`. Point lights get a real `lightProject[4]` too (`R_DeriveLightData`'s
  box-to-unit-cube map, `Q` always 1), so the same S/T/Q sample works unchanged — the gate
  was simply wrong and has been removed. Practical effect: this generalizes to nearly
  every light with a stage image, not just a handful of fan/grate fixtures — most other
  materials' stages are plain gradients so will sample the same as today, but Stage 2/3
  should expect a much wider set of lights carrying `GI_LIGHT_FLAG_HAS_COOKIE` than the
  doc originally implied.

**Stage 2 — direct lighting. ✅ Implemented, not yet in-game validated.**
`rt_SampleLightCookie` (new `rt_light_cookie.glsl`) is wired into `rt_LightContribAt`
(shared by `gi_ray.rchit`, `reflect_ray.rchit` and `player_reflect.rchit` via
`rt_light_eval.glsl`), gated on `GI_LIGHT_FLAG_HAS_COOKIE`. Debug overlay implemented as
a compile-time toggle (`RT_LIGHT_COOKIE_DEBUG` in `rt_light_eval.glsl`, default 0) rather
than a runtime cvar — flip to 1 and rebuild to tint any shaded point whose cookie
luminance is below `RT_LIGHT_COOKIE_DEBUG_LUM_THRESHOLD` (0.05) magenta. A cvar-driven
toggle was considered and rejected: `reflect_ray.rchit`/`player_reflect.rchit` cannot read
the params UBO (binding 3 is raygen-only, see this file's includer contract) so a runtime
flag would need cross-pipeline payload plumbing for a throwaway validation aid — not
worth it. Not yet validated in-game (this session cannot build/run — see project memory
`project_light_cookie_stage2`); validate against a known fanlightgrate/fanblade3 fixture
next session before proceeding to Stage 3.

Implementation deviated from this doc in ways worth knowing before Stage 3 — see the
project memory `project_light_cookie_stage2` for full detail:
- `RTLightBuf`'s `lights[]` changed from an unsized trailing array to a fixed-size
  `lights[RT_LIGHT_MAX_LIGHTS]`, with `cookies[RT_LIGHT_MAX_LIGHTS]` appended after it
  (GLSL disallows two unsized arrays in one block, and cookies must be reachable by
  index, not just trailing bytes). This shrank the SSBO's required minimum bound size and
  broke `vk_reflections.cpp`'s 16-byte null-light fallback buffer — fixed by sizing that
  buffer to the real `GILightBuffer` size via a new `VK_RT_GetGILightBufferSize()`
  accessor, not by keeping the old unsized-array trick.
- `GILightEntry::flags` (CPU) was already correctly plumbed in Stage 1, but the GLSL
  mirror (`RTLight` in `rt_light_eval.glsl`) had never been updated to read it — the byte
  offset was declared `_pad0` and silently discarded. Renamed to `flags`, no layout change.

**Stage 3 — volumetrics. ✅ Implemented and in-game validated 2026-09-12.**
Same sampling wired into `vol_march.comp`'s per-step light loop, applied to *any*
cookie-flagged light reachable by the march (not gated on `lightType`, matching Stage
1/2's finding that point lights need this too — `fanblade3` is one). This is the actual
"see the blades in the light shaft" payoff, and it's confirmed working — volumetric
light following the rotating fan blades, visible in-game.

Deviations from this doc, and a refactor done while implementing — see project memory
`project_light_cookie_stage3` for full detail:
- No standalone `GILightCookieBuf` — same as Stage 2, `cookies[]` lives inside the
  existing `GILightBuffer`/`GILightBuf`, now mirrored in `vol_march.comp` too
  (`RTLightCookie cookies[RT_LIGHT_MAX_LIGHTS]`, fixed-size like Stage 2's `lights[]`).
- `vol_march.comp` had no set=1 (material table) binding at all before this — it's a
  compute shader that never sampled a material texture. Added by extending its
  pipeline layout to 2 sets and reusing the existing shared `vkRT.matDescLayout`/
  `matDescSet` (same object every RT hit shader already binds), not a new descriptor
  set — `vk_vol.cpp`'s `VK_RT_InitVolMarchPipeline` and its dispatch call both updated.
- Refactored `rt_light_cookie.glsl` to own `rt_LightLuminance` and the
  `RT_LIGHT_COOKIE_DEBUG` toggle (moved out of `rt_light_eval.glsl`) plus a new shared
  `rt_ApplyLightCookie(contrib, cookie, threshold)` helper, so Stage 2 (direct
  lighting/reflections) and Stage 3 (volumetrics) share one debug-tint implementation
  instead of drifting. Each caller passes its own `threshold` — a "meaningful
  contribution" is a very different absolute magnitude in a single NdotL-lit hit point
  vs. one exponential march step (further scaled by stepSize/stepTransmittance/phase) —
  `vol_march.comp`'s constant is a first guess (`0.0001`), not yet tuned against a build.

Watch for two things specific to volumetrics once validated:
- **Temporal EMA ghosting.** `r_rtVolTemporal`'s EMA (`vk_vol.cpp:803`) blends toward
  history; a fast-rotating blade pattern will trail/smear more than the existing
  flashlight-cone content it was tuned against. May need a faster blend factor when a
  march pixel's cookie sample changed sharply from the previous frame (reuse the
  camera-cut L-inf idea already in the temporal pass, but per-pixel), or accept some
  softening as correct motion blur — decide from the debug capture, not by eye on the
  final composite.
- **March step count vs. pattern frequency.** The existing step count is tuned for
  smooth density falloff, not for resolving a thin rotating blade edge — may alias/strobe
  at low `r_rtVolSamples`. Check with the color-coded overlay before assuming it needs
  more samples.

**Stage 4 — cleanup/generalize. ✅ Done 2026-09-12.**
Removed the debug-tint scaffolding entirely now that Stages 2/3 are in-game validated:
`RT_LIGHT_COOKIE_DEBUG`, both per-file threshold constants, and the tint branch in
`rt_ApplyLightCookie` are gone from `rt_light_cookie.glsl` — it's back to the three
plain helpers (`RTLightCookie`, `rt_SampleLightCookie`, `rt_ApplyLightCookie`,
`rt_LightLuminance`), no `#if` branches. `rt_LightLuminance` stayed (moved there during
Stage 3) since `rt_light_eval.glsl`'s shadow-budget/stochastic-selection code depends on
it independent of cookies.

Confirming ordinary non-fan lights are unaffected: not done as an explicit before/after
screenshot pair, but reasonably covered by evidence already gathered during Stage 2/3
validation — `biground1`, `squarelight1`, `spot01`, `lanternglow` etc. all carried
`GI_LIGHT_FLAG_HAS_COOKIE` and contributed to both reflections and volumetrics
throughout those sessions with no reported darkening. Worth a real screenshot pair if a
darkening regression is ever suspected later, but not blocking.

---

## Explicitly out of scope for this doc

- **True parallel/directional ("sun") lights in volumetrics.** `vk_gi.cpp:1227`
  currently rejects `p.parallel` outright ("no volume boundary or falloff"). This is a
  separate, smaller feature (uniform-density shaft with a camera-relative bounding slab
  instead of point/cone containment) — real, but not what makes Doom 3's fan-shadow
  shots work, since those fixtures are always local projected lights. Worth its own
  short doc if a map with a real sunbeam-through-window shot comes up.
- **Point-light falloff cube images.** Point lights also technically carry a falloff
  material, but it's a smooth radial gradient in every retail light def, not a
  patterned cookie — no visual payoff for the complexity of adding it here.
- **Multi-stage accumulation** (both halves of a `global0`-branched pair active at once).
  Not observed as necessary for the retail fan materials; revisit only if a specific
  fixture needs it.

## Files touched (expected → actual)

- `neo/renderer/Vulkan/vk_gi.cpp` — `considerLight` cookie admission, dump line (Stage 1);
  `VK_RT_GetGILightBufferSize()` accessor (Stage 2)
- `neo/renderer/Vulkan/vk_material_table.cpp` — public bindless-registration wrapper (Stage 1)
- `neo/renderer/Vulkan/vk_reflections.cpp` — null-light-SSBO fallback resized (Stage 2,
  not in original plan — see Stage 2 deviation note)
- ~~`vk_raytracing.h` new SSBO handle~~ — not needed; cookies live inside the existing
  `GILightBuffer`/`RTLightBuf`, no new binding (Stage 1 deviation)
- `neo/renderer/glsl/rt_light_cookie.glsl` — new, shared sampling helper (Stage 2), added
  to `GLSL_INCLUDES` in CMakeLists
- `neo/renderer/glsl/rt_light_eval.glsl` — `RTLightCookie`/`cookies[]` mirror, `flags`
  field, cookie multiply in `rt_LightContribAt`, debug tint (Stage 2)
- `gi_ray.rchit`, `reflect_ray.rchit`, `player_reflect.rchit` — get the cookie multiply for
  free via `rt_light_eval.glsl` (Stage 2); no direct edits needed
- `neo/renderer/glsl/vol_march.comp` — volumetric path (Stage 3): new set=1 material-table
  include, fixed-size `lights[]`/`cookies[]` mirror, per-step cookie multiply
- `neo/renderer/Vulkan/vk_vol.cpp` — vol march pipeline layout extended to 2 sets
  (Stage 3, not in original plan — needed matDescLayout for bindless texture access)
- `neo/CMakeLists.txt` — new glsl include (Stage 2)
