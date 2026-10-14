# Optimization: Vulkan core, RT uploads, shadow dispatch

**Owns all performance work** — findings, fix stages, measurement, order.
`20261010_cleanup_polish.md` owns deletion and polish and contains no optimization.
ROADMAP owns which arc runs when.

Three groups; the labels are cited by ROADMAP and by code comments, so they stay:

| Group | Scope | Files |
|---|---|---|
| **S** | RT shadow dispatch (code comments call it "Phase 6a") | `vk_shadows.cpp`, `vk_backend.cpp`, `shadow_ray.rgen` |
| **R** findings → **B** stages | RT upload paths | `vk_accelstruct.cpp`, `vk_material_table.cpp`, the buffers RT passes read |
| **F** findings → **A** stages | Vulkan core CPU | `vk_buffer.cpp`, `vk_image.cpp`, `VertexCache.cpp`, raster path in `vk_backend.cpp` |

All three measured scenarios are **GPU-bound by 4-7x** (CPU recording 1.4-1.7 ms), so most of
the F/A group is parked and S runs first. Gate: no fix stage starts until a counter shows its
finding on a chugging section. A stage whose counter reads near zero is dropped, not deferred.

## Ranked actions

| # | Action | Risk | Why it is here |
|---|---|---|---|
| 1 | **Stage S** — shadow dispatch, S2-S7 | med | Only double-digit GPU item. 74% of mchq frames overflow the 7-light batch; each overflowed light costs ~0.37 ms billed to `Interactions` |
| 2 | **BLAS rebuild burst** — cap and spread rebuilds per frame | med | 36 rebuilds in one frame = 49 ms backend. Overlaps B2's flag work |
| 3 | **B0 remaining A/Bs** — R2 serialize-frames, R7 memory type | none | Two GPU-side questions, both cheap, both gate later stages |
| 4 | **B2** — static BLAS flags, shared scratch | med | Read an RRA capture first |
| 5 | **B1** — zero-copy BLAS (needs A1) | med-high | Run if R2 or R7 confirmed. Removes all host-visible BLAS copies |
| 6 | **A1** — static index buffers | med | **Only as B1's prerequisite.** Its own CPU case is dead (`cpu.raster` 0.9 ms) |
| 7 | **B3** — TLAS loop and material-table CPU | low-med | `cpu.TLAS` p50 0.6 ms every frame, 5% of a 12 ms frame |
| 8 | **B4** — BAR memory placement, TLAS buffer churn | low | Run if the R7 A/B shows a gain |
| 9 | **A3** — sampler cache, texture staging | low-med | Load time and allocation-limit safety, not frame time |
| 10 | **E2-E4** — sync, memory and upload regression audits | none | Cheap guards, no number demands them |
| — | A2, A4, A5 parked | — | Dispatched by measurement; reopen only if a counter changes |

Done: Stage 0 triage, A0 (profiler), the 2026-10-12 baseline. B0's counter list is in flight.
One change per commit with before/after numbers in `notes/`.

Exit for the arc: `Interactions` flat against `lights.serial`; mchq GPU p50 under 9.17 ms;
BLAS host-visible bytes ~0; live RT allocations under 100 after a level load; no difference
between `r_vkSerializeFrames 0` and `1` with 12+ skinned characters.

---

# Stage S — RT shadow dispatch

Top GPU item, confirmed 2026-10-12 across 2978 frames of the Mars City Walkthrough with
`lights.serial` in the CSV (`python tools/vk_profile_report.py mchq.csv` reproduces this):

| `lights.serial` | frames | `gpu.Interactions` | `gpu.Shadows` | casters |
|---|---|---|---|---|
| 0 | 780 | 0.08 | 2.48 | 6 |
| 5 | 229 | 5.03 | 2.18 | 12 |
| 10 | 92 | 7.56 | 1.08 | 17 |
| 15 | 40 | 10.40 | 0.31 | 22 |
| 20 | 31 | 7.49 | 0.10 | 27 |

`Interactions` +7.41 ms while `Shadows` −2.38 ms: work leaves the batched dispatch and
reappears in the interaction loop. Total GPU 6.50 → 13.13 ms. `lights.batched` pins to exactly
7 and 74% of frames overflow the cap.

| Finding | Evidence | Where |
|---|---|---|
| Only 7 lights batch; the rest trace one at a time inside the interaction loop, each with a render-pass break, depth round-trip and its own blur | 7 batched, 14 serial | `vk_backend.cpp:4107`, `vk_shadows.cpp:1248` |
| Serial shadow time is billed to `Interactions` (raster), so `Shadows` reads near zero when the cost is highest | `Shadows=0.16 Interactions=12.97` | span at `vk_backend.cpp:5419` |
| Layers go out in list order; the batch got 1.9 M pixels, the serial path 31.9 M (8.6 screens, 128 M rays at 4 samples) | `VK RT TRACE dims=` lines | `VK_RT_ShadowBatchAddLight` |
| Dispatch rect is the light scissor — full screen when the camera is inside the light volume. Four full-screen lights touch 2-4 surfaces each | lights 20-23 | `VK_ComputeLightScissor` |
| No per-pixel volume test. The only cull is a sphere of the box diagonal, point lights only, off by default (`r_rtShadowDistanceCutoff 0`) | every pixel in the rect traces | `shadow_ray.rgen:180`, `vk_shadows.cpp:1095` |
| Stencil is cleared per light although no stencil volumes are drawn with RT shadows | — | `vk_backend.cpp:4126` |

