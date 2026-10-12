# Vulkan Core Audit: buffers, uploads, raster path, RT uploads

Code review only, no telemetry, no edits made. Part 1 (F1-F13, stages A0-A5): `vk_buffer.cpp`, upload batch in `vk_instance.cpp`, `vk_image.cpp`, raster draw path in `vk_backend.cpp`, `VertexCache.cpp`, `Vulkan/VKBackend.cpp`. Part 2 (R1-R10, stages B0-B4): `vk_accelstruct.cpp`, `vk_material_table.cpp` and the buffers RT passes read.
Feeds Phase 6 of `20261010_cleanup_optimization.md`. Each finding is a code-read claim; Stage 0 and A0/B0 turn them into numbers before any fix.

Gate: no fix stage (A1-A5, B1-B4) starts until Stage 0 shows its finding on a chugging section. A stage whose counter reads near zero is dropped, not deferred.

## Findings

| # | Sev | Finding | Where | Effect |
|---|---|---|---|---|
| F1 | High | Vulkan forces `r_useIndexBuffers 0`, so `geo->indexCache` is always NULL. Every draw `memcpy`s the whole index array into the per-frame data ring. TAG_TEMP vertices (every dynamic entity) are copied per draw too | `VertexCache.cpp` (virtualMemory block), `vk_backend.cpp:1587-1620` (interaction), 11 `VK_AllocDataRing` sites | A skinned mesh lit by 4 lights = 5 vertex + 5 index copies per frame. CPU memcpy + 64 MB x2 host-visible rings. Cost unmeasured: bounded by data-ring bytes per frame (A0). Main value is unblocking B1 |
| F2 | High | Upload batch ends in a synchronous submit + `vkWaitForFences`, run at the start of every frame that has pending uploads | `vk_instance.cpp:617-678`, `vk_backend.cpp:4783` | CPU waits for the upload, which queues behind the previous in-flight frame. Any frame with a new static block (moving lights, new interactions) loses CPU/GPU overlap |
| F3 | High | Lazy image load during command recording: `VK_Image_GetDescriptorInfo` calls `ActuallyLoadImage` (disk + decode) then `VK_FlushPendingUploads()` per image | `vk_image.cpp:1253-1282`, `1296-1325` | One blocking submit+wait per unloaded image, mid-frame. This is the "one-by-one" pattern |
| F4 | High | Per-image Vulkan objects: one `vkAllocateMemory` for the image, one staging `vkCreateBuffer`+`vkAllocateMemory`, one `VkSampler`. No suballocation, no sampler cache | `vk_image.cpp:692-892`, `936-1081` | Device limits are typically 4096 allocations and ~4000 samplers; a level with thousands of images plus 512 pending staging buffers approaches both. Slow loads. `vkAllocateMemory` is a kernel call |
| F5 | Med | Static vertex blocks keep a CPU copy forever (`virtMem` from `idVertexCache::Alloc`) after the GPU upload | `VertexCache.cpp:345`, freed only in `VK_VertexCache_Free` | All static geometry resident twice (RAM + VRAM) |
| F6 | Med | Interaction draw pushes 10 descriptors and sets scissor, depth bias, cull mode every draw with no "same as last" check | `vk_backend.cpp:1556`, 8 push sites | Driver-side CPU per draw; scales with draws x lights |
| F7 | Low | Shadow draw scans all indexes for the max index when shadow verts are CPU-side (a second scan runs only under `r_vkLogShadowGeom` or a branch change) | `vk_backend.cpp:3601-3642` | O(indexes) per CPU-side shadow draw. Zero with `r_rtShadows 1` (default): shadow volumes are not generated (`Interaction.cpp:1060`) |
| F8 | Med | Ring overflow is silent in effect: UBO ring clamps to the last slot (wrong data drawn), data ring returns `VK_WHOLE_SIZE` and the draw is skipped. One warning per frame | `vk_backend.cpp:931-943`, `1177-1192` | Visual glitches at high light/surface counts with no high-water mark to size against |
| F9 | Low | New command buffer allocated and freed on every upload flush | `vk_instance.cpp:563-587`, `554-561` | Driver alloc per frame with uploads |
| F10 | Low | Image upload does not `VK_CHECK` staging `vkCreateBuffer`/`vkAllocateMemory`/`vkMapMemory`; later failure paths destroy the image after copies were already recorded into the open batch | `vk_image.cpp:728-743`, `864-884` | OOM = null-handle crash, or a poisoned batch |
| F11 | Low | Vertex-cache best-fit scans every free range in every chunk; `Insert` into the sorted list is O(n) | `vk_buffer.cpp:326-399` | Fine today; grows with fragmentation |
| F12 | Info | All textures are `R8G8B8A8_UNORM`; compressed sources are expanded | `vk_image.cpp:673` | 4-8x VRAM and upload bandwidth versus BC1/BC3/BC7. A feature, not a bug |
| F13 | Info | Garbage slot choice (`currentFrame-1`) is correct for frontend frees. Frees during backend recording would land one slot early. Current backend frees (`vk_backend.cpp:4680`) happen before the frame opens | `vk_buffer.cpp:589` | Verify only; add an assert in debug builds |

