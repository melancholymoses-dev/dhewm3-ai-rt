# GI Enhancements — Emissive Surface Contributions and Volumetric Lighting

**Date:** 2026-04-21
**Branch:** tweak_gi

---

## Overview

Two additions to the GI and lighting system:

- **Phase 7.1 — Emissive GI:** Surfaces with self-illuminated (SL_AMBIENT) material stages
  contribute their own colour as direct radiance when hit by a GI bounce ray, instead
  of relying solely on a light in the uploaded GI light list.

- **Phase 7.2 — Volumetric Lighting:** A half-resolution compute pass ray-marches along
  each view ray and fires shadow ray queries toward scene lights at each step,
  accumulating in-scattering for visible light shafts.

---

## Phase 7.1 — Emissive Surface GI

### Background

Doom 3 material stages with `lighting == SL_AMBIENT` are unlit/self-illuminated — computer
screens, glowing panels, fires, indicator lights.  Currently the GI rchit shader sees only
the diffuse albedo of a hit surface and then evaluates the uploaded light list.  If a GI
ray hits a glowing screen, it returns black (no light within lightRadius) or the albedo
multiplied by distant lights that may not reach it — neither of which captures the actual
emitted light.

With emissive GI, a ray that hits an emissive surface returns the emissive colour directly
as bounce radiance, bypassing the light loop.  This is one-line physics: emissive surfaces
ARE lights.

### Material System Changes

**`neo/renderer/Vulkan/vk_raytracing.h`** — add to `VkMaterialEntry`:

```cpp
uint32_t emissiveTexIndex; // replaces pad1; 0 = no emissive (white fallback = black for emissive)
// flags gains MAT_FLAG_EMISSIVE 0x10
```

The struct is currently 32 bytes (8 × uint32).  `pad1` becomes `emissiveTexIndex`, size
unchanged, no layout break downstream.

**`neo/renderer/Vulkan/vk_raytracing.cpp`** — in the material table population loop, scan
each material's parse stages for any stage with `lighting == SL_AMBIENT` and a valid
`texture.image`.  If found, register that image in the bindless `matTextures` array
(same path as diffuse) and store its index in `emissiveTexIndex`.  Set `MAT_FLAG_EMISSIVE`.

Doom 3 `idMaterial` provides:
```cpp
int GetNumStages() const;
const shaderStage_t *GetStage(int index) const;
// stage->lighting == SL_AMBIENT means self-illuminated
// stage->texture.image gives the image pointer
```

### Shader Changes

**`neo/renderer/glsl/rt_material.glsl`**

- Add `emissiveTexIndex` to `MaterialEntry`.
- Add `MAT_FLAG_EMISSIVE 0x10u`.
- Add helper:
  ```glsl
  vec3 rt_SampleEmissive(uint matIdx, int primId, vec2 bary)
  {
      if ((materials[matIdx].flags & MAT_FLAG_EMISSIVE) == 0u)
          return vec3(0.0);
      vec2 uv = rt_InterpolateUV(matIdx, primId, bary);
      uint texIdx = materials[matIdx].emissiveTexIndex;
      if (texIdx >= 4096u) texIdx = 0u;
      return texture(matTextures[nonuniformEXT(texIdx)], uv).rgb;
  }
  ```

**`neo/renderer/glsl/gi_ray.rchit`**

In `main()`, after sampling `albedo`, before the light loop:
```glsl
// Emissive surfaces contribute directly without a light bounce.
vec3 emissive = rt_SampleEmissive(matIdx, gl_PrimitiveID, baryCoord);
if (dot(emissive, emissive) > 0.001)
{
    giPayload.colour = emissive * giLightBuf.bounceScale;
    return;
}
```

The early return means emissive surfaces skip shadow rays entirely — correct and cheaper.
`bounceScale` is reused as the emissive strength multiplier (already tuned by the user);
a separate `r_rtGIEmissiveScale` CVar can be added later if needed.

