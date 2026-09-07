# Phase 8.0 — Bloom Post-Processing

**Date:** 2026-05-15
**Branch:** TBD

---

## Overview

Bloom is a screen-space post-process that simulates light bleeding and lens glow around
bright areas.  It is complementary to — not a replacement for — the volumetric lighting
implemented in Phase 7.2.  Together they produce the complete effect: volumetrics fill the
air volume with in-scattered light; bloom makes the brightest parts of that result (and
emissive surfaces, torch flames, etc.) glow outward into surrounding pixels.

Doom 3's original engine had a material-based bloom system (`textures/smf/bloom2/*`) routed
through `FullscreenFX_Bloom` in `PlayerView.cpp`.  This does not translate to the Vulkan RT
pipeline: `VK_RB_CopyRender()` is currently a no-op stub and the material-based approach
requires fixed-function pipeline state we no longer have.

The replacement is a modern compute-shader bloom operating on the final composited HDR image
before it is presented.

---

## What Already Exists

| Component | Location | Status |
|-----------|----------|--------|
| Game bloom state (`bloomEnabled`, `bloomSpeed`, `bloomIntensity`) | `neo/d3xp/Player.h:420-422` | Exists, unused |
| `Event_ToggleBloom()` / `Event_SetBloomParms()` | `neo/d3xp/Player.cpp:9016,9031` | Exists, game fires events |
| `FullscreenFX_Bloom` class | `neo/d3xp/PlayerView.h:337-350`, `PlayerView.cpp:1664-1759` | Exists, calls `CopyRender` which is a no-op |
| `VK_RB_CopyRender()` | `neo/renderer/Vulkan/vk_backend.cpp:~4443` | **No-op stub** |
| Bloom shaders | (none) | **Missing** |
| Material files `textures/smf/bloom2/*` | (none in repo) | Pak asset only — not usable in Vulkan |

---

## Design

### Algorithm — Dual Kawase Blur

Dual Kawase is a good fit: fewer texture samples than Gaussian for equivalent quality,
naturally GPU-friendly, scales with resolution, and trivially implemented in compute.

```
Frame HDR image
       │
       ▼
[1] Threshold pass   → bright_0  (half resolution)
       │
       ▼
[2] Downsample chain → bright_1, bright_2, bright_3   (quarter, eighth, sixteenth)
       │
       ▼
[3] Upsample chain   → merged back to half resolution
       │
       ▼
[4] Composite pass   → final image (additive blend bloom onto HDR, then tonemapped)
```

Three shaders cover all four steps:
- `bloom_threshold.comp` — downsample + luminance threshold into half-res bright buffer
- `bloom_blur.comp`      — one Kawase downsample OR upsample step (dispatched multiple times)
- `bloom_composite.comp` — additive blend of blurred result onto the HDR framebuffer

### CVars

| CVar | Default | Description |
|------|---------|-------------|
| `r_rtBloom` | 0 | Enable bloom (0=off, 1=on) |
| `r_rtBloomThreshold` | 1.2 | Luminance threshold; pixels above this are extracted |
| `r_rtBloomStrength` | 0.5 | Additive blend weight of the blurred result |
| `r_rtBloomRadius` | 3 | Number of downsample/upsample steps (1–5) |

---

## Implementation Steps

### Step 1 — Shaders

**`neo/renderer/glsl/bloom_threshold.comp`**

- Input:  `rgba16f` storage image of the composited HDR frame (read-only)
- Output: `rgba16f` storage image at half resolution (the bright buffer mip 0)
- Logic:  sample the 2×2 neighbourhood around each half-res texel with a box filter;
  compute luminance (`dot(rgb, vec3(0.2126, 0.7152, 0.0722))`); if luminance exceeds
  `threshold`, write the colour, else write black.
- UBO: `BloomParams` — threshold, strength, texel sizes

**`neo/renderer/glsl/bloom_blur.comp`**

- Two modes via specialization constant `DOWNSAMPLE` (0/1)
- Downsample: 13-tap Kawase downsample (used by COD / Sledgehammer, widely documented)
- Upsample:   tent filter upsample that additively blends into the destination mip
- Input/output: two `rgba16f` storage images (src mip N → dst mip N+1 or vice versa)

**`neo/renderer/glsl/bloom_composite.comp`**

- Reads the HDR frame and the blurred bright buffer
- Writes: `HDR + bloom_buffer * strength` back to the HDR image
- Runs at full resolution; output is consumed by the existing tonemap pass

Add all three to `CMakeLists.txt` under `GLSL_SOURCES` and add the `.comp` files to
`GLSL_INCLUDES` for the SPIR-V compile step.

---

### Step 2 — Bloom Images