Already good: chunk sub-allocator with generation guard, garbage rings with overflow lists, 16 MB staging ring for buffers, batched `VK_BeginSingleTimeCommands`.

## Stage 0 — Triage with existing logging (no code)

Capture: `r_vkRTProfile 2` (every frame) through a section that chugs, plus Mars City. Run PresentMon alongside for the frame-time trace.

| Reading (`VK FRAME PROFILE` / `VK CPU PROFILE` / `VK RT PROFILE`) | Means | Next |
|---|---|---|
| Frame time high and steady, `fenceWait` large | GPU-bound | Feature differential (toggle each RT feature, diff frame ms), render-scale sweep 0.5/0.75/1.0, GPU profiler. Skip A1, A5, B3 |
| Spikes with `upload=` large on the bad frames | Blocking uploads | A2, F3 part first |
| `frontend=` or `game=` large | Not the backend | Sampling profiler; out of scope here, see X1-X2 |
| `backend=` large, sum of per-phase CPU ms small | Unattributed backend time | Sampling profiler on `VK_RB_DrawView`; add spans |
| TLAS CPU ms large | Gather loop / material table | B3 |
| Spikes, none of the above | Pipeline create, lazy image load, allocation | A0c spike line + A0 midframe counters |
| `VK VC SITES` shows `shadowVP`, `shadowPriv`, `indexShadow` or `lighting` non-zero with `r_rtShadows 1` | Wasted frontend allocs | X3 |
| `r_vkSerializeFrames 1` removes shading flicker on skinned meshes | R2 confirmed | B1 |

Check: `notes/20261010_vk_triage.txt` holds the log excerpts and one line per row above (yes / no / value). It names the stages that survive.

## Tooling (Windows, free)

| Tool | GPU | Use | Notes |
|---|---|---|---|
| PresentMon | any | Frame-time CSV; separates hitches from sustained low fps; CPU-busy vs GPU-busy | First tool for "it chugs here" |
| Visual Studio Performance Profiler (Community) or Very Sleepy | any | CPU sampling, per-function time without instrumentation | Needs PDBs; `RelWithDebInfo` is the CMake default |
| Windows Performance Recorder / Analyzer (Windows ADK) | any | ETW: CPU samples, disk I/O, page faults, time blocked in driver calls | For load time and for hitches the sampler shows as "waiting" |
| Nsight Graphics, GPU Trace | NVIDIA | Per-pass GPU time including RT dispatches | Needs debug labels (A0a) to be readable |
| Radeon Developer Tool Suite: RGP, RMV, RRA | AMD | RGP per-event GPU time; RMV VRAM by allocation (F4, R4); RRA BLAS/TLAS quality and opaque flags (R3, X6) | RRA is the direct check for B2 |
| RenderDoc | any | Raster draw inspection, overdraw, state per draw (F6) | Confirm RT capture support in the installed version |
| Vulkan SDK validation via vkconfig: sync + best-practices | any | Best-practices flags small allocations and redundant work; sync covers GPU-GPU hazards | Sync cannot see CPU writes to mapped memory (R2) |

Tracy (free) only if log spans stop scaling; it adds a dependency.

## Stage A0 — Close logging gaps (no behaviour change)

Already logged under `r_vkRTProfile`: `fenceWait`, `acquire`, `drain`, `upload`, `submit`, `present`; per-phase CPU and GPU ms; `game`/`frontend`/`backend`; vertex-cache allocs by site; BLAS updates/rebuilds.

