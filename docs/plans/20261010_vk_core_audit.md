# Optimization: Vulkan core, RT uploads, shadow dispatch

**Owns all performance work** — open findings, open stages, measurement, order.
Closed work (Stage S, closed B stages, the 10 findings dispatched by measurement, parked
stages) is in `completed/20261010_vk_core_audit_closed.md`.
`20261010_cleanup_polish.md` owns deletion and polish. ROADMAP owns which arc runs when.

Group labels are cited by ROADMAP and by code comments, so they stay:
**S** = RT shadow dispatch (code says "Phase 6a", closed), **R**/**B** = RT upload paths,
**F**/**A** = Vulkan core CPU, **X** = not yet audited.

**Everything is GPU-bound by 4-7x** (CPU recording 1.4-1.7 ms against 9-15 ms of GPU), so
every CPU-side stage is parked and only GPU work is scheduled. Gate: no fix stage starts
until a counter shows its finding on a chugging section. A stage whose counter reads near
zero is dropped, not deferred — that rule retired 15 of the original findings.

## Next levers

Ranked by measured size, cheapest first within a row.

| # | Lever | Cost to try | Why here |
|---|---|---|---|
| 1 | **X7** — geometry submitted 3+ times (depth prepass, G-buffer, interactions × lights, shader passes) | capture only | `gpu.Interactions` is the biggest single item (4.55-7.45 ms p50) and owns commfire's 20 ms p95 on both machines |
| 2 | **AO ray budget** — `r_rtAOSamples` 4 → 2 leaning on the existing EMA; `aoRadius` 64 down; then half-res + bilateral upsample; then first-ray-closest-hit with the rest `TerminateOnFirstHit` | (a)(b) cvar sweep, (c)(d) code | 2.0-2.7 ms p50. `ao_ray.rgen:166` traces `gl_RayFlagsNoneEXT` — closest hit, because the `(t/R)²` falloff needs the nearest occluder, so each AO ray costs more than a shadow ray, 4x per pixel at full res |
| 3 | **ProbeResolve / FroxelFill sweep** — probe counts and spacing, `r_rtVolFroxel` grid dims | cvar sweep | ProbeResolve 1.0-1.4 ms is a full-res screen pass; FroxelFill 0.5-1.2 ms is the largest resolution-independent item in the engine. ProbeTrace (0.05) and ProbeBlend (0.09) are free — the GI cost is not rays |
| 4 | **E3 leak** — VRAM and RSS growth across `vid_restart` | 1 restart | RSS 210 → 614 MB after one restart (R7 side note). See E3 below |
| 5 | **X6** — non-opaque BLAS geometry, any-hit cost per ray | counter + tint | Compounds with row 2: every AO ray can invoke `ao_ray.rahit` |
| 6 | **X8** — load time, ~10 s per map | read one log | Three of the four phase timers already print. See X8 below |
| 7 | **B2 remainder, B3, B4, A3** | med | Memory, allocation-limit safety and hitches, not frame time. Read an RRA capture before B2 |
| 8 | **E2, E4** | none | Cheap guards; no number demands them |

Shadow dispatch (Stage S) is closed: the remaining cost is lit, front-facing rays reaching
the light, so it scales with `r_rtShadowSamples` (now 2). Next lever there, if it matters
again, is adaptive sampling — 1 ray, refine where a 3x3 neighbourhood disagrees.

Exit for the arc: `Interactions` flat against `lights.serial`; mchq GPU p50 under 9.17 ms;
live RT allocations under 100 after a level load; VRAM flat across three `vid_restart`s.
One change per commit with before/after numbers in `notes/`.

---

## Open findings

