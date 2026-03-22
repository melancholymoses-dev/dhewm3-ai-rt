# Pipeline Math Companion (For Optics + Data Science Background)

This is a companion to explain_pipeline.md.

Companion quick reference:
- `docs/plans/explain_pipeline_cheatsheet.md` (one-page symbols + flow + debugging map)
- `docs/plans/explain_pipeline_stencil_symbols.md` (stencil-only symbol-to-code mapping)

Goal:
1. Explain the math used by the current stencil and RT shadow paths.
2. Translate graphics jargon into optics / inverse-problem / probabilistic language.
3. Keep implementation hooks visible so code reading is easier.

---

## 1) Coordinate Systems And Transforms

In this engine, the relevant maps are:

```text
model space --M--> world space --V--> camera/view space --P--> clip space --/w--> NDC --viewport--> framebuffer
```

Using homogeneous coordinates:

x_clip = P * V * M * x_model

and

x_ndc = x_clip / w_clip

For depth-based world reconstruction (in RT raygen), we do the inverse map:

x_world = (P*V)^(-1) * x_clip

with x_clip recovered from pixel coordinate and sampled depth.

---

## 2) Why Y and Z Remapping Exists (GL vs Vulkan conventions)

Two convention mismatches matter:

1. Framebuffer Y direction.
2. NDC depth convention.

Current raygen uses:

x_ndc = 2u - 1

y_ndc = 1 - 2v

z_ndc_gl = 2d_vk - 1

So the clip vector is:

c = [2u-1, 1-2v, 2d-1, 1]^T

Then world point:

x_world = invViewProj * c

Interpretation:
- u,v are pixel coordinates normalized to [0,1].
- d is depth texture sample in [0,1].
- invViewProj is inverse of GL-style projection*view matrix.

This is exactly the kind of "change of coordinates + inverse operator" problem you would see in tomography/optics calibration pipelines.

---

## 3) Shadowing As A Visibility Functional

For each light l and surface point x:

V_l(x) = 1 if segment (x, light_l) is unoccluded
         0 otherwise

Then per-light shading is multiplied by V_l.

In stochastic soft-shadow form, with N jittered shadow rays:

V_l_hat(x) = (1/N) * sum_i 1[ray_i reaches light]

This is a Monte Carlo estimator of geometric visibility over an area-light cone.

### Jargon bridge
- graphics: shadow mask
- optics/probability: estimated transmittance / visibility estimator

---

## 4) Stencil Shadow Volume Math (Topological Counting View)

Stencil shadows are not ray queries; they are parity/counter logic over rasterized shadow volumes.

Think of it as counting oriented crossings of a volume boundary along the eye-to-surface depth path.

### Z-fail intuition
Instead of counting only in front of depth, Z-fail uses depth-test failures against shadow-volume faces to detect when the sightline is inside the shadow volume at the shaded depth.

Equivalent conceptual output is still a binary visibility classification per light.

### Jargon bridge
- graphics: Carmack's Reverse / Z-fail
- math viewpoint: winding/crossing-number style inside-outside test under depth constraints

---

## 5) RT Shadow Pipeline Math (Current)

For each pixel p and light l:

1. Reconstruct x_world(p) from depth.
2. Define ray direction to light:

   d = (l - x)/||l-x||

3. Trace segment [x + epsilon*d, l - epsilon*d].
4. Any hit => occluded, miss => visible.

Soft shadows jitter d within a cone determined by light radius / distance.

### In code terms
- raygen computes x_world and launches rays
- any-hit sets payload to shadowed on first intersection
- miss sets payload lit

### Jargon bridge
- graphics: any-hit shader
- probability/inference: stopping rule for first event in line-of-sight process

---

## 6) tMin Bias As Regularization

Self-intersection prevention uses a small start offset epsilon (often called tMin bias):

origin = x + epsilon * d

This acts like a geometric regularizer against finite precision and mesh/discretization artifacts.

Too small:
- self-shadow acne

Too large:
- peter-panning/light leaks near contact points

### Jargon bridge
- graphics: shadow bias
- numerical analysis: stability vs bias trade-off in discretized geometry queries

---

## 7) BLAS/TLAS As Hierarchical Acceleration Operators

You can view AS traversal as evaluating a sparse hierarchical culling operator before exact triangle intersection tests.

- BLAS: per-object geometry hierarchy
- TLAS: scene-level hierarchy over BLAS instances

Complexity idea:
- naive segment-triangle checks are O(num triangles)
- BVH traversal gives sublinear expected traversal for typical scenes

### Jargon bridge
- graphics: BVH traversal
- data science: hierarchical indexing / branch-and-bound candidate pruning

---

## 8) Render Pass / Barrier Jargon Through A Dataflow Lens

In Vulkan, you explicitly define when producer writes become visible to consumer reads.

Think of pipeline stages as nodes in a DAG and barriers as dependency edges.

Example in this code path:

```text
depth attachment writes  -->  RT shader depth reads  -->  fragment shadowMask reads
```