Two defects in what exists, both fixed by A0b:

- `r_vkRTProfile 1` logs one frame in `r_vkRTProfileLogEvery` (30) and discards the other 29 —
  a point sample, not a mean (`vk_backend.cpp:717`). A hitch is logged only if it lands on
  frame ≡ 0 mod N. Mode 2 floods.
- The `VK CPU PROFILE` line reads `tr.pcLastFrame`, a different frame from the GPU line above
  it (`vk_backend.cpp:758`).

### Substages — **code landed 2026-10-11, not yet run in-game**

New module `vk_profile.cpp/.h` owns the metric registry, window, CSV and labels;
`vk_backend.cpp` keeps the query pools and records raw numbers into it.

| # | Change | Where | Check |
|---|---|---|---|
| A0a | `vkCmdBeginDebugUtilsLabelEXT`/`End` from `VK_RTProfile_PhaseBegin/End`, covering all 26 phases from one name table. Pushed **before** the profiling gate and popped before the eventIdx check, so a capture is readable at `r_vkRTProfile 0`. `r_vkDebugLabels`; `VK_EXT_debug_utils` now enabled whenever the loader has it, not only under validation | `vk_profile.cpp`, `vk_backend.cpp` PhaseBegin/End, `vk_instance.cpp` | Nsight/RGP timeline shows named passes with `r_vkRTProfile 0`; no validation error about unbalanced labels |
| A0b | Window aggregator: every frame accumulates into a ring of `r_vkRTProfileWindow` (30); at close prints a `mean/p5/p50/p95/max` block, one metric per line, omitting metrics zero across the window. Column set is `r_vkRTProfileStats` (default mean/p50/p95/max). `pcLastFrame` snapshotted at frame open, fixing the two-frame skew | `VK_RTProfile_CollectAndLog` | Same corner captured twice: p50 within 5%; the 52-field line is gone |
| A0c | Spike line: frames whose `frame.backend` exceeds `r_vkRTProfileSpikeMs` dump every non-zero metric immediately, outside the window cadence | `vk_profile.cpp` | A forced stall emits a line; steady state emits none |
| A0d | CSV: `r_vkRTProfileCSV <path>`, one row per frame, all ~130 columns, buffered and flushed at window close and at shutdown | `vk_profile.cpp` | Row count == frames run; p50 recomputed from the CSV matches the console |
| A0e | `r_vkRTProfileGroups` bitmask (phases/frame/upload/waits/memory/midframe) replaces a cvar per group | `vk_profile.cpp` | Each bit toggles its rows |

### Counters landed

| Group | Metrics | Answers |
|---|---|---|
| phases | `gpu.<phase>`, `cpu.<phase>` x26, plus gpu/cpu total and raster | the RT budget table |
| frame | fenceWait, acquire, drain, upload, submit, present, **backend** (fence→present wall clock), **other** (backend minus every span), game, frontend, findView, addLights, addModels | unattributed CPU, X1 |
| upload | flushes, KB staged, staticBlocks, draws, ring vert/idx KB, data+UBO high-water, overflows, vc allocs/KB/ms/frees, BLAS updates/rebuilds/tris/dynInst | F1, F2, F8, X3 |
| waits | ms and count per tag across 40 wrapped sites in 20 files (uploadFlush, image, accelStruct, ao, gi, giProbe, vol, bloom, gbuffer, temporal, reflect, tonemap, upscale, swapchain, readback, serialize, shutdown, other) | F2, F3 |
| memory | `mem.rssMB` (new `Sys_GetProcessMemoryBytes`), `mem.virtMemMB` (new `vertexCacheVirtMemBytes`), `mem.vramUsedMB`/`BudgetMB` via `VK_EXT_memory_budget` | F5, F12 |
| midframe | lazy image loads + ms (one `VK_Image_LazyLoadTimed` for all four sites), first-use pipeline builds + ms | F3, X4 |

### Deliberately not done

