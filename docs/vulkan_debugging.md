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

- **`VK_LAYER_LUNARG_crash_diagnostic`** — ships with the Vulkan SDK,
  purpose-built for device-lost/hang diagnosis (reports GPU progress at the
  point of failure). Not yet tried on this project; worth reaching for if
  GPU-AV output doesn't explain a device-lost.


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
- **`r_vkRTProfile`** — GPU/CPU phase timings; see Profiling below. For a
  device-lost, per-phase cost climbing steadily up to the failure points at a
  growing workload tripping the driver's hang detection, not corruption.
- **`r_vkRTReflDataDiag 2`** — validates material-table addresses/texture
  slots/base indices CPU-side and logs bad slots. Cheap, no layers needed.

## Profiling

Owned by `neo/renderer/Vulkan/vk_profile.cpp`; `vk_backend.cpp` keeps the
timestamp query pools and records raw numbers into it. **This section is the
profiler reference.** What the numbers mean for the engine lives in
`plans/20261010_vk_core_audit.md`; the raw captures in
`plans/notes/20261010_vk_core_baseline.txt`.

### The capture recipe

There is one. Don't hand-tune the groups filter — everything is always
collected and always written to the CSV.

```
com_fixedTic 1
r_vkRTProfileCSV mchq.csv
r_vkRTProfileWindow 60
r_vkRTProfile 1
```

Then, before reading any individual frame:

```
python tools/vk_profile_report.py mchq.csv
python tools/vk_profile_report.py after.csv --baseline before.csv
```

`com_fixedTic 1` unlocks the framerate. Doom 3 caps at **62.5 fps**, not 60:
`USERCMD_MSEC` is integer `1000/60 = 16` and `Session.cpp:3121` blocks one
render per tick. Capped, any GPU cost under ~16 ms is hidden and the pacing
wait splits between `Sys_WaitForEvent` and the upload fence. Uncapped,
`frame.backend` ≈ GPU time. Game time then advances 16 ms per *frame*
regardless of real time, so it is a measurement mode, not a play mode.

Two rules that come out of how the numbers are built:

- `frame.backend` includes blocking waits, so it is not CPU work. Subtract the
  `wait.*` tags before calling anything CPU-bound — CONDITIONS does this.
- A wait that tracks the **previous** frame's GPU cost is back-pressure, not a
  cost of whatever the wait is named after. Shortening it moves the stall
  elsewhere; it does not recover time while GPU-bound.

### Cvars

| Cvar | Default | What it does |
|---|---|---|
| `r_vkRTProfile` | 0 | 0=off, 1=window summaries, 2=one row per frame. Mode 2 floods the console; use a CSV instead |
| `r_vkRTProfileWindow` | 30 | Frames per summary window (1-256) |
| `r_vkRTProfileCSV` | "" | One CSV row per frame, ~130 columns, buffered and flushed at window close. **Also suppresses the console block** in favour of one heartbeat line per window |
| `r_vkRTProfileStats` | 29 | Console summary columns: 1=mean 2=p5 4=p50 8=p95 16=max |
| `r_vkRTProfileGroups` | 63 | **Console filter only**, never a collection switch: 1=phases 2=frame 4=upload 8=waits 16=memory 32=midframe. Every group is always collected and always in the CSV |
| `r_vkRTProfileSpikeMs` | 0 | Dump every metric for frames over this backend ms. Ignored while a CSV is open |
| `r_vkDebugLabels` | 0 | `VK_EXT_debug_utils` pass labels and object names, covering all 26 phases. Works at `r_vkRTProfile 0`, so an Nsight/RGP/RenderDoc capture is readable without the profiler running |

### What gets logged

| Group | Metrics | Answers |
|---|---|---|
| phases | `gpu.<phase>` and `cpu.<phase>` for 26 phases, plus `gpu.total`/`cpu.total` (RT) and `gpu.raster`/`cpu.raster` | Which pass costs what. `gpu.*` is the only group that needs query pools |
| frame | `fenceWait`, `acquire`, `drain`, `upload`, `submit`, `present`, `profilePrint`, `backend` (fence→present wall clock), `other` (backend minus every span), `game`, `frontend`, `findView`, `addLights`, `addModels` | Where backend time goes. A large `other` means a span is missing, not that the engine has mystery time |
| upload | `flushes`, `KB` staged, `staticBlocks`, `ring.draws`, ring vert/idx KB, data+UBO high-water, `overflows`, `vc.allocs/allocKB/allocMs/frees`, `blas.updates/rebuilds/tris/dynInst` | Per-frame copy volume; BLAS refit vs rebuild |
| waits | ms and count per tag across 40 wrapped blocking-wait sites in 20 files (`uploadFlush`, `image`, `accelStruct`, `ao`, `gi`, `giProbe`, `vol`, `bloom`, `gbuffer`, `temporal`, `reflect`, `tonemap`, `upscale`, `swapchain`, `readback`, `serialize`, `shutdown`, `other`) | Which wait blocks, and whether any run per frame |
| memory | `mem.rssMB` (`Sys_GetProcessMemoryBytes`), `mem.virtMemMB` (vertex-cache CPU copies), `mem.vramUsedMB`/`BudgetMB` via `VK_EXT_memory_budget` | Leaks across map loads; VRAM pressure |
| midframe | lazy image loads + ms, first-use pipeline builds + ms | Hitches from streaming or pipeline creation |
| lights | `lights.view`, `lights.shadow` (casters), `lights.batched`, `lights.serial` | Whether a capture met the shadow-dispatch condition at all |

