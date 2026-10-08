# Bloom

**Status:** B0-B2 validated in-game 2026-10-07 (B2 confirmed at `r_rtBloomStrength 3`; the
0.15 default is swallowed by the tonemap toe — see B4). B3's menu written, **unvalidated**;
B3's `r_rtBloomDebug 3` and B4 open.
Revised 2026-09-30 against the FSR / froxel-vol / probe-GI pipeline.
**Owns:** screen-space bloom, its debug views and cvars.

Bloom adds a short glow around light-emitting surfaces: fixtures, screens, flames, muzzle
flashes, projectiles. It is not a haze pass. The volumetrics already light the air, and a
wide low-threshold bloom would lift dark regions, which breaks pillars 2 and 3.

## Constraints from the current pipeline

| Fact | Consequence |
|---|---|
| `r_lightScale` 2 overbright: lit walls and specular reach HDR lum ≈ 2 | A luminance threshold alone blooms lit walls *before* fixtures (blend-add stages sit ≈ 1). Source must be emissive-weighted |
| Tonemap runs in `VK_RB_SwapBuffers`, **after** the GUI/HUD drew into `hdrScene` | "Bloom just before tonemap" (old plan) would bloom the HUD and menus |
| 3D→GUI boundary is the upscale slot (`vk_backend.cpp` ~4840), with a SwapBuffers fallback | Bloom blur+composite go here, at display res, after FSR (AMD's guidance) |
| U4 already splits the frame at "interactions done / shader passes next" | That boundary gives an emissive source: `after_shader_passes − before_shader_passes` |
| Vol composite (default late site) and fog lights draw *after* shader passes | The diff excludes them, so fog shafts never bloom |
| `preAlphaColor` exists only when FSR 2 is possible, at full size | Bloom owns a half-res snapshot instead; no FSR dependency |
| `VK_RB_CopyRender` is implemented now (no longer a stub) | RoE's scripted `FullscreenFX_Bloom` may run on top; check for doubling in B4 |
| Deferred glass overlays draw at the end of `VK_RB_DrawShaderPasses` | Glass reflections land in the diff and bloom. Wanted for reflected fixtures/projectiles; watch for glass panes glowing as a whole in B4 |
| `blend add` smoke (e.g. `textures/particles/smokepuff`) and alpha smoke over dark backgrounds give a positive diff | The emissive term needs its own threshold, not only the lit term |

## Design

```
interactions → [B1a pre-snapshot, ½ render res] → shader passes → [B1b extract] → vol → fog → ...
... → upscale → [B2 Kawase down/up chain + composite, display res] → GUI/HUD → tonemap
```

Extract (B1b): `src = softKnee(max(curr − pre, 0), emissiveThreshold)·emissiveWeight + softKnee(curr, threshold)·litWeight`.
`emissiveThreshold` starts at 0.5; B0's bands for smoke vs fixtures set it.
Defaults: `emissiveWeight 1` (not a cvar), `litWeight 0`.

**Karis placement, changed during B1.** The plan had the first downsample Karis-weighted.
It can't be: the extract subtracts `bloomPre`, and a weighted average makes the two
operators disagree, so the diff stops meaning "what the blend stages added". Instead:

| Term | Downsample | Firefly defence |
|---|---|---|
| emissive (diff) | plain 2×2 box, same as `bloom_prepass` | the diff itself — a GI/reflection firefly sits in both `pre` and `curr` and cancels |
| lit | Karis 2×2 | the Karis weights |
| both | — | `clampMax` 16.0, a hard ceiling in `bloom_extract.comp` |

Karis on mip0→mip1 moves to B2's `bloom_down.comp`, which is where most
implementations put it anyway.

## CVars

| CVar | Default | Range | Notes |
|---|---|---|---|
| `r_rtBloom` | 0 | 0/1 | Drives the B1 capture only until B2 lands. Default flips to 1 after B4 |
| `r_rtBloomThreshold` | 0.8 | 0.2–4 | Soft-knee threshold, pre-exposure units |
| `r_rtBloomEmissiveThreshold` | 0.5 | 0–2 | Soft-knee on the emissive diff; keeps smoke out |
| `r_rtBloomKnee` | 0.5 | 0–1 | |
| `r_rtBloomLitWeight` | 0 | 0–1 | 0 = emissive-only |
| `r_rtBloomStrength` | 0.15 | 0–4 | Additive weight. **Default is too low — see B4.** Range widened from 0–1 on the 2026-10-07 finding |
| `r_rtBloomMips` | 4 | 2–6 | Radius; keep tight. Consumed by B2 |
| `r_rtBloomDebug` | 0 | 0–3 | 1 and 2 live; 3 is B3 |

`emissiveWeight` is deliberately not a cvar — the design is emissive-sourced and the only
honest A/B against it is `r_rtBloomLitWeight`.

---

## B0 — Measure before building ✅ validated 2026-10-07

| Change | Where |
|---|---|
| `r_rtBloomDebug 1`: luminance bands of `hdrScene` (<0.5 black, 0.5–1 blue, 1–2 green, 2–4 yellow, >4 red) | `tonemap.comp` `LuminanceBand`, push constant 16 → 20 B; `vk_tonemap.cpp` |

Bands are read **before** exposure and bypass the Uchimura curve — a band pushed through
the toe stops being distinguishable from its neighbour. The cvar thresholds are quoted in
those same units.

Check: Mars City 1/2, Alpha Labs, a Delta lab, Hell. Screenshot fixtures, monitors, flames,
muzzle flash, plasma, sky, and the brightest lit wall beside a point light. Record in this doc
whether emissives and lit surfaces overlap in band. If they do not, `litWeight` may be raised
later; the emissive path is built either way.

## B1 — Source capture ✅ validated 2026-10-07

| Change | Where |
|---|---|
| `bloom_prepass.comp`: plain 2×2 box of `hdrScene` → `bloomPre` | new shader; shares the U4 capture's end/resume |
| `bloom_extract.comp`: same box of current `hdrScene`, diff vs `bloomPre`, soft-knee → `bloomMip[0]` | new shader; after shader passes, before the late `Vol_Composite` |
| `bloom_debug.comp`: `r_rtBloomDebug 2`, mip0 magnified into `hdrScene`, gain ×4 | new shader; SwapBuffers slot beside the FSR overlays |
| Both capture sites gated on real camera, not subview, not mirror (same test as U4) | `vk_backend.cpp` |
| Images per frame slot: `bloomPre`, `bloomMip[0..5]`, all rgba16f, permanently `GENERAL` | new `vk_bloom.cpp/.h` |
| `VK_RTPROF_PHASE_BLOOM` (pulled forward from B2; both dispatches accumulate into it) | `vk_backend.cpp` |

Sizing: images are **half the display extent and fully written** — deliberately *not* the U0
convention of a valid `renderExtent` sub-rect. The prepass and extract map a 2×2 box by the
render-scale ratio, so they absorb `r_fsrRenderScale` and everything downstream is
display-space. A chain bounded by the render extent instead keeps a constant radius in
*render* texels, which is a radius in display pixels proportional to `1/renderScale` — the
glow doubles in width at scale 0.5. Both passes must derive their taps identically or the
diff compares different texels. All six mips are allocated regardless of `r_rtBloomMips`, so
that cvar stays a dispatch count rather than a reallocation.

Cost consequence: the chain runs at half display at every render scale, so lowering
`r_fsrRenderScale` no longer makes bloom cheaper. The composite already ran at display extent
and dominates.

The extract refuses to run unless a prepass filled the same slot on the same `tr.frameCount`;
a stale snapshot would diff as a bloom source smeared over everything that moved. That also
stands the debug view down on the main menu, where no 3D view exists.

Check: `r_rtBloomDebug 2` shows `bloomMip[0]` upscaled: fixtures/screens/particles lit,
walls and fog black. Decals (blend filter) must not appear (clamped negative).
`r_vkRTProfile 1` reports `Bloom=`.

## B2 — Blur chain + composite ✅ validated 2026-10-07

| Change | Where |
|---|---|
| `bloom_down.comp`: COD/Jimenez 13-tap, Karis-grouped on mip0→mip1 only | new shader |
| `bloom_up.comp`: 3×3 tent, `mix(dst, tent, 0.5)` into the next mip up | new shader |
| `bloom_composite.comp`: `hdrScene += bilinear(bloomMip[0]) · strength` over the display rect | new shader |
| `VK_RT_DispatchBloom(cmd)` at the 3D→GUI boundary after `VK_RT_DispatchUpscale`, **also when upscale is inactive**, plus the SwapBuffers fallback; `s_bloomDone` seeded from `firstIsGui` | `vk_backend.cpp` |
| Profiler phase `VK_RTPROF_PHASE_BLOOM` | landed in B1 |
| Shaders in `CMakeLists.txt`; `vk_bloom.cpp` in sources | `CMakeLists.txt` |

**Mix, not add, on the upsample.** A plain additive upsample makes the finished glow
brighter as `r_rtBloomMips` rises, turning a radius control into a brightness control.
`mix(dst, tent, 0.5)` holds the chain's total weight at 1 whatever the level count is,
so `r_rtBloomMips` only changes spread and `r_rtBloomStrength` stays the one brightness knob.

`VK_RT_BloomCompositeActive()` gates on `r_rtBloom` **alone**, not `VK_RT_BloomActive()` —
`r_rtBloomDebug 2` pulls the capture up by itself and must not start compositing glow that
was switched off.

The down and up passes need one descriptor set *per level per frame slot*: a later level
would otherwise overwrite the descriptors an earlier, still-unsubmitted dispatch points at.

Check: HUD, PDA and main menu show no bloom. `r_fsr 0/1/2` and `r_fsrRenderScale 0.5/0.67/1.0`
give the same glow **radius** on screen — compare a single fixture at 0.5 against 1.0, not the
whole frame. `r_rtBloomMips` 2 vs 6 changes radius but not brightness. `r_vkRTProfile 1`
reports `Bloom=`, and unlike the other phases it does not shrink with render scale.

## Review fixes 2026-10-07 — unvalidated

Six findings from the B0–B2 review, all landed in one pass.

| Finding | Fix | Where |
|---|---|---|
| Blur chain worked in render texels, so glow radius scaled with `1/renderScale` | Prepass and extract resample to half **display**; chain is display-space throughout | `bloom_prepass/extract.comp`, `vk_bloom.cpp` |
| `r_rtBloomDebug 2` showed blurred glow — the up chain's last level writes `bloomMip[0]` | `VK_RT_BloomCompositeActive()` stands the chain down in mode 2 | `vk_bloom.cpp` |
| `r_rtBloomDebug 1` dead under `r_rtTonemap 0` (bypass blit returns first) | Band view also enables the compute path | `vk_tonemap.cpp` |
| Band view used Rec. 709 luminance; the extract thresholds `max(r,g,b)` | Bands switched to `max(r,g,b)` — see below | `tonemap.comp` |
| RoE portal sky gives two capture-gated views per frame, rewriting bound descriptor sets | Per-set view cache; skip the identical rewrite | `vk_bloom.cpp` |
| `VK_RT_ResizeBloom` unguarded — ~75 MiB at 4K on devices that cannot run bloom | Guarded on `vk.rayTracingSupported` | `vk_swapchain.cpp` |

**Bands follow the threshold, not the other way round.** `max(r,g,b)` is what the soft knee
tests, and that is the right metric to keep: a saturated blue screen is luminance 0.07 but
`max` 1.0, and it *should* bloom. Banding by luminance would have hidden exactly the
saturated emissives the game is full of. The threshold cvar help now states the units.

The descriptor cache must be cleared in `VK_RT_ResizeBloom`: every view it compares has just
been recreated and Vulkan may reuse handle values, so an uncleared cache would skip the
rewrite and leave descriptors resolved against freed resources. Every (pass, set) pair maps to
a fixed view pair, so after the first frame the cache always hits.

Check: `r_rtBloomDebug 2` with `r_rtBloom 1` shows hard-edged extract, not soft glow.
`r_rtBloomDebug 1` works at `r_rtTonemap 0/1`. A blue monitor lands in the blue band, not the
black one. Validation layers stay quiet on a RoE portal-sky map (`erebus1`-style outdoor view).

## B3 — Debug + menu

| Change | Where | Status |
|---|---|---|
| "Bloom" block under the tonemap section: enable, strength, thresholds, knee, radius, lit-weight sub-tree, debug combo | `Dhewm3SettingsMenu.cpp` (`RTCVars` + draw) | ✅ written 2026-10-07, unvalidated |
| `r_rtBloomDebug 3`: bloom-only (composite result minus scene), gain ×4 | `bloom_composite.comp` | open |

The menu block **lifts `BeginDisabled(!rtEnabled)` across itself and restores it after**.
Bloom traces nothing — it diffs `hdrScene`, which is the colour attachment whether or not
`r_useRayTracing` is on — so greying it out with RT off would be wrong. The tonemap sliders
above are RT-independent for the same reason and are still gated; not changed as a side effect.

The debug combo sits outside the `!bloomOn` guard: mode 1 is what tells you where to put the
thresholds, so it has to work before bloom is switched on.

## B4 — Tune against the pillars

**Finding 2026-10-07:** B2 confirmed working in-game, but only visible from
`r_rtBloomStrength` ≈ 3 — the 0.15 default reads as "bloom does nothing". The cause is the
toe, not the chain. At `r_rtTonemapToe 2.7` a dark pixel at luminance 0.01 gaining +0.015
moves from 3.1e-5 to 3.7e-4 after the curve: a 12× linear increase that is still under one
8-bit LSB. Near the source, where the scene is already ~1.5, the same addition is a 1%
change. So the glow is crushed exactly in the dark surroundings where it should read.

Pick the default from `r_rtBloomDebug 3`, not from the composite (pillar 6) — which means
finishing B3's mode 3 first. Open question for the sweep: whether the answer is a higher
strength, or feeding the composite through a small pre-toe lift.

| Check | Pass condition |
|---|---|
| Dark corridor with one fixture | Black stays black ≥ 1 fixture-width away (pillar 2) |
| Fog-lit room (vol on) | No added haze; shafts unchanged with bloom on/off (pillar 3) |
| Moving camera past GI-noisy area | No flicker in `r_rtBloomDebug 2` |
| RoE scripted bloom scene | No double glow; if doubled, skip ours while `player->bloomEnabled` |
| Cost on 9070 XT at 1440p | ≤ 0.3 ms |

Then set `r_rtBloom 1` default and record the tuned values here.

## Cut

Lens dirt, anamorphic streaks, and full-scene luminance bloom as the default. All three
push the image toward a soft or cinematic look that fights the high-contrast art.