If dependencies are wrong, you get undefined behavior (stale data, corruption, or device loss).

### Jargon bridge
- graphics: pipeline barrier
- systems/dataflow: memory visibility fence + stage ordering constraint

---

## 9) Useful Symbol Table (Math -> Code)

```text
P            projection matrix                viewDef->projectionMatrix (or s_projVk variant)
V            view matrix                      viewDef->worldSpace.modelViewMatrix
inv(PV)      unprojection operator            ubo.invViewProj in shadow params
x_world      reconstructed world point        reconstructWorldPos() output
l            light origin                     renderLight_t origin
V_l(x)       per-light visibility             shadowMask sample in interaction.frag
epsilon      ray start/end bias               tMin usage in raygen
```

## 9.1 Paper-Style Notation Glossary

| Symbol | Dimensions | Meaning | Engine mapping |
|---|---:|---|---|
| x_m | 4x1 | Model-space homogeneous point | Vertex in mesh local coordinates |
| M | 4x4 | Model to world transform | entity model matrix |
| V | 4x4 | World to camera/view transform | worldSpace.modelViewMatrix |
| P | 4x4 | Camera projection transform | projectionMatrix (or Vulkan-remapped variant) |
| x_c | 4x1 | Clip-space homogeneous point, x_c = PVMx_m | Intermediate before divide |
| x_n | 3x1 | Normalized device coordinates | x_n = x_c.xyz / x_c.w |
| u,v | scalar | Pixel UV in [0,1] | Derived from gl_LaunchIDEXT / framebuffer size |
| d | scalar | Depth sample in [0,1] | texelFetch(depthSampler, coord).r |
| c | 4x1 | Reconstructed clip vector [2u-1, 1-2v, 2d-1, 1]^T | Input to unprojection |
| inv(PV) | 4x4 | Unprojection operator from clip to world | ubo.invViewProj |
| x | 3x1 | Reconstructed world point | reconstructWorldPos output |
| l | 3x1 | Light position in world space | renderLight_t origin |
| r | 3x1 | Unit ray direction to light, r=(l-x)/||l-x|| | rayDir in raygen |
| epsilon | scalar | Ray start/end bias to avoid self-hit | tMin-style bias |
| V_l(x) | scalar | Binary visibility to light l at point x | Shadow mask sample |
| \hat{V}_l(x) | scalar | Monte Carlo estimate of visibility (soft shadows) | Averaged ray outcomes |
| N | integer | Number of shadow samples | r_rtShadowSamples |

---

## 10) ASCII: Unprojection Geometry

```text
screen pixel p(u,v,d)
      |
      v
clip c = [2u-1, 1-2v, 2d-1, 1]
      |
      v
x_world_h = inv(PV) * c
      |
      v
x_world = x_world_h.xyz / x_world_h.w
      |
      +----> shoot shadow ray toward light
```

---

## 11) ASCII: Stencil vs RT As Two Visibility Solvers

```text
Method A (Stencil):
  build shadow volume geometry
  raster + stencil ops
  classify lit/shadowed by stencil test

Method B (RT):
  reconstruct x
  trace segment x->light in TLAS/BLAS
  classify lit/shadowed by hit/miss
```

Both target the same quantity V_l(x), but with very different numerical machinery.

---

## 12) Common Failure Modes Mapped To Math Intuition

1. Wrong Y/Z convention in unprojection:
- Symptom: mirrored or globally wrong shadowing
- Math: inverse map applied to coordinates from wrong chart

2. Incorrect or non-conformant depth sampling view:
- Symptom: blocky/chaotic masks, possible GPU fault later
- Math: corrupted measurement input d to inverse operator

3. Missing geometry in BLAS:
- Symptom: light leaks / missing occluders
- Math: forward visibility operator evaluated on incomplete domain

4. Bias mis-tuned:
- Symptom: acne or detached shadows
- Math: regularization parameter not in stable regime

---

## 13) Jargon Crosswalk (Graphics <-> Optics/Data Science)

- BRDF: bidirectional reflectance model (material response kernel)
- NdotL / NdotH: cosine factors in local scattering model
- G-buffer/depth prepass: cached sufficient statistics for later shading stages
- Shadow map/mask: sampled visibility field
- Denoising: variance reduction + temporal/spatial regularization
- Temporal accumulation: recursive estimator / exponential smoother
- Reprojection: transporting previous estimate through motion transform

---

## 14) Reading Path If You Want The Math First Then Code

1. Read this file once.
2. Read explain_pipeline.md for flow and architecture.
3. Open shadow_ray.rgen and map each equation to code.
4. Open vk_shadows.cpp and map dataflow/barriers.
5. Open vk_accelstruct.cpp and map hierarchical visibility acceleration.

That sequence usually minimizes cognitive load when coming from quantitative fields.

---

## 15) One-Page Shortcut

If you want a compact summary for quick context switches, use:

- `docs/plans/explain_pipeline_cheatsheet.md`
- `docs/plans/explain_pipeline_stencil_symbols.md`
