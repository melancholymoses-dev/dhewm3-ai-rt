# RT engine rules

Check new RT code against this. Not a plan — no ordering, no status, no stages.
`docs/plans/ROADMAP.md` owns what to work on.

Each entry is the short form. The debugging session behind it lives in a memory file or the
relevant `docs/plans/completed/` doc; it is not repeated here.

---

## Taste contract

Anything that fights these gets cut or demoted.

1. **Drama of light and shadow is the goal.** High dynamic range, strong contrast. Shadow —
   the absence of light — is what makes the contrast; soft penumbrae are a refinement, not
   the point.
2. **Darkness stays black.** GI may tint lit regions; it must never lift the noise floor of
   dark ones.
3. **Volumetrics shape the light.** Beams and shafts that show where light goes. Not a
   uniform haze over every room.
4. **Reflections add presence.** Glass, sheen on metal, screens and emitters glowing in nearby
   surfaces. Rough surfaces read as a wash, not mirrors or pinpoint highlights. The assets
   carry no PBR data — F0 and roughness are derived — so don't chase physical exactness.
5. **No map editing.** Engine-side rules, budgets and debug overlays. Small def-file mods
   are allowed.
6. **Debug visualization before tuning.** Every feature ships with an overlay mode;
   constants get tuned from the overlay, not by eye on the final composite. Overlays are
   scaffolding: delete one once its feature is settled, and never let one constrain a design.
7. **Optimization means the same image for less work.** Turning a feature down is not an
   optimization — pure black renders very fast. A change that alters the image is a quality
   setting and must be an explicit opt-in with its own cvar, never a default. Corollary for
   culling work: the shipping threshold is the one where screenshots are unchanged; anything
   past it is a quality tier the player picks. `r_skip*` cvars are probes for splitting a
   measurement, never a configuration.
8. **Budget follows impact.** Reflections and volumetrics carry the look; GI and soft RT
   shadows have to justify their cost with a visible change.

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
| Admit rays at the scale the denoiser filters at, and keep the per-pixel weight out of the filtered signal | A cull on a per-pixel quantity leaves holes no filter can blur across, and the surviving pixels carry that quantity's pattern into the blur. Coarse mask to admit, fine weight in alpha, multiply at composite |
| Trace below full res, but reconstruct the per-pixel BRDF weight at full res — only the incident light is upsampled | Interpolating the weight smears every specular trim edge; recomputing it from the full-res G-buffer keeps material detail at quarter ray rate. The corollary of the demodulation rule above |
| A reconstruction pass belongs **before** the temporal and spatial filters, not after | Reprojection and plane edge stops are what most dislike a coarse grid; upsampling first leaves both working full-res and leaves the denoisers unmodified |
| A sub-pixel pattern that rotates must not feed a reprojecting accumulator | One history pixel then accumulates several different world positions, which straddle edges, so the disocclusion test rejects forever and the EMA never converges |
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
| **The opaque-reflection firefly clamp runs every à-trous pass (1/2/4) with no absolute floor** | A floor or a pass-0-only clamp "to preserve real highlights" left white specks on black and outlined railings. Single-sample GGX reflections aren't physical enough for isolated highlights to be trusted |
| **Screen-space filter edge stops use plane distance, not depth** | `|dot(pT − pC, nGeo)| / viewDist` with `nGeo` from depth. Raw NDC depth is dead past ~20 units; relative depth blocks grazing floors. The bump-mapped G-buffer normal only for the normal stop, at low power (~4) |
| **Opaque reflections clamp fireflies before temporal accumulation, and get no variance-boosted alpha** | An unclamped spike enters history and lingers ~1/alpha frames. The variance boost (built for moving glass) gave the noisiest GGX pixels alpha ≈ 1, so they flickered through unaveraged |

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
