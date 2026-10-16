# Volumetrics in reflections

Reflection rays integrate no participating media, so the flashlight beam is absent from
glass. Fix: march the reflected segment through `froxelScatter`, the pre-integration
volume the froxel path already produces.

**Closed 2026-10-15.** Stages 1-6 landed and validated in-game; Stage 4's debug overlay was
not built.

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

Menu entries landed with Stage 5. The planned debug mode (reflected-ray volumetric term
alone) was **not built** — validation passed without it, and the refl debug range is already
crowded. If it is ever needed, it goes in at `r_rtReflectionDebugMode 10`.

**Check:** `r_rtReflVol 0` restores the current image exactly.

---

## Stage 5 — mean-direction grid (landed and validated 2026-10-15)

In-game 2026-10-15: the reflected flashlight beam reads too bright at high angles and far
too dim head-on.  Cause is the baked phase angle below, measured rather than estimated.

`vol_froxel_fill.comp:207` bakes `PhaseFunction(dot(camToCell, cellToLight), …)` per cell.
The flashlight sits *on* the camera, so `cosθ ≈ −1` across the whole beam — the dimmest
point of the lobe, and near-constant, so the beam has no angular structure at all.  A
reflected ray travels back toward the eye, where the light also is, so its correct angle is
`cosθ ≈ +1`: the brightest single-scatter geometry there is.

| Case | cosθ | phase (`g` 0.7, `isoMix` 0.45) |
|---|---|---|
| Baked (camera at the light) | ≈ −1 | 0.040 |
| Correct, reflected ray toward the eye | ≈ +1 | 0.863 |
| Correct, reflected ray at 60° | +0.5 | 0.068 |
| Correct, reflected ray at 90° | 0 | 0.048 |

`r_rtReflVolGain 2.0` is a scalar standing in for a missing lobe: after it, head-on is still
~11x short and 60-90° is 1.2-1.7x over.  Only `isotropicMix`'s direction-independent half
transports correctly, which is why the high angles are merely wrong and not absurd.

Fix: store in-scatter **unphased**, plus a mean scattering direction whose length is the
mean cosine — `D = Σ wᵢ·gᵢ·dirᵢ / Σ wᵢ` — and have every consumer apply
`PhaseFunction(dot(rayDir, normalize(D)), length(D), isoMix)` at sample time.  Encoding `g`
as `|D|` is the standard HG convention, costs no channel, and degrades multi-light cells
toward isotropic.  This makes `froxelScatter`'s advertised ray-agnosticism true.