**`neo/renderer/glsl/reflect_ray.rchit`** — same treatment: if a reflection ray hits an
emissive surface, return the emissive colour instead of the shadowed diffuse.  This makes
screens visible in reflections and mirrors.

### CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtGIEmissiveScale` | `1.0` | Multiplier on emissive bounce contribution |

### Key Files

| File | Change |
|------|--------|
| `neo/renderer/Vulkan/vk_raytracing.h` | Add `emissiveTexIndex`, `MAT_FLAG_EMISSIVE` |
| `neo/renderer/Vulkan/vk_raytracing.cpp` | Populate emissive slot in material loop |
| `neo/renderer/glsl/rt_material.glsl` | Struct field + `rt_SampleEmissive()` |
| `neo/renderer/glsl/gi_ray.rchit` | Emissive early-return before light loop |
| `neo/renderer/glsl/reflect_ray.rchit` | Same emissive path |
| `neo/CMakeLists.txt` | No new files needed |

### Potential Issues

- **Multiple SL_AMBIENT stages**: Some materials have more than one ambient stage
  (e.g., a base glow + an animated overlay).  For now, use the first valid SL_AMBIENT
  stage found.  The brightest-stage strategy can be added if results look wrong.
- **Colour scale**: Doom 3 emissive textures are not HDR; raw texel values in [0,1].
  `r_rtGIEmissiveScale` > 1 needed to make them visually impactful as bounce sources.
- **Material table size**: The struct stays 32 bytes (pad1 → emissiveTexIndex), so no
  allocation or alignment changes required.

---

## Phase 7.2 — Volumetric Lighting

### Background

Doom 3's dark corridors, flashlight beams, and fog-filled rooms are where volumetrics have
the biggest visual impact.  The goal is visible light shafts — the player's flashlight
cutting through dusty air, light bleeding through gaps in doors, coloured fog in hell
sections.

Doom 3 already renders fog volumes via rasterization (fogLight materials).  This phase adds
a ray-marched in-scattering pass on top of the existing pipeline, independent of the fog
system.  Fog lights are excluded from the volumetric light list to avoid double-counting.

### Architecture

A half-resolution **compute shader** (`vol_march.comp`) steps along each view ray from the
camera to the depth-reconstructed hit point (or a max distance), firing **ray queries**
(`GL_EXT_ray_query`) toward each in-range light at each step.  This avoids a full RT
pipeline dispatch and keeps the volumetric pass self-contained.

Output is a half-res RGBA16F `volBuffer`, upsampled to full resolution with a separable
bilateral filter (preserving depth edges to avoid halo bleed), then added to the frame
additively in the final composite pass.

```
depth buffer ──►  vol_march.comp  ──►  volBuffer (half-res)
                  ↑                        │
             GI light SSBO                 ▼
                                    bilateral upsample
                                          │
                                          ▼
                                  additive composite
                                  (post_composite.frag or
                                   added to interaction.frag final gather)
```

### In-Scattering Model

Single-scattering with a Henyey-Greenstein phase function:

```
scatter(step) = density × phase(cosTheta, g) × lightColor × NdotL × shadow × atten
```

- `density` — global fog density CVar, scales all scattering
- `g` — anisotropy parameter (0 = isotropic, 0.5–0.8 = forward-scattering flashlight)
- `shadow` — 0 or 1 from ray query (single hit test, no filtering)
- `atten` — same Cauchy-with-ramp as GI bounce (matches light list already uploaded)

The phase function is cheap (a few multiplies) and produces the teardrop shape that makes
flashlight beams visible.

### Compute Shader — `vol_march.comp`

**Dispatch:** one thread per half-res pixel.  `localSize` (8×8) thread groups.

