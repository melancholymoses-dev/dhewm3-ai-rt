# G-Buffer Normal/F0 Pass — Implementation Plan (Stage 3.5, corrected)

**Date:** 2026-08-08
**Branch:** shaped-shadows
**Supersedes:** Stage 3.5 in `completed/lighting_shadows_refinement.md` (which contains a fatal
ordering flaw — see "Why the previous attempt went in circles" below).

---

## Goal

Give the RT passes (reflections first, AO/GI later) access to the **bump-mapped shading
normal** and a **normalized F0** per pixel, so that:

1. Grate floors / flat polygons stop reflecting like mirrors (reflection *direction* fix).
2. The reflection pass can early-out on non-reflective pixels (large perf win).
3. Reflection compositing can move out of the per-light interaction shader into a
   dedicated fullscreen pass, fixing the N-lights-= N× reflection over-brightness bug
   (already documented as a known limitation in `vk_reflections.cpp:16-19`).

---

## Why the previous attempt went in circles

The original Phase 9 Stage 3.5 said: *"Add a world-space normal render
target written during the interaction pass and sampled by the reflection rgen."*

**That cannot work in-frame.** The frame order in `vk_backend.cpp` (~line 4220) is:

```
1. Depth prepass            (VK_RB_FillDepthBuffer, vk_backend.cpp:2226)
2. End render pass → TLAS rebuild
3. RT dispatches:  AO → REFLECTIONS → GI → GI temporal/atrous → volumetrics
4. Resume render pass → GI composite → per-light INTERACTION passes
5. Shader passes, fog, tonemap
```

The reflection rays are traced at step 3; the interaction pass runs at step 4. A normal
buffer written by the interaction shader is only available one frame late (ghosting on
camera motion, garbage after view changes). Any implementation that starts from the
interaction pass, then discovers the ordering problem, then tries to move the dispatch,
will chase its tail: the interaction shader *needs* the reflection buffer, and the
reflection rgen *needs* the normals.

**Resolution: write the G-buffer in the depth prepass (step 1).** It already draws every
opaque + alpha-tested surface with depth-writes before any RT dispatch. We extend it
into a thin geometry prepass ("depth+normal prepass"). No frame-order changes at all.

---

## Design decisions (locked — do not relitigate during implementation)

| Decision | Choice | Rationale |
|---|---|---|
| Where written | Depth prepass (`VK_RB_FillDepthBuffer`) | Only pass that runs before RT dispatches; see above |
| Normal space | **World space** | Every RT shader already works in world space; there is no separate view matrix in the refl UBO, only `invViewProj` (which includes projection, so extracting a view rotation is error-prone). View-space encoding buys nothing here. |
| Format | `VK_FORMAT_R8G8B8A8_UNORM` | rgb = worldNormal × 0.5 + 0.5, **a = normalized F0** (the Stage-2 remap, computed in the prepass). 8-bit normal (~0.4° quantization) is plenty for reflection direction. Alpha = 0 doubles as the "no G-buffer data here" flag → rgen falls back to `rt_ReconstructNormal`. |
| Attachment strategy | Second color attachment on the **HDR render passes only** | All 10 graphics-pipeline creation sites target `vk.hdrRenderPass` (verified by grep). The LDR `vk.renderPass`/`renderPassResume` are not used for scene drawing (tonemap resolves via blit/compute) — leave them untouched. Verify this at implementation time with a grep for `renderPass = vk.renderPass` in begin-info structs; if something does draw there, it contains no G-buffer-writing pipeline and needs no change. |
| Who else consumes it | Reflections now; AO + GI rgen in a follow-up commit — **landed Wave 3 (2026-08-14, P9)**: both rgens sample binding 5, validity = rgb ≠ 0.5 clear sentinel (alpha only means F0 == 0) | AO/GI currently use `rt_ReconstructNormal` too and will benefit (bump-mapped GI, fewer depth fetches), but keep the first PR small. |
| Reflection compositing | Move OUT of `interaction.frag` into a new fullscreen `refl_composite` pass modeled on `gi_composite` | Kills the per-light N× accumulation artifact — a major cause of "way too bright metal". The interaction shader's reflection block (`interaction.frag:139-155`) is deleted, not re-enabled. |

---

## Step 0 — Prerequisite: enable `independentBlend`