| Item | Why |
|---|---|
| Live `vkAllocateMemory` / `VkSampler` ledger (F4, R4) | 134 call sites across 19 files for a number RMV, Nsight and the best-practices layer already report. Startup prints the two device limits (`VK PROFILE LIMITS`); A0a's object names make an RMV capture attribute them. Only the vertex-cache chunks are named so far |
| Draw-state pushes and redundant state sets | Raster CPU measured 0.4-1.6 ms on 2026-10-11, which demoted A5. X7 (draws and triangles per pass) comes from an A0a capture instead |
| `r_vkRTProfileLogEvery` | Removed; `r_vkRTProfileWindow` replaces it. Old configs setting it just create an unused cvar |

B0's counters belong to the same groups — A0a/A0b are in, so B0 can follow in the same session.
New cvars are debug-only, listed in `../Dhewm3-AI-RT-Settings.md`, no settings-menu entry.

Check: Mars City and one chugging section written to `notes/20261010_vk_core_baseline.txt` as
window summaries plus the CSV, ranked by measured cost. The order of A1-A5 may change.

## Stage A1 — Stop re-copying geometry (F1, F7)

| Change | Detail |
|---|---|
| Static index buffers on GPU | Allow `indexCache` under Vulkan: allocate non-TAG_TEMP index blocks via `VK_VertexCache_Alloc(..., true)`; draws bind chunk buffer + offset like vertices |
| Dynamic geometry once per frame | Cache `(vertCache_t* / indexes ptr) -> ring offset` for the frame; passes and lights reuse it |
| Shadow geometry | Out of scope: shadow indexes stay on the CPU upload path (`vk_backend.cpp:3679`). Max-index caching only if stencil shadows show up in A0 |
| Keep CPU fallback | Used only when `GetBuffer` fails |
| A/B switch | Debug cvar selecting GPU vs ring indexes until screenshots match on all baseline maps |

Check: bytes copied per frame drops to dynamic-geometry size; `VK CPU PROFILE` DrawView ms drops; screenshots identical; validation clean.

## Stage A2 — Remove blocking uploads (F2, F3, F9)

Do the lazy-load rows first. The async flush only if A0 shows `upload=` above ~0.5 ms in steady state; when GPU-bound the wait only moves time out of the next fence wait.

| Change | Detail |
|---|---|
| Async flush in gameplay | Submit the upload batch without waiting. Staging is retired when its frame slot's fence signals (staging ring per slot). A `vkCmdPipelineBarrier` at the start of the frame command buffer covers earlier-submitted transfers |
| Sync flush only on load | Level load and shutdown keep today's wait |
| No mid-frame lazy load | At level load, load every image the level references; in-game miss draws the fallback image and queues a background load applied at the next frame start |
| Reuse the upload command buffer | One per slot, reset instead of free/alloc |

Check: steady-state `uploadMs` ~0; no frame has a flush wait unless a level is loading; sync validation clean; no pop-in beyond the fallback frame.

## Stage A3 — Cut per-image object counts (F4, F10)

| Change | Detail |
|---|---|
| Sampler cache | Key on (filter, wrap, anisotropy, mip count, lod bias); images share. Rebuild on cvar change touches a handful, not thousands |
| Staging for textures | Use the staging ring (or a second, larger one) instead of one buffer+memory per texture |
| Image memory pools | Suballocate device-local image memory from large blocks per memory type; respect alignment and `bufferImageGranularity`; free by range after the garbage drain |
| Check results | `VK_CHECK` or a clean failure path on every create in `VK_Image_Upload` / `UploadCubemap` |

Check: live `vkAllocateMemory` < 200 and samplers < 100 after a level load; load time not worse; texture screenshots identical; FSR mip-bias change still rebuilds correctly.

## Stage A4 — Residency (F5, F12)

| Change | Detail |
|---|---|
| Drop `virtMem` after a successful GPU upload for non-TAG_TEMP blocks | First audit every `vertexCache.Position()` caller; fallback paths in the shadow draw need `GetBuffer` to be reliable |
| BC compression (separate decision) | Only if A0 shows VRAM pressure |

Check: RSS drops by roughly the static geometry size; shadow, mirror and GUI screenshots identical.

## Stage A5 — Draw state and guard rails (F6, F8, F11, F13)

| Change | Detail |
|---|---|
| Skip redundant state | Track last pipeline, descriptor image set, scissor, depth bias, cull mode; skip identical sets. Push descriptors only when material bindings change |
| Ring sizing | Replace the silent clamp with high-water tracking; size from A0 data; on overflow drop the draw and count it, never reuse a live slot |
| Debug asserts | Buffer free during recording asserts in debug builds |

