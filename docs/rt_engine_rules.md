# RT engine rules

Check new RT code against this. Not a plan — no ordering, no status, no stages.
`docs/plans/ROADMAP.md` owns what to work on.

Each entry is the short form. The debugging session behind it lives in a memory file or the
relevant `docs/plans/completed/` doc; it is not repeated here.

---

## Taste contract

Anything that fights these gets cut or demoted.

1. **Shadows are the feature.** Soft, shaped, from more lights. This is where "dynamic
   lighting" actually lives.
2. **Darkness stays black.** GI may tint lit regions; it must never lift the noise floor of
   dark ones.
3. **Light the air sparingly.** Volumetrics from hero lights only.
4. **Reflections are set dressing.** Glass and mirrors, plus the odd hero surface by
   explicit opt-in. The assets carry no PBR data; don't pretend they do.
5. **No map editing.** Engine-side rules, budgets and debug overlays. Small def-file mods
   are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite.
7. **Optimization means the same image for less work.** Turning a feature down is not an
   optimization — pure black renders very fast. A change that alters the image is a quality
   setting and must be an explicit opt-in with its own cvar, never a default. Corollary for
   culling work: the shipping threshold is the one where screenshots are unchanged; anything
   past it is a quality tier the player picks. `r_skip*` cvars are probes for splitting a
   measurement, never a configuration.

---

## Correctness rules

Each cost a debugging session. They apply to any new RT code.

| Rule | Because |
|---|---|
| A ray origin from `rt_ReconstructWorldPos` floors its bias at `d²·ulp/znear` | A fixed bias is a distance-limited bias, and `r_znear` is game-owned — it drops to 1.0 in cinematics |
| A shadow ray's cull mask is a parameter, never a constant | `noSelfShadow` is instance mask `0x01`; `0xFF` makes the player self-shadow in reflections only |
| A light's *current* value comes from `EvaluateRegisters()` + the stage's `color.registers[]` | Doom 3 puts flicker in material expressions. `shaderParms` is the constant amplitude, so reading it pins animated lights at peak |
| A mapped buffer the CPU **reads** needs `HOST_CACHED`, memcpy'd out before use | `HOST_VISIBLE\|HOST_COHERENT` alone is write-combined: ~200 ns per scalar read. That per-element cost is the diagnostic signature |
| A mapped buffer or descriptor set the CPU **writes** while recording needs one copy per frame slot, indexed by `vk.currentFrame` | The previous frame is still on the GPU. `r_vkSerializeFrames 1` is the check; the failure is GPU-dependent and can hide on one vendor |
| Use the vertex normal, not `gl_HitKindEXT` | Doom 3 winds front faces opposite to GL/Vulkan, so every visible surface reports back-facing |
| A pixel-skipping pattern keys off a per-slot counter, never `tr.frameCount` | Frames in flight desynchronise the pattern from the resource it writes |
| Shared GLSL that compute shaders include stays pipeline-agnostic — no ray payload, no `traceRayEXT`, no TLAS | `rt_light_struct.glsl` is that home; `rt_light_eval.glsl` is not. An RT-only built-in in a compute shader fails silently |
| Validate bindless descriptors against `backendData`; reset append-only slot tables on level load | An `idImage*` survives a purge but its `VkImageView` does not |
| Per-object GPU buffers come from a `vk_buffer_pool`, never their own `vkAllocateMemory` | ~0.3 ms per call on Windows regardless of size; per-surface allocation made a 35-BLAS first-sight frame cost 133 ms |
| `neo/game` and `neo/d3xp` are mirrored | Near-identical trees; a one-sided change is a latent expansion-pack bug |

---

## Settled conclusions

Re-litigate only with new measurements.

| Conclusion | Detail |
|---|---|
| **Albedo is the volumetric lever** — not extinction, not the tonemap toe | It cuts in-scatter while leaving attenuation intact, so the medium darkens more than it glows and lit-vs-unlit contrast goes *up* |
| **Do not "fix" RT direct lighting being half the raster path's** | True at the source (`r_lightScale` 2 is not applied to the RT upload), but the downstream gains overshot it. A tuning matter, not a plumbing one |
| **The tonemap toe is not monotonically compressive** | At default settings its slope exceeds 1.0 through the midtones while compressing the bottom. Evaluate the whole curve before blaming tonemapping |
| **Reflected surfaces get no indirect light** | GI modulates by the *primary* G-buffer albedo. Structural, not a tuning miss |
| **Light falloff matches the raster path** (reach = 1) | Keeps the benefit of the original lighting art; new techniques enhance rather than fight it |
| **A reflection threshold cannot open opaque surfaces** | The worst artifacts are the *highest*-F0 pixels while any threshold culls from the bottom. Only changing the reflection's character (roughness) can |
| **Patch vertex normals can invent curvature on flat glass; tell creases from arcs by symmetry, not angle** | `GenerateNormals` averages across a patch crease, and a one-row flat facet then ramps its normal across its whole height — an inverting lens. Deviation size overlaps real arcs (both ~5°); lopsided per-triangle vertex tilt doesn't (`r_rtGlassNormalMode 2`) |
| **F0 and roughness are independent, and GGX needs both** | F0 is reflectance at normal incidence, roughness is lobe width. Doom 3 has no per-material gloss (one global NdotH table), and the specular map is already spent on F0 — so roughness comes from bump-map normal variance, which is real data the assets carry |

---

## Discriminators

Symptom → where to look. The general "log and colour-code before theorising" posture is in
`.claude/CLAUDE.md`; these are the specific tells.

| Symptom | Look at |
|---|---|
| A CPU span grows by ~1 ms per event while a per-event log is on | The log. Each console line costs ~1 ms here (log file under OneDrive); recapture with the per-event log off before blaming the span |
| A knob "does nothing" | Whether it reaches the pass at all. RT stages have no derivatives, so material fetches are mip 0 through `matSampler` — unreachable by any sampler cvar |
| A debug overlay reads as a gradient when it should be binary | Something downstream is blurring or EMA-ing it |
| An effect flips on a pure yaw at a fixed position | Per-frame view-dependent scene data, i.e. the TLAS — not the shader |
| An `r_skip*` A/B shows a big win | `ring.draws`. **If it moved, the probe deleted draws and the delta is void.** `r_skipDiffuse 1` + `r_skipSpecular 1` together make both images black, which fails the submit test at `tr_render.cpp:742` and skips the interaction entirely — 25% off `gpu.Interactions` for −52% draws. `r_skipBump` alone is safe (not in that condition); `r_skipSpecular` alone is safe; `r_skipDiffuse` alone drops every surface with no specular stage |
| Vulkan validation / device-lost / vendor-specific | `docs/vulkan_debugging.md` before anything else |