`vk_instance.cpp` ~line 327 (`features2.features…`): `independentBlend` is **not
currently enabled**. With two color attachments, any pipeline whose two
`VkPipelineColorBlendAttachmentState` entries differ (write masks *will* differ)
requires it. Add:

```cpp
features2.features.independentBlend = supportedFeatures2.features.independentBlend;
```

(universally supported on desktop; warn-and-bail into the old path if absent).

---

## Step 1 — G-buffer image (new file `neo/renderer/Vulkan/vk_gbuffer.cpp`)

Per-frame `vkRT.gbufNormal[VK_MAX_FRAMES_IN_FLIGHT]` (reuse `vkReflBuffer_t` struct):

- `R8G8B8A8_UNORM`, swapchain extent, `COLOR_ATTACHMENT_BIT | SAMPLED_BIT`.
- Lifetime/resize: create + destroy alongside `hdrScene` in `vk_tonemap.cpp`
  (`VK_CreateHDRTargets` ~line 72 and the resize path at `vk_swapchain.cpp:557`), or
  call into `vk_gbuffer.cpp` helpers from those sites. The HDR framebuffers are rebuilt
  there already — same hook points.
- Initial layout transition: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL at creation
  (one-time submit, same pattern as `VK_RT_CreateReflImages`).
- Include the standard new-file GPL/GenAI header comment block.

## Step 2 — Render pass + framebuffer changes

`vk_swapchain.cpp` `VK_CreateRenderPass()`:

- `vk.hdrRenderPass`: add attachment index **2** = gbufNormal,
  `loadOp=CLEAR` (clear value `{0.5, 0.5, 0.5, 0.0}` = null normal, F0 0),
  `storeOp=STORE`, `initialLayout=COLOR_ATTACHMENT_OPTIMAL`*,
  `finalLayout=COLOR_ATTACHMENT_OPTIMAL`.
  Subpass: `colorAttachmentCount = 2`, `pColorAttachments = {{0, COLOR}, {2, COLOR}}`
  (depth stays attachment ref 1).
  *Use COLOR_ATTACHMENT_OPTIMAL, not UNDEFINED, so the resume round-trip is symmetric;
  the image was transitioned at creation.
- `vk.hdrRenderPassResume`: same attachment with `loadOp=LOAD`,
  `initialLayout=COLOR_ATTACHMENT_OPTIMAL`.
- **Do not touch** `vk.renderPass` / `vk.renderPassResume` (LDR).

`vk_tonemap.cpp` ~line 227: HDR framebuffers become
`{hdrScene[i].view, vk.depthView, gbufNormal[i].view}` — array order must match render
pass attachment order.

`vk_backend.cpp:4106-4117`: `clearValueCount = 3`; `clearValues[2].color = {0.5f, 0.5f, 0.5f, 0.0f}`.

**Subview note (write this down so nobody chases it):** subviews/mirrors render first
and scribble the G-buffer inside their scissor, but the main view's own depth prepass
redraws everything afterwards, before the main view's RT dispatches run. RT is already
skipped for subviews (`vk_backend.cpp:4294`). No extra handling needed.

## Step 3 — Update every graphics pipeline (mechanical checklist)

Every `vkCreateGraphicsPipelines` site targeting `vk.hdrRenderPass` needs
`blendState.attachmentCount = 2` with a second `VkPipelineColorBlendAttachmentState`:

