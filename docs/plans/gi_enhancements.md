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

1. **vk_vol.cpp skeleton** — allocate volBuffer, create pipeline plumbing, dispatch stub ✓
2. **vol_march.comp** — sky early-out, uniform steps first, point lights only, white constant scatter ✓
3. **Composite** — additive blend into interaction.frag (even with placeholder output, validates pipeline) ✓
4. **Exponential steps** — replace uniform t(s) with exponential distribution, compare visually ✓
5. **Phase function** — add Henyey-Greenstein, tune `r_rtVolAnisotropy` ✓
6. **Per-type CVars** — separate density/anisotropy/strength for flashlight vs point lights; add `lightType`
   field to `GILightEntry`; player-model self-shadowing fix (`0xFEu` cull mask) ✓
7. **Light volume correctness** — replace sphere test with AABB/cone containment (see section below)
8. **Temporal EMA** — add temporal accumulation before declaring feature complete (noise is severe without it)
9. **Bilateral upsample** — replace bilinear to fix depth-edge halos

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

## Phase 7.2 — Light Volume Correctness

**Status: done**

### Problem

The initial implementation uses a spherical volume test for all lights:

```glsl
float dist = length(stepPos - lightPos);
if (dist > lightRadius) continue;
float atten = CauchyRamp(dist, lightRadius);
```

`lightRadius` is `max(p.lightRadius.xyz)` — the largest axis of the actual bounding box used
as a sphere radius.  This produces visible glowing spheres centred on each light origin,
rather than fog uniformly filling the light's real volume.

### How Doom 3 lights actually work

| `pointLight` | `parallel` | Volume shape |
|---|---|---|
| `true` | `false` | OBB — `p.lightRadius` gives half-extents; `p.axis` gives orientation |
| `false` | `false` | Frustum — `p.origin`, `p.target`, `p.right`, `p.up` define a cone/pyramid |
| any | `true` | Directional/infinite — skip for volumetrics |

The flashlight is a projected light (`pointLight == false`), identical in structure to any
world spotlight or door-crack projector.

### Fix: AABB containment for point lights

Replace the sphere test with a box containment test.  For axis-aligned lights (the majority
of Doom 3 lights) this is:

```glsl
bool inBox = all(lessThanEqual(abs(stepPos - lightPos), boxHalfExtents.xyz));
if (!inBox) continue;
```

Attenuation inside the box: a simple linear ramp from 1.0 at the centre to 0.0 at the
box edge, or just uniform 1.0 (the shadow ray already shapes the contribution).

**OBB / rotated lights** (deferred): Doom 3 lights can have a non-identity `p.axis`.  A
full OBB test requires transforming `stepPos` into light-local space.  For now the AABB
approximation is acceptable — fog blending hides the corner over-extension of slightly
rotated lights.  Add OBB support only if specific maps show obvious artefacts.

### Fix: cone containment for projected lights

Replace the sphere test with a cone/frustum containment test:

```glsl
vec3  toStep    = stepPos - lightPos;
float alongCone = dot(toStep, coneDir.xyz);
float cosAngle  = alongCone / max(length(toStep), 0.001);
bool  inCone    = cosAngle > coneDir.w          // within half-angle
               && alongCone > 0.0               // in front of light origin
               && alongCone < boxExtents.w;     // within max reach
if (!inCone) continue;
```

Attenuation: `1/d²` along the cone axis gives the physically correct spotlight falloff.

This single code path handles the flashlight, ceiling spotlights, angled projectors, and
window-light beams — any light with `lightType == 1`.

### Required struct change: add `boxExtents` to `GILightEntry`

The current `coneDir[4]` field carries cone direction for projected lights.  Point lights
need their half-extents.  Rather than overloading `coneDir`, add a dedicated field:

```cpp
struct GILightEntry {
    float    posRadius[4];   // xyz=pos, w=sphere pre-cull radius (cheap early-out)
    float    colorIntensity[4];
    float    coneDir[4];     // projected: xyz=normalised dir, w=cos(halfAngle); zeroed for point
    float    boxExtents[4];  // point: xyz=AABB half-extents, w=unused
                             // projected: w=max-reach distance along cone axis
    uint32_t lightType;      // 0=point, 1=projected
    uint32_t pad[3];
};
// 80 bytes per entry; static_assert → 16 + VK_GI_MAX_LIGHTS * 80
```

