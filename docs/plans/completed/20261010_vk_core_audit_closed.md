# Closed: optimization arc 8 — Stage S, closed B stages, dispatched findings

Archive half of `../20261010_vk_core_audit.md`, which keeps only open work.
Nothing here is scheduled. Kept for the numbers and for the negative results —
several of these were re-proposed once already.

## Outcome of the arc

| Landed | Effect |
|---|---|
| S2 shadow pixel cull | Interactions 4.55 → 3.10, Shadows 2.07 → 1.30 (9070 XT mchq p50). The only real GPU win |
| `r_rtShadowSamples` 4 → 2 | 2 samples match 4 visually; 1 matches stencil |
| BLAS pooling (`vk_buffer_pool`) | 133 ms rebuild-burst frame → 0.9 ms. Hitch and allocation-count win, ~0 p50 |

Everything else measured inside the noise floor or to zero. The cause is structural:
all three scenarios are GPU-bound by 4-7x (CPU recording 1.4-1.7 ms against 9-15 ms GPU),
and the F group audited CPU paths.

**The three measurement defects are the arc's other output.** Each had already produced a
wrong conclusion, and S2 was unfindable until all three were fixed:

| Defect | What it hid |
|---|---|
| `USERCMD_MSEC` is integer `1000/60`, so uncapped play still capped at 62.5 fps | Every GPU cost beyond ~16 ms, in every capture before 2026-10-12 |
| The profiler's own console block cost 2.4 ms/frame mean | Read as a periodic engine hitch at exactly the window interval |
| `lights.*` read 0 every frame — the 2D GUI overlay is a second `RC_DRAW_VIEW` whose `ShadowBatchBegin` zeroed them before the read | Three captures looked like they had no shadow casters at all |

---

## Stage S — RT shadow dispatch (closed 2026-10-13)

Confirmed across 2978 frames of Mars City HQ with `lights.serial` in the CSV
(`python tools/vk_profile_report.py mchq.csv` reproduces it):

| `lights.serial` | frames | `gpu.Interactions` | `gpu.Shadows` | casters |
|---|---|---|---|---|
| 0 | 780 | 0.08 | 2.48 | 6 |
| 5 | 229 | 5.03 | 2.18 | 12 |
| 10 | 92 | 7.56 | 1.08 | 17 |
| 15 | 40 | 10.40 | 0.31 | 22 |
| 20 | 31 | 7.49 | 0.10 | 27 |

`Interactions` +7.41 ms while `Shadows` −2.38 ms: work leaves the batched dispatch and
reappears in the interaction loop, billed to raster. Total GPU 6.50 → 13.13 ms.
That handoff is why no single p50 can see the finding, and why a `Shadows` reading near
zero is the signature of the cost being highest.

| Finding | Where |
|---|---|
| Only 7 lights batched (`VK_RT_SHADOW_BATCH_MAX`); 74% of frames overflowed. The rest traced one at a time with a render-pass break, depth round-trip and its own blur | `vk_backend.cpp:4107`, `vk_shadows.cpp:1248` |
| Batch got 1.9 M pixels, the serial path 31.9 M (8.6 screens) | `VK_RT_ShadowBatchAddLight` |
| Dispatch rect is the light scissor — full screen when the camera is inside the light volume | `VK_ComputeLightScissor` |
| No per-pixel volume test; the only cull was a point-light sphere, off by default | `shadow_ray.rgen:180` |

| Step | Verdict |
|---|---|
| S2 pixel cull | **Done.** World pos tested against `lightDef->frustum[6]` padded 4 units, right after reconstruction; outside stores 1.0 and returns. `r_rtShadowVolumeCull` (default 1) replaced `r_rtShadowDistanceCutoff`. 0.50 → 0.33 ms per serial light |
| S2b back-face skip | **Reverted, no gain.** Back-facing rays hit their own surface within a few units and were already cheap |
| S3 looped batches | **Done, no gain.** Breaks p50 1 / max 4, 0.34 ms per serial light unchanged — break overhead was never the cost. Added the `gpu.Interactions.Shadows` span, which is what proved it |
| S4 delete serial path | **Done.** Scratch layer became a batch layer, W=8 |
| S5 rect shrink, S6 stencil clear, S7 W=16 | **Dropped.** S5 only trims pixels S2 already makes cheap; S4/S7 had no break overhead left to win |

Measured split (mchq3, 9070 XT, 4 samples): per serial light, in-loop trace + blur is ~90%
of the climb and shading ~10% (18 serial: 5.23 vs 0.65 ms). Blur off saves 0.3-0.7 ms.

**Remaining cost is lit, front-facing rays reaching the light, so it scales with
`r_rtShadowSamples`.** If shadow cost matters again the next lever is adaptive sampling:
1 ray, refine to N only where a 3x3 neighbourhood disagrees.

---

## Closed B stages and the A/B results