| # | Sev | Finding | Where | Effect |
|---|---|---|---|---|
| R3 | Med | Dynamic BLASes use `PREFER_FAST_TRACE` and refit in place with no periodic rebuild. (Static-flag half closed — no trace gain) | `vk_accelstruct.cpp:908-911` | Degrading refit quality on skinned meshes |
| R4 | Med | Per BLAS: AS buffer, scratch, plus 2 per geometry in the fallback = at least 4 `vkAllocateMemory`. Same limit exposure as F4 | `AllocASBuffer`, `vk_accelstruct.cpp:474-499` | Hundreds of cached model BLASes approach the device allocation limit; slow load |
| R5 | Med | Every frame, for every static instance and geometry (up to 4096 / 16384), the TLAS loop calls `VK_RT_MakeMaterialEntry` (stage walk plus up to 3 linear scans of a 4096-slot table) and hashes the signature, even when nothing changed | `vk_accelstruct.cpp:1670-1695`, `vk_material_table.cpp:635-726` | Worst case millions of compares per frame; needed only when `rewriteStatic`. `cpu.TLAS` p50 0.6 ms |
| R6 | Med | Bindless descriptor rebuild rewrites every assigned slot (up to 4096) per frame slot whenever anything is dirty. Dirty is set by any image upload or purge and by any new texture | `vk_material_table.cpp:100-236` | Thousands of descriptor writes per event, twice; stalls during streaming |
| R8 | Low | TLAS instance buffer, storage and scratch are sized exactly to the current count and destroy+recreated (3 allocations) whenever it grows. Instance buffer mapped and unmapped every frame | `vk_accelstruct.cpp:1918-1956`, `2187-2227` | ~2.5 ms on burst frames; no headroom |
| R9 | Low | BLAS garbage queue (1024) falls back to immediate free when full. Retirement is `tr.frameCount + 2`, not a fence | `vk_accelstruct.cpp:1145-1157` | Device-lost risk after a mass invalidation (an RT shadow toggle frees all interactions) |
| R10 | Low | Models over 512 surfaces are silently truncated for RT | `vk_accelstruct.cpp:706` | Log line only |
| F4 | Med | Per-image Vulkan objects: one `vkAllocateMemory` for the image, one staging `vkCreateBuffer`+`vkAllocateMemory`, one `VkSampler`. No suballocation, no sampler cache | `vk_image.cpp:692-892` | Limits are typically 4096 allocations and ~4000 samplers; a level with thousands of images plus 512 pending staging buffers approaches both |
| F10 | Low | Image upload does not `VK_CHECK` staging `vkCreateBuffer`/`vkAllocateMemory`/`vkMapMemory`; later failure paths destroy the image after copies were recorded into the open batch | `vk_image.cpp:728-743` | OOM = null-handle crash, or a poisoned batch |
| F13 | Info | Garbage slot choice (`currentFrame-1`) is correct for frontend frees; frees during backend recording would land one slot early | `vk_buffer.cpp:589` | Verify only; add a debug-build assert |

Already good: per-slot TLAS, material table and address tables; static-block signature caching
of instance and material uploads; model BLAS cache keyed by `hModel`; one barrier for all BLAS
builds; a single shared RT bindless sampler; chunk sub-allocator with generation guard;
garbage rings with overflow lists; 16 MB staging ring; batched `VK_BeginSingleTimeCommands`.

## Stage B2 remainder — BLAS memory (R3, R4, R10)

| Change | Detail |
|---|---|
| Dynamic BLAS flags | `PREFER_FAST_BUILD` for per-frame updated BLASes; full rebuild every N updates |
| Shared transient scratch | Scratch is pooled but still kept per BLAS for life. One scratch per frame slot sized to the largest build would cut the scratch total (3.5 MB today, dynamic only) |
| Truncation | Raise `MAX_BLAS_SURFACES` or split the model into several BLASes; keep the log |

Check: VRAM drops by the scratch total; `r_vkLogTLASSlow 5` shows `blas=` under 2 ms on
first-sight burst frames; BLAS count and trace time not worse on static scenes.
Read an RRA capture first — it reports BLAS sizes, update flags and overlap directly.

Risk: a range overrun corrupts a neighbouring BLAS silently (validation cannot see inside a
buffer); shared scratch lets builds in one command buffer overlap. Guards: `r_vkBLASPoolCheck 1`,
ranges freed only via the BLAS garbage clock, per-build offsets within the shared buffer.

## Stage B3 — CPU in the TLAS loop and material table (R5, R6)

| Change | Detail |
|---|---|
| Cache per BLAS geometry | Compute `VkMaterialEntry` once when the BLAS is built or the bindless generation changes; the per-frame loop copies it and patches `baseGeomIdx` |
| Texture slot lookup | Replace the linear scan with a hash map `idImage* -> slot`; reset at level load with the table |
| Skip static work when unchanged | Skip the per-geometry loop for instances whose BLAS and slot generation are unchanged |
| Incremental bindless writes | Track dirty slot indices and write only those. Full rebuild only on level-load reset |
| No image loads from here | Slots never lazy-load; unresolved images take the white fallback |