Add to `VkGlobals` (or a dedicated `VkBloomResources` struct in `vk_bloom.cpp`):

```cpp
// Bright-pass mip chain: mip 0 = half-res, mip N = (half >> N)
// Dual Kawase needs BLOOM_MIP_LEVELS = r_rtBloomRadius + 1 images (max 6).
static constexpr int BLOOM_MIP_LEVELS = 6;
VkImage     bloomImages[BLOOM_MIP_LEVELS];
VkImageView bloomViews[BLOOM_MIP_LEVELS];
VkDeviceMemory bloomMemory[BLOOM_MIP_LEVELS];
```

Create at swapchain init / resize; destroy on shutdown.  Format `VK_FORMAT_R16G16B16A16_SFLOAT`.

---

### Step 3 — C++ Pass (`vk_bloom.cpp` / `vk_bloom.h`)

New file: `neo/renderer/Vulkan/vk_bloom.cpp`

Public API:
```cpp
void VK_BloomInit();          // create pipelines, descriptor sets
void VK_BloomResize();        // recreate images when swapchain changes
void VK_BloomRender(VkCommandBuffer cmd, VkImageView hdrView);
void VK_BloomShutdown();
```

`VK_BloomRender` sequence:
1. Barrier: HDR image → `GENERAL` for compute read
2. Dispatch `bloom_threshold.comp` at half-res
3. Loop `r_rtBloomRadius` times: dispatch `bloom_blur.comp` (DOWNSAMPLE=1) at progressively smaller mips
4. Loop `r_rtBloomRadius` times in reverse: dispatch `bloom_blur.comp` (DOWNSAMPLE=0) upsampling back
5. Dispatch `bloom_composite.comp` at full res
6. Barrier: HDR image → `SHADER_READ_ONLY_OPTIMAL` for tonemap

Skip all dispatches if `r_rtBloom->GetBool() == false`.

---

### Step 4 — Wire into Backend

In `vk_backend.cpp`, after the RT composite pass and before tonemapping (or as the first
step of tonemapping if they share a pass):

```cpp
if (r_rtBloom && r_rtBloom->GetBool())
    VK_BloomRender(cmd, hdrImageView);
```

Also register `r_rtBloom` etc. in the settings menu (`Dhewm3SettingsMenu.cpp`) under a new
**Post-Processing** section, using the same two-column `ImGui::BeginTable` layout as the
Volumetric section.

---

### Step 5 — Settings Menu

Add to `RTCVars` struct:
```cpp
idCVar *rtBloom            = nullptr;
idCVar *rtBloomThreshold   = nullptr;
idCVar *rtBloomStrength    = nullptr;
idCVar *rtBloomRadius      = nullptr;
```

Add `DrawRTOptionsMenu` section **Post-Processing** with a checkbox for bloom enable and
sliders for threshold (0.5–3.0), strength (0.0–2.0), radius (1–5).

---

### Step 6 — Deferred / Optional

- **Lens dirt texture** — multiply bloom result by a screen-space dirt/scratch mask before
  composite for a cinematic look.  Low priority.
- **Anamorphic streaks** — directional horizontal blur on the brightest pixels only.
  Cosmetic, skip unless requested.
- **HDR tonemapping interaction** — ensure bloom is applied before the Reinhard/ACES
  tonemap so it participates in exposure correctly.  Check the existing tonemap pass order.

---

## File Checklist

| File | Action |
|------|--------|
| `neo/renderer/glsl/bloom_threshold.comp` | **Create** |
| `neo/renderer/glsl/bloom_blur.comp` | **Create** |
| `neo/renderer/glsl/bloom_composite.comp` | **Create** |
| `neo/renderer/Vulkan/vk_bloom.h` | **Create** |
| `neo/renderer/Vulkan/vk_bloom.cpp` | **Create** |
| `neo/renderer/Vulkan/vk_backend.cpp` | **Modify** — call `VK_BloomRender` after composite |
| `neo/framework/Dhewm3SettingsMenu.cpp` | **Modify** — add Post-Processing section |
| `CMakeLists.txt` | **Modify** — add shaders + vk_bloom.cpp |

---

## Notes

- The game-level `FullscreenFX_Bloom` / `Event_ToggleBloom` hooks can be left in place;
  they currently no-op because `VK_RB_CopyRender` does nothing.  They do not need to be
  removed or wired up — bloom will be controlled via CVars directly.
- Bloom operates on the half-resolution bright buffer so total dispatch cost is modest:
  three threshold + six blur + one composite = 10 passes over small images.
- Volumetric output feeds naturally into bloom: if `r_rtBloom` is on, bright fog shafts
  will glow at their peaks, reinforcing the atmospheric effect.
