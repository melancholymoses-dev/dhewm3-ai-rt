# Volumetrics in reflections

Reflection rays integrate no participating media, so the flashlight beam is absent from
glass. Fix: march the reflected segment through `froxelScatter`, the pre-integration
volume the froxel path already produces.

## What already exists

| Resource | State | Why it fits |
|---|---|---|
| `vkRT.froxelScatter[slot]` | rgba16f 3D, **always GENERAL** | rgb = in-scatter per unit length, a = extinction. Ray-agnostic |
| `vkRT.froxelIntegrated[slot]` | rgba16f 3D | Camera-prefix integral — **not** reusable, do not sample |
| `vkRT.froxelSampler` | trilinear + clamp | Reuse as-is |
| `vf_DistToTexW`, `vf_SliceToDist` | `vol_froxel_common.glsl` | Slice math, already correct |

No layout transitions anywhere: the grids are cleared to GENERAL at alloc and stay there.

---

## Stage 1 — pure froxel sampling math

`vol_froxel_common.glsl` reads its UBO at `set=0 binding=2`, which is the reflection set's
depth sampler. Extract the math so an RT shader can use it without that binding.

| File | Change |
|---|---|
| `glsl/vol_froxel_sample.glsl` | **New.** `VolFroxelGrid` struct (camPos, camFwd, gridDim, depthParams, rangeParams, viewProj) + `vfs_DistToTexW`, `vfs_WorldToTexCoord`, `vfs_InGrid` |
| `glsl/vol_froxel_common.glsl` | Include it; `vf_DistToTexW` forwards to `vfs_DistToTexW(fp…)`. One copy of the exponential slice formula |
| `CMakeLists.txt` | Add to `GLSL_INCLUDES` |

`vfs_WorldToTexCoord` is the inverse of what the grid has today: `xy` from
`viewProj * worldPos`, `w` from `vfs_DistToTexW(dot(pos - camPos, camFwd))` — planar
distance, matching the slice convention.

**Check:** froxel volumetrics unchanged in-game (`r_rtVolFroxel 1`); this stage moves code
only. `r_rtVolFroxelDebug` slice views identical to before.

---

## Stage 2 — reach the volume from the reflection pass

| File | Change |
|---|---|
| `vk_backend.cpp` | Move `VK_RT_DispatchVolFroxelFill` **before** `VK_RT_DispatchReflections` (~5319). Integrate/resolve stay put |
| `vk_backend.cpp` | Memory barrier after fill: `SHADER_WRITE → SHADER_READ`, compute → RT. No layout change |
| `vk_reflections.cpp` | Descriptor binding 4 (free): `COMBINED_IMAGE_SAMPLER`, `froxelSampler` + `froxelScatter[slot]` view, layout `GENERAL`. Pool +1 per slot |
| `vk_reflections.cpp` | `ReflParamsUBO` += `viewProj[16]`, `froxelGrid` fields, `volSteps`, `volGain`. 128 → 224 bytes, update the offset comment and `static_assert` |
| `reflect_ray.rgen` | Declare binding 4 + the new UBO tail; include `vol_froxel_sample.glsl` |

Fill needs only the TLAS and `VolLightBuf`, both ready well before reflections — confirm
in the log that `Froxel: fill` precedes `Refl: dispatch`.

**Check:** `VK RT Refl` logs `volSteps=`; no validation errors; image unchanged (nothing
samples the volume yet). A null 1x1 3D image when `VK_RT_VolFroxelActive()` is false, with
`volSteps = 0` to match — same pattern as the G-buffer class binding.

---

## Stage 3 — march the reflected segment

In `reflect_ray.rgen`, after the reflection ray resolves, with `hitT` from the payload
(`maxDist` on a miss):

```
t_i   = hitT * (i + jitter) / N          i in [0, N)
cell  = vfs_WorldToTexCoord(origin + dir * t_i)
if !vfs_InGrid(cell) continue            // outside the frustum: no data, contribute 0
s     = texture(froxelScatter, cell)
L    += transmittance * s.rgb * segLen
transmittance *= exp(-s.a * segLen)
```

Add `L * volGain` to the reflection colour before the Fresnel/blend weighting, so the beam
is a reflected emissive term rather than something the surface tints.

| File | Change |
|---|---|
| `reflect_ray.rgen` | The march above; blue-noise jitter on `t` reusing the existing per-pixel hash |
| `reflect_payload.glsl` | Confirm hit distance is carried; add if not |

**Check:** stand with the flashlight on, facing glass. The beam appears in the reflection
and tracks the light. `r_rtReflVolSteps 1` vs `16` changes banding, not brightness — if it
changes brightness the segment length is wrong.

---

## Stage 4 — controls

The three cvars below landed with stage 3 — the march is untestable without them.

| Cvar | Default | Purpose |
|---|---|---|
| `r_rtReflVol` | 1 | Off switch / A-B |
| `r_rtReflVolSteps` | 6 | Taps per reflection ray |
| `r_rtReflVolGain` | 1.0 | Scale against the primary view |

Remaining: `r_rtReflectionDebugMode 9` showing the reflected-ray volumetric term alone,
and the first two cvars in `Dhewm3SettingsMenu.cpp` next to the reflection block.

**Check:** `r_rtReflVol 0` restores the current image exactly. Mode 9 shows the beam shape
against black and nothing on surfaces the beam does not cross.

---

## Known limits

**Phase angle is the camera's.** `vol_froxel_fill.comp:207` bakes
`PhaseFunction(dot(camToCell, lightDir), …)` into each cell, so a reflected ray inherits the
primary view's forward-scatter lobe. Worst for the flashlight, which carries its own
anisotropy (`fp.anisos.z`). Lever is `isotropicMix` (`fp.strengths.w`); a correct fix needs
per-cell direction storage and is out of scope. If the beam reads wrong rather than merely
approximate, the fallback for that one light is an analytic cone in `reflect_ray.rchit`.

**Frustum coverage only.** A reflected ray outside the view frustum samples nothing. The
target case — your own beam in glass ahead of you — is inside it. Beams reflected from
off-screen will not appear.

**Cost:** N taps on glass-only pixels, no rays. Expect it in the noise next to the
reflection trace; `VK_RTPROF_PHASE_REFLECTIONS` is the number to watch.