Counters this stage needs, neither of which exists yet: `MakeMaterialEntry` calls and
texture-slot compares per frame (R5); bindless rebuilds per minute, slots written per
rebuild, and the trigger — new slot vs `VK_Image_ChangeCounter` bump (R6).

Check: TLAS gather CPU ms drops; descriptor writes per rebuild drop from thousands to the
changed slots; reflection textures correct after a level change and after `vid_restart`.
Risk: cached entries go stale after a skin/material swap or a bindless generation bump —
invalidate on generation and screenshot across `vid_restart` and a level change.

## Stage B4 — TLAS buffers (R8, R9)

| Change | Detail |
|---|---|
| TLAS buffers | Allocate instance buffer, storage and scratch at a capped size (or grow by 2x) so they are not recreated per count change; map once |
| BLAS garbage | When the queue fills, flush the oldest after a fence wait instead of freeing immediately |

BAR memory placement was the third row here and was **dropped** — see the R7 A/B in the
closed doc.

## Stage A3 — Cut per-image object counts (F4, F10)

| Change | Detail |
|---|---|
| Sampler cache | Key on (filter, wrap, anisotropy, mip count, lod bias); images share. A cvar change then rebuilds a handful, not thousands |
| Staging for textures | Use the staging ring (or a second, larger one) instead of one buffer+memory per texture |
| Image memory pools | Suballocate device-local image memory from large blocks per memory type; respect alignment and `bufferImageGranularity`; free by range after the garbage drain. **Last**, and only if the counts near the limit — `bufferImageGranularity`, fragmentation and free-by-range racing the garbage drain are all live risks |
| Check results | `VK_CHECK` or a clean failure path on every create in `VK_Image_Upload` / `UploadCubemap` |

Check: live `vkAllocateMemory` < 200 and samplers < 100 after a level load; load time not
worse; texture screenshots identical; the FSR mip-bias change still rebuilds correctly.

## Not yet audited (X)

| # | Where | Question | How | Verdict |
|---|---|---|---|---|
| X7 | Depth prepass, G-buffer, interactions × lights, shader passes | Geometry submitted 3+ times | RenderDoc/Nsight with the `r_vkDebugLabels` names | **Do first.** Points at the biggest item |
| X6 | Non-opaque BLAS geometry (perforated, translucent) | Any-hit cost per ray | count + debug tint; RRA | **Do.** Real GPU lever |
| X8 | Level load | Where the ~10 s goes | read one load log (below) | **Do.** Nearly free |
| X2 | `tr_trisurf.cpp:2085-2111` | How often tangents are re-derived because the TLAS is active | count and ms at the site | Fold into X8 — almost certainly first-sight only |
| X3 | Vertex-cache sites with RT shadows on | Do shadow or lighting caches still allocate | `VK VC SITES` (exists, not in the CSV) | **Park.** CPU-side |
| X5 | Whole-world walks: `vk_accelstruct.cpp:1554`, `1723`; `vk_rt_emitters.cpp:674`; `vk_gi.cpp:1635`; `vk_backend.cpp:4675` | CPU with large entity counts | visited vs accepted; `frame.other` | **Park.** CPU is 4x clear of the frame |

### X8 — load time, before writing any code

Three of the four phases already print to console on every map load:

| Phase | Print | Where |
|---|---|---|
| Images | `N purged / N kept / N new loaded`, `all images loaded in X seconds` | `Image_init.cpp:2391-2394` |
| Models | `N new models loaded in X seconds` | `ModelManager.cpp:651` |
| Interactions | `GenerateAllInteractions, msec = N` | `RenderWorld.cpp:1733` |
| Decl parse, collision models | **nothing** | — |

So step one is reading one load log. Those counts also answer "do we load everything every
time": `EndLevelLoad` purges unreferenced images and loads the delta, and prints all three
numbers, so the log says directly whether a transition reloads the world or the delta.
`image_preload` is 1, which is why F3 found `midframe.imageLoads` = 0 — it is all paid at
load. Add timers only where that log leaves a gap.

## Regression audits (E2-E4)

