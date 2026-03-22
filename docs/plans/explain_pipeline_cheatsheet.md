# Rendering Pipeline Cheat Sheet (One Page)

Purpose: fast context reset for stencil + RT shadow pipelines in dhewm3_rtx.

---

## A) Core Equations

Forward projection:

x_clip = P V M x_model

x_ndc = x_clip / w_clip

RT unprojection from depth:

c = [2u-1, 1-2v, 2d-1, 1]^T

x_world = inv(PV) c

Per-light visibility:

V_l(x) in {0,1}

Soft shadow estimate:

\hat{V}_l(x) = (1/N) sum_i 1[ray_i reaches light]

Lighting modulation (conceptual):

L_direct = sum_l \hat{V}_l(x) * BRDF * light_term

---

## B) Pipeline Flow At A Glance

```text
Depth prepass
  -> (if RT) end RP, clear shadow mask, rebuild TLAS, resume RP
  -> for each light:
       (if RT) end RP -> dispatch shadow rays -> resume RP
       clear stencil region
       (if non-RT) draw shadow volumes
       draw interaction pass (samples shadow mask when RT path active)
  -> fog / shader passes / GUI
  -> submit + present
```

---

## C) Stencil vs RT (Same Goal, Different Solver)

```text
Stencil path:
  shadow volume rasterization + stencil ops -> visibility classification

RT path:
  depth unprojection + segment ray query in TLAS/BLAS -> visibility classification
```

Both compute visibility to each light, V_l(x).

---

## D) Minimum Symbol Dictionary

- M: model-to-world transform
- V: world-to-view transform
- P: projection transform
- inv(PV): clip-to-world unprojection operator
- u,v,d: screen UV + depth sample
- x: reconstructed world point
- l: light position
- r: normalized ray direction to light
- epsilon: bias for self-hit avoidance
- BLAS/TLAS: per-object / scene acceleration structures

---

## E) Jargon Crosswalk

- BRDF: local scattering kernel
- shadow mask: sampled visibility field
- any-hit shader: first-occluder event handler
- denoising: variance reduction + regularization
- reprojection: transport prior estimate through motion map
- barrier: explicit memory visibility edge in execution DAG

---

## F) Typical Failure Signatures

1. Wrong Y/Z convention in unprojection:
- mirror-like/globally wrong shadows

2. Bad depth sampling view:
- blocky/noisy shadow masks, possible device loss later

3. Incomplete BLAS coverage:
- light leaks / missing occluders

4. Bias epsilon poorly tuned:
- acne (too small) or peter-panning (too large)

---

## G) Where To Read In Code

1. neo/renderer/Vulkan/vk_backend.cpp
2. neo/renderer/Vulkan/vk_shadows.cpp
3. neo/renderer/glsl/shadow_ray.rgen
4. neo/renderer/Vulkan/vk_accelstruct.cpp

---

## H) If You Have 60 Seconds

- Both stencil and RT compute per-light visibility V_l(x).
- Stencil does it with volume raster + stencil counters.
- RT does it with depth-unprojected rays through TLAS/BLAS.
- Most hard bugs are coordinate convention, memory ordering, or missing geometry coverage.
