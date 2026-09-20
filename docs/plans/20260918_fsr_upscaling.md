# FSR upscaling for dhewm3-rt

**Status:** Not started — design only.
**Written:** 2026-09-18
**Owns:** render-resolution decoupling, AMD FidelityFX Super Resolution integration,
motion vectors, jitter, and the licensing paperwork that comes with vendored code.

---

## Thesis

Every RT pass in this engine is screen-resolution. The 2026-09-11 Mars City budget is
**11.93 ms of RT at the window's native resolution**, and that number is a pure function
of pixel count — GI 4.41, reflections 3.24 (pre-gating), vol 1.53, AO 1.17, denoise ~0.65.
Nothing in that list is geometry-bound or draw-call-bound.

So decoupling render resolution from display resolution is worth more here than it is in
a raster game. At FSR *Quality* (1.5× linear, 44 % of the pixels) the same scene costs
**≈ 5.3 ms of RT instead of 11.93** — a 6.6 ms saving before the raster passes are counted,
against an upscaler cost of roughly 1.0–1.5 ms at 1080p output. That is a bigger win than
any individual optimisation left in `rt_optimization_tuning.md`.

The corollary, and the reason this doc is structured the way it is: **the perf win comes
from the resolution split, not from FSR.** FSR is what stops the split from looking bad.
Stage U0 delivers the entire performance benefit with a bilinear blit and no third-party
code at all; U1–U4 buy the image quality back. Sequence the work so the win lands first
and is measurable on its own.

---

## Pillar check

| Pillar | Effect |
|---|---|
| 1. Shadows are the feature | Neutral-to-positive. Soft shadows are low-frequency and reconstruct well; the freed milliseconds can buy more shadow-casting lights, which is the pillar's actual goal. |
| 2. Darkness stays black | **At risk.** A temporal upscaler has a history buffer, and history buffers leak light from bright frames into dark ones. FSR2's clamping is luminance-based and tuned on much brighter games. This needs an explicit check in U3's exit criteria, not a hope. |
| 3. Light the air sparingly | Neutral. Froxel vol is already resolution-decoupled in its grid; only its screen-space resolve scales. |
| 4. Reflections are set dressing | Mild risk: glass is exactly the high-frequency, disocclusion-prone content FSR2 handles worst, and `r_rtReflectionMode 1` traces only over the glass screen rect, so the rect has to be scaled correctly or reflections land in the wrong place. |
| 5. No map editing | Held. Entirely engine-side. |
| 6. Debug visualization before tuning | Enforced: `r_fsrDebug` modes ship in the same chunk as the mechanism they visualise (§7). |

---

## 1. Which SDK

### Recommendation

**Primary: `FidelityFX-FSR2`, the standalone FSR 2.2.1 release, with its native Vulkan
backend.** License MIT.

Reasons, in order of weight:

1. **It has a real Vulkan backend that is not an afterthought.** `ffx_fsr2_vk.h` takes a
   `VkPhysicalDevice` and a `PFN_vkGetDeviceProcAddr`, allocates its own internal
   resources through that device, and dispatches into a `VkCommandBuffer` we hand it.
   No D3D12 interop layer, no swapchain ownership.
2. **It is self-contained.** One `ffx-fsr2-api/` directory, a CMake target, no `ffx_api`
   loader DLL, no DX12 sibling that has to be stubbed out to make the build work on
   Linux. That matters more here than quality-per-frame, for a project that has to build
   on Windows and Linux from the same tree.
3. **It does not own the present path.** Frame *generation* (FSR3) does, and that is where
   the Linux story gets bad — an interpolating swapchain has to hook present, manage its
   own timing, and interact with the compositor. Upscaling alone is a compute dispatch in
   the middle of our frame. Keep it that way.
4. Cross-vendor by construction: a hand-written compute algorithm, not an inference model,
   so it runs on NVIDIA, Intel and AMD, on RADV/ANV/amdvlk/proprietary, with no hardware
   gate.

### Alternatives, and why not

| Option | Verdict |
|---|---|
| **FSR 1** (`ffx_a.h` + `ffx_fsr1.h`, EASU + RCAS) | **Keep as a fallback, do not make it the goal.** Two MIT headers that compile under `glslc` as GLSL with `#define A_GLSL 1` — genuinely zero new build dependencies, which is why it is tempting. But it is a spatial filter that assumes a *clean, anti-aliased, perceptual-space* input, and this engine supplies the opposite: no AA at all, plus a stochastic RT noise floor. EASU will sharpen the aliasing and RCAS will sharpen the noise. Worth having behind `r_fsr 1` as an escape hatch (driver problems, a platform where the FSR2 backend misbehaves, a card where FSR2's internal resources don't fit), not worth tuning. |
| **FidelityFX SDK / FSR 3.1 upscaler** (`ffx_api`, `ffxCreateContext`/`ffxDispatch`) | **The upgrade path, not the starting point.** Better image quality than 2.2.1 and a stable ABI, and its dispatch inputs are a *superset* of FSR2's — same colour/depth/MV/exposure/reactive set — so a later port is largely renaming, which is exactly why FSR2 first is cheap rather than wasted. Deferred because the SDK's build is structured around a Windows/DX12 sample framework and an `ffx_api` loader, and untangling a Linux-clean Vulkan-only build of it is unbounded work to do *before* we know our motion vectors are correct. Revisit at U5, once U3 has proven the plumbing. |
| **FSR 4** | **Out of scope.** ML-based, RDNA4-gated, and distributed as a binary rather than buildable source. Fails the cross-vendor requirement and the redistribution requirement at the same time. |
| DLSS / XeSS | Out of scope — vendor-locked, and DLSS's SDK terms are not GPL-compatible for redistribution. If they are ever wanted, U2's motion-vector and jitter work is the shared prerequisite and is deliberately written to be upscaler-agnostic. |

### Before writing any code

> ⚠️ **Verify the API surface against the pinned upstream tag.** The symbol names below
> (`ffxFsr2GetInterfaceVK`, `FfxFsr2DispatchDescription` field names, the flag spellings)
> are from FSR 2.2.1 and are written here to make the plan concrete, not because they have
> been compiled against in this tree. Pin an exact tag in the vendoring commit, read
> `ffx-fsr2-api/ffx_fsr2.h` and `ffx-fsr2-api/vk/ffx_fsr2_vk.h` at that tag, and correct
> this document if anything has drifted. Do not guess a symbol.

---

## 2. The architecture: render into a sub-rectangle

The naive approach — reallocate every offscreen target at render resolution and leave the
swapchain at display resolution — runs straight into Vulkan render-pass compatibility.
The HUD, the 2D GUI and ImGui all draw into `hdrScene` through `vk.hdrRenderPass` /
`vk.hdrRenderPassResume` (`vk_backend.cpp:5178`, `vk_tonemap.cpp:18-22`), using the same
pipeline objects as the 3D scene. Splitting the UI onto a display-resolution render pass
means either duplicating every pipeline in `vk_pipeline.cpp` or allocating a second
display-resolution copy of *all four* attachments (hdrScene, depth, gbufNormal,
gbufAlbedo) — a framebuffer's attachments must be at least as large as the framebuffer, so
render-resolution images cannot back a display-resolution framebuffer.