The full-screen rects are stock scissor behaviour plus the missing volume test — no leftover
GI "reach" extension in this path.

## Instrument

One counter, added with S2 and used by every step: shadow-ray launch area per frame,
summed over all dispatches (batched and serial), reported as `shadow.Mpix` next to
`lights.serial`. Today's route reads 33.8 M (1.9 batched + 31.9 serial). It counts launches,
so S2's in-shader cull moves GPU time, not this counter; S5 is the step that lowers it.

Steps are read live off that counter plus the frame time in the corner. No per-step CSV
capture: if `shadow.pixels` collapses and `Interactions + Shadows` does not, break overhead
is the remaining cost and S3/S4 holds the win — which is the ray-vs-break split that an
up-front measurement step would have produced, arriving as a side effect.

`S0 Measure` and `S1 Label` are dropped. `VK_RT_SHADOW_BATCH_MAX` is a `#define`
(`vk_raytracing.h:177`), so a sweep costs a rebuild per point and S3 reorganises the budget
anyway; S1 labels a span that S4 deletes. Until S4 lands, read `Interactions + Shadows` as
one number. Numbering stays S2-S7 — the labels are cited elsewhere.

| Step | Change | Check |
|---|---|---|
| S2 Pixel cull — **done 2026-10-13** (9070 XT mchq p50: Int 4.55→3.10, Shadows 2.07→1.30, 0.50→0.33 ms/serial light; debug 11 clean) | `shadow_ray.rgen` tests world pos against `lightDef->frustum[6]` (padded 4 units) right after reconstruction; outside stores 1.0 and returns. `r_rtShadowVolumeCull` (default 1) replaces `r_rtShadowDistanceCutoff` | `r_rtShadowDebugMode 11`: every shadowed light should vanish, any light left is a wrong cull; shadows identical with `r_rtShadowVolumeCull 0/1`; `gpu.Interactions + gpu.Shadows` drops at equal `shadow.Mpix` |
| S3 Looped batches — **implemented, awaiting check** (W=7 until S4) | Assign shadow lights to batch `k = n / W`. Batch 0 traces where it does today. On reaching the first light of batch k>0: end pass, clear used layers, trace + blur the batch, resume. Lights are additive, so order within the loop is free | `shadow.breaks` = ceil(casters/7)−1 (21 lights = 2, was 14); `r_vkLogRT 1` prints one `SHADOW BATCH` line per batch; Interactions flat against `lights.serial` |
| S4 Delete serial path | Remove `VK_RT_DispatchShadowRaysForLight`, `VK_RT_SHADOW_SERIAL_LAYER`, `r_rtShadowBatch`, overflow log; the scratch layer becomes a batch layer (W=8) | `grep -r ShadowBatch\b` clean; settings doc row removed |
| S5 Rect shrink | Dispatch rect = light scissor ∩ screen rect of (lit surface bounds ∩ light bounds). Blur and interaction scissor use the same rect | `shadow.Mpix` well below the post-S2 figure in the corner |
| S6 Stencil clear | Skip the per-light stencil clear when RT shadows are on, after confirming the pass starts with stencil 128 | Screenshots identical; stencil-shadow fallback (`r_rtShadows 0`) unchanged |
| S7 Layer count (optional) | W=16 only if S3 shows break overhead still matters. First share one mask + blur-temp pair across frame slots (mask has no history), which makes 16 layers VRAM-neutral | VRAM logged before/after; no flicker with 2 frames in flight |

Order: S2, S3+S4, S5, S6, S7. One commit each; the counter's before/after goes in the commit
message, not a report.

### S2 notes

`idRenderLightLocal::frustum[6]` (`tr_local.h:232`) is the light volume for both kinds —
global space, positive side out, built unconditionally by `R_SetLightFrustum`
(`tr_lightrun.cpp:438`) from the same `lightProject` the interaction pass attenuates with.
Point lights get the box's planes, projected lights the frustum's, so one 6-plane test covers
both and agrees with the lighting by construction.

Do not reuse the UBO's `lightAxisRight/Up/Fwd`: those are penumbra extents, scaled by
`r_rtShadowSoftRadiusScale` and clamped to Min/Max (`vk_shadows.cpp:1036-1047`). S2 needs 6
new `vec4` planes in `ShadowParams`.