| # | Result |
|---|---|
| B2 rebuild burst | Cause was allocation, not build work: 2N+2 `vkAllocateMemory` per rebuild at ~0.28 ms each, flat in size. r2r frame 15770 spent 129 of 133 ms allocating (35 first-sight BLASes, 7 `_areaN` = 86 ms). `vk_buffer_pool` (the vertex-cache allocator, generalized) now backs AS storage, scratch and host-mapped geometry. The later ~31-37 ms "residual" was the rebuild log itself at ~1 ms per line |
| B2 static BLAS flags (R3) | No `ALLOW_UPDATE` for static-model BLASes. **No trace gain** — every pass between or beside the two baselines. Kept for memory: static scratch 0, pool holds only dynamic scratch (3.5 MB) |
| R7 BAR placement | `DEVICE_LOCAL\|HOST_VISIBLE` for material/address/BLAS-geom buffers: `gpu.total` p50 8.65 → 8.48, −2.0%, against a −1.8% same-config repeat. **Noise.** Per-hit reads over PCIe are not a cost here. B4's BAR row dropped, B1 parked — R1's read-cost claim rested on the same result |
| R2 frame serialization | Single-copy dynamic BLAS written while the previous frame may still trace it. `r_vkSerializeFrames 1` at 13 dynamic BLAS/frame: backend 12.08 → 15.65 from lost CPU/GPU overlap, **no visible artifact difference** on character shadows or reflections. Latent, not visible. Sync validation cannot see the CPU `memcpy`, so this stays unproven rather than disproven |

B0 instrumentation landed as part of these; its counter list is in the baseline notes.

## Dispatched by measurement

Not deleted: a different GPU, a CPU-bound scenario or a streaming-heavy map could revive any
of them. Numbers from the 2026-10-12 capture, 8761 frames across three scenarios.

| # | Counter | Reading | Verdict |
|---|---|---|---|
| F1 | `ring.idxKB` | p50 338-589 KB/frame re-copied, inside `cpu.raster`'s 0.9 ms | CPU case dead. Survives only as B1's dependency |
| F2 | `frame.upload` vs previous-frame GPU | Correlates 62-74x across quintiles; real upload cost `vc.allocMs` p50 0.014 ms | GPU back-pressure, not upload cost. An async flush moves the wait |
| F3 | `midframe.imageLoads` | 0 across 8761 frames | The one-by-one lazy load never fires in gameplay (`image_preload 1`) |
| F5, F12 | `mem.rssMB`, `mem.vramUsedMB` | RSS 145-219 MB flat; VRAM 14% of 15.5 GB | No residency pressure; BC compression has no case |
| F6 | `cpu.raster` | p50 0.5-0.95 ms for every push and draw | Dead |
| F7 | — | Zero with `r_rtShadows 1`; shadow volumes never generated (`Interaction.cpp:1060`) | Dead as documented |
| F8 | `ring.overflows` | 0 every frame; peak 1023 KB of a 64 MB ring | Dead as a fix, kept as a guard rail |
| F11 | `vc.allocMs` | p50 0.01 ms, max 0.08 | Dead |
| X1 | `frame.frontend` | p50 0.75-1.36 ms (`addModels` 0.68-1.02) | Not a factor |
| X4 | `midframe.pipelines` | 2 events in 8761 frames, 0.23 ms each | Dead as a hitch source. A `VkPipelineCache` is still worth having for startup |

## Parked stages

Written up, measured to zero. Each needs its counter re-read before reopening; guards are in
git history.

| Stage | Was | Why parked |
|---|---|---|
| A2 | Async upload flush, no mid-frame lazy load, reuse the upload command buffer (F2, F3, F9) | F2 is back-pressure; `midframe.imageLoads` is 0 |
| A4 | Drop `virtMem` after GPU upload; BC compression (F5, F12) | RSS 145-219 MB flat, VRAM 2.1 of 15.5 GB |
| A5 | Skip redundant state, ring high-water sizing, debug asserts (F6, F8, F11, F13) | `cpu.raster` p50 0.5-0.95 ms; `ring.overflows` 0 |
| B1 | Zero-copy BLAS: static `indexCache` on the GPU, per-slot dynamic geometry ring, persistent maps | Gate was "R2 or R7 confirmed"; neither was. Reopen for memory or correctness only |
| A1 | Static index buffers (F1) | Only ever B1's prerequisite |

The two findings B1 existed to fix, kept because both are still true in the code:

| # | Finding | Where |
|---|---|---|
| R1 | The BLAS "zero-copy" path needs both `ambientCache` and `indexCache` on the GPU, and `indexCache` never exists under Vulkan (F1), so **every** BLAS geometry takes the fallback: its own host-visible vertex buffer (full 60-byte `idDrawVert`) and index buffer, 2 `vkAllocateMemory` + `memcpy` per geometry. Dynamic entities pay `vkMapMemory` + full copy + unmap per geometry per frame. Confirmed by `VK RT BLAS SRC gpuPct=0.0`. R7 disproved the read-cost half of the claim; the allocation half was fixed by pooling, so what is left is PCIe traffic that does not measure | `vk_accelstruct.cpp:520-523`, `1005-1037`, `876-882` |
| R2 | Dynamic-entity BLAS geometry buffers and the AS itself are single-copy, updated in place every frame, with 2 frames in flight. Frame N's `memcpy` and `MODE_UPDATE` can overlap N-1's trace; TLAS and material tables are per slot, the BLAS is not, and no frame-start barrier orders N-1's reads before N's write. Expected symptom is a one-frame normal/position mismatch on skinned entities under load — not observed in the A/B above | `vk_accelstruct.cpp:847-930`, `1301-1311` |

Risky-edit guards for the closed and parked stages, if any reopen: A1 — index block purged
while a BLAS or draw still references it, TAG_TEMP misclassified as static; needs an A/B cvar
and shadow indexes excluded. B1 — address tables pointing at the other frame slot, TLAS
static-signature churn (the update path relies on a stable device address).