**Do neither. Keep every attachment at display resolution and render the 3D scene into the
top-left `renderExtent` sub-rectangle of it.**

```
  ┌───────────────────────────────┐  hdrScene / depth / gbufNormal / gbufAlbedo
  │ ┌──────────────┐              │  and every RT buffer: allocated at DISPLAY res
  │ │              │              │
  │ │ renderExtent │   unwritten  │  3D pass viewport+scissor = renderExtent
  │ │  (top-left)  │              │  RT dispatch rects        = renderExtent-clamped
  │ └──────────────┘              │
  │                               │
  └───────────────────────────────┘
```

What this buys:

- **Zero new render passes and zero new pipeline variants.** Framebuffer extents are a
  property of the framebuffer, not the render pass, and we are not even changing the
  framebuffer — only the viewport, scissor and `renderArea`.
- **Every `gl_FragCoord`-indexed composite keeps working untouched.** `gi_composite`,
  `refl_composite`, `vol_composite` and the interaction pass address the RT buffers by
  fragment coordinate (`screenSize`, `vk_backend.cpp:1084`). Because the RT buffers are
  written in the *same* top-left sub-rect at the *same* scale, the indexing stays identity.
  No UV rescaling anywhere in the shader chain. This is the single biggest reason to prefer
  this layout.
- **FSR2 supports it natively.** `FfxFsr2ContextDescription::maxRenderSize` plus a
  per-dispatch `FfxFsr2DispatchDescription::renderSize` *is* the dynamic-resolution
  contract: the upscaler reads the top-left `renderSize` region of larger textures. Dynamic
  resolution later becomes nearly free.
- **No VRAM regression.** Allocations are the same size they are today.

What it costs:

- VRAM is sized for display resolution even though only part is used. Since that is today's
  footprint, it is a non-regression rather than a saving. (The alternative layout would
  have *saved* memory; we are trading that for not touching the pipeline cache. Correct
  trade.)
- Every display-space → Vulkan-space rect conversion must now scale *and* flip Y against
  the render height rather than the swapchain height. Concentrated in two helpers (§4).

### Frame graph after U3

```
[3D pass, viewport/scissor = renderExtent, JITTERED projection]
  depth prepass  → depth, gbufNormal, gbufAlbedo, motionVectors     ← U2 adds MV
  (end pass)
  RT: shadows → AO → reflections → GI (± probes) → vol/froxel       ← all renderExtent rects
  (resume pass)
  GI composite / refl composite / [early vol composite]  → hdrScene
  interactions                                           → hdrScene
  (snapshot hdrScene → preAlphaColor, renderExtent only) ← U4, auto reactive mask only
  shader passes (blend stages, particles, glass overlay) → hdrScene
  [late vol composite] / fog lights                      → hdrScene
  (end pass)

[upscale]
  ffxFsr2ContextGenerateReactiveMask(preAlpha, hdrScene) → reactiveMask   ← U4
  ffxFsr2ContextDispatch(color=hdrScene, depth, MV, exposure, reactive)
        → hdrUpscaled   (display res, RGBA16F, HDR — still pre-tonemap)
  copy hdrUpscaled → hdrScene    (full display res)

[resume pass, viewport/scissor = full display extent]
  2D GUI / HUD  (second RC_DRAW_VIEW, viewaxis == 0)     → hdrScene
  ImGui                                                  → hdrScene
  (end pass)

[VK_RT_DispatchTonemap]  hdrScene → tonemapResolve → blit → swapchain    ← unchanged
```

Two properties of this ordering are deliberate:

1. **FSR2 runs on pre-tonemap HDR**, which is what it wants
   (`FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE`) and what `hdrScene` already holds. No reordering
   of the tonemap.
2. **The UI is composited after the upscale, at display resolution, and is still tonemapped**
   — exactly as today. So the UI's appearance does not change at all, and no new "already
   tonemapped" pipeline state is introduced. The one extra full-resolution copy
   (`hdrUpscaled → hdrScene`) is ~0.1–0.2 ms at 4K and buys that invariance. Take the trade.

---

## 3. Resources to add

Per frame-in-flight slot unless noted. Sizes quoted for 1920×1080 display.

| Resource | Format | Extent | Usage | Stage | ~Size |
|---|---|---|---|---|---|
| `vkRT.motionVectors[i]` | `R16G16_SFLOAT` | display (written in render sub-rect) | COLOR_ATTACHMENT, SAMPLED, STORAGE | U2 | 8 MB ×2 |
| `vkRT.hdrUpscaled` | `R16G16B16A16_SFLOAT` | display | STORAGE, SAMPLED, TRANSFER_SRC | U0 | 16 MB ×1 |
| `vkRT.preAlphaColor[i]` | `R16G16B16A16_SFLOAT` | display | TRANSFER_DST, SAMPLED | U4 | 16 MB ×2 |
| `vkRT.reactiveMask[i]` | `R8_UNORM` | display | STORAGE, SAMPLED | U4 | 2 MB ×2 |
| `vkRT.exposure1x1` | `R32_SFLOAT` | 1×1 | STORAGE, SAMPLED | U3 | — |
| FSR2 internal resources | — | — | owned by the FSR2 context | U3 | **~150–250 MB at 1080p→4K; ~60–90 MB at 720p→1080p** |
| FSR2 host scratch | host memory | `ffxFsr2GetScratchMemorySizeVK()` | — | U3 | ~1 MB |

**Not** needed: no new render pass, no new framebuffer, no new pipeline objects for the
existing raster path (§2). New *compute* pipelines: the bilinear/point resolve (U0), and
EASU + RCAS if U1 is built.

Shaders to add (register in `CMakeLists.txt` `GLSL_SHADER_SOURCES`):

| File | Stage | Notes |
|---|---|---|
| `renderer/glsl/upscale_blit.comp` | U0 | bilinear/point resolve of the render sub-rect → `hdrUpscaled`; also carries the `r_fsrDebug` overlays |
| `renderer/glsl/fsr_easu.comp` | U1 (optional) | `#define A_GLSL 1` + `ffx_a.h` + `ffx_fsr1.h` |
| `renderer/glsl/fsr_rcas.comp` | U1 (optional) | ditto |
| `renderer/glsl/motion_debug.comp` | U2 | `r_fsrDebug 2` motion-vector visualisation |

`ffx_a.h` / `ffx_fsr1.h` go in `renderer/glsl/fsr1/` and are added to **`GLSL_INCLUDES`**
(per CLAUDE.md), not `GLSL_SHADER_SOURCES` — they are headers, not stages.

C++ files to add:

| File | Owns |
|---|---|
| `renderer/Vulkan/vk_upscale.h/.cpp` | `vk.renderExtent`, quality-mode → scale mapping, the jitter sequence, the resolve dispatch, and (U3) the whole FSR2 context lifetime. One translation unit, so the FSR2 headers are included in exactly one place. |

Vendored third party:

