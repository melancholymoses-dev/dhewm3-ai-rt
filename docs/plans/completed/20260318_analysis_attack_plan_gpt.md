# Vulkan Artifact Attack Plan (GPT)

As of 2026-03-18. Targets: shots 5, 6, 8 (initial area) and shot 9 (white window + depth inversion).

---

## Objective

Find and isolate root causes for:

1. Screen-space line artifacts and black triangles along geometry edges.
2. White rendering for surfaces that should be transparent (window).
3. Distant geometry appearing through foreground geometry.

This is an analysis-first plan only. No production fixes are included.

---

## Strategy Overview

Use a two-track approach:

1. **Pass ownership isolation** to identify *which pass* writes the bad pixels.
2. **State parity audit** against vkDOOM3 to identify *which state mismatch* causes the behavior.

Then validate top hypotheses with minimal, high-signal instrumentation.

---

## Phase 1: Pass Ownership Isolation (Fast Binary Checks)

Run captures at fixed camera positions for shots 5/6/8/9 with these variants:

1. Baseline
2. Skip shader passes
3. Skip stencil shadows
4. Skip fog/blend lights

Record artifact presence per variant:

- Screen-space lines
- Black edge triangles
- White transparent surface
- Foreground/background depth inversion

Interpretation:

- If white window and depth inversion disappear when shader passes are skipped, ownership is shader-pass path.
- If hangar artifacts disappear when shadows are skipped, ownership is shadow-stencil path.
- If artifacts persist across all toggles, suspect vertex/index input, depth prepass, or scissor conversion.

---

## Phase 2: Ordering and State Parity Audit (dhewm3_rtx vs vkDOOM3)

### 2.1 Interaction ordering parity

Verify per-light order in dhewm3_rtx matches canonical vkDOOM3 behavior:

1. globalShadows
2. localInteractions
3. localShadows
4. globalInteractions
5. translucentInteractions

Audit points:

- Ensure no extra draws are interleaved between these buckets.
- Ensure pipeline rebinds after shadow draws are correct.

### 2.2 Interaction depth/stencil policy

Check opaque interaction policy (prepass + interaction) and translucent policy separately:

- Opaque interactions should match GL/vkDOOM3 intent for depth compare against prepass results.
- Translucent interactions should bypass stencil as intended but still have sane depth semantics.

### 2.3 Per-light stencil clear semantics

Confirm all of the following:

1. Clear value is 128 before each light.
2. Clear rect uses the same intended light scissor domain as draws.
3. OpenGL-style Y-up scissor inputs are correctly converted to Vulkan framebuffer Y-down coordinates.
4. Inclusive/exclusive rect math is consistent (+1 width/height usage audited).

### 2.4 Fog/blend-light parity

Compare fog/blend dispatch and state with vkDOOM3 FogAllLights behavior:

- Depth compare op
- Cull mode
- Blend factors
- Surface chain selection (global/local interactions used by fog/blend passes)

---

## Phase 3: High-Risk Pipeline/Shader Integrity Checks

### 3.1 Depth prepass shader pairing

Validate depth prepass pipeline is intentionally using the correct shader pair and descriptor/UBO contract:

- Opaque prepass fragment path
- Alpha-clip prepass fragment path
- Vertex-fragment interface compatibility
- UBO layout compatibility with chosen shader pair

### 3.2 Shader-pass pipeline keying

Audit dynamic pipeline cache key for shader-pass path:

- Confirm it keys not only on blend factors but also on depth-function-relevant stage bits where required.
- Confirm selected pipeline depth enable/compare matches stage drawStateBits intent.

### 3.3 Vertex/index input correctness

Validate across depth/interactions/shader-pass paths:

1. Vertex stride equals sizeof(idDrawVert)
2. Attribute offsets match idDrawVert layout
3. Attribute formats are appropriate for that layout
4. Index type matches glIndex_t (uint16/uint32)
5. Primitive topology is triangle list where expected

### 3.4 Screen-space edge artifact checks

Audit and instrument:

- Scissor Y conversion and extent math
- Viewport/scissor interaction under negative viewport height
- Any per-pass rect clamping that could leave fixed screen-space seams

---

## Phase 4: White Window + Foreground Bleed Deep Dive

Trace one known failing window material stage through runtime:

1. Material stage drawStateBits
2. Selected pipeline variant
3. Bound texture descriptor image view/sampler
4. Whether fallback descriptor path was used
5. Effective depth/stencil/blend state at draw

Use one failing pixel in RenderDoc to attribute ownership and state:

- Which pass wrote final color
- Whether a later pass overrode it due to disabled depth
- Whether sampled texture was valid or fallback

---

## Phase 5: Decision and Fix Sequencing (After Analysis)

Only after phases 1-4 produce evidence, sequence fixes one at a time:

1. Highest confidence + broadest symptom coverage first
2. Re-capture same camera positions after each single change
3. Avoid batching first-round fixes

Expected likely order (subject to evidence):

1. Shader-pass pipeline state selection depth behavior
2. Depth prepass shader/pipeline contract issues
3. Scissor conversion/extent edge handling
4. Remaining interaction/fog parity mismatches

---

## Instrumentation Checklist

Collect these for each failing scene:

1. Draw counts per pass (depth, interactions, shader passes, fog/blend)
2. Per-light stencil clear rect and value
3. Selected pipeline identifiers + drawStateBits for failing materials
4. Fallback texture-descriptor usage logs for window stages
5. Shadow path mode and index counts for representative shadow surfaces

---

## Verification Matrix

For each shot (5/6/8/9), record:

1. Which artifacts are present
2. Which pass last wrote the failing pixel
3. Whether depth test was enabled and compare op used
4. Whether stencil test/reference/mask were active
5. Whether blend factors matched stage intent

A hypothesis is accepted only if it predicts all observed deltas under pass toggles.

---

## File Focus Map

- docs/plans/vk_comparison2.md
- neo/renderer/Vulkan/vk_backend.cpp
- neo/renderer/Vulkan/vk_pipeline.cpp
- neo/renderer/Vulkan/vk_swapchain.cpp
- neo/renderer/glsl/gui.vert
- neo/renderer/glsl/gui.frag
- neo/renderer/glsl/depth.vert
- neo/renderer/glsl/depth.frag
- neo/renderer/glsl/depth_clip.frag
- ../vkDOOM3/neo/renderer/RenderBackend.cpp
- ../vkDOOM3/neo/renderer/Vulkan/RenderProgs_VK.cpp

---

## Notes

- Prefer RenderDoc pixel-history first for fast disambiguation of screen-space artifacts.
- Keep vkDOOM3 comparison behavior-focused, not architecture-copy-focused.
- Keep this plan analysis-only until evidence points to a specific minimal fix set.