Check: pushes per frame and DrawView CPU ms drop; no visual change; overflow counters stay 0 on all baseline maps.

---

# Part 2: RT upload paths (acceleration structures, material table)

Scope: `vk_accelstruct.cpp` (BLAS/TLAS), `vk_material_table.cpp`, and the host-visible buffers the RT passes read.

## RT findings

| # | Sev | Finding | Where | Effect |
|---|---|---|---|---|
| R1 | High | The BLAS "zero-copy" path needs both `ambientCache` and `indexCache` on the GPU. `indexCache` never exists under Vulkan (F1), so every BLAS geometry takes the fallback: its own host-visible vertex buffer (full 60-byte `idDrawVert`) and index buffer, 2 `vkAllocateMemory` + `memcpy` per geometry | `vk_accelstruct.cpp:520-523`, `766-769`, `1005-1037` | AS builds and every hit-shader vertex fetch (by device address) read system RAM over PCIe. Per-frame cost for dynamic entities: `vkMapMemory` + full copy + `vkUnmapMemory` per geometry (`876-882`). The `VK RT BLAS SRC gpuPct` log line should read ~0% today |
| R2 | Med (verify) | Dynamic-entity BLAS geometry buffers and the AS itself are single-copy and updated in place every frame, while 2 frames are in flight. Frame N's CPU `memcpy` and `MODE_UPDATE` can overlap frame N-1's trace and hit-shader vertex reads. The TLAS and material tables are per slot, the BLAS is not. No barrier at frame start orders N-1's trace reads before N's AS write | `vk_accelstruct.cpp:847-930`, `1301-1311` | Most likely symptom: one-frame normal/position mismatch on skinned entities under load. Sync validation does not see the CPU `memcpy`; confirm with an `r_vkSerializeFrames 1` A/B |
| R3 | Med | Every BLAS is built `ALLOW_UPDATE` and keeps its scratch buffer for life, including static world and model BLASes that never update. Dynamic BLASes use `PREFER_FAST_TRACE` and refit in place with no periodic rebuild | `vk_accelstruct.cpp:635`, `908-911`, `1068`, `1099` | Larger, slower-to-trace AS for static geometry; scratch (about the size of the AS) resident forever; slow per-frame updates and degrading refit quality for skinned meshes |
| R4 | Med | Per BLAS: AS buffer, scratch buffer, plus 2 per geometry in the fallback = at least 4 `vkAllocateMemory`. Same allocation-limit exposure as F4 | `AllocASBuffer`, `vk_accelstruct.cpp:474-499` | Hundreds of cached model BLASes approach the device allocation limit; slow level load |
| R5 | Med | Every frame, for every static instance and geometry (up to 4096 / 16384), the TLAS loop calls `VK_RT_MakeMaterialEntry` (stage walk plus up to 3 linear scans of the bindless table, up to 4096 slots) and hashes the signature, even when nothing changed | `vk_accelstruct.cpp:1670-1695`, `vk_material_table.cpp:126-154`, `635-726` | Worst case millions of compares per frame, pure CPU. The result is only needed when `rewriteStatic` is true |
| R6 | Med | Bindless descriptor rebuild rewrites every assigned slot (up to 4096) per frame slot whenever anything is dirty. Dirty is set by any image upload or purge anywhere (`VK_Image_ChangeCounter`) and by any new texture. New slots can also lazy-load an image and flush (F3) | `vk_material_table.cpp:100-236` | Thousands of descriptor writes per event, twice (once per frame slot); stalls during streaming |
| R7 | Med | Host-written, GPU-read buffers all request `HOST_VISIBLE\|COHERENT`. `VK_TryFindMemoryType` returns the first matching type, normally system RAM, not the device-local BAR type | material/address SSBOs (`vk_material_table.cpp:276-285`), GI/vol light SSBOs (`vk_gi.cpp:655-670`), emitters, SBTs, data ring, UBO ring, TLAS instances, BLAS fallback geometry | Per-hit material fetches and vertex reads cross PCIe. Machines with Resizable BAR get VRAM-speed reads by preferring `DEVICE_LOCAL\|HOST_VISIBLE`. Size of the gain unknown; B0 A/B decides. R1's read-cost claim depends on the same result |
| R8 | Low | TLAS instance buffer, TLAS storage and TLAS scratch are sized exactly to the current count and destroy+recreated (3 allocations) whenever the count grows. Instance buffer is mapped and unmapped every frame | `vk_accelstruct.cpp:1918-1956`, `2187-2227`, `1971`, `2006` | Churn while instance count fluctuates; no headroom |
| R9 | Low | BLAS garbage queue (1024) falls back to immediate free when full. Retirement is `tr.frameCount + 2`, not a fence | `vk_accelstruct.cpp:1145-1157` | Fine in practice; an immediate free with frames in flight is a device-lost risk after a mass invalidation (RT shadow toggle frees all interactions) |
| R10 | Low | Models over 512 surfaces are silently truncated for RT | `vk_accelstruct.cpp:706` | Log line only |

