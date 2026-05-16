# Phase 8.1 — Tonemapping Plan

Builds on the Uchimura suggestion in `tonemapping.md`.  Addresses the washed-out
volumetric look and the associated dynamic-range problem without requiring level redesign.

---

## Problem statement

The swapchain is `VK_FORMAT_B8G8R8A8_UNORM` — 8 bits per channel, clamped at 1.0.  GI
(`gi_composite.frag`) and volumetrics (`vol_composite.frag`) are both blended additively
into this LDR framebuffer with `ONE + ONE`.  Any combined value above 1.0 clips silently;
tonemapping applied *after* the fact cannot recover that information.

A tonemapper that reads the swapchain image and rewrites it would only see already-clamped
data and would accomplish nothing useful.  The fix requires an HDR intermediate buffer.

---

## Architecture: HDR intermediate buffer

Add a single `rgba16f` image (`vkHDRScene`) that replaces the swapchain image as the
colour attachment for all rendering passes.  A final resolve pass reads `vkHDRScene`,
applies the Uchimura tonemap, and writes to the swapchain.

```
[Doom 3 deferred scene]  ─┐
[GI composite — additive] ├──► vkHDRScene (rgba16f) ──► [tonemap.comp] ──► swapchain
[Vol composite — additive]─┘                                (Uchimura)
```

The HDR buffer carries full floating-point precision through all additive passes so the
tonemapper sees the true combined energy before any clamp.

---

## Step-by-step implementation

### Step 1 — HDR scene image (`vk_tonemap.cpp`)

New file `neo/renderer/Vulkan/vk_tonemap.cpp` (+ `vk_tonemap.h`).

Allocate `vkRT.hdrScene[VK_MAX_FRAMES_IN_FLIGHT]` (type `vkReflBuffer_t`) at
`rgba16f`, `COLOR_ATTACHMENT_BIT | STORAGE_BIT | SAMPLED_BIT | TRANSFER_DST_BIT`.

Public API (mirrors the vol/GI pattern):
```cpp
void VK_RT_InitTonemap(void);
void VK_RT_ShutdownTonemap(void);
void VK_RT_ResizeTonemap(uint32_t w, uint32_t h);
void VK_RT_DispatchTonemap(VkCommandBuffer cmd);
```

### Step 2 — Redirect render passes to HDR framebuffer

Currently `vk.swapchainFramebuffers[i]` is used as the colour attachment for the main
and resume render passes.  Add a parallel set `vk.hdrFramebuffers[i]` backed by
`hdrScene[i].view` using the same depth attachment and render-pass layout.

In `vk_backend.cpp`, replace `vk.swapchainFramebuffers[s_frameImageIndex]` with
`vk.hdrFramebuffers[vk.currentFrame]` in the `rpBegin` and `rpResume` structs.

The swapchain framebuffers are then only written by the tonemap resolve pass.

### Step 3 — Tonemap compute shader (`tonemap.comp`)

New file `neo/renderer/glsl/tonemap.comp`.

```glsl
#version 460
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform readonly image2D hdrIn;
layout(set = 0, binding = 1, rgba8)   uniform image2D ldrOut;   // swapchain storage view

layout(push_constant) uniform PC {
    float exposure;       // r_rtTonemapExposure  (default 1.0)
    float toeStrength;    // r_rtTonemapToe        (default 2.0 — aggressive for Doom 3)
    float linearStart;    // r_rtTonemapLinStart   (default 0.22)
    float linearLength;   // r_rtTonemapLinLen     (default 0.40)
} pc;

// Uchimura / Gran Turismo filmic curve.
// P=1 (display max), a=1 (contrast), b=0 (pedestal).
vec3 Uchimura(vec3 x) {
    float P = 1.0;
    float a = 1.0;
    float m = pc.linearStart;
    float l = pc.linearLength;
    float c = pc.toeStrength;

    float l0  = ((P - m) * l) / a;
    float S0  = m + l0;
    float S1  = m + a * l0;
    float C2  = (a * P) / (P - S1);
    float CP  = -C2 / P;

    vec3 w0 = 1.0 - smoothstep(vec3(0.0), vec3(m),      x);
    vec3 w2 = step(vec3(S0), x);
    vec3 w1 = 1.0 - w0 - w2;

    vec3 T = m * pow(max(x / m, 0.0), vec3(c));   // toe  — crushed blacks
    vec3 L = m + a * (x - m);                      // linear section
    vec3 S = P - (P - S1) * exp(CP * (x - S0));    // shoulder — compressed highlights

    return T * w0 + L * w1 + S * w2;
}

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dims  = imageSize(ldrOut);
    if (coord.x >= dims.x || coord.y >= dims.y) return;

    vec3 hdr    = imageLoad(hdrIn, coord).rgb * pc.exposure;
    vec3 mapped = Uchimura(hdr);
    imageStore(ldrOut, coord, vec4(mapped, 1.0));
}
```

The `toeStrength` (`c`) parameter is the key control from `tonemapping.md`.
- `c = 1.0` — linear toe (neutral, no crushing)
- `c = 2.0` — default: moderately crushes blacks, restores Doom 3 contrast
- `c = 3.0+` — very dark game; usable for areas with no ambient fill