| Path | Contents |
|---|---|
| `neo/libs/fsr2/` | the pinned `ffx-fsr2-api/` tree, **unmodified**, AMD headers intact |
| `neo/libs/fsr2/LICENSE.txt` | AMD's MIT text, verbatim from upstream |

---

## 4. The resolution-decoupling change list

This is the bulk of the engineering, and it is all in U0. `vk.swapchainExtent` currently
means two different things — "the size of the thing we present" and "the size of the thing
we render" — and every use site has to be sorted into one bucket or the other.

Add to `vk_common.h`:

```c
VkExtent2D renderExtent;   // 3D scene sub-rect; == swapchainExtent when r_fsr is off
```

and to `vk_upscale.h`:

```c
VkExtent2D VK_RT_RenderExtent(void);   // clamped, even-aligned
float      VK_RT_RenderScaleX(void);   // renderExtent.width / swapchainExtent.width
float      VK_RT_RenderScaleY(void);
bool       VK_RT_UpscaleActive(void);  // renderExtent != swapchainExtent
```

### Sites that become `renderExtent`

| `vk_backend.cpp` | What |
|---|---|
| `784-800` `VK_ComputeViewScissor` | **The critical one.** Scale `viewDef->scissor` (display space, from `R_ScreenRectFromWinding` and friends) by the render scale, then flip Y against `renderExtent.height`. Round *outward* — a scissor one pixel too small drops surface edges. |
| `824-845` `VK_ComputeDrawSurfScissor` | Same treatment for `viewport.x1/y1 + surf->scissorRect`. |
| `1084-1085` `screenSize` uniform | → `renderExtent`. This is what the interaction pass uses to turn `gl_FragCoord` into a shadow-mask UV; leaving it at swapchain size shifts every shadow. |
| `2257-2258` overlay `texGenS` | → `renderExtent`. |
| `3549-3560` `lightScissor` default | → `renderExtent`. |
| `138-146`, `3816-3826`, `4495-4510`, `4544-4546`, `4902-4912`, `5008-5012` | Render-pass `renderArea` and the negative-height viewports for the 3D pass and its resumes → `renderExtent`. |
| `5181-5190` (`VK_RB_CopyRender`'s resume) | → `renderExtent`; `_currentRender` capture happens inside the 3D pass. |
| `5102-5130` `VK_RB_CopyRender` blit | Source rect is in display coordinates and must be scaled into the render sub-rect before the blit; the destination `idImage` extent is unchanged. |

### Sites that stay `swapchainExtent`

- `VK_RT_DispatchTonemap` and its blit (`vk_tonemap.cpp:544+`) — display res.
- The screenshot readback in `VK_RB_SwapBuffers` — reads the swapchain, already correct.
- The UI/HUD resume pass introduced in U0 (full-extent viewport after the upscale).
- Swapchain creation and recreation.

### Sites that need an audit, not a mechanical edit

- **`vk_reflections.cpp`'s glass screen-rect union.** `r_rtReflectionMode 1` builds the
  union of `SURFTYPE_GLASS` surface rects CPU-side in display coordinates and dispatches
  `vkCmdTraceRaysKHR` over it. It must go through the same scaling helper, or reflections
  trace the wrong region — and because the mode also *skips the dispatch entirely* when the
  rect is empty, a scaling bug shows up as reflections silently vanishing rather than as
  visible corruption. Check it explicitly.
- **Every RT dispatch rect.** `vk_ao.cpp`, `vk_shadows.cpp`, `vk_gi.cpp`, `vk_vol*.cpp`
  derive their rects from `viewDef->scissor` via the same path, so they inherit the fix —
  but each also clamps against `vk.swapchainExtent` somewhere. Grep `swapchainExtent`
  across `renderer/Vulkan/` and justify every remaining hit.
- **`r_rtGICheckerboard`.** A checkerboard pattern at render resolution, later upscaled and
  temporally accumulated by FSR2 *and* by our own EMA, is three interleaved sampling
  patterns. Expect to turn it off when `r_fsr 2` is active; confirm in U4.

### Texture LOD bias

`vk_image.cpp:758` computes `mipLodBias` per image from `image_lodbias` (+ `r_vkBumpMipBias`
for bump maps) at **upload time**, and the sampler is baked into `vkImageData_t`. Rendering
at 67 % without a bias change means every texture is effectively a third of a mip too
blurry, and FSR2 has less high-frequency detail to reconstruct than it should.

The standard bias is `log2(renderWidth / displayWidth) - 1.0` (≈ −1.58 at Quality, −2.0 at
Performance). Two options:

- **Recreate samplers on scale change.** Add the FSR bias to the `lodBias` computation and
  bump `VK_Image_ChangeCounter()` so `vk_material_table.cpp`'s bindless descriptors refresh
  — that counter exists for exactly this class of problem (see `vk_image.h`). Costs a
  device-idle sampler rebuild whenever `r_fsrQuality` changes, which is a menu action, not
  a hot path.
- Defer to U5 and ship U0–U4 at the default bias. Acceptable; it is a sharpness deficit,
  not a correctness bug.

Recommend the first, implemented in U5, because the second makes U3's image-quality
comparison against native resolution unfair in FSR2's disfavour and may produce a wrong
verdict at the U3 gate.

---

## 5. Motion vectors (U2) — the real work

FSR2 wants, per pixel, the 2D screen-space offset from *this* frame's position to *last*
frame's. Nothing in this engine produces that today (`grep -rn "motion\|velocity\|reproject"
renderer/Vulkan/` finds only the froxel reprojection stub at `vk_vol_froxel.cpp:139` and
comments). This is the chunk that can go wrong quietly, so it ships with its own overlay
before anything consumes it.

### Where it is written

The depth prepass (`VK_RB_FillDepthBuffer`, `vk_backend.cpp:2362`) already writes
`gbufNormal` and `gbufAlbedo` as colour attachments 2 and 3 via `gbuffer.vert/frag`. Motion
vectors become **attachment 4**.

⚠️ **This is not free.** Adding a fifth attachment to `vk.hdrRenderPass` forces every
pipeline created against it to declare five `VkPipelineColorBlendAttachmentState` entries.
`vk_pipeline.cpp` already handles exactly this shape — eight or so sites with the comment
*"every other pipeline on `vk.hdrRenderPass` supplies write-mask-0"* building
`blendAttachments[3]` with `attachmentCount = vk.gbufferSupported ? 3 : 1`
(`vk_pipeline.cpp:311, 471, 620, 776, 936, 1123, 1373`, plus `vk_gi.cpp:1003`). Extend the
array to 4 and the count with a `vk.motionVectorsSupported`-style gate. Mechanical, but
miss one and you get a validation error or a silent pipeline-creation failure.

### What it computes

```glsl
// gbuffer.vert, per vertex, with the UNJITTERED projection on BOTH sides
vec4 curClip  = mvpNoJitter     * vec4(pos, 1.0);
vec4 prevClip = prevMvpNoJitter * vec4(pos, 1.0);
// gbuffer.frag
vec2 mv = (prevClip.xy / prevClip.w) - (curClip.xy / curClip.w);   // NDC units
```

Use **unjittered** matrices on both sides and leave
`FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` off; mixing jittered and unjittered is
the classic source of a 1-pixel shimmer that looks like an upscaler bug and is not. Set
`motionVectorScale` in the dispatch description to convert NDC → the units FSR2 expects
(`{0.5 * renderWidth, -0.5 * renderHeight}` for NDC into a Y-down target — **sign-check
this against a known camera pan before trusting it**; this engine uses a negative-height
viewport for the Y flip, which makes the sign non-obvious).

### Where `prevMvp` comes from

| Geometry class | Source of the previous transform | Risk |
|---|---|---|
| **Static world** | Previous frame's `viewDef->worldSpace.modelViewMatrix` × unjittered projection, cached once per frame in the backend. | None. This is the bulk of the screen and it is exact. |
| **Rigid entities** (doors, lifts, crates, most props) | Cache `prevModelMatrix` + the `tr.frameCount` it was written on, on `idRenderEntityLocal`. Entities persist in `idRenderWorldLocal::entityDefs`, so there is a stable place to put it. If the entity was not drawn last frame, emit a zero MV and let FSR2's disocclusion logic handle it. | Low. Watch for entities that teleport — a huge MV is worse than a zero one; clamp. |
| **Skinned MD5 meshes** (every monster, the player body) | **Not available.** Skinning runs CPU-side into the per-frame vertex cache; the previous frame's deformed positions are gone by the time we draw. | **This is the known gap.** |
| **`deform`-generated geometry** (sprites, particles, beams, flares, tubes) | Same — regenerated into the vertex cache every frame. | Same gap. |

**Decision for U2: ship camera + rigid-entity motion vectors only.** Skinned and deformed
geometry gets the camera-only MV (correct for the camera's motion, wrong for the object's
own motion) and is flagged into the transparency-and-composition mask in U4 so FSR2 reduces
its reliance on history there. Monsters will ghost slightly under fast lateral motion.
Measure how bad it actually is before deciding whether to pay for the fix.

**If it is bad enough to fix (U5, optional):** double-buffer the deformed vertex output for
skinned surfaces, keyed on `idRenderModelStatic`'s deformed-surface cache, and pass the
previous positions as a second vertex stream. That is a `VertexCache.cpp` change with a
real memory cost, and it is not justified until the ghosting is *seen*.

### Two specific hazards

- **The weapon depth hack.** `VK_BuildSurfMVP` (`vk_backend.cpp:856`) applies
  `weaponDepthHack` (`proj[14] *= 0.25`, remapped into the `[0, 0.5]` Z range) so the
  viewmodel never clips into the world. The viewmodel therefore carries a depth value that
  does not correspond to its real distance, and it fills a large, central, high-contrast
  part of the screen. Build its motion vectors from the *same* hacked matrices on both
  frames so MV and depth stay mutually consistent, and expect to mark it reactive in U4.
  If the viewmodel ghosts, this is why.
- **`r_znear` is game-owned and drops to 1.0 in cinematics** (already burned us in A12 — see
  ROADMAP's 2026-09-12 decisions). `FfxFsr2DispatchDescription::cameraNear` must be read
  per frame from `viewDef`, never from a cached startup value. Pass
  `FFX_FSR2_ENABLE_DEPTH_INFINITE` — `R_SetupProjection` uses the far-plane-at-infinity
  formulation (`tr_main.cpp:1029`). Depth is **not** reversed, so do *not* pass
  `FFX_FSR2_ENABLE_DEPTH_INVERTED`.

---

## 6. Jitter (U2)

`R_SetupProjection` already has a jitter slot (`tr_main.cpp:978-1017`) driven by `r_jitter`,
applied as a sub-pixel shift of the projection's frustum edges — structurally exactly what
FSR2 needs. But its current contents are wrong for this purpose: it uses
`idRandom::RandomFloat()` over the **full** `[0,1)` pixel, i.e. whole-pixel white noise,
where FSR2 requires a low-discrepancy Halton(2,3) sequence over `[-0.5, +0.5]` with a phase
count derived from the scale ratio.

```c
const int32_t phaseCount = ffxFsr2GetJitterPhaseCount(renderWidth, displayWidth);
ffxFsr2GetJitterOffset(&jitterX, &jitterY, frameIndex % phaseCount, phaseCount);
// jitter is in RENDER-resolution pixels; R_SetupProjection already divides by the
// viewport span, so feed it the render-resolution span.
```

Rules:

- Gate the FSR jitter behind `VK_RT_UpscaleActive() && r_fsr == 2`; leave `r_jitter`'s
  behaviour alone when it is off. Two mechanisms writing the same two floats is a bug
  waiting to happen — make the precedence explicit in one place.
- The **same jittered matrix must be used by everything downstream**, including the RT
  passes that reconstruct world position from depth (`rt_ReconstructWorldPos`). They take
  their matrices from `viewDef`, so consistency is automatic — but only as long as nobody
  caches an unjittered copy. The unjittered matrices needed for motion vectors must be
  built as a *separate, explicitly-named* pair, never by "un-applying" the jitter.
- **Ray-origin bias is unaffected.** The engine-wide `d²·ulp/znear` floor rule still
  applies unchanged; jitter moves the frustum, not the camera, so reconstruction error does
  not grow.

### Interaction with the existing denoisers

This needs its own paragraph because it is the most likely source of a disappointing U3.

The RT chain already accumulates temporally: `r_rtGITemporalAlpha 0.5`, the AO EMA, the
a-trous passes, and (on the probe path) `r_rtGIProbeHysteresis`. FSR2 accumulates too.
Stacking two accumulators multiplies their ghosting and, worse, the first one *destroys the
sub-pixel information the second one needs* — a denoiser that blurs across the jittered
samples hands FSR2 an already-resolved image with nothing left to reconstruct from.

This is survivable because of *what the signals are*: GI, AO and volumetrics are
low-frequency and are supposed to be smooth, so losing their sub-pixel detail costs little.
The signal that must carry the jitter cleanly is the **direct lighting and albedo** — which
has no denoiser in front of it. So the expected outcome is "fine", but that is an
expectation, not a guarantee. U4 owns measuring it and retuning the denoiser constants
downward if FSR2's history proves to be doing the job already.

---

## 7. Debug overlays (pillar 6 — these ship first, in their own chunk)

| `r_fsrDebug` | Shows |
|---|---|
| `1` | Outline of the render sub-rect plus a text readout of `renderExtent`, scale, quality mode, and the jitter offset in pixels. The cheapest way to catch a scissor-scaling mistake. |
| `2` | Motion vectors: `rg = mv * scale + 0.5` (green = static, red/blue = ±x, bright/dark = ±y) plus a magnitude heat ramp. **Pan the camera and confirm the whole screen shifts uniformly; strafe past a door and confirm the door differs from the wall.** A wrong sign looks identical to a correct one until you test both axes. |
| `3` | Reactive mask and transparency-and-composition mask, side by side. |
| `4` | Render at reduced resolution but *point-magnify* instead of upscaling — the honest "what did the resolution actually cost" A/B, with no reconstruction hiding it. |
| `5` | Upscaler input/output luminance histogram difference, for the pillar-2 black-level check. |

---

## 8. CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_fsr` | `0` | `0` = off (native), `1` = FSR 1 spatial (EASU+RCAS), `2` = FSR 2 temporal. Archived. |
| `r_fsrQuality` | `1` | `0` = use `r_fsrRenderScale`, `1` = Quality (1.5×), `2` = Balanced (1.7×), `3` = Performance (2.0×), `4` = Ultra Performance (3.0×). Archived. |
| `r_fsrRenderScale` | `0.67` | Explicit linear scale when `r_fsrQuality 0`. Clamped `[0.33, 1.0]`, snapped to an even pixel count. |
| `r_fsrSharpness` | `0.5` | RCAS sharpness `[0,1]`; feeds `enableSharpening`/`sharpness` on the FSR2 path. |
| `r_fsrAutoReactive` | `1` | Use `ffxFsr2ContextGenerateReactiveMask` (costs one render-res colour copy) vs. no reactive mask. |
| `r_fsrMipBias` | `1` | `0` = leave `image_lodbias` alone, `1` = add `log2(scale) - 1.0`. Forces a sampler rebuild on change. |
| `r_fsrDebug` | `0` | §7. |

Changing `r_fsr`, `r_fsrQuality` or `r_fsrRenderScale` requires a device-idle resource
rebuild — route them through the same path `VK_RT_ResizeTonemap` already uses and treat
them exactly like a window resize.

### 8.1 How render resolution is actually chosen

Worth writing down because the intuitive answer — a table of resolutions per card family
and aspect ratio — is not what anybody ships, and getting this wrong creates work that
does not need doing.

**The rule is: one linear divisor, applied to the actual output extent, per axis.**

```c
renderW = round(displayW / ratio);
renderH = round(displayH / ratio);
```

FSR, DLSS and XeSS have all converged on the same divisors, which is why the mode names
are interchangeable across them:

| Mode | Divisor | Linear | Pixels |
|---|---|---|---|
| Native AA | 1.0 | 100 % | 100 % |
| Quality | 1.5 | 66.7 % | 44.4 % |
| Balanced | 1.7 | 58.8 % | 34.6 % |
| Performance | 2.0 | 50 % | 25 % |
| Ultra Performance | 3.0 | 33.3 % | 11.1 % |

FSR2 ships this as `ffxFsr2GetRenderResolutionFromQualityMode(&w, &h, displayW, displayH,
mode)` — use it rather than reimplementing the table, so our rounding matches the
jitter phase count that `ffxFsr2GetJitterPhaseCount` derives from the same ratio.

**Aspect ratio needs no handling whatsoever.** The same divisor on both axes preserves it
by construction, so 16:9, 16:10, 4:3 and ultrawide are all the same code path. The
upscaler never sees an aspect ratio — only `renderSize` and `displaySize`. In this engine
it is doubly a non-issue: FOV comes from `renderView.fov_x/fov_y`, which game code derives
from the *display* dimensions (`idPlayer::CalcFov`, `Player.cpp:8191`), and §2's sub-rect
architecture deliberately leaves `glConfig.vidWidth/Height` alone — so the projection keeps
using display aspect without anyone having to arrange it.


**The other real answer is dynamic resolution scaling**, and it is the one that makes §2's
layout pay off. Target a frame time, measure the previous frame's GPU time, and move the
scale inside a band (typically 50–100 %) with a damped controller. `maxRenderSize` is set
once at context creation to the top of the band; per-frame `renderSize` varies. Two
practicalities that are always learned the hard way: **quantise the scale to steps** (~5 %)
and **rate-limit changes**, because a temporal upscaler's history is mildly invalidated
every time the sample grid moves, and a controller that hunts produces visible breathing.
Deferred to U5; the architecture already supports it because we allocate at display
resolution and render into a varying sub-rect.

**Rounding, for this engine specifically.** Every screen-space compute pass here is
`local_size 8×8` (`atrous_filter`, `gi_atrous`, `gi_probe_resolve`, `temporal_resolve`,
`vol_*`, `tonemap` — checked 2026-09-18), so snapping `renderExtent` to a multiple of 8
eliminates partial workgroups across the whole chain and keeps `r_rtGICheckerboard`'s
parity stable. But **snap only when actually upscaling.** Common display widths are not
all multiples of 8 (1366 is not), and snapping at ratio 1.0 would round 1366 → 1360 and
break U0's cheapest regression test — that `r_fsrRenderScale 1.0` is bit-identical to
`r_fsr 0`. Special-case the identity ratio to pass the display extent through untouched.

One cosmetic note, so nobody "fixes" it later: per-axis rounding makes the render aspect
differ from the display aspect by a fraction of a pixel (1920/1.7 = 1129.4, 1080/1.7 =
635.3 → 1129×635, an aspect of 1.7780 against 1.7778). This is what every implementation
does and the error is far below a pixel. Do **not** try to correct it by adjusting the
projection — that would make the render and display frusta disagree, which is a real bug
in exchange for an imaginary one.

---

## 9. Chunks

### U0 — resolution decoupling + bilinear resolve  🔴
No third-party code. Add `vk_upscale.h/.cpp`, `vk.renderExtent`, `upscale_blit.comp`, and
work the §4 change list. `r_fsr 0` with `r_fsrRenderScale < 1` renders the 3D scene into the
sub-rect, bilinear-resolves it into `hdrUpscaled`, copies back into `hdrScene`, and lets the
UI draw on top at full resolution. `r_fsrDebug 1` and `4` ship here.

- **Exit:** at `r_fsrRenderScale 0.67` on the Mars City test scene, `r_vkRTProfile 1` shows
  RT total drop from ~11.9 ms to **≈ 5.5 ms ± 0.5** (the measurement that justifies the
  whole arc); HUD, PDA and main menu pixel-identical to native; no validation errors;
  shadows, AO, GI, reflections and volumetrics all land in the right place at `0.5`, `0.67`
  and `1.0` — three scales, because an off-by-one in the scissor conversion can be
  invisible at one of them. Mirrors and security-camera subviews correct.
  `r_rtReflectionMode 1` still finds and traces glass.
- **This chunk is independently shippable and delivers the entire performance win.**
- **Sequencing:** the rest of this arc waits for arcs 1b and 2, but **U0 should land just
  before `20260906_froxel_probe_gi.md`'s G6**. G6 decides whether to retire per-pixel GI on
  a cost/quality trade, and U0 cuts the two candidates' costs *unevenly* — per-pixel GI is
  entirely screen-resolution work (4.41 → ~1.95 ms at 0.67), while the probe trace is
  probe-count-bound and barely moves. Deciding at full resolution and then halving the cost
  side risks deciding G6 twice. See ROADMAP's sequencing note.

### U1 — FSR 1 (EASU + RCAS)  🔴 *optional*
Vendor `ffx_a.h` + `ffx_fsr1.h` into `renderer/glsl/fsr1/`, add to `GLSL_INCLUDES`, write
`fsr_easu.comp` + `fsr_rcas.comp`. Needs a perceptual-space input: tonemap the render
sub-rect first, run EASU+RCAS into `hdrUpscaled` as already-tonemapped values, and bypass
the final tonemap (`r_rtTonemap`'s existing bypass at `vk_tonemap.cpp:559`).

- **Exit:** visibly better than U0's bilinear on static geometry at `0.67`. **If it is not —
  a real possibility given no AA and the RT noise floor — record that and drop the chunk
  rather than tuning it.** Its purpose is to be a fallback, not a product.

### U2 — motion vectors + jitter  🔴
`motionVectors` attachment, the fifth blend-attachment slot across `vk_pipeline.cpp`,
`prevModelMatrix` on `idRenderEntityLocal`, the unjittered matrix pair, Halton jitter behind
`R_SetupProjection`'s existing hook, and `motion_debug.comp`. **Nothing consumes the motion
vectors in this chunk** — that is the point. Ship the overlay and validate it alone.

- **Exit:** `r_fsrDebug 2` shows uniform screen-wide flow on a camera pan (both axes, both
  directions), a moving door differing from its frame, and a *static* screen under a
  *static* camera reading exactly zero — including the viewmodel. Jitter on with no consumer
  produces the expected 1-pixel shimmer and nothing worse; RT shadows/AO/GI show no new
  flicker (i.e. `VK_RT_DetectCameraCut` is not firing on jitter — it compares camera
  position and orientation, which jitter does not change, so this should pass by
  construction; verify that it does).

### U3 — FSR 2 integration  🔴
Vendor `neo/libs/fsr2/` at a pinned tag, add the CMake target and `src_fsr2` list,
create/destroy the context alongside `VK_RT_ResizeTonemap`, and dispatch. No reactive mask
yet; `FFX_FSR2_ENABLE_AUTO_EXPOSURE` initially (the simplest correct answer given the
tonemap is a fixed curve downstream), `FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE` +
`_DEPTH_INFINITE` on, `_DEPTH_INVERTED` off. Per frame: `renderSize`, `jitterOffset`,
`motionVectorScale`, `frameTimeDelta` in **milliseconds**, `cameraNear` from `viewDef` (not
cached), `cameraFovAngleVertical` in radians, `reset` wired to the existing camera-cut
detector.

- **Exit:** at Quality, sharper than U0's bilinear and close to native on static geometry;
  **no light bleeding into dark corridors when the camera pans from a lit room to a dark
  one** (pillar 2 — a gate, not a nice-to-have; use `r_fsrDebug 5`); RT total still ≈ 5.5 ms
  and FSR2's own dispatch ≤ 1.5 ms at 1080p; builds and runs clean on Linux/RADV as well as
  Windows.

### U4 — reactive mask, T&C mask, denoiser retune  🔴
`preAlphaColor` snapshot before `VK_RB_DrawShaderPasses`,
`ffxFsr2ContextGenerateReactiveMask`, and a transparency-and-composition mask covering
skinned geometry (whose MVs are known wrong — §5), the glass reflection overlay, particles
and the viewmodel. Then sweep `r_rtGITemporalAlpha`, the AO EMA and the a-trous iteration
counts *downward* and find out how much of our denoising FSR2 has made redundant.

- **Exit:** muzzle flashes, fire and steam do not smear; monster ghosting is acceptable or
  the gap is documented with a measurement; the denoiser retune is recorded in
  `rt_optimization_tuning.md` with before/after ms.

### U5 — polish  🔴
Mip bias (§4) with sampler rebuild; `r_fsrQuality` exposed in the video menu; the FSR 3.1
port evaluation now that the plumbing is proven; optionally skinned-MV double-buffering if
U4 said it was needed; dynamic resolution (nearly free given §2's layout).

---

## 10. Licensing

### What we take on

| Component | License | Compatibility with the Doom 3 GPL-3 release |
|---|---|---|
| `FidelityFX-FSR2` (`ffx-fsr2-api/`, including its shaders) | **MIT** (Copyright © Advanced Micro Devices, Inc.) | ✅ MIT is permissive and GPL-compatible. Combining it into a GPLv3 work is explicitly allowed; the combined work is distributed under GPLv3 while the MIT files keep their own notice. |
| `ffx_a.h`, `ffx_fsr1.h` (FSR 1) | **MIT** (AMD) | ✅ Same. |
| FidelityFX SDK / FSR 3.1 (if U5 takes the upgrade) | **MIT** (AMD) | ✅ Same — but re-check the exact tag; AMD has shipped SDK components under other terms before. |
| FSR 4 | binary distribution, **not** MIT source | ❌ Do not vendor. Already out of scope for hardware reasons. |

The Doom 3 release's "modified GPL v3" adds terms on top of GPLv3; it does not restrict
*inbound* permissive code. Pulling MIT code in is fine. Pushing our code out stays GPLv3.

### Concrete obligations

MIT requires exactly one thing — that the copyright notice and permission notice travel
with the software, in source *and* binary distributions.

1. **Do not touch AMD's file headers.** Every vendored file keeps its
   `Copyright (c) … Advanced Micro Devices, Inc.` block verbatim.
2. **`neo/libs/fsr2/LICENSE.txt`** — copy upstream's licence file unchanged.
3. **`README.md` → `# LICENSES`** — add an `## AMD FidelityFX Super Resolution` section
   listing `neo/libs/fsr2/*` and `neo/renderer/glsl/fsr1/*` with the full MIT text. The file
   already does exactly this for Dear ImGui (`README.md:166`) and two other dependencies;
   follow that shape precisely.
4. **Binary distribution.** The MIT notice must reach anyone who gets a build, not just
   anyone who clones the repo. Check whether the install rules at `CMakeLists.txt:1591+`
   ship a licence directory; if not, add one, and make sure `dist/` picks it up.
5. **`Changelog.md` / `changes.md`** — note the new dependency, its version and its licence,
   as the repo does for other additions.

### Two things MIT does *not* give us

- **Trademarks.** "AMD", "FidelityFX" and "FSR" are AMD trademarks and the MIT grant is
  copyright-only. Internal identifiers (`r_fsr`, `vk_upscale.cpp`) are ordinary descriptive
  use and are fine. **User-visible UI is where care is needed**: AMD's branding guidance
  asks for the full "AMD FidelityFX™ Super Resolution 2" on first mention. Practical rule:
  name it accurately and don't imply AMD endorsement, sponsorship or certification of this
  fork. Write the video-menu string once, in U5, and write it correctly.
- **Patents.** MIT carries no express patent grant (unlike Apache-2.0). Normal for graphics
  code and not a practical concern for a GPL hobby fork, but worth knowing that it is a
  difference rather than an oversight.

### Our own new files

Per CLAUDE.md, **our** new files (`vk_upscale.cpp`, `upscale_blit.comp`, `motion_debug.comp`,
`fsr_easu.comp`, `fsr_rcas.comp`) carry the dhewm3-rt GenAI copyright block. **Vendored AMD
files do not** — they keep AMD's header and nothing else. A file carrying both notices
misrepresents its provenance, which is the one thing the block exists to prevent.

---

## 11. Known risks

| Risk | Signal | Mitigation |
|---|---|---|
| **Scissor/viewport scaling subtly wrong**, visible only at some scale factors | Shadows, AO or reflections offset by a few pixels; glass reflections vanish | `r_fsrDebug 1` outline; test at 0.5 / 0.67 / 1.0, not one value; `r_fsrRenderScale 1.0` must be bit-identical to `r_fsr 0` — make that an assertion, it is the cheapest regression test available |
| **Pillar 2: FSR2's history lifts the black floor** | Dark corridors glow after panning off a bright light | `r_fsrDebug 5`; U3 gate. If it fails, the levers are the reactive mask and `preExposure`, in that order. This is the one risk that could veto the arc |
| **Motion-vector sign or scale wrong** | Image smears *worse* under motion than bilinear does | U2 ships the overlay before any consumer exists, precisely so this is caught in isolation. Test both axes and both directions |
| **Skinned-geometry ghosting** (known gap, §5) | Monsters trail under fast lateral motion | T&C mask in U4; double-buffered deform verts in U5 only if measured to be needed |
| **Viewmodel ghosting** from the weapon depth hack | The weapon trails or shimmers while the world is clean | Build its MVs from the hacked matrices; mark reactive |
| **Double temporal accumulation** with the RT denoisers | Ghosting worse than either alone; FSR2 reconstructs less detail than expected | U4's retune sweep; `r_rtGICheckerboard 0` under `r_fsr 2` |
| FSR2's internal resources push VRAM over at 4K | Allocation failure on a small card | **Low.** This is capacity, not speed — FSR2's per-frame traffic at 720p→1080p is only ~60-100 MB and its cost is the accumulate pass's compute, not memory. Doom 3's asset footprint is tiny by modern standards, so 250 MB competes with very little. Report the figure at context creation and warn above a threshold; a fallback to `r_fsr 1` is belt-and-braces |
| **The FSR2 Vulkan backend needs an extension we don't enable** | Context creation fails at startup | Read the backend's device-extension list at the pinned tag and add them in `vk_instance.cpp` during U3; fail *soft* to `r_fsr 0` with a warning, never hard |
| The fifth blend attachment is missed on one pipeline | Validation error, or that pipeline silently fails to create | Grep `blendAttachments` and `attachmentCount` in `vk_pipeline.cpp` + `vk_gi.cpp` and fix all of them in one commit |

---

## Hygiene

- New `.comp` → `GLSL_SHADER_SOURCES`; `ffx_a.h` / `ffx_fsr1.h` → `GLSL_INCLUDES`.
- Vendored AMD sources go in `neo/libs/fsr2/` **unmodified**; if a local change is
  unavoidable, mark it `// dhewm3-rt:` on the line so `git diff` against upstream stays
  readable.
- Our new files carry the dhewm3-rt GenAI block; AMD's do not.
- `r_fsr 0` must remain a complete bypass — the native path stays compiled and default
  until U3 passes its gate.
- Every chunk's overlay ships before anything it enables is tuned.

---

## Appendix A — vendoring FSR2 (file/build setup)

Written 2026-09-19 against upstream `FidelityFX-FSR2` tag **`v2.2.1`**, commit
`1680d1edd5c034f88ebbbb793d8b88f8842cf804`. Pin this in the vendoring commit.

### A.1 Cauldron is not a dependency

`src/ffx-fsr2-api/` references cauldron nowhere (`grep -rn "cauldron\|common.cmake"` is
empty). Cauldron is the *sample app's* framework, pulled in by the top-level
`CMakeLists.txt` for `src/VK` and `src/Common`. It was needed to build the sample, not the
library. **Do not vendor `libs/cauldron/`.**

### A.2 What actually compiles — four .cpp files

| Source | Role |
|---|---|
| `ffx_fsr2.cpp` | API core, resource scheduling, pass ordering |
| `ffx_assert.cpp` | assert reporting (`#ifdef _WIN32` guarded; Linux-clean) |
| `vk/ffx_fsr2_vk.cpp` | Vulkan backend |
| `vk/shaders/ffx_fsr2_shaders_vk.cpp` | blob lookup table; `#include`s the 8 permutation headers |

AMD's CMake builds these as two static libs (`ffx_fsr2_api_x64` + `ffx_fsr2_api_vk_x64`).
**We don't need libs.** Compile all four into `${DHEWM3BINARY}` via an `src_fsr2` list,
exactly as `src_imgui` does (`neo/CMakeLists.txt:1044`). One target, no export sets, no
MSVC-only `/MP` `/Z7` flags, no `FATAL_ERROR` on non-x64 platforms.

Consequences: **delete `neo/libs/ffx_fsr2_api_vk_x64{,d}.lib`** (MSVC-only, config-specific,
and the core `ffx_fsr2_api_x64.lib` half was never copied anyway), and **do not use
`neo/libs/ffx-fsr2-api/CMakeLists.txt`, `vk/CMakeLists.txt` or `dx12/CMakeLists.txt`** —
keep them on disk for provenance, never `add_subdirectory` them.

### A.3 The blocker: precompiled SPIR-V permutation headers

`ffx_fsr2_shaders_vk.cpp` includes `ffx_fsr2_*_pass_permutations.h`, which are **generated
at build time**, not shipped in the repo. The generator is
`tools/sc/FidelityFX_SC.exe` — a PE32+ Windows binary with **no source in the 2.2.1
release**. It cannot run in a Linux CI or a clean Linux checkout.

**Decision: generate once on Windows, vendor the output, and never run the generator from
our build.** Copy the whole generated directory in and add it to the include path.

| Item | Value |
|---|---|
| Generated at | `<fsr2>/build/VK/src/ffx-fsr2-api/shaders/vk/` |
| Blob headers | 109 (`accumulate` 48, `reconstruct_previous_depth` 32, `depth_clip` 16, `lock` 4, `tcr_autogen` 4, `autogen_reactive` 2, `rcas` 2, `luminance_pyramid` 1) |
| Index headers | 8 `*_permutations.h`, flat `#include` by name |
| Total | **~12 MB** of C hex arrays |
| Vendor to | `neo/libs/ffx-fsr2-api/vk/shaders/permutations/` |
| Do **not** copy | the 8 `*.h.d` depfiles |

Regeneration command, for the record (run on Windows, from the upstream clone, when the
pinned tag moves) — this is what `vk/CMakeLists.txt` runs per pass:

```
tools/sc/FidelityFX_SC.exe -reflection -deps=gcc -DFFX_GPU=1 \
  -DFFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 \
  -DFFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0 \
  -DFFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 \
  -DFFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0 \
  -DFFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 \
  -compiler=glslang -e main --target-env vulkan1.1 -S comp -Os -DFFX_GLSL=1 \
  -DFFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1} -DFFX_FSR2_OPTION_HDR_COLOR_INPUT={0,1} \
  -DFFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1} -DFFX_FSR2_OPTION_JITTERED_MOTION_VECTORS={0,1} \
  -DFFX_FSR2_OPTION_INVERTED_DEPTH={0,1} -DFFX_FSR2_OPTION_APPLY_SHARPENING={0,1} \
  -DFFX_HALF={0,1}   # -DFFX_HALF=0 only, for ffx_fsr2_compute_luminance_pyramid_pass
  -name=<pass_name> -I src/ffx-fsr2-api/vk/shaders -output=<outdir> <pass>.glsl
```

Trimming the permutation set to the six options we actually use would cut 109 → ~14
headers, but the runtime selects blobs by option hash from the generated tables, so a
missing permutation is a dispatch-time failure rather than a link error. Not worth it;
revisit in U5 if repo size matters.

### A.4 Drop `dx12/`

`dx12/` is 264 KB of HLSL, `ffx_fsr2_dx12.cpp` and Microsoft's `d3dx12.h` — which carries
its **own** MIT notice (`dx12/license.txt`, Copyright © 2015 Microsoft), a second
third-party licensing obligation for code we will never compile. Delete
`neo/libs/ffx-fsr2-api/dx12/` and the `*.hlsl` files under `shaders/`. Record the deletion
in the vendoring commit message so the tree is still diffable against upstream.

`shaders/*.h` and `shaders/*.glsl` stay: they are the GLSL sources the vendored blobs were
compiled from, and keeping them is what makes A.3's regeneration auditable.

### A.5 Final tree

```
neo/libs/ffx-fsr2-api/
  LICENSE.txt                       ← upstream root LICENSE.txt, verbatim (already present)
  ffx_fsr2.cpp/.h  ffx_assert.cpp/.h  ffx_error.h  ffx_fsr2_interface.h
  ffx_fsr2_maximum_bias.h  ffx_fsr2_private.h  ffx_types.h  ffx_util.h
  CMakeLists.txt                    ← kept for provenance, NOT add_subdirectory'd
  shaders/*.h *.glsl                ← GLSL sources (drop the .hlsl)
  vk/ffx_fsr2_vk.cpp/.h
  vk/shaders/ffx_fsr2_shaders_vk.cpp/.h
  vk/shaders/permutations/*.h       ← 109 blobs + 8 indices, vendored per A.3
```

Current state (2026-09-19): the tree matches upstream `src/ffx-fsr2-api` exactly, plus
`LICENSE.txt`. Outstanding: add `vk/shaders/permutations/`, remove `dx12/` and the `.hlsl`
files, remove the two stray `.lib` files from `neo/libs/`.

### A.6 CMake wiring

```cmake
set(src_fsr2
    libs/ffx-fsr2-api/ffx_fsr2.cpp
    libs/ffx-fsr2-api/ffx_assert.cpp
    libs/ffx-fsr2-api/vk/ffx_fsr2_vk.cpp
    libs/ffx-fsr2-api/vk/shaders/ffx_fsr2_shaders_vk.cpp)

target_include_directories(${DHEWM3BINARY} PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/ffx-fsr2-api"
    "${CMAKE_SOURCE_DIR}/libs/ffx-fsr2-api/vk/shaders/permutations")
```

Gate both behind an `option(DHEWM3_FSR2 "..." ON)` so `r_fsr 2` can be compiled out.

| Path | Why |
|---|---|
| `libs/ffx-fsr2-api` | `vk_upscale.cpp` includes `ffx_fsr2.h` / `vk/ffx_fsr2_vk.h` |
| `.../vk/shaders/permutations` | **Mandatory.** `ffx_fsr2_shaders_vk.cpp` includes the 8 indices by *bare name* (`#include "ffx_fsr2_rcas_pass_permutations.h"`), not by subdirectory. Quoted-include fallback searches the including file's own dir — `vk/shaders/` — which does not contain them. Without this `-I`, `ffx_fsr2_shaders_vk.cpp` fails to compile. We moved them into `permutations/`; upstream's build put them on the include path from the build tree. |
| Vulkan headers | already handled — the `_vk_inc` helper at `neo/CMakeLists.txt:657-671` applies to `${DHEWM3BINARY}` |

Not needed: `_UNICODE` / `UNICODE` (AMD's CMake sets them, but nothing in the four sources
selects a W-suffixed Win32 API — `ffx_assert.cpp` uses `char*` throughout).

### A.7 Portability notes to expect at first compile

| Item | Effect |
|---|---|
| `#include <codecvt>` in `ffx_fsr2_vk.cpp:29` | Deprecated since C++17, removed in C++26. Compiles with a warning on GCC/Clang today. Suppress locally; do not patch AMD's file. |
| `wchar_t name[64]` in `ffx_types.h` | 2 bytes on Windows, 4 on Linux. Internal to FSR2's debug naming — sizes differ, behaviour doesn't. |
| `VkPhysicalDeviceVulkan11Properties`, `VkPhysicalDeviceSubgroupSizeControlProperties` | Core-promoted names; needs Vulkan headers ≥ 1.3. Already satisfied by our SDK requirement. |
| Device extensions | All **optional probes** (`VK_EXT_subgroup_size_control`, `VK_KHR_shader_float16_int8`, `VK_KHR_acceleration_structure`) — the backend queries and degrades. §11's "needs an extension we don't enable" risk is lower than written, but still verify at U3. |
| `-Wall` noise from AMD sources | Expect unused-variable/sign-compare warnings. Mark the include dirs `SYSTEM` if it gets loud; do not edit the sources. |

### A.8 Licensing checklist (concrete, supersedes §10's item list)

| Action | Target |
|---|---|
| Keep verbatim | every AMD file header; `neo/libs/ffx-fsr2-api/LICENSE.txt` (already correct — byte-identical to upstream root) |
| Notice location | ✅ Done. The README's 263-line inline licence dump moved to root `THIRD-PARTY-LICENSES.md` (index table + EXCLUDED CODE notice + every component's text verbatim). `README.md` keeps `# LICENSES` with the `COPYING.txt` pointer and a pointer to the new file; 420 → 167 lines. AMD's section names the pinned commit and defers to `neo/libs/ffx-fsr2-api/LICENSE.txt` as authoritative. |
| Avoids an obligation | ✅ dropping `dx12/` removes the Microsoft `d3dx12.h` MIT notice from our tree entirely — no second entry needed |
| FSR 1 headers (U1 only) | `ffx_a.h` / `ffx_fsr1.h` → `neo/renderer/glsl/fsr1/`, same MIT. Add the path to AMD's existing entry in `THIRD-PARTY-LICENSES.md`; the MIT text is already there. |
| Binary dist | ✅ Done. There was **no** licence install rule at all — `neo/CMakeLists.txt` installed only shaders and targets, so binaries shipped without even the GPL. Added an `install(FILES COPYING.txt THIRD-PARTY-LICENSES.md)` to `${bindir}`, guarded `NOT APPLE AND NOT WIN32` to match the sibling target install. **Windows and macOS packaging still ship no notices** — those paths don't use `install()`; whatever produces their archives has to copy both files. |
| Changelog | note "AMD FidelityFX Super Resolution 2.2.1 (MIT)" with the pinned commit |
