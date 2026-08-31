# Debugging Vulkan on this project

How to get the Vulkan validation layer (including GPU-Assisted Validation) to
actually produce output for `dhewm3_rt.exe`, plus the engine's own built-in
diagnostics. Written up after the 2026-08-29 AMD device-lost investigation
(`docs/plans/amd_vulkan_cleanup.md`), where getting this working was most of
the battle.

## The one thing that matters: don't use stdout redirection

`dhewm3_rt.exe` is a GUI-subsystem app. PowerShell's `*>` output redirection
is **unreliable** for this — it can silently produce an empty file even when
the validation layer is loading and generating errors. Confirmed on this
project: both a manual run and an independent same-machine test both produced
0-byte files this way.

**Use the validation layer's own file-logging setting instead.** It writes
directly to a named file, bypassing the app's stdio entirely.

## Setup

1. Requires the LunarG Vulkan SDK installed (provides `VK_LAYER_KHRONOS_validation`
   and diagnostic tools). Check with `vulkaninfo --summary` — it lists
   `Instance Layers` if present.

2. Create `vk_layer_settings.txt` in the **same directory as the exe**
   (e.g. `build_rt\RelWithDebInfo\`):

   ```
   khronos_validation.gpuav_enable = true
   khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG
   khronos_validation.log_filename = C:\full\path\to\build_rt\RelWithDebInfo\vklog.txt
   ```

   Use `khronos_validation.gpuav_enable` (not the deprecated
   `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` env var —
   it still works but the layer nags about it being deprecated and mixing old
   and new settings is unsupported). `log_filename` is an absolute path;
   relative paths resolve against whatever the process's CWD happens to be,
   which is not reliably the exe's directory.

3. Launch with the layer forced on:

   ```powershell
   $env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
   & ".\dhewm3_rt.exe"
   ```

   `VK_INSTANCE_LAYERS` forces the layer into the instance regardless of
   whether the app itself requests it — no engine changes needed.

4. **Delete/truncate `vklog.txt` before each run** — the layer appends, it
   doesn't overwrite. Check it a few seconds after launch (instance/device
   creation happens immediately): if it's still empty, the layer isn't
   loading — see Gotchas below before assuming anything about the app.

5. Expect the game to run *much* slower with GPU-AV on (single-digit fps is
   normal) — it instruments every shader. Core validation alone (without
   `gpuav_enable`) is far cheaper if you just need API-usage checks, not
   in-shader bounds checks.

## Gotchas

- **Empty log, instant exit, code 0**: another copy of the game is probably
  already running. dhewm3 appears to refuse a second instance and exit
  silently rather than erroring. Close every existing window first.
- **Deprecated-setting warning at the top of the log**: harmless, but means
  you're mixing `VK_LAYER_ENABLES` (env var, old) with settings-file keys
  (new) — pick one scheme, prefer the settings file.
- GPU-AV and plain Core Validation running together is supported but the
  layer will warn it's slow and recommends fixing Core Validation errors
  first, then disabling it and running GPU-AV alone for anything Core misses.

## What to look for in the log

Real examples from this project's investigation, roughly in the order they're
worth checking for:

- **`VUID-VkShaderModuleCreateInfo-pCode-0874x`** — a shader declares a
  SPIR-V capability/extension the *device* wasn't created with (missing
  feature/extension at `vkCreateDevice`, not a shader bug). Caught a real bug
  this way: `vol_march.comp` used `rayQueryEXT` but `VK_KHR_ray_query` /
  `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery` were never requested in
  `vk_instance.cpp` (fixed — see `amd_vulkan_cleanup.md` A9). Check this any
  time a new GLSL `#extension` is added without a matching change in
  `vk_instance.cpp`'s device extension/feature lists.
- **`VUID-RuntimeSpirv-PhysicalStorageBuffer64-11819`** ("Out of bounds:
  trying to read N bytes at [...] but no buffer device address was found at
  this range") — **only GPU-AV catches this**, not plain validation. A
  `buffer_reference` (`GL_EXT_buffer_reference`) pointer read past the end of
  its buffer. Tells you the exact stage (`Stage = Closest Hit`, etc.), the
  `Global Launch ID`, and the SPIR-V instruction. Caught a real bug this way:
  `rt_material.glsl` indexed the triangle index buffer by `primId` with no
  bounds check before the read (fixed — see `amd_vulkan_cleanup.md` A10).
  This is the class of bug plain validation and `robustBufferAccess`
  **cannot** see at all — it's the whole reason to run GPU-AV specifically.
- **`VUID-vkCmd*-commandBuffer-recording`** ("was called in VkCommandBuffer
  ... invalid state ... because ... VkDescriptorSet ... was destroyed or
  updated without UPDATE_AFTER_BIND") — usually fallout from something
  earlier in the same submission going wrong (e.g. a shader module that
  failed to create cleanly), not a separate bug. If this shows up alongside
  one of the two errors above, fix that first and see if the cascade
  disappears before chasing this separately.
- **`WARNING-Setting-Limit-Adjusted`** at startup ("Forcing X to VK_TRUE") —
  informational; GPU-AV needs certain features itself and turns them on for
  you. Not a bug in this codebase.

## This project's own diagnostics (no external tools needed)

- **`r_vkLogRT <n>`** — verbose per-frame prints from most RT dispatch
  functions (`0`=off, `1`=on). Look for lines like
  `VK RT GI Atrous: N passes slot=... final=...` or
  `VK RT Vol Temporal: camera cut ...`.
- **`r_vkSplitSubmitMask <bitmask>`** — forces extra `vkQueueSubmit` calls
  mid-frame to bisect which stage a `VK_ERROR_DEVICE_LOST` happened in. Add
  bits together. Requires `r_vkLowPerturbationMode 0` (it suppresses the
  probes otherwise). Current bits:

  | Bit | Value | Splits after |
  |---|---|---|
  | 0 | 1 | RT block |
  | 1 | 2 | Interactions |
  | 2 | 4 | Shader passes |
  | 3 | 8 | Fog |
  | 4 | 16 | TLAS rebuild |
  | 5 | 32 | AO |
  | 6 | 64 | Reflections |
  | 7 | 128 | RT render-pass resume (after GI temporal/à-trous/volumetrics/composite-setup) |
  | 8 | 256 | GI dispatch (outside render pass) |
  | 9 | 512 | GI composite (inside render pass) |
  | 10 | 1024 | GI temporal+à-trous, before volumetrics starts |

  `1023` = all of bits 0-9. On a device-lost, the log prints
  `VK: recent stage breadcrumbs` — the last stage name before the failed
  submit narrows the fault window to whatever ran between that split point
  and the previous one.
- **`r_vkRTProfile 1`** (`r_vkRTProfileLogEvery` controls print frequency) —
  periodic GPU phase timings. Useful for telling a real correctness bug
  apart from a GPU timeout/TDR: if per-phase cost climbs steadily over a
  play session right up to a device-lost, suspect a growing workload (more
  admitted lights/entities) tripping the driver's hang detection, not memory
  corruption.
- **`r_vkRTReflDataDiag 2`** — validates material-table addresses/texture
  slots/base indices CPU-side and logs bad slots. Cheap, no layers needed.
- **`VK_LAYER_LUNARG_crash_diagnostic`** — ships with the Vulkan SDK,
  purpose-built for device-lost/hang diagnosis (reports GPU progress at the
  point of failure). Not yet tried on this project; worth reaching for if
  GPU-AV output doesn't explain a device-lost.
