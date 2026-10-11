# Vulkan Core Audit: buffers, uploads, raster path, RT uploads

Code review only, no telemetry, no edits made. Part 1 (F1-F13, stages A0-A5): `vk_buffer.cpp`, upload batch in `vk_instance.cpp`, `vk_image.cpp`, raster draw path in `vk_backend.cpp`, `VertexCache.cpp`, `Vulkan/VKBackend.cpp`. Part 2 (R1-R10, stages B0-B4): `vk_accelstruct.cpp`, `vk_material_table.cpp` and the buffers RT passes read.
Feeds Phase 6 of `20261010_cleanup_optimization.md`. Each finding is a code-read claim; Stage A0 turns them into numbers before any fix.

## Findings

| # | Sev | Finding | Where | Effect |
|---|---|---|---|---|
| F1 | High | Vulkan forces `r_useIndexBuffers 0`, so `geo->indexCache` is always NULL. Every draw `memcpy`s the whole index array into the per-frame data ring. TAG_TEMP vertices (every dynamic entity) are copied per draw too | `VertexCache.cpp` (virtualMemory block), `vk_backend.cpp:1587-1620` (interaction), 11 `VK_AllocDataRing` sites | A skinned mesh lit by 4 lights = 5 vertex + 5 index copies per frame. CPU memcpy + 64 MB x2 host-visible rings. Likely the main steady-state CPU cost |
| F2 | High | Upload batch ends in a synchronous submit + `vkWaitForFences`, run at the start of every frame that has pending uploads | `vk_instance.cpp:617-678`, `vk_backend.cpp:4783` | CPU waits for the upload, which queues behind the previous in-flight frame. Any frame with a new static block (moving lights, new interactions) loses CPU/GPU overlap |
| F3 | High | Lazy image load during command recording: `VK_Image_GetDescriptorInfo` calls `ActuallyLoadImage` (disk + decode) then `VK_FlushPendingUploads()` per image | `vk_image.cpp:1253-1282`, `1296-1325` | One blocking submit+wait per unloaded image, mid-frame. This is the "one-by-one" pattern |
| F4 | High | Per-image Vulkan objects: one `vkAllocateMemory` for the image, one staging `vkCreateBuffer`+`vkAllocateMemory`, one `VkSampler`. No suballocation, no sampler cache | `vk_image.cpp:692-892`, `936-1081` | Device limits are typically 4096 allocations and ~4000 samplers; a level with thousands of images plus 512 pending staging buffers approaches both. Slow loads. `vkAllocateMemory` is a kernel call |
| F5 | Med | Static vertex blocks keep a CPU copy forever (`virtMem` from `idVertexCache::Alloc`) after the GPU upload | `VertexCache.cpp:345`, freed only in `VK_VertexCache_Free` | All static geometry resident twice (RAM + VRAM) |
| F6 | Med | Interaction draw pushes 10 descriptors and sets scissor, depth bias, cull mode every draw with no "same as last" check | `vk_backend.cpp:1556`, 8 push sites | Driver-side CPU per draw; scales with draws x lights |
| F7 | Med | Shadow draw scans all indexes for the max index, twice, every draw when geometry is CPU-side | `vk_backend.cpp:3598-3650` | O(indexes) per shadow draw per frame |
| F8 | Med | Ring overflow is silent in effect: UBO ring clamps to the last slot (wrong data drawn), data ring returns `VK_WHOLE_SIZE` and the draw is skipped. One warning per frame | `vk_backend.cpp:931-943`, `1177-1192` | Visual glitches at high light/surface counts with no high-water mark to size against |
| F9 | Low | New command buffer allocated and freed on every upload flush | `vk_instance.cpp:563-587`, `554-561` | Driver alloc per frame with uploads |
| F10 | Low | Image upload does not `VK_CHECK` staging `vkCreateBuffer`/`vkAllocateMemory`/`vkMapMemory`; later failure paths destroy the image after copies were already recorded into the open batch | `vk_image.cpp:728-743`, `864-884` | OOM = null-handle crash, or a poisoned batch |
| F11 | Low | Vertex-cache best-fit scans every free range in every chunk; `Insert` into the sorted list is O(n) | `vk_buffer.cpp:326-399` | Fine today; grows with fragmentation |
| F12 | Info | All textures are `R8G8B8A8_UNORM`; compressed sources are expanded | `vk_image.cpp:673` | 4-8x VRAM and upload bandwidth versus BC1/BC3/BC7. A feature, not a bug |
| F13 | Info | Garbage slot choice (`currentFrame-1`) is correct for frontend frees. Frees during backend recording would land one slot early. Current backend frees (`vk_backend.cpp:4680`) happen before the frame opens | `vk_buffer.cpp:589` | Verify only; add an assert in debug builds |