| # | Audit |
|---|---|
| E2 | Confirm no per-frame `vkQueueWaitIdle`/`vkWaitForFences`: `vk_ao.cpp:257`, `vk_bloom.cpp:340`, `vk_gbuffer.cpp:185`, `vk_gi.cpp:831/3187`, `vk_gi_probe.cpp:545`, plus `vkDeviceWaitIdle` in resize/cvar paths. The tagged wait wrapper answers this directly |
| E3 | **The `vid_restart` leak.** A full restart destroys the `VkDevice` *and* `VkInstance` (`vk_instance.cpp:870-872`), so the validation layer's object-lifetime check fires at device destroy and enumerates every leaked object with its `r_vkDebugLabels` name — **one restart is enough**. Then 3 restarts with `r_vkRTProfile 1` for `mem.vramUsedMB` / `mem.rssMB`: linear growth = leak, decaying = fragmentation or reload footprint. VRAM and RSS are likely different bugs — 400 MB of RSS is not the ~11 MB of RT tables; if validation comes back clean and RSS still climbs, that is the `USE_LIBC_MALLOC=1` + ETW heap hunt. Note `VK_RT_Shutdown`'s closing `memset(&vkRT, 0, ...)` (`vk_accelstruct.cpp:2505`) makes any handle missed above that line silent to everything *except* validation |
| E4 | Full-buffer re-uploads of unchanged data (light tables, material table, instance data); write-combined reads; host-visible buffers not per-frame-slotted; BLAS rebuild vs update |

---

# Measurement

Method, raw numbers and the per-capture write-ups: `notes/20261010_vk_core_baseline.txt`.
Capture recipe, profiler cvars and `vk_profile_report.py`: `../vulkan_debugging.md`.
Run `python tools/vk_profile_report.py <csv>` before reading individual frames.

**Current baseline** — Fast 2026-10-12, Slow 2026-10-13. `com_fixedTic 1`, 1440p, no
upscaling. Fast = 5950X / 4070 Ti Super, Slow = 2600 / 9070 XT. p50 GPU ms:

| Machine | GI | Refl | Vol | AO | Shadows | TLAS | RT total | raster |
|---|---|---|---|---|---|---|---|---|
| Fast | 0.00 | 0.00-1.11 | 0.50-1.13 | 1.69-2.51 | 1.33-4.07 | 0.17 | **5.07-9.68** | 3.47-6.56 |
| Slow | 0.00 | 0.00-0.62 | 0.76-1.19 | 2.02-2.66 | 2.07-4.78 | 0.24-0.55 | **7.23-11.41** | 5.43-8.25 |

| Total GPU p50 (fps) | mchq | r2r | commfire |
|---|---|---|---|
| Fast | 9.17 (109) | 11.63 (86) | 14.94 (67) |
| Slow | 14.31 (64) | 15.12 (61) | 19.66 (49) |

`gpu.GI` reads 0.00 because probe GI replaced the screen-space path; its cost is
`ProbeResolve` (0.57-1.42), `ProbeTrace` (0.05) and `ProbeBlend` (0.09).

Three traps in reading any of this — the first two already produced wrong conclusions, and
**any new per-frame counter has the third exposure**:

- **The `Shadows` column understates shadows.** Serial shadow time was billed to
  `Interactions`, so a `Shadows` reading near zero was the signature of the cost being
  *highest*. Stage S closed the serial path, but the general hazard stands: check which span
  a cost lands in before quoting a column.
- **Every capture before 2026-10-12 is capped at 62.5 fps** (`USERCMD_MSEC` is integer
  `1000/60`), so GPU cost beyond ~16 ms was invisible. Never compare a capped row to this
  baseline. The same rule retires the pre-arc history table.
- **`lights.*` read zero until 2026-10-12** because the 2D GUI overlay is a second
  `RC_DRAW_VIEW` whose `ShadowBatchBegin` zeroed them before the read. Three captures looked
  like they had no shadow casters at all.

**Render scale below 0.67 buys ~0.1 ms** (`completed/20260918_fsr_upscaling.md` §12): arc 2
left a ~0.73-1.06 ms world-space floor (TLAS, probe trace/blend, froxel fill/integrate) that
screen resolution cannot touch. That floor is itself optimization surface — re-measure the
scale sweep after any B-stage or probe/froxel work before quoting it.

Two methodology notes earned the hard way:

- Demo playback rebuilds every dynamic BLAS each frame where live play updates in place
  (`cpu.TLAS.BLAS` 3-4.5 ms + `drain` 3-5 ms vs 0.2 / 0.005). Don't take CPU numbers from a
  timedemo.
- `vk_profile_report.py` catches the two mistakes already made: quoting a p50 from a capture
  that never met the finding's condition, and reading the profiler's own console print as an
  engine hitch.