**Per-thread:**
```
1. Early-out: if depth >= 1.0 (sky pixel), write vec3(0) and return — no march needed.
2. Reconstruct world-space view ray from depth buffer (same rt_indirect.glsl helpers).
3. Compute tMax = min(depth-based hit distance, r_rtVolMaxDist).
4. Jitter starting step offset by frameIndex (temporal stability, same as GI).
5. Distribute steps exponentially along [0, tMax]:
     t(s) = tMax * (exp(s / numSteps * log(tMax + 1)) - 1) / tMax
   This concentrates samples near the camera where transmittance is highest
   and contribution is greatest, rather than wasting samples far away.
6. For step s in [0, numSteps):
     stepPos = cameraPos + dir * t(s)
     stepSize = t(s+1) - t(s)   // variable, larger at distance
     // GI light list is pre-sorted nearest-first; evaluate only the first
     // r_rtVolMaxLights entries — they are guaranteed to be the closest lights.
     For each light in giLightBuf[0 .. r_rtVolMaxLights):
         if (dist > lightRadius) continue;
         fire rayQuery toward light, test occlusion
         if (unoccluded):
             cosTheta = dot(dir, lightDir)
             phase    = HenyeyGreenstein(cosTheta, anisotropy)
             atten    = CauchyRamp(dist, lightRadius)
             scatter += lightColor * intensity * phase * atten * stepSize
7. scatter *= density * transmittance_approx
8. Write scatter.rgb to volBuffer (additive; store with alpha=1)
```

**Transmittance:** for simplicity, use Beer-Lambert with a constant extinction coefficient
(same as `density`).  This gives the correct darkening of the background through fog
without requiring a full path integral.

### Light Data

The existing `GILightBuffer` SSBO already supplies positions, radii, and colours.  No new
light data is needed for point-light volumetrics.

**Projected lights (flashlight) require extension**: the current GILight struct stores only
`posRadius + colorIntensity`.  To get the flashlight cone, need to add:
- `vec4 coneDir` — spot direction (xyz) + cos(halfAngle) (w)
- `uint lightType` — 0=point, 1=spot, 2=parallel

This is a `GILightBuffer` layout change that affects gi_ray.rchit as well.  Recommended to
implement point-light volumetrics first (no struct change), then extend to spots in a
follow-up step.

### Output and Composite

**volBuffer:** `VkImage` RGBA16F, half resolution (`screenW/2 × screenH/2`), allocated
alongside giBuffer in `vk_gi.cpp`.

**Bilateral upsample:** a small compute pass reads volBuffer at half-res and writes to a
full-res `volBufferFull`, using depth difference to select weights.  A 3×3 or 5×5 kernel
with depth edge-stop (similar to the existing À-trous depth sigma) is sufficient.

**Composite:** add `volBufferFull` additively to the frame after the interaction pass.
In `interaction.frag`, sample `volSampler` and add to `fragColor`.  Alternatively, a
dedicated blit pass avoids touching the interaction shader.

### New Files

| File | Purpose |
|------|---------|
| `neo/renderer/glsl/vol_march.comp` | Main ray-marching compute shader |
| `neo/renderer/glsl/vol_upsample.comp` | Bilateral upsample to full res |
| `neo/renderer/Vulkan/vk_vol.cpp` | Pipeline, dispatch, image lifecycle |
| `neo/renderer/Vulkan/vk_vol.h` | Public API (`VK_RT_DispatchVolumetrics`) |

### Modified Files

| File | Change |
|------|--------|
| `neo/renderer/Vulkan/vk_gi.cpp` | Allocate volBuffer alongside giBuffer; extend GILight if adding spots |
| `neo/renderer/glsl/interaction.frag` | Sample volSampler, additive add to output |
| `neo/renderer/Vulkan/vk_backend.cpp` | Call `VK_RT_DispatchVolumetrics` in the RT frame path |
| `neo/CMakeLists.txt` | Add `vol_march.comp`, `vol_upsample.comp` to GLSL_INCLUDES and GLSL_SOURCES |

### Performance Analysis

At half-res (960×540 = 518K pixels):

| Config | Ray queries (worst case) | Approx GPU time |
|--------|--------------------------|-----------------|
| 16 steps / 4 lights | ~33M | 6–10ms |
| 8 steps / 2 lights | ~8.3M | 2–4ms |
| 4 steps / 2 lights | ~4.1M | 1–2ms |