Already good: per-slot TLAS, material table and address tables; static-block signature caching of instance and material uploads; model BLAS cache keyed by `hModel`; one barrier for all BLAS builds; RT bindless sampler is a single shared sampler.

## Stage B0 — Instrument

| Counter | Answers |
|---|---|
| `VK RT BLAS SRC` gpuPct, host-visible bytes held by BLAS geometry, BLAS builds vs updates per frame, bytes memcpy'd per frame for update | R1 |
| Live allocation count split by owner (BLAS, TLAS, images, rings) | R4 |
| CPU ms in the TLAS gather loop; `MakeMaterialEntry` calls and texture-slot compares per frame | R5 |
| Bindless rebuilds per minute, slots written per rebuild, trigger (new slot vs image-counter bump) | R6 |
| Memory type chosen per buffer (heap, flags) logged once at creation; report whether a `DEVICE_LOCAL\|HOST_VISIBLE` type exists and its heap size | R7 |
| Debug cvar that prefers `DEVICE_LOCAL\|HOST_VISIBLE` for the material and address SSBOs only; compare RT pass GPU ms on both GPUs | R7, R1 read cost |
| `r_vkSerializeFrames 0` vs `1` on a map with 12+ skinned characters, screenshots and video; sync validation (`vulkan_debugging.md`) as a second pass for the AS write only | R2 |
| TLAS: entities visited vs accepted per frame; non-opaque geometry count; a debug view tinting non-opaque hits | R5, X5, X6 |
| Open one capture in RRA (AMD) to read BLAS sizes, update flags and overlap | R3 |

Check: baseline in `notes/20261010_vk_rt_baseline.txt`.

## Stage B1 — Zero-copy BLAS and correctness (R1, R2)

| Change | Detail |
|---|---|
| Depends on A1 | Static `indexCache` in the device-local chunks makes `haveGpuIdx` true for static geometry, so static BLASes use the vertex cache directly and drop their host copies |
| Dynamic geometry | Skinned/deformed vertices go into a per-frame-slot device-visible ring (not per-BLAS buffers). Hit-shader address tables point into the current slot's ring |
| Dynamic BLAS per slot | Two AS objects per dynamic entity (or build, never update in place), so frame N never writes what N-1 reads. Alternative: a barrier at frame start (RT read to AS build write) plus per-slot geometry; pick from B0/sync-validation results |
| Persistent maps | Drop per-call `vkMapMemory`/`vkUnmapMemory` |

Check: `gpuPct` near 100% for static, host-visible BLAS bytes ~0; serialize-frames A/B shows no difference on the 12-character map; sync validation clean; reflection/shadow screenshots identical; BLAS GPU ms (`r_vkRTProfile`) not worse.

## Stage B2 — BLAS memory (R3, R4, R10)

| Change | Detail |
|---|---|
| Static BLAS flags | No `ALLOW_UPDATE` for BLASes built from static models; free scratch after the build completes (deferred with the BLAS garbage clock) |
| Dynamic BLAS flags | `PREFER_FAST_BUILD` for per-frame updated BLASes; full rebuild every N updates |
| Shared transient scratch | One scratch buffer per frame slot sized to the largest build, reused with a barrier between builds, replacing a scratch per BLAS |
| Pooled AS storage | Suballocate AS storage from large blocks (256-byte AS alignment) instead of one allocation per BLAS |
| Truncation | Raise `MAX_BLAS_SURFACES` or split the model into several BLASes; keep the log |