Already good: chunk sub-allocator with generation guard, garbage rings with overflow lists, 16 MB staging ring for buffers, batched `VK_BeginSingleTimeCommands`.

## Stage A0 — Instrument (no behaviour change)

| Counter (per frame, in `r_vkRTProfile` output) | Answers |
|---|---|
| Draws; index bytes and vertex bytes memcpy'd to the data ring; data ring and UBO ring high-water | F1, F8 |
| Upload flushes per frame, ms waited, bytes staged, static blocks allocated | F2 |
| Images loaded lazily during recording (name, ms) | F3 |
| Live `vkAllocateMemory` count and `VkSampler` count (wrap create/destroy); print `maxMemoryAllocationCount`, `maxSamplerAllocationCount` at startup and level end | F4 |
| RSS delta across level load; bytes held in `virtMem` | F5 |
| Pushes and redundant state sets per frame | F6 |

Check: Mars City baseline written to `notes/20261010_vk_core_baseline.txt`, ranked by measured cost. The order of A1-A5 may change.

## Stage A1 — Stop re-copying geometry (F1, F7)

| Change | Detail |
|---|---|
| Static index buffers on GPU | Allow `indexCache` under Vulkan: allocate non-TAG_TEMP index blocks via `VK_VertexCache_Alloc(..., true)`; draws bind chunk buffer + offset like vertices |
| Dynamic geometry once per frame | Cache `(vertCache_t* / indexes ptr) -> ring offset` for the frame; passes and lights reuse it |
| Shadow max index | Compute once when the shadow tri is built, store on the tri |
| Keep CPU fallback | Used only when `GetBuffer` fails |

Check: bytes copied per frame drops to dynamic-geometry size; `VK CPU PROFILE` DrawView ms drops; screenshots identical; validation clean.