### Step 4 — Resolve pipeline (`vk_tonemap.cpp`)

Single compute pipeline:
- 2-binding descriptor layout: `STORAGE_IMAGE` (hdrIn) + `STORAGE_IMAGE` (ldrOut)
- 16-byte push constant: `{exposure, toeStrength, linearStart, linearLength}`
- Dispatch: `ceil(W/8) × ceil(H/8)` groups

`ldrOut` requires a **storage view** of the swapchain image.  Swapchain images support
`VK_IMAGE_USAGE_STORAGE_BIT` on most desktop GPUs; this must be requested at swapchain
creation time by adding `VK_IMAGE_USAGE_STORAGE_BIT` to `swapInfo.imageUsage` in
`vk_swapchain.cpp`.  If the physical device does not support storage writes to the
swapchain format, fall back to a blit from a separate resolve image.

### Step 5 — Backend wiring (`vk_backend.cpp`)

After `VK_RT_DispatchVolBilateral` and before `vkCmdBeginRenderPass(rpResume)`:

```cpp
// Barrier: all additive composites finished → tonemap reads hdrScene
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
VK_SetRenderStage("RT_Tonemap");
VK_RT_DispatchTonemap(cmd);
```

The resume render pass (HUD, 2D) then runs against the swapchain directly (or against a
separate UNORM surface if the HUD needs to overdraw after tonemap).

### Step 6 — CVars

```cpp
idCVar r_rtTonemap           ("r_rtTonemap",            "1",    CVAR_RENDERER | CVAR_BOOL,  "Enable HDR resolve with Uchimura tonemapping.");
idCVar r_rtTonemapExposure   ("r_rtTonemapExposure",    "1.0",  CVAR_RENDERER | CVAR_FLOAT, "Exposure scale applied before tonemapping.");
idCVar r_rtTonemapToe        ("r_rtTonemapToe",         "2.0",  CVAR_RENDERER | CVAR_FLOAT, "Toe strength (c). 1=linear, 2=Doom-dark, 3+=crushed blacks.");
idCVar r_rtTonemapLinStart   ("r_rtTonemapLinStart",    "0.22", CVAR_RENDERER | CVAR_FLOAT, "Luminance at which the linear section begins.");
idCVar r_rtTonemapLinLen     ("r_rtTonemapLinLen",      "0.40", CVAR_RENDERER | CVAR_FLOAT, "Length of the linear section.");
```

### Step 7 — CMakeLists.txt

Add `renderer/glsl/tonemap.comp` to `GLSL_INCLUDES`.
Add `renderer/Vulkan/vk_tonemap.cpp` to the source list.

---

## Blob / fill-light fixes (from tonemapping.md §2)

These are independent of the tonemapping pass but address the remaining point-light
volume artifacts.

### 7a — Skip `noShadows` fill lights

In the GI light upload loop (`vk_gi.cpp` ~line 977), add:

```cpp
if (p.noShadows)   // pure ambient fill — no volumetric contribution
    continue;
```

Doom 3 uses `noShadows` lights extensively as cheap ambient lifts.  They produce the
"floating blob" effect because they have large radii and no shadow occlusion path.

### 7b — Soft-start fade near light origin

In `vol_march.comp`, after computing `atten` for point lights (lt == 0), multiply by a
fade that goes to zero at the light centre:

```glsl
// Prevent singularity glow at the light origin.
float coreFade = smoothstep(0.0, sphereRad * 0.15, dist);
atten *= coreFade;
```

This is lighter than a CauchyRamp (which caused visible spherical halos previously)
because it only attenuates the innermost 15% of the light radius.

---

## Files created / modified

| File | Change |
|---|---|
| `neo/renderer/Vulkan/vk_tonemap.h` | new — HDR buffer and pipeline declarations |
| `neo/renderer/Vulkan/vk_tonemap.cpp` | new — HDR alloc, tonemap pipeline, dispatch |
| `neo/renderer/glsl/tonemap.comp` | new — Uchimura compute shader |
| `neo/renderer/Vulkan/vk_swapchain.cpp` | add `STORAGE_BIT` to swapchain usage |
| `neo/renderer/Vulkan/vk_backend.cpp` | redirect scene to HDR fb; insert tonemap dispatch |
| `neo/renderer/Vulkan/vk_gi.cpp` | skip `noShadows` lights |
| `neo/renderer/glsl/vol_march.comp` | soft-start coreFade for point lights |
| `neo/CMakeLists.txt` | register `tonemap.comp` |

---

## Tuning starting point

| CVar | Value | Effect |
|---|---|---|
| `r_rtTonemapToe 2.0` | default | Moderate black crush; GI fills don't lift darks visibly |
| `r_rtTonemapToe 2.5` | darker game | Restores near-black areas in Doom 3 corridors |
| `r_rtTonemapExposure 0.85` | slightly underexposed | Dims scene before curve; reduces overall brightness lift |
| `r_rtTonemapLinStart 0.18` | lower crossover | More of the range handled by the toe curve |
| `r_rtVolStrength 0.1` | lower | Reduce point-light ambient scatter independently |