Check: live RT allocations < 100 after level load; VRAM drops by the scratch total; BLAS count and trace time (`r_vkRTProfile` AO/shadow ms) not worse on static scenes.

## Stage B3 — CPU in the TLAS loop and material table (R5, R6)

| Change | Detail |
|---|---|
| Cache per BLAS geometry | Compute `VkMaterialEntry` once when the BLAS is built or the bindless generation changes; the per-frame loop copies it and patches `baseGeomIdx` |
| Texture slot lookup | Replace the linear scan with a hash map `idImage* -> slot`; reset at level load with the table |
| Skip static work when unchanged | Static signature already exists; skip the per-geometry loop for instances whose BLAS and slot generation are unchanged |
| Incremental bindless writes | Track dirty slot indices; write only those. Full rebuild only on level-load reset. A purge/reupload rewrites just the affected slots |
| No image loads from here | Slots never lazy-load; unresolved images take the white fallback and are picked up when the raster path loads them |

Check: TLAS gather CPU ms (B0 counter) drops; descriptor writes per rebuild drop from thousands to the changed slots; reflection textures correct after a level change and after `vid_restart`.

## Stage B4 — Memory placement and TLAS buffers (R7, R8, R9)

| Change | Detail |
|---|---|
| Helper | `VK_CreateBufferHostToGpu`: prefer `DEVICE_LOCAL\|HOST_VISIBLE\|COHERENT`; on type-missing or allocation failure fall back to `HOST_VISIBLE\|COHERENT`; log which was used |
| Apply to | Material/address SSBOs, GI/vol light SSBOs, emitter buffer, TLAS instance buffer, data ring, UBO ring, BLAS dynamic geometry ring. Not to readbacks (keep `HOST_CACHED`) or the staging ring (write-only, sequential) |
| TLAS buffers | Allocate instance buffer, storage and scratch at a capped size (or grow by 2x) so they are not recreated per count change; map once |
| BLAS garbage | When the queue fills, flush the oldest after a fence wait instead of freeing immediately |

Check: per-buffer memory type log shows the BAR type on this machine; RT pass GPU ms on the 4070 Ti Super and the 9070 XT compared against B0; machine without ReBAR still runs (force the fallback with a debug cvar and confirm).

## Measured 2026-10-11 (timedemo + live, Mars City 2)

| Result | Numbers | Consequence |
|---|---|---|
| Largest GPU cost is RT shadows traced serially and reported as `Interactions` | 14 of 21 shadow lights serial, 31.9 M pixels | Own chunk: `20261010_cleanup_optimization.md` Phase 6a. Runs before A1-B4 |
| F2 confirmed: every frame flushes and waits; `fenceWait` ~0, `upload` 9-18 ms live on heavy frames | Frame time = GPU + CPU, no overlap | A2 moves up. Cause is 7-27 `ambient.cached` static allocs per frame; first try routing per-frame dynamic geometry to the frame ring |
| Demo playback rebuilds every dynamic BLAS each frame (live: in-place updates) | `TLAS.BLAS` CPU 3-4.5 ms + `drain` 3-5 ms vs 0.2 / 0.005 | Fix before using timedemo for CPU numbers. A rebuild costs ~3 ms (R4) |
| Raster CPU is small | 0.4-1.6 ms per frame | F1 and A5 drop in priority |
| R1 confirmed | `VK RT BLAS SRC gpuPct=0.0` | B1 stands |

## Not yet audited

Candidates outside the files this audit read. Each needs a Stage 0 / A0 number before it becomes a finding.

| # | Where | Question | Counter |
|---|---|---|---|
| X1 | Frontend: md5 skinning, interaction and light-tri building | Share of frame on busy scenes | `frontend=`, `addModels=`, `dynCached=`, `dynContinuous=` (exist) |
| X2 | `tr_trisurf.cpp:2085-2111` | How often tangents are re-derived because the TLAS is active | count and ms at the site |
| X3 | Vertex-cache sites with RT shadows on | Do shadow or lighting caches still allocate | `VK VC SITES` (exists) |
| X4 | `VK_GetOrCreateGuiBlendPipeline`, `VK_GetOrCreateBlendlightPipeline`; no `VkPipelineCache` in the tree | First-use hitches; startup and `vid_restart` time | pipeline-create log (A0) |
| X5 | Whole-world walks per frame: `vk_accelstruct.cpp:1554`, `1723`; `vk_rt_emitters.cpp:674`; `vk_gi.cpp:1635`; `vk_backend.cpp:4675` | CPU with large entity counts | visited vs accepted (B0); `other=` (A0) |
| X6 | Non-opaque BLAS geometry (perforated, translucent) | Any-hit cost per ray | count + debug tint (B0), RRA |
| X7 | Depth prepass, G-buffer, interactions x lights, shader passes | Geometry submitted 3+ times | RenderDoc/Nsight capture with A0a labels |
| X8 | Level load: single-threaded image decode; CPU `R_MipMap` vs GPU blit mips (`vk_image.cpp:775`); BLAS build per model | Load time | WPR trace of one map load |