| File | Change |
|---|---|
| `rt_phase.glsl` | **New.** `rt_HenyeyGreenstein` / `rt_PhaseFunction` (the three duplicated copies) + `rt_PhaseFromMeanDir` |
| `vk_vol_froxel.cpp` | `froxelDir[slot]`, rgba8_snorm, froxel dims (12.4 MiB/slot at 240x135x96); `VK_RT_AllocFroxelImage` takes a format; `VK_RT_GetFroxelDirView` + its own null grid; fill binding 4, integrate binding 3 |
| `vol_froxel_fill.comp` | No `phase` on the contrib line; accumulates `D` and writes it to binding 4 |
| `vol_froxel_integrate.comp` | Applies the phase per cell from `D` against the column's view ray |
| `reflect_ray.rgen` | Same, with the reflected `d`; binding 8 + `volIsoMix` in the UBO |
| `vk_reflections.cpp` | Binding 8 + `volIsoMix`; `r_rtReflVolGain` default 2.0 → 1.0 |
| `Dhewm3SettingsMenu.cpp` | `r_rtReflVol` + `r_rtReflVolSteps` under Reflection Settings (Stage 4's menu half) |

**No gain retune.** The phase is applied in the integrate pass from the same
camera→cell direction the fill used, so the primary view is unchanged and
`r_rtVolFlashlightGain 50` still means what it meant.  The one primary-view
difference is cells lit from several directions at once, where one blended phase
replaces the old per-light evaluation.

**Check:** `r_rtVolFlashlightAnisotropy 0` collapses HG to `1/4π`, making the phase
direction-independent — the head-on/high-angle balance that looked right only there before
must now hold at the `0.7` default with gain 1.0.  `r_rtReflVolSteps 1` vs `16` still
changes banding, not brightness.  `r_rtVolFroxelDebug 1` now shows UNPHASED scatter, ~25x
brighter for a flashlight cell, so drop `r_rtVolFroxelDebugGain` well below 20 when reading
that overlay.

---

## Stage 6 — projected-light falloff image (landed and validated 2026-10-15)

Same in-game report, second cause, independent of the phase grid: RT invents a linear axial
ramp for projected/flashlight lights where GL uses the light's `lightFalloffImage`.
`rt_light_eval.glsl:176`:

```glsl
float alongCone = dot(hitPos - lPos, cd.xyz);
atten = clamp(1.0 - alongCone / max(bx.w, 0.001), 0.0, 1.0);
```

| Problem | Consequence |
|---|---|
| GL applies no axial falloff here.  `lights/flashlight5` is an implicit material with no falloff image, so `R_DeriveLightData` falls back to `lights/defaultProjectedLight` → `lightFalloffImage _noFalloff`, flat white | ~30% of the head-on pool gone at 400 units (`end` 1280, `bx.w` 1408) |
| `alongCone` is the **axial** projection, so at constant true distance the cone edge gets `d·cosθ < d` and a *larger* `atten` | The term meant to darken with distance brightens off-axis |

GL's math — `interaction.vert:90` + `interaction.frag:99`:

```glsl
falloffS = dot(vec4(pos,1.0), lightProject[3]);   // 0 at light_start, 1 at light_end
falloff  = texture(lightFalloffImage, vec2(falloffS, 0.5)).r;
```

`lightProject[3]` is already world-space (`tr_lightrun.cpp:449-454`), beside the three
planes the cookie upload already copies.  `_noFalloff` is `TR_CLAMP_TO_ZERO`: outside [0,1]
is black, not a passthrough.

| File | Change |
|---|---|
| `vk_gi.cpp` | `GILightCookie` += `planeF[4]`, `falloffIndex`; `pad[3]` → `pad[2]`.  64 → 80 bytes, update both `static_assert`s |
| `vk_gi.cpp` | New flag `GI_LIGHT_FLAG_HAS_FALLOFF 0x8u`, set independently of `HAS_COOKIE` — a projected light with no usable projection stage still needs its falloff |
| `vk_gi.cpp` | `planeF` from `lightLocal->lightProject[3]` raw (GL applies no stage texture matrix to the falloff); `falloffIndex` from `VK_RT_GetOrAssignTexIndex(lightLocal->falloffImage)` |
| `rt_light_cookie.glsl` | `RTLightCookie` += `planeF`, `falloffIndex`; `rt_SampleLightFalloff` with an explicit `[0,1]` → 0 clamp so the result cannot depend on `matSampler`'s address mode |
| `rt_light_eval.glsl` | Projected branch: `atten = rt_SampleLightFalloff(…)` when `HAS_FALLOFF`, keeping the cone containment test.  Fallback uses `dist`, never `alongCone` |

Scope is the shared GI-bounce/reflection loop.  `vol_froxel_fill.comp:199` and
`vol_march.comp` carry the same ramp plus their own `projMode` cvar and rim `smoothstep`;
changing those is a separate retune, not part of this stage.

**Check:** `r_rtGILightDump 1` prints `falloff:` with the image name — the flashlight must
read `_noFalloff`.  Flashlight pool on a flat wall: brightness constant with distance,
falling off radially from the cookie alone, no edge brightening.
`r_rtGIFalloffMode`/`Reach` still affect point lights only.

---

## Known limits

**Frustum coverage only.** A reflected ray outside the view frustum samples nothing. The
target case — your own beam in glass ahead of you — is inside it. Beams reflected from
off-screen will not appear.

**Cost:** N taps on glass-only pixels, no rays. Expect it in the noise next to the
reflection trace; `VK_RTPROF_PHASE_REFLECTIONS` is the number to watch.