Ray queries are ~2–3× cheaper than full shadow rays (no shader invocation at hit).
In practice the dist > lightRadius cull fires before the query; in dark corridors with
1–2 nearby lights, 60–80% of step-light pairs are culled, making real query counts
significantly lower than worst case.

The three optimisations built into the design that have the largest impact:

1. **Sky early-out** — pixels at depth >= 1.0 skip the entire march.  Free for outdoor
   sky areas; meaningful saving in any scene with visible sky portals.

2. **Pre-sorted light list** — the GI light SSBO is already sorted nearest-first by
   `VK_RT_UploadGILights`.  Evaluating only `giLightBuf[0 .. r_rtVolMaxLights)` always
   picks the closest lights, which are the only ones that produce sharp, visible shafts.
   No per-step distance sort needed.

3. **Exponential step distribution** — concentrates samples near the camera (high
   transmittance, most visible contribution) and spreads them at distance (low
   transmittance, diminishing returns).  Same step count, significantly better quality
   per query budget than uniform steps.

### CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtVol` | `0` | Enable volumetric pass (off by default until tuned) |
| `r_rtVolSamples` | `8` | Ray-march steps per pixel (safe default; push to 16 on high-end) |
| `r_rtVolDensity` | `0.02` | Global scattering density |
| `r_rtVolAnisotropy` | `0.5` | Henyey-Greenstein g parameter (0=isotropic, 1=full forward) |
| `r_rtVolMaxDist` | `512.0` | Max ray-march distance (world units) |
| `r_rtVolMaxLights` | `2` | Nearest lights evaluated per step (pre-sorted, so always closest) |
| `r_rtVolStrength` | `1.0` | Final composite scale |

### Implementation Order

1. **vk_vol.cpp skeleton** — allocate volBuffer, create pipeline plumbing, dispatch stub
2. **vol_march.comp** — sky early-out, uniform steps first, point lights only, white constant scatter
3. **Composite** — additive blend into interaction.frag (even with placeholder output, validates pipeline)
4. **Exponential steps** — replace uniform t(s) with exponential distribution, compare visually
5. **Phase function** — add Henyey-Greenstein, tune `r_rtVolAnisotropy`
6. **Temporal EMA** — add temporal accumulation before declaring feature complete (noise is severe without it)
7. **Bilateral upsample** — replace bilinear to fix depth-edge halos
8. **Spot light extension** — extend GILight struct to add cone data for the flashlight

### Potential Issues

- **Temporal noise**: exponential step jitter at single-sample still produces visible
  crawl.  The existing temporal EMA infrastructure from RTAO can be reused (same
  world-anchored seed strategy).  Add a temporal accumulation pass matching
  `vk_ao.cpp`'s EMA approach — build this before shipping, not as a follow-up.
- **Fog light interaction**: skip lights where `lightShader->IsFogLight()` is true in the
  CPU-side light upload.  Add a `lightType` flag for this even before adding spot support.
- **Shadow ray tmax precision**: `dist - GI_SHADOW_BIAS` can go negative for very close
  lights.  Guard with `max(0.01, dist - GI_SHADOW_BIAS)` as in gi_ray.rchit.

---

## Recommended Sequence

| Step | Feature | Notes |
|------|---------|-------|
| 1 | Emissive material slot | Small, self-contained, high visual payoff on screens/panels |
| 2 | Emissive in GI rchit | Depends on step 1 |
| 3 | Emissive in reflect_ray.rchit | Same change, screens visible in mirrors |
| 4 | vk_vol.cpp scaffolding | Allocate buffer, stub dispatch |
| 5 | vol_march.comp (point lights) | No spot cone, simplified |
| 6 | Composite + tuning | Get it visible before refining |
| 7 | Bilateral upsample | Replace bilinear to fix halos |
| 8 | Temporal EMA for volumetrics | Reduce noise |
| 9 | Spot light cone in GILight | Flashlight shaft |