## Risky edits

| Stage | Edit | What can break | Guard |
|---|---|---|---|
| A1 | Static index buffers on GPU | Index block purged while a BLAS or draw still references it; TAG_TEMP misclassified as static | A/B cvar; shadow indexes excluded; generation guard on `GetBuffer` |
| A1 | Per-frame dynamic geometry cache | Key reused after the frame-temp pool recycles a pointer | Key includes frame number; clear at ring reset |
| A2 | Async upload flush | Staging freed before the copy runs; flush-before-drain order (`vk_backend.cpp:4779`); image sampled in the frame that uploads it | Only if A0 justifies it; lazy-load removal ships separately first |
| A3 | Image memory pools | `bufferImageGranularity`, fragmentation, free-by-range racing the garbage drain | Sampler cache and staging ring first; pools last and only if allocation counts near the limit |
| A4 | Drop `virtMem` | `vertexCache.Position()` callers, BLAS CPU fallback, shadow CPU path read freed memory | After A1 and B1; debug build poisons instead of freeing for one pass |
| A5 | Skip redundant state | Tracked state stale across command buffer or render pass boundaries | Reset tracking at every begin; cvar to disable skipping |
| B1 | Dynamic geometry ring, per-slot BLAS | Address tables pointing at the other slot; TLAS static signature churn (update path relies on a stable device address) | Per-slot address tables verified with the R2 A/B; watch TLAS rebuild count |
| B2 | Shared scratch | Builds in one command buffer overlap on scratch; updates need scratch too | Offsets within the shared buffer per build, or dynamic BLASes keep their own |
| B3 | Cached material entries | Stale after skin/material swap or bindless generation bump | Invalidate on generation; `vid_restart` and level-change screenshots |
| B4 | BAR memory type | ~256 MB heap without Resizable BAR; data rings alone are 128 MB; any CPU read-back from write-combined memory | Fallback on allocation failure; small buffers first, rings last |

## Order

| Stage | Risk | Gate / expected gain |
|---|---|---|
| 0 | none | Existing logs + PresentMon; decides which stages run |
| A0 + B0 | none | Close logging gaps; one session covers both; includes the R2 and R7 A/Bs. A0a (labels) and A0b (window stats) first — labels turn several B0 counters into RGP/RRA/RMV reads |
| A2 (lazy-load part) | low-med | Run if lazy image loads appear on hitch frames (F3) |
| B3 | low-med | Run if TLAS CPU ms is material; per-frame CPU, streaming stalls |
| A3 (sampler cache, staging) | low-med | Run if object counts are high; load time, limit safety |
| B2 | med | Run on RRA / allocation evidence; VRAM, update cost |
| A1 | med | Run if ring bytes are large or B1 is wanted; prerequisite for B1 |
| B1 | med-high | Run if R2 or R7 confirmed; removes host copies |
| A5 | low | Run if draw-state counters are high and the frame is CPU-bound |
| B4 | low | Run if the R7 A/B shows a gain |
| A2 (async flush), A3 (pools), A4 | med | Only on direct evidence |

B1 cannot start before A1 lands the static index buffers. One change per commit with before/after numbers in the notes file.

Exit: with `r_vkRTProfile 1`, `uploadMs` ~0, data-ring bytes per frame limited to dynamic geometry, BLAS host-visible bytes ~0, and TLAS gather CPU ms below baseline on Mars City; live allocations and samplers under the limits above; no difference between `r_vkSerializeFrames 0` and `1` with 12+ skinned characters; no frame over the spike threshold on the triage sections outside level load; RSS and VRAM flat across three map reloads.
