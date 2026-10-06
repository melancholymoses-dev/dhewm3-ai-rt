# Bloom

**Status:** B0 + B1 written 2026-10-06, **not yet validated in-game**. B2-B4 open.
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
| `r_rtBloomStrength` | 0.15 | 0–1 | Additive weight |
| `r_rtBloomMips` | 4 | 2–6 | Radius; keep tight. Consumed by B2 |
| `r_rtBloomDebug` | 0 | 0–3 | 1 and 2 live; 3 is B3 |

`emissiveWeight` is deliberately not a cvar — the design is emissive-sourced and the only
honest A/B against it is `r_rtBloomLitWeight`.

---

## B0 — Measure before building ✅ written, unvalidated

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

## B1 — Source capture ✅ written, unvalidated

| Change | Where |
|---|---|
| `bloom_prepass.comp`: plain 2×2 box of `hdrScene` → `bloomPre` | new shader; shares the U4 capture's end/resume |
| `bloom_extract.comp`: same box of current `hdrScene`, diff vs `bloomPre`, soft-knee → `bloomMip[0]` | new shader; after shader passes, before the late `Vol_Composite` |
| `bloom_debug.comp`: `r_rtBloomDebug 2`, mip0 magnified into `hdrScene`, gain ×4 | new shader; SwapBuffers slot beside the FSR overlays |
| Both capture sites gated on real camera, not subview, not mirror (same test as U4) | `vk_backend.cpp` |
| Images per frame slot: `bloomPre`, `bloomMip[0..5]`, all rgba16f, permanently `GENERAL` | new `vk_bloom.cpp/.h` |
| `VK_RTPROF_PHASE_BLOOM` (pulled forward from B2; both dispatches accumulate into it) | `vk_backend.cpp` |

Sizing: images are **half the display extent** with only the top-left `renderExtent/2`
sub-rect valid — the convention every other U0 buffer follows, so `r_fsrRenderScale` stays a
push constant instead of a reallocation. All six mips are allocated regardless of
`r_rtBloomMips`, for the same reason.

The extract refuses to run unless a prepass filled the same slot on the same `tr.frameCount`;
a stale snapshot would diff as a bloom source smeared over everything that moved. That also
stands the debug view down on the main menu, where no 3D view exists.

Check: `r_rtBloomDebug 2` shows `bloomMip[0]` upscaled: fixtures/screens/particles lit,
walls and fog black. Decals (blend filter) must not appear (clamped negative).
`r_vkRTProfile 1` reports `Bloom=`.

## B2 — Blur chain + composite

| Change | Where |
|---|---|
| `bloom_down.comp` (13-tap) and `bloom_up.comp` (3×3 tent, additive into next mip up) | new shaders |
| `bloom_composite.comp`: `hdrScene += bilinear(bloomMip[0]) · strength` over the display rect | new shader |
| `VK_RT_DispatchBloom(cmd)` at the 3D→GUI boundary after `VK_RT_DispatchUpscale`, **also when upscale is inactive**, plus the SwapBuffers fallback; skip when `firstIsGui` | `vk_backend.cpp` |
| Profiler phase `VK_RTPROF_PHASE_BLOOM` | `vk_backend.cpp`, profile enum |
| Shaders in `CMakeLists.txt`; `vk_bloom.cpp` in sources | `CMakeLists.txt` |

Check: HUD, PDA and main menu show no bloom. `r_fsr 0/1/2` and `r_fsrRenderScale 0.67/1.0`
give the same glow size on screen. `r_vkRTProfile 1` reports `bloom=`.

## B3 — Debug + menu

| Change | Where |
|---|---|
| `r_rtBloomDebug 3`: bloom-only (composite result minus scene), gain ×4 | `bloom_composite.comp` |
| "Bloom" block under the tonemap section: enable, strength, threshold, mips, lit weight | `Dhewm3SettingsMenu.cpp` (`RTCVars` + draw) |

## B4 — Tune against the pillars

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