Cull before the bias block, not before the trace — a culled pixel currently also pays 9
`texelFetch` calls and 5 `invViewProj` multiplies (`shadow_ray.rgen:248-260`).

S2 is the only step that touches the camera-inside-light-volume case; no rect shrink helps
when the rect genuinely is the screen.

| Risk | Guard |
|---|---|
| S2 plane test disagrees with the falloff texture at the edges, giving a lit rim | Pad the planes a few units; compare against the tinted debug view |
| S3 layer reused before the previous batch's draws executed | Barrier fragment-read → RT-write at each batch start; sync validation |
| S5 rect too tight when surface bounds are stale (animated models) | Use the same bounds the frontend culled with; fall back to light scissor when unsure |
| S7 shared mask across slots | Only after sync validation is clean on S3 |

Exit: `shadow.Mpix` under ~8 on the mchq route, `Interactions` flat against
`lights.serial` instead of climbing, and GPU p50 under 9.17 ms. One confirming capture at the
end of the stage, not per step.

---

# RT upload paths (R findings, B stages)

| # | Sev | Finding | Where | Effect |
|---|---|---|---|---|
| R1 | High | The BLAS "zero-copy" path needs both `ambientCache` and `indexCache` on the GPU. `indexCache` never exists under Vulkan (F1), so every BLAS geometry takes the fallback: its own host-visible vertex buffer (full 60-byte `idDrawVert`) and index buffer, 2 `vkAllocateMemory` + `memcpy` per geometry | `vk_accelstruct.cpp:520-523`, `766-769`, `1005-1037` | AS builds and every hit-shader vertex fetch read system RAM over PCIe. Dynamic entities pay `vkMapMemory` + full copy + unmap per geometry per frame (`876-882`). Confirmed: `VK RT BLAS SRC gpuPct=0.0` |
| R2 | Med (verify) | Dynamic-entity BLAS geometry buffers and the AS itself are single-copy, updated in place every frame, with 2 frames in flight. Frame N's `memcpy` and `MODE_UPDATE` can overlap N-1's trace. TLAS and material tables are per slot; the BLAS is not, and no frame-start barrier orders N-1's reads before N's write | `vk_accelstruct.cpp:847-930`, `1301-1311` | Expected symptom: one-frame normal/position mismatch on skinned entities under load. Sync validation cannot see the CPU `memcpy`; confirm with an `r_vkSerializeFrames 1` A/B |
| R3 | Med | Every BLAS is built `ALLOW_UPDATE` and keeps its scratch for life, including static world and model BLASes. Dynamic BLASes use `PREFER_FAST_TRACE` and refit in place with no periodic rebuild | `vk_accelstruct.cpp:635`, `908-911`, `1068`, `1099` | Larger, slower-to-trace static AS; scratch (about AS size) resident forever; degrading refit quality on skinned meshes |
| R4 | Med | Per BLAS: AS buffer, scratch, plus 2 per geometry in the fallback = at least 4 `vkAllocateMemory`. Same limit exposure as F4 | `AllocASBuffer`, `vk_accelstruct.cpp:474-499` | Hundreds of cached model BLASes approach the device allocation limit; slow level load |
| R5 | Med | Every frame, for every static instance and geometry (up to 4096 / 16384), the TLAS loop calls `VK_RT_MakeMaterialEntry` (stage walk plus up to 3 linear scans of a 4096-slot table) and hashes the signature, even when nothing changed | `vk_accelstruct.cpp:1670-1695`, `vk_material_table.cpp:126-154`, `635-726` | Worst case millions of compares per frame. The result is only needed when `rewriteStatic` is true. `cpu.TLAS` p50 0.6 ms |
| R6 | Med | Bindless descriptor rebuild rewrites every assigned slot (up to 4096) per frame slot whenever anything is dirty. Dirty is set by any image upload or purge (`VK_Image_ChangeCounter`) and by any new texture | `vk_material_table.cpp:100-236` | Thousands of descriptor writes per event, twice; stalls during streaming |
| R7 | Med | Host-written, GPU-read buffers all request `HOST_VISIBLE\|COHERENT`. `VK_TryFindMemoryType` returns the first match, normally system RAM, not the device-local BAR type | material/address SSBOs (`vk_material_table.cpp:276-285`), GI/vol light SSBOs (`vk_gi.cpp:655-670`), emitters, SBTs, data ring, UBO ring, TLAS instances, BLAS fallback geometry | Per-hit material and vertex fetches cross PCIe. Resizable BAR would give VRAM-speed reads. Size of the gain unknown; the B0 A/B decides, and R1's read-cost claim rests on the same result |
| R8 | Low | TLAS instance buffer, storage and scratch are sized exactly to the current count and destroy+recreated (3 allocations) whenever it grows. Instance buffer is mapped and unmapped every frame | `vk_accelstruct.cpp:1918-1956`, `2187-2227`, `1971`, `2006` | Churn while instance count fluctuates; no headroom |
| R9 | Low | BLAS garbage queue (1024) falls back to immediate free when full. Retirement is `tr.frameCount + 2`, not a fence | `vk_accelstruct.cpp:1145-1157` | Device-lost risk after a mass invalidation (an RT shadow toggle frees all interactions) |
| R10 | Low | Models over 512 surfaces are silently truncated for RT | `vk_accelstruct.cpp:706` | Log line only |