| Site | Second attachment state |
|---|---|
| `vk_pipeline.cpp:338` (interaction ×4 variants) | writeMask **0**, blend off |
| `vk_pipeline.cpp:488` | writeMask 0, blend off |
| `vk_pipeline.cpp:630` | writeMask 0, blend off |
| `vk_pipeline.cpp:780` | writeMask 0, blend off |
| `vk_pipeline.cpp:933` | writeMask 0, blend off |
| `vk_pipeline.cpp:1181` (GUI Ex factory) | writeMask 0, blend off |
| `vk_pipeline.cpp:1301` (skybox) | writeMask 0, blend off |
| `vk_pipeline.cpp:1556` (cached GUI blend factory) | writeMask 0, blend off |
| `vk_gi.cpp:773` (GI composite) | writeMask 0, blend off |
| `vk_vol.cpp:541` (vol composite) | writeMask 0, blend off |
| **new** G-buffer prepass pipelines (Step 4) | attachment 0: writeMask 0 (like today's depth pipeline); attachment 1: writeMask RGBA, blend off |

Suggested: add a small helper `VK_FillSecondBlendAttachment(VkPipelineColorBlendAttachmentState*)`
so the 10 edits are one-liners. A pipeline that misses this fails validation immediately
(attachment count mismatch), so mistakes are loud, not silent.

## Step 4 — G-buffer prepass shaders + pipelines

New shader pair (add to `GLSL_INCLUDES` in CMakeLists):

- **`gbuffer.vert`** — inputs: position, st, normal (loc 2), tangent0/1 (idDrawVert
  layout; `VK_GetInteractionVertexInput` already emits all attribute descriptions).
  UBO adds `mat4 u_ModelMatrix` (from `surf->space->modelMatrix` — rigid transform in
  Doom 3, plain `mat3` multiply is safe for normals). Outputs world-space T, B, N and UV
  (bump-stage texture matrix applied).
- **`gbuffer.frag`** — bindings: 1 = bump map, 2 = diffuse (alpha test, clip variant
  only), 3 = specular map. Fragment:
  ```glsl
  vec3 nTS  = normalize(texture(u_BumpMap, uv).rgb * 2.0 - 1.0);
  vec3 nWS  = normalize(mat3(T, B, N) * nTS);
  float specLum = dot(texture(u_SpecularMap, uvSpec).rgb, vec3(0.299, 0.587, 0.114));
  float f0 = clamp(pow(specLum, u_SpecF0Gamma) * u_SpecF0Scale, 0.0, 1.0);
  outGbuf = vec4(nWS * 0.5 + 0.5, f0);          // layout(location = 1) out
  outColor = vec4(0.0);                          // location 0, write-masked anyway
  ```
  Reuse the same luminance/remap constants as `interaction.frag:118-119` so the debug
  overlay (`r_rtReflectionDebugMode 1`) stays truthful.
- Two pipelines: `gbufferPipeline` (opaque, replaces `depthPipeline` when RT active) and
  `gbufferClipPipeline` (perforated: adds diffuse alpha test exactly like
  `depth_clip.frag`). Depth state identical to the existing depth pipelines.
  Keep the old depth pipelines: non-RT path (`r_useRayTracing 0`) is untouched.
- New pipeline layout with push descriptors (UBO + 3 samplers), mirroring `guiLayout`
  usage in the prepass.

## Step 5 — Backend: `VK_RB_FillDepthBuffer` (`vk_backend.cpp:2226`)

When RT is enabled and the G-buffer pipelines exist:

- Per surface, walk `mat->GetStage(i)` for the first `SL_BUMP` and `SL_SPECULAR` stage
  images (same sources the interaction path uses). Fallbacks: flat-normal texture for
  bump (geometric normal survives), black for specular (F0 = 0 → no reflection — the
  correct default).
- Fill the new UBO (MVP, model matrix, bump texture matrix, alpha-test params,
  `r_rtSpecF0Scale/Gamma`), push-descriptor the three images, select
  `gbufferPipeline`/`gbufferClipPipeline` instead of `depthPipeline`/`depthClipPipeline`.
- Everything else in the loop (scissors, vertex/index sourcing, subview guard) is reused
  unchanged.
- Add a breadcrumb log (once, at `r_vkLogRT >= 1`): count of surfaces drawn with
  bump/spec bound vs. fallback — per project convention of logging new mechanisms
  immediately.

## Step 6 — Barriers around the RT block (`vk_backend.cpp` ~4236/4364)

Immediately after `vkCmdEndRenderPass` (before TLAS rebuild):

- gbufNormal: `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`
  (src: COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE, dst: RAY_TRACING_SHADER / SHADER_READ).

Just before the resume `vkCmdBeginRenderPass`:

- `SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL` (mirror of the depth barrier
  pattern already inside `VK_RT_DispatchReflections`, but do it **once in the backend**
  for the whole RT block, since AO/GI will consume it later too).

## Step 7 — Reflection rgen consumes it

`vk_reflections.cpp` + `reflect_ray.rgen`:

- New binding 5: `sampler2D gbufNormalSampler` (descriptor layout, pool sizes, writes,
  the resource-changed tracking statics).
- rgen, replacing the `rt_ReconstructNormal` branch (`reflect_ray.rgen:104-107`):
  ```glsl
  vec4 g = texelFetch(gbufNormalSampler, coord, 0);
  float f0 = g.a;
  if (glassProbe.hitT <= 0.01 && f0 < 1.0/255.0) {   // EARLY-OUT: non-reflective pixel
      imageStore(reflImage, coord, vec4(0.0));
      return;                                         // skips refl ray + shadow rays
  }
  vec3 reflNormal = (g.a > 0.0)
      ? normalize(g.rgb * 2.0 - 1.0)
      : rt_ReconstructNormal(coord, size, params.invViewProj, worldPos, -viewDir);
  ```
  Write `f0` into the output alpha for opaque pixels (glass keeps its glassWeight path)
  so the composite pass doesn't need a second G-buffer fetch.
  Note: place the early-out **after** the glass probe (glass reflections don't depend
  on surface F0), or gate the probe on a scene-has-glass flag (see perf review).

## Step 8 — Reflection composite pass (replaces interaction.frag blend)

Clone the `gi_composite` machinery (`vk_gi.cpp:773` + `gi_composite.frag/vert`):

- New `refl_composite.frag`: samples reflBuffer (rgb = radiance, a = F0 or glassWeight),
  depth (for NdotV via reconstructed view dir), gbufNormal. Computes Schlick **once**:
  ```glsl
  float NdotV   = clamp(dot(nWS, -viewDir), 0.0, 1.0);
  float grazing = mix(u_GrazingMax, 1.0, smoothstep(0.3, 0.7, f0)); // rough-surface horizon clamp
  float fresnel = f0 + (grazing - f0) * pow(1.0 - NdotV, 5.0);
  out = vec4(refl.rgb * fresnel, 1.0);   // additive blend, drawn once per frame
  ```
- Dispatch right after GI composite in `vk_backend.cpp` (same resume-pass slot).
- Delete the disabled reflection block + `u_UseReflections`/`u_ReflectionMap` sampling
  from `interaction.frag` (keep the Fresnel debug mode; it still visualizes the remap).
- `r_rtReflectionBlend` default should drop from 1.5 toward 1.0 — the 1.5 was
  compensating the multi-light accumulation this pass removes.
- New CVar `r_rtSpecGrazingMax` (default ~0.35): the `u_GrazingMax` clamp above. This is
  the direct answer to "suppress large contributions at grazing reflection" — physically,
  rough surfaces never reach mirror-like grazing Fresnel because of shadowing/masking;
  clamping the grazing lobe for low-F0 surfaces is the standard approximation when you
  don't have per-pixel roughness.

## Step 9 — Debug modes (do these FIRST when bringing it up)

Extend `r_rtReflectionDebugMode` (plumbed through the composite pass or rgen):

- `2` = output G-buffer world normal as RGB (`g.rgb` raw). Walk a grate floor: it must
  look bumpy/detailed, not flat single-color. Silhouette edges must match geometry.
- `3` = output F0 (alpha) greyscale. Most of the scene near-black; metal trim/highlights
  grey; nothing solid white.
- `4` = pixels using fallback reconstruction (alpha == 0) tinted magenta — should be
  only sky/translucents after the prepass is complete.

## Commit slicing

1. `independentBlend` + image + render pass/framebuffer/pipeline plumbing + prepass
   writes normals only (F0 = spec remap), debug modes 2-4. **No behavior change** to
   final image. Validate with debug overlays in-game.
2. rgen reads G-buffer + early-out + composite pass + delete interaction blend +
   re-enable reflections (Stage 3). Tune `r_rtSpecF0Scale/Gamma/GrazingMax`.
3. Follow-up: AO and GI rgen switch `rt_ReconstructNormal` → G-buffer sample.

## Risks

- **Pipeline churn:** 10 sites + 2 new pipelines; each miss is a loud validation error.
  Low risk, tedious.
- **TBN sign/handedness:** Doom 3 stores two tangents rather than tangent+sign; the
  interaction vertex shader (`interaction.vert`) shows the exact usage to copy. Debug
  mode 2 catches mistakes immediately (inverted normals = wrong hemisphere colors).
- **Prepass cost:** the depth prepass gains 3 texture binds + a bump/spec fetch per
  pixel. It was previously bandwidth-trivial; expect ~0.1-0.3 ms at 1440p, and the
  reflection early-out should win back far more.