## Stage A2 — Remove blocking uploads (F2, F3, F9)

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
| R2 | High (verify) | Dynamic-entity BLAS geometry buffers and the AS itself are single-copy and updated in place every frame, while 2 frames are in flight. Frame N's CPU `memcpy` and `MODE_UPDATE` can overlap frame N-1's trace and hit-shader vertex reads. The TLAS and material tables are per slot, the BLAS is not. No barrier at frame start orders N-1's trace reads before N's AS write | `vk_accelstruct.cpp:847-930`, `1301-1311` | Possible shading tear / AS corruption on skinned entities, only under load. Confirm with sync validation before changing |
| R3 | Med | Every BLAS is built `ALLOW_UPDATE` and keeps its scratch buffer for life, including static world and model BLASes that never update | `vk_accelstruct.cpp:635`, `1068`, `1099` | Larger, slower-to-trace AS for static geometry; scratch (about the size of the AS) resident forever |
| R4 | Med | Per BLAS: AS buffer, scratch buffer, plus 2 per geometry in the fallback = at least 4 `vkAllocateMemory`. Same allocation-limit exposure as F4 | `AllocASBuffer`, `vk_accelstruct.cpp:474-499` | Hundreds of cached model BLASes approach the device allocation limit; slow level load |
| R5 | Med | Every frame, for every static instance and geometry (up to 4096 / 16384), the TLAS loop calls `VK_RT_MakeMaterialEntry` (stage walk plus up to 3 linear scans of the bindless table, up to 4096 slots) and hashes the signature, even when nothing changed | `vk_accelstruct.cpp:1670-1695`, `vk_material_table.cpp:126-154`, `635-726` | Worst case millions of compares per frame, pure CPU. The result is only needed when `rewriteStatic` is true |
| R6 | Med | Bindless descriptor rebuild rewrites every assigned slot (up to 4096) per frame slot whenever anything is dirty. Dirty is set by any image upload or purge anywhere (`VK_Image_ChangeCounter`) and by any new texture. New slots can also lazy-load an image and flush (F3) | `vk_material_table.cpp:100-236` | Thousands of descriptor writes per event, twice (once per frame slot); stalls during streaming |
| R7 | Med | Host-written, GPU-read buffers all request `HOST_VISIBLE\|COHERENT`. `VK_TryFindMemoryType` returns the first matching type, normally system RAM, not the device-local BAR type | material/address SSBOs (`vk_material_table.cpp:276-285`), GI/vol light SSBOs (`vk_gi.cpp:655-670`), emitters, SBTs, data ring, UBO ring, TLAS instances, BLAS fallback geometry | Per-hit material fetches and vertex reads cross PCIe. Machines with Resizable BAR get VRAM-speed reads by preferring `DEVICE_LOCAL\|HOST_VISIBLE` |
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
| Run sync validation (`vulkan_debugging.md`) on a map with 12+ skinned characters | R2 |

Check: baseline in `notes/20261010_vk_rt_baseline.txt`.

## Stage B1 — Zero-copy BLAS and correctness (R1, R2)

| Change | Detail |
|---|---|
| Depends on A1 | Static `indexCache` in the device-local chunks makes `haveGpuIdx` true for static geometry, so static BLASes use the vertex cache directly and drop their host copies |
| Dynamic geometry | Skinned/deformed vertices go into a per-frame-slot device-visible ring (not per-BLAS buffers). Hit-shader address tables point into the current slot's ring |
| Dynamic BLAS per slot | Two AS objects per dynamic entity (or build, never update in place), so frame N never writes what N-1 reads. Alternative: a barrier at frame start (RT read to AS build write) plus per-slot geometry; pick from B0/sync-validation results |
| Persistent maps | Drop per-call `vkMapMemory`/`vkUnmapMemory` |

Check: `gpuPct` near 100% for static, host-visible BLAS bytes ~0; sync validation clean on the 12-character map; reflection/shadow screenshots identical; BLAS GPU ms (`r_vkRTProfile`) not worse.

## Stage B2 — BLAS memory (R3, R4, R10)

| Change | Detail |
|---|---|
| Static BLAS flags | No `ALLOW_UPDATE` for BLASes built from static models; free scratch after the build completes (deferred with the BLAS garbage clock) |
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

## Order

| Stage | Risk | Expected gain |
|---|---|---|
| A0 | none | ranks the raster findings |
| A1 | med | largest steady-state CPU win (F1); prerequisite for B1 |
| A2 | med | removes frame stalls (F2, F3) |
| A3 | med | load time, allocation-limit safety |
| A4 | low | RAM |
| A5 | low | CPU per draw |
| B0 | none | ranks the RT findings; sync-validation answer for R2 |
| B1 | med-high | removes host copies, fixes the dynamic-BLAS race (R2) if confirmed |
| B2 | med | VRAM, allocation count |
| B3 | low-med | per-frame CPU in the TLAS loop, streaming stalls |
| B4 | low | PCIe-bound reads, TLAS churn |

B0 can run together with A0 so one profiling session covers both. B1 cannot start before A1 lands the static index buffers.

Exit: with `r_vkRTProfile 1`, `uploadMs` ~0, data-ring bytes per frame limited to dynamic geometry, BLAS host-visible bytes ~0, and TLAS gather CPU ms below baseline on Mars City; live allocations and samplers under the limits above; sync validation clean with 12+ skinned characters; RSS and VRAM flat across three map reloads.