Already good: per-slot TLAS, material table and address tables; static-block signature caching
of instance and material uploads; model BLAS cache keyed by `hModel`; one barrier for all BLAS
builds; a single shared RT bindless sampler.

## Stage B0 — Instrument

| Counter | Answers |
|---|---|
| `VK RT BLAS SRC` gpuPct, host-visible bytes held by BLAS geometry, builds vs updates per frame, bytes memcpy'd per update | R1 |
| Live allocation count split by owner (BLAS, TLAS, images, rings) | R4 |
| CPU ms in the TLAS gather loop; `MakeMaterialEntry` calls and texture-slot compares per frame | R5 |
| Bindless rebuilds per minute, slots written per rebuild, trigger (new slot vs image-counter bump) | R6 |
| Memory type chosen per buffer logged once at creation; whether a `DEVICE_LOCAL\|HOST_VISIBLE` type exists and its heap size | R7 |
| Debug cvar preferring `DEVICE_LOCAL\|HOST_VISIBLE` for the material and address SSBOs only; compare RT pass GPU ms on both GPUs | R7, R1 read cost |
| `r_vkSerializeFrames 0` vs `1` on a map with 12+ skinned characters, screenshots and video; sync validation (`vulkan_debugging.md`) as a second pass for the AS write | R2 |
| TLAS: entities visited vs accepted per frame; non-opaque geometry count; a debug view tinting non-opaque hits | R5, X5, X6 |
| One RRA capture (AMD) for BLAS sizes, update flags and overlap | R3 |

Check: baseline appended to `notes/20261010_vk_core_baseline.txt`.

## Stage B1 — Zero-copy BLAS and correctness (R1, R2)

| Change | Detail |
|---|---|
| Depends on A1 | Static `indexCache` in the device-local chunks makes `haveGpuIdx` true for static geometry, so static BLASes use the vertex cache directly and drop their host copies |
| Dynamic geometry | Skinned/deformed vertices go into a per-frame-slot device-visible ring, not per-BLAS buffers. Hit-shader address tables point into the current slot's ring |
| Dynamic BLAS per slot | Two AS objects per dynamic entity (or build, never update in place), so frame N never writes what N-1 reads. Alternative: a frame-start barrier (RT read → AS build write) plus per-slot geometry; pick from the B0 / sync-validation result |
| Persistent maps | Drop per-call `vkMapMemory`/`vkUnmapMemory` |

Check: `gpuPct` near 100% for static, host-visible BLAS bytes ~0; serialize-frames A/B shows no
difference on the 12-character map; sync validation clean; reflection/shadow screenshots
identical; BLAS GPU ms not worse.

## Stage B2 — BLAS memory (R3, R4, R10) + rebuild burst

| Change | Detail |
|---|---|
| Static BLAS flags | No `ALLOW_UPDATE` for BLASes built from static models; free scratch after the build completes (deferred with the BLAS garbage clock) |
| Dynamic BLAS flags | `PREFER_FAST_BUILD` for per-frame updated BLASes; full rebuild every N updates |
| Rebuild burst | Cap rebuilds per frame and spread the rest over following frames. Measured: ~0.9-1.2 ms CPU each, linear, arriving in bursts; worst frame 36 rebuilds = 42.6 ms in `cpu.TLAS.BLAS`, 49 ms backend |
| Shared transient scratch | One scratch per frame slot sized to the largest build, reused with a barrier between builds, replacing a scratch per BLAS |
| Pooled AS storage | Suballocate AS storage from large blocks (256-byte AS alignment) instead of one allocation per BLAS |
| Truncation | Raise `MAX_BLAS_SURFACES` or split the model into several BLASes; keep the log |

Check: live RT allocations < 100 after level load; VRAM drops by the scratch total; no frame
over ~5 rebuilds; BLAS count and trace time (AO/shadow ms) not worse on static scenes.

## Stage B3 — CPU in the TLAS loop and material table (R5, R6)

| Change | Detail |
|---|---|
| Cache per BLAS geometry | Compute `VkMaterialEntry` once when the BLAS is built or the bindless generation changes; the per-frame loop copies it and patches `baseGeomIdx` |
| Texture slot lookup | Replace the linear scan with a hash map `idImage* -> slot`; reset at level load with the table |
| Skip static work when unchanged | Skip the per-geometry loop for instances whose BLAS and slot generation are unchanged |
| Incremental bindless writes | Track dirty slot indices and write only those. Full rebuild only on level-load reset |
| No image loads from here | Slots never lazy-load; unresolved images take the white fallback and are picked up when the raster path loads them |