GLSL mirrors:
```glsl
struct GILight {
    vec4 posRadius;
    vec4 colorIntensity;
    vec4 coneDir;       // projected: xyz=dir, w=cosHalfAngle
    vec4 boxExtents;    // point: xyz=halfExtents; projected: w=maxReach
    uint lightType;
    uint _pad0; uint _pad1; uint _pad2;
};
```

### Required CPU upload changes

**Point lights** (`p.pointLight == true`):
```cpp
c.entry.boxExtents[0] = p.lightRadius.x;
c.entry.boxExtents[1] = p.lightRadius.y;
c.entry.boxExtents[2] = p.lightRadius.z;
c.entry.boxExtents[3] = 0.0f;
c.entry.coneDir[0] = c.entry.coneDir[1] = c.entry.coneDir[2] = c.entry.coneDir[3] = 0.0f;
// posRadius.w stays as max(lightRadius) for cheap sphere pre-cull
```

**Projected lights** (`p.pointLight == false && p.parallel == false`):
```cpp
idVec3 dir = p.target - p.origin;
float  reach = dir.Length();
dir /= reach;
float maxHalfExtent = Max(p.right.Length(), p.up.Length());
float cosHalfAngle  = reach / sqrtf(reach * reach + maxHalfExtent * maxHalfExtent);

c.entry.coneDir[0] = dir.x;   c.entry.coneDir[1] = dir.y;
c.entry.coneDir[2] = dir.z;   c.entry.coneDir[3] = cosHalfAngle;
c.entry.boxExtents[0] = c.entry.boxExtents[1] = c.entry.boxExtents[2] = 0.0f;
c.entry.boxExtents[3] = reach * 1.1f;   // slight margin beyond frustum tip
c.entry.posRadius[3]  = reach * 1.1f;   // sphere pre-cull matches max reach
```

### Files to change

| File | Change |
|------|--------|
| `neo/renderer/Vulkan/vk_gi.cpp` | `GILightEntry` add `boxExtents[4]`; static_assert 80; upload fills |
| `neo/renderer/glsl/gi_ray.rchit` | `GILight` add `vec4 boxExtents` |
| `neo/renderer/glsl/reflect_ray.rchit` | `ReflGILight` add `vec4 boxExtents` |
| `neo/renderer/glsl/player_reflect.rchit` | `ReflGILight` add `vec4 boxExtents` |
| `neo/renderer/glsl/vol_march.comp` | Replace sphere test with AABB/cone containment; update `GILight` |

### Deferred / stretch

- **OBB rotation**: add `p.axis` (3×3ux = 9 floats, 3 × vec4 extra) to `GILightEntry` and
  transform `stepPos` into light-local space before AABB test.  Only if axis-aligned AABB
  shows visible artefacts on specific maps.
- **Frustum vs cone**: a proper 4-plane frustum test is more accurate for wide projectors.
  The cone approximation is sufficient for the volumetric blur level.

### Spatial smoothing

Deferred — evaluate after temporal EMA is in place.

The noise sources in the volumetric buffer are: (1) temporal jitter between frames,
(2) step-boundary banding from discrete march steps, and (3) hard binary shadows from
ray queries.  Temporal EMA handles (1) almost entirely.  If (2) or (3) are still visible
after EMA, a bilateral denoise pass (depth-aware, same depth edge-stop as the
planned bilateral upsample) resolves them.  A plain Gaussian is cheaper but bleeds fog
through depth edges.

Since we run at full resolution, the planned bilateral upsample step becomes a bilateral
denoise pass if needed — same compute shader, no size change.  Do not implement before
EMA; let the result decide whether it is necessary.

---

## Recommended Sequence

| Step | Feature | Status |
|------|---------|--------|
| 1 | Emissive material slot | done |
| 2 | Emissive in GI rchit | done |
| 3 | Emissive in reflect_ray.rchit | done |
| 4 | vk_vol.cpp scaffolding | done |
| 5 | vol_march.comp (point lights) | done |
| 6 | Composite + tuning | done |
| 7 | Per-type CVars + lightType field + self-shadow fix | done |
| 8 | Temporal EMA for volumetrics | done |
| 9 | Light volume correctness (AABB/cone) | done |
| 10 | Bilateral upsample | deferred — evaluate after temporal EMA |
