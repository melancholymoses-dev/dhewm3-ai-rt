# RT Projected Light Cookies (fan blades, grates, window blinds)

**Date:** 2026-08-31
**Status:** Spec only — not started. Not yet linked from ROADMAP.md; add a row there
before picking this up.
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

**Stage 1 — CPU plumbing + dump validation.**
Add `GILightCookieBuf`, the admission-time stage walk, bindless registration. Extend the
existing `r_rtGILightDump` to print `cookie=<image name>@stage<i>` per admitted light so
we can confirm in a log that `fanlightgrate`/`fanblade3` are actually picked up, before
touching any shader — matches the project's debug-before-theory rule, and catches a
wrong-stage or condition-register bug for free.

**Stage 2 — direct lighting.**
Wire `rt_SampleLightCookie` into `rt_LightContribAt` (shared by `gi_ray.rchit` and
`reflect_ray.rchit` via `rt_light_eval.glsl`). Debug overlay: tint any shaded point whose
cookie luminance is below ~0.05 magenta, so the blade silhouette on a wall/floor can be
visually confirmed moving frame-to-frame before trusting the full composite. Validate
against a known fanlightgrate fixture in-game.

**Stage 3 — volumetrics.**
Same sampling in `vol_march.comp`'s per-step light loop for `lightType` 1 and 2, reusing
`GILightCookieBuf`. This is the actual "see the blades in the light shaft" payoff.
Watch for two things specific to volumetrics:
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

**Stage 4 — cleanup/generalize.**
Confirm ordinary non-fan projected lights (wall sconces, simple spot fixtures) are
unaffected — most just have a plain gradient stage, so the cookie multiply should be a
near-no-op. Any visible darkening regression there means the stage-selection heuristic
(first passing stage) picked the wrong stage or a falloff-only texture that shouldn't be
treated as a cookie — worth an explicit before/after screenshot pair of a plain corridor
light, not just the fan fixture.

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

## Files touched (expected)

- `neo/renderer/Vulkan/vk_gi.cpp` — `considerLight` cookie admission, dump line
- `neo/renderer/Vulkan/vk_material_table.cpp` — public bindless-registration wrapper
- `neo/renderer/Vulkan/vk_vol.cpp` — `GILightCookieBuf` allocation/upload, descriptor binding
- `neo/renderer/Vulkan/vk_raytracing.h` — new SSBO handle + struct decl
- `neo/renderer/glsl/rt_light_cookie.glsl` — new, shared sampling helper (add to
  `GLSL_INCLUDES` in CMakeLists per project convention)
- `neo/renderer/glsl/rt_light_eval.glsl`, `gi_ray.rchit`, `reflect_ray.rchit` — direct path
- `neo/renderer/glsl/vol_march.comp` — volumetric path
- `neo/CMakeLists.txt` — new glsl include