Check: TLAS gather CPU ms drops; descriptor writes per rebuild drop from thousands to the
changed slots; reflection textures correct after a level change and after `vid_restart`.

## Stage B4 — Memory placement and TLAS buffers (R7, R8, R9)

| Change | Detail |
|---|---|
| Helper | `VK_CreateBufferHostToGpu`: prefer `DEVICE_LOCAL\|HOST_VISIBLE\|COHERENT`; on type-missing or allocation failure fall back to `HOST_VISIBLE\|COHERENT`; log which was used |
| Apply to | Material/address SSBOs, GI/vol light SSBOs, emitter buffer, TLAS instance buffer, data ring, UBO ring, BLAS dynamic geometry ring. Not readbacks (keep `HOST_CACHED`) or the staging ring (write-only, sequential) |
| TLAS buffers | Allocate instance buffer, storage and scratch at a capped size (or grow by 2x) so they are not recreated per count change; map once |
| BLAS garbage | When the queue fills, flush the oldest after a fence wait instead of freeing immediately |

Check: per-buffer memory type log shows the BAR type on this machine; RT pass GPU ms on the
4070 Ti Super and the 9070 XT compared against B0; a machine without ReBAR still runs (force
the fallback with a debug cvar).

---

# Vulkan core (F findings, A stages)