Startup also prints `VK PROFILE LIMITS` (max memory allocations, max samplers).

### The CSV

`r_vkRTProfileCSV <name>.csv` writes through `fileSystem`, so the file lands in
the writable game dir — on Windows `Documents/My Games/dhewm3/base/<name>.csv`.
Format:

- One header row, then **one row per rendered frame**: `frame` followed by every
  registered metric, `%.4f`, in a fixed order. ~130 columns.
- The header comes from the full metric table, so **columns are stable across
  captures** even when a group is off — two CSVs are always diffable.
- Buffered in memory and flushed at window close and at shutdown. Setting
  `r_vkRTProfile 0` or clearing the cvar closes the file cleanly.
- Row count should equal frames run. A short file means it was still buffered;
  flush with `r_vkRTProfile 0`.

### Reading a capture: `tools/vk_profile_report.py`

Stdlib-only; takes a CSV and prints six sections:

| Section | What it answers |
|---|---|
| CONDITIONS | What the capture contains and what it therefore cannot speak to, plus GPU-bound vs CPU-bound from the backend with blocking waits removed |
| SHADOW DISPATCH | Pass cost bucketed by `lights.serial`. Serial shadow dispatch is billed to `gpu.Interactions`, so no single p50 can separate them |
| CONSISTENCY | `frame.backend` against the sum of its spans. A gap means a span is missing |
| SWINGS | Passes whose p95 is over 0.5 ms above their p50, labelled with the cause where it is testable: the measurement, frame pacing, or scene content |
| TIMES / COUNTERS | Percentile tables, alphabetical, split so times can be scanned without bytes and counts in the way |
| WORST | The N worst frames by a chosen column, all non-zero fields |

| Flag | Default | Use |
|---|---|---|
| `--baseline <csv>` | — | Per-column p50 diff instead of the full report. What a fix stage quotes for before/after |
| `--top N` | 0 | Worst-frame dumps; 0 skips them |
| `--sort-by <col>` | `frame.backend` | Column the worst-frame dump ranks on |
| `--floor <ms>` | 0.02 | Hide small fields in worst-frame dumps |
| `--group <substr>` | — | Filter the tables, e.g. `gpu.` or `wait.` |
| `--min-pct N` | 5 | Diff mode: hide moves under this percent |

### If you edit the profiler

`vk_profile.cpp/.h` owns the metric registry, window, CSV and debug labels;
`vk_backend.cpp` keeps the query pools and records raw numbers into it. Five
rules, each with a silent failure mode:

| Rule | Failure if broken |
|---|---|
| Gate new metrics on `VK_Prof_Active()`, not `VK_RTProfileEnabled()` (that one also needs RT support and a query pool) | No output at all on a non-RT GPU. Only `gpu.<phase>` needs the pools |
| A new CPU span between `present` and `backendMs` must be added to `accounted` | Its cost silently becomes `frame.other`, the one metric read as "unexplained" |
| One `PhaseEnd` per `VK_RTProfile_PhaseBegin` | With labels on, `vkEndCommandBuffer` fails. `VK_RTProfile_CloseStrayLabels` warns at both exits |
| Don't move `VK_Prof_BeginFrameSlot` or the slot clear that follows `CollectAndLog` | It is the tick that runs with profiling off (CSV open/close, partial-window discard); clearing later wipes the pre-command-buffer upload counters |
| `r_vkRTProfileGroups` filters the console only — never gate collection on it | A console filter turns a CSV column into zeros |

Deliberately not built: a live `vkAllocateMemory`/`VkSampler` ledger (134 call
sites for a number RMV, Nsight and the best-practices layer already report).
Startup prints the two device limits as `VK PROFILE LIMITS`, and the
`r_vkDebugLabels` object names let an RMV capture attribute allocations.

## External tools (Windows, free)

| Tool | GPU | Use |
|---|---|---|
| `tools/vk_profile_report.py <csv>` | any | First pass on any capture — see above |
| PresentMon | any | Frame-time CSV; hitches vs sustained low fps; CPU-busy vs GPU-busy. First tool for "it chugs here" |
| VS Performance Profiler or Very Sleepy | any | CPU sampling without instrumentation. Needs PDBs; `RelWithDebInfo` is the CMake default |
| Windows Performance Recorder / Analyzer | any | ETW: CPU samples, disk I/O, page faults, time blocked in driver calls. Load time, and hitches the sampler shows as "waiting" |
| Nsight Graphics, GPU Trace | NVIDIA | Per-pass GPU time including RT dispatches. Needs `r_vkDebugLabels` to be readable |
| RGP, RMV, RRA | AMD | RGP per-event GPU time; RMV VRAM by allocation; RRA BLAS/TLAS quality and opaque flags |
| RenderDoc | any | Raster draw inspection, overdraw, state per draw. **Captures are raster-only** — it does not support the RT extensions, so it hides them from the app and the engine comes up `(RT=no)`, with no TLAS and no RT passes. Fine for geometry-resubmission work, useless for RT. Use Nsight or RGP for anything RT |
| Validation via vkconfig: sync + best-practices | any | Best-practices flags small allocations and redundant work; sync covers GPU-GPU hazards. **Sync cannot see CPU writes to mapped memory.** Object-lifetime reporting fires at `vkDestroyDevice`, which a full `vid_restart` reaches — that is the leak check |

Tracy only if log spans stop scaling; it adds a dependency.


