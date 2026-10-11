# Vulkan Core Audit: buffers, uploads, raster path (non-RT)

Code review only, no telemetry, no edits made. Scope: `vk_buffer.cpp`, upload batch in `vk_instance.cpp`, `vk_image.cpp`, raster draw path in `vk_backend.cpp`, `VertexCache.cpp`, `Vulkan/VKBackend.cpp`.
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

## Order

| Stage | Risk | Expected gain |
|---|---|---|
| A0 | none | ranks the rest |
| A1 | med | largest steady-state CPU win (F1) |
| A2 | med | removes frame stalls (F2, F3) |
| A3 | med | load time, allocation-limit safety |
| A4 | low | RAM |
| A5 | low | CPU per draw |

Exit: with `r_vkRTProfile 1`, `uploadMs` ~0 and data-ring bytes per frame limited to dynamic geometry on Mars City; live allocations and samplers under the limits above; RSS and VRAM flat across three map reloads.