Ten of the thirteen F findings were measured to zero — see
[Dispatched by measurement](#dispatched-by-measurement). What survives:

| # | Sev | Finding | Where | Effect |
|---|---|---|---|---|
| F1 | — | Vulkan forces `r_useIndexBuffers 0`, so `geo->indexCache` is always NULL and every draw `memcpy`s the whole index array into the per-frame data ring | `VertexCache.cpp` (virtualMemory block), `vk_backend.cpp:1587-1620`, 11 `VK_AllocDataRing` sites | CPU cost is dead (lands inside `cpu.raster`'s 0.9 ms). Lives on only because R1/B1 cannot work without a GPU `indexCache` |
| F4 | Med | Per-image Vulkan objects: one `vkAllocateMemory` for the image, one staging `vkCreateBuffer`+`vkAllocateMemory`, one `VkSampler`. No suballocation, no sampler cache | `vk_image.cpp:692-892`, `936-1081` | Device limits are typically 4096 allocations and ~4000 samplers; a level with thousands of images plus 512 pending staging buffers approaches both. Slow loads |
| F10 | Low | Image upload does not `VK_CHECK` staging `vkCreateBuffer`/`vkAllocateMemory`/`vkMapMemory`; later failure paths destroy the image after copies were recorded into the open batch | `vk_image.cpp:728-743`, `864-884` | OOM = null-handle crash, or a poisoned batch |
| F13 | Info | Garbage slot choice (`currentFrame-1`) is correct for frontend frees; frees during backend recording would land one slot early. Current backend frees (`vk_backend.cpp:4680`) happen before the frame opens | `vk_buffer.cpp:589` | Verify only; add a debug-build assert |

Already good: chunk sub-allocator with generation guard, garbage rings with overflow lists,
16 MB staging ring for buffers, batched `VK_BeginSingleTimeCommands`.

## Stage A1 — Static index buffers (F1) — B1 prerequisite only

| Change | Detail |
|---|---|
| Static index buffers on GPU | Allow `indexCache` under Vulkan: allocate non-TAG_TEMP index blocks via `VK_VertexCache_Alloc(..., true)`; draws bind chunk buffer + offset like vertices |
| Dynamic geometry once per frame | Cache `(vertCache_t* / indexes ptr) -> ring offset` for the frame; passes and lights reuse it |
| Shadow geometry | Out of scope: shadow indexes stay on the CPU upload path (`vk_backend.cpp:3679`) |
| Keep CPU fallback | Used only when `GetBuffer` fails |
| A/B switch | Debug cvar selecting GPU vs ring indexes until screenshots match on all baseline maps |

Check: screenshots identical, validation clean, `ring.idxKB` drops to dynamic geometry only.
Frame time is not expected to move — that is not a failure of this stage.

## Stage A3 — Cut per-image object counts (F4, F10)

| Change | Detail |
|---|---|
| Sampler cache | Key on (filter, wrap, anisotropy, mip count, lod bias); images share. A cvar change then rebuilds a handful, not thousands |
| Staging for textures | Use the staging ring (or a second, larger one) instead of one buffer+memory per texture |
| Image memory pools | Suballocate device-local image memory from large blocks per memory type; respect alignment and `bufferImageGranularity`; free by range after the garbage drain. Last, and only if the counts near the limit |
| Check results | `VK_CHECK` or a clean failure path on every create in `VK_Image_Upload` / `UploadCubemap` |

Check: live `vkAllocateMemory` < 200 and samplers < 100 after a level load; load time not
worse; texture screenshots identical; the FSR mip-bias change still rebuilds correctly.

## Stages A2, A4, A5 — parked

Written up, measured to zero, not deleted. Each needs its counter re-read before reopening.

| Stage | Was | Why parked |
|---|---|---|
| A2 | Async upload flush, no mid-frame lazy load, reuse the upload command buffer (F2, F3, F9) | `frame.upload` tracks the **previous** frame's GPU cost 62-74x across quintiles — the "wait" is GPU back-pressure, not upload cost (`vc.allocMs` p50 0.014 ms). An async flush moves the wait, it does not recover time. `midframe.imageLoads` is 0 across 8761 frames |
| A4 | Drop `virtMem` after GPU upload; BC compression (F5, F12) | RSS 145-219 MB flat, VRAM 2.1 of 15.5 GB. No residency pressure to relieve |
| A5 | Skip redundant state, ring high-water sizing, debug asserts (F6, F8, F11, F13) | `cpu.raster` p50 0.5-0.95 ms for all pushes and draws; `ring.overflows` 0 everywhere, `ring.dataPeakKB` 1023 of a 64 MB ring. High-water tracking is still worth having as a guard rail |

## Dispatched by measurement

Not removed: a different GPU, a CPU-bound scenario or a streaming-heavy map could revive any
of them. Numbers from the 2026-10-12 capture, 8761 frames across three scenarios.

| # | Counter | Reading | Verdict |
|---|---|---|---|
| F2 | `frame.upload` vs previous-frame GPU | Correlates 62-74x; real upload cost `vc.allocMs` p50 0.014 ms | Reframed as GPU back-pressure. A2 demoted |
| F3 | `midframe.imageLoads` | 0 across 8761 frames | The "one-by-one" lazy load never fires in gameplay |
| F5, F12 | `mem.rssMB`, `mem.vramUsedMB` | RSS 145-219 MB flat; VRAM 14% of 15.5 GB | No residency or VRAM pressure; BC compression has no case |
| F6 | `cpu.raster` | p50 0.5-0.95 ms including every push and draw | Dead |
| F7 | — | Zero with `r_rtShadows 1`; shadow volumes are never generated (`Interaction.cpp:1060`) | Dead as documented |
| F8 | `ring.overflows` | 0 in every frame; peak 1023 KB of a 64 MB ring | Dead as a fix, kept as a guard rail |
| F11 | `vc.allocMs` | p50 0.01 ms, max 0.08 | Dead |
| F1 | `ring.idxKB` | p50 338-589 KB/frame re-copied, inside `cpu.raster`'s 0.9 ms | CPU case dead; A1 survives only as B1's dependency |
| X1 | `frame.frontend` | p50 0.75-1.36 ms (`addModels` 0.68-1.02) | Not a factor |
| X4 | `midframe.pipelines` | 2 events in 8761 frames, 0.23 ms each | Dead as a hitch source. A `VkPipelineCache` is still worth adding for startup time |

Still live and unmeasured: X3 (`VK VC SITES` is not in the CSV), X5-X8, R5/R6, R7.

## Not yet audited

Candidates outside the files this audit read. Each needs a number before it becomes a finding.

| # | Where | Question | Counter |
|---|---|---|---|
| X2 | `tr_trisurf.cpp:2085-2111` | How often tangents are re-derived because the TLAS is active | count and ms at the site |
| X3 | Vertex-cache sites with RT shadows on | Do shadow or lighting caches still allocate | `VK VC SITES` (exists) |
| X5 | Whole-world walks per frame: `vk_accelstruct.cpp:1554`, `1723`; `vk_rt_emitters.cpp:674`; `vk_gi.cpp:1635`; `vk_backend.cpp:4675` | CPU with large entity counts | visited vs accepted (B0); `frame.other` |
| X6 | Non-opaque BLAS geometry (perforated, translucent) | Any-hit cost per ray | count + debug tint (B0), RRA |
| X7 | Depth prepass, G-buffer, interactions x lights, shader passes | Geometry submitted 3+ times | RenderDoc/Nsight capture with A0a labels |
| X8 | Level load: single-threaded image decode; CPU `R_MipMap` vs GPU blit mips (`vk_image.cpp:775`); BLAS build per model | Load time | WPR trace of one map load |

## Regression audits (E2-E4)

Guards, not fixes. Cheap; no number demands them.

| # | Audit |
|---|---|
| E2 | Confirm no per-frame `vkQueueWaitIdle`/`vkWaitForFences`: `vk_ao.cpp:257`, `vk_bloom.cpp:340`, `vk_gbuffer.cpp:185`, `vk_gi.cpp:831/3187`, `vk_gi_probe.cpp:545`, plus `vkDeviceWaitIdle` in resize/cvar paths. A0's tagged wait wrapper answers this directly |
| E3 | 3 map loads + 10 min play; create-vs-destroy counts at shutdown (use `r_vkLogBufferGarbage` **before** cleanup Phase 1b removes it); validation leak report at exit |
| E4 | Full-buffer re-uploads of unchanged data (light tables, material table, instance data); write-combined reads; host-visible buffers not per-frame-slotted; BLAS rebuild vs update |

## Risky edits

| Stage | Edit | What can break | Guard |
|---|---|---|---|
| A1 | Static index buffers on GPU | Index block purged while a BLAS or draw still references it; TAG_TEMP misclassified as static | A/B cvar; shadow indexes excluded; generation guard on `GetBuffer` |
| A1 | Per-frame dynamic geometry cache | Key reused after the frame-temp pool recycles a pointer | Key includes frame number; clear at ring reset |
| A3 | Image memory pools | `bufferImageGranularity`, fragmentation, free-by-range racing the garbage drain | Sampler cache and staging ring first; pools last |
| B1 | Dynamic geometry ring, per-slot BLAS | Address tables pointing at the other slot; TLAS static signature churn (the update path relies on a stable device address) | Per-slot address tables verified with the R2 A/B; watch TLAS rebuild count |
| B2 | Shared scratch | Builds in one command buffer overlap on scratch; updates need scratch too | Offsets within the shared buffer per build, or dynamic BLASes keep their own |
| B3 | Cached material entries | Stale after a skin/material swap or a bindless generation bump | Invalidate on generation; `vid_restart` and level-change screenshots |
| B4 | BAR memory type | ~256 MB heap without Resizable BAR; data rings alone are 128 MB; any CPU read-back from write-combined memory | Fallback on allocation failure; small buffers first, rings last |
| S | — | See Stage S's own risk table |

Parked stages (A2, A4, A5) keep their guards in git history; re-derive them if a stage reopens.

---

# Measurement

Method and raw numbers: `notes/20261010_vk_core_baseline.txt`. Run
`tools/vk_profile_report.py <csv>` before reading individual frames.

**Current baseline** — 2026-10-12, `com_fixedTic 1`, 1440p, no upscaling, 5950X / 4070 Ti
Super. mchq 2978 frames, r2r 4456, commfire 1327. p50 GPU ms:

| | GI | Refl | Vol | AO | Shadows | TLAS | RT total | raster |
|---|---|---|---|---|---|---|---|---|
| mchq / r2r / commfire | 0.00 | 0.00-1.11 | 0.50-1.13 | 1.69-2.51 | 1.33-4.07 | 0.17 | **5.07-9.68** | 3.47-6.56 |

Total GPU p50: mchq 9.17 ms (109 fps), r2r 11.63 (86), commfire 14.94 (67).
Slow machine (2600 / 9070 XT), 2026-10-10 MC 1440p: ~8.0 ms RT.

Three traps in reading any of this:

- **The `Shadows` column understates shadows.** Only 7 lights batch; the rest are billed to
  `Interactions`. A `Shadows` reading near zero is the signature of the cost being *highest*.
- **Every capture before 2026-10-12 is capped at 62.5 fps** (`USERCMD_MSEC` is integer
  `1000/60`), so GPU cost beyond ~16 ms was invisible. Never compare a capped row to the
  baseline.
- **`lights.*` counters read zero until 2026-10-12** because the 2D GUI overlay is a second
  `RC_DRAW_VIEW` whose `ShadowBatchBegin` zeroed them before the read. Three captures looked
  like they had no shadow casters at all. Any new per-frame counter has the same exposure.

**History**, for the shape of the arc only — all rows below are capped, don't compare them
against the baseline:

| | GI | Refl | Vol | AO | Shadows | TLAS | total |
|---|---|---|---|---|---|---|---|
| 2026-09-11 (pre-arc-1/2) | 4.41 | 3.24 | 1.53 | 1.17 | — | 0.15 | **11.93** |
| 2026-09-23 MC2, scale 1.00 | 1.29 | ~0 | 0.52 | 1.27 | 1.80 | 0.17 | **5.05** |
| 2026-10-10 MC 1440p, scale 1.00 | 1.72 | ~0 | 0.82 | 2.05 | 2.93 | 0.46 | **~8.0** |

**Render scale below 0.67 buys ~0.1 ms** (`completed/20260918_fsr_upscaling.md` §12): arc 2
left a ~0.73 ms world-space floor (TLAS, probe trace/blend, froxel fill/integrate) that screen
resolution cannot touch. That floor is itself optimization surface — re-measure the scale sweep
after any B-stage or probe/froxel work before quoting it.

Methodology notes earned the hard way:

- Demo playback rebuilds every dynamic BLAS each frame where live play updates in place
  (`cpu.TLAS.BLAS` 3-4.5 ms + `drain` 3-5 ms vs 0.2 / 0.005). Don't take CPU numbers from a
  timedemo.
- `vk_profile_report.py` catches the two mistakes already made: quoting a p50 from a capture
  that never met the finding's condition, and reading the profiler's own console print as an
  engine hitch.

## Profiler reference (Stage A0 — done)

`vk_profile.cpp/.h` owns the metric registry, window, CSV and debug labels; `vk_backend.cpp`
keeps the query pools and records raw numbers into it. Cvars: `r_vkRTProfile` (1 = window
summary, 2 = per frame), `r_vkRTProfileWindow` (30), `r_vkRTProfileStats`,
`r_vkRTProfileSpikeMs`, `r_vkRTProfileCSV <path>`, `r_vkRTProfileGroups`, `r_vkDebugLabels`.
Debug-only, listed in `../Dhewm3-AI-RT-Settings.md`, no settings-menu entry.

| Group | Metrics | Answers |
|---|---|---|
| phases | `gpu.<phase>`, `cpu.<phase>` x26, plus gpu/cpu total and raster | the RT budget table |
| frame | fenceWait, acquire, drain, upload, submit, present, **backend** (fence→present wall clock), **other** (backend minus every span), game, frontend, findView, addLights, addModels | unattributed CPU, X1 |
| upload | flushes, KB staged, staticBlocks, draws, ring vert/idx KB, data+UBO high-water, overflows, vc allocs/KB/ms/frees, BLAS updates/rebuilds/tris/dynInst | F1, F2, F8, X3 |
| waits | ms and count per tag across 40 wrapped sites in 20 files (uploadFlush, image, accelStruct, ao, gi, giProbe, vol, bloom, gbuffer, temporal, reflect, tonemap, upscale, swapchain, readback, serialize, shutdown, other) | F2, F3 |
| memory | `mem.rssMB`, `mem.virtMemMB`, `mem.vramUsedMB`/`BudgetMB` via `VK_EXT_memory_budget` | F5, F12 |
| midframe | lazy image loads + ms, first-use pipeline builds + ms | F3, X4 |

### If you edit the profiler

| Rule | Failure if broken |
|---|---|
| Gate new metrics on `VK_Prof_Active()`, not `VK_RTProfileEnabled()` (that one also needs RT support and a query pool) | No output at all on a non-RT GPU. Only `gpu.<phase>` needs the pools |
| A new CPU span between `present` and `backendMs` must be added to `accounted` | Its cost silently becomes `frame.other`, the one metric read as "unexplained" |
| One `PhaseEnd` per `VK_RTProfile_PhaseBegin` | With labels on, `vkEndCommandBuffer` fails. `VK_RTProfile_CloseStrayLabels` warns at both exits |
| Don't move `VK_Prof_BeginFrameSlot` or the slot clear that follows `CollectAndLog` | It is the tick that runs with profiling off (CSV open/close, partial-window discard); clearing later wipes the pre-command-buffer upload counters |
| `r_vkRTProfileGroups` filters the console only — never gate collection on it | A console filter turns a CSV column into zeros |

Deliberately not built: a live `vkAllocateMemory`/`VkSampler` ledger (134 call sites for a
number RMV, Nsight and the best-practices layer already report; startup prints the two device
limits as `VK PROFILE LIMITS`, and A0a's object names let an RMV capture attribute them).

## Tooling (Windows, free)

| Tool | GPU | Use |
|---|---|---|
| `tools/vk_profile_report.py <csv>` | any | First pass on any `r_vkRTProfileCSV` capture: capture conditions, periodic-artifact detection, span-closure audit, bimodality, percentiles, worst-frame dumps. `--baseline before.csv` for a stage's before/after |
| PresentMon | any | Frame-time CSV; hitches vs sustained low fps; CPU-busy vs GPU-busy. First tool for "it chugs here" |
| VS Performance Profiler or Very Sleepy | any | CPU sampling without instrumentation. Needs PDBs; `RelWithDebInfo` is the CMake default |
| Windows Performance Recorder / Analyzer | any | ETW: CPU samples, disk I/O, page faults, time blocked in driver calls. Load time, and hitches the sampler shows as "waiting" |
| Nsight Graphics, GPU Trace | NVIDIA | Per-pass GPU time including RT dispatches. Needs the A0a debug labels to be readable |
| RGP, RMV, RRA | AMD | RGP per-event GPU time; RMV VRAM by allocation (F4, R4); RRA BLAS/TLAS quality and opaque flags (R3, X6) — the direct check for B2 |
| RenderDoc | any | Raster draw inspection, overdraw, state per draw (F6) |
| Validation via vkconfig: sync + best-practices | any | Best-practices flags small allocations and redundant work; sync covers GPU-GPU hazards. Sync cannot see CPU writes to mapped memory (R2) |

Tracy only if log spans stop scaling; it adds a dependency.
