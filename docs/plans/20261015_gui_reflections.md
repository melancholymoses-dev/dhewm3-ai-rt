# Live GUI Screens in RT Reflections

Goal: in-world GUI screens (mars_city1 bioscan room, terminals, door panels) appear, live, in
glass reflections. Today they are missing, and you see the housing behind them.

## Current state

| Fact | Where |
|---|---|
| Stock GUI surfaces use stage-less materials (`textures/common/entityGui`: `guiSurf entity`, no stages) | `pak000/materials/invisible.mtr` |
| No stages → `MC_TRANSLUCENT` → `MF_NOSHADOWS` | `Material.cpp:2550`, `:2575` |
| BLAS skips `MF_NOSHADOWS` unless real glass, so GUI surfaces never reach the TLAS | `vk_accelstruct.cpp:614` |
| Even if admitted, `MC_TRANSLUCENT` sets `VK_MAT_FLAG_GLASS`, which `reflect_ray.rahit` ignores | `vk_material_table.cpp:688` |
| The live GUI exists only as per-view quads: `gui->Redraw()` → `tr.guiModel` → `EmitToCurrentView` | `tr_guisurf.cpp:139` |
| Which GUI a surface shows: `renderEntity->gui[GetEntityGui()-1]`, else `GlobalGui()` | `tr_light.cpp:1671` |
| Material entries are per BLAS geometry, per instance, so a per-surface texture override fits | `vk_accelstruct.cpp:1318`, `:1407`, `:1594`, `:1743` |
| Hit shading already has an emissive GUI path (`MAT_FLAG_GUI_EMISSIVE`, ×3 boost, 0.2 floor) | `rt_material.glsl:233` |
| A 2D view with zero `viewaxis` already skips every RT pass | `vk_backend.cpp:5371` |

Approach: render each live GUI into an offscreen slot texture, admit GUI surfaces to the BLAS,
and point their material entry's emissive index at the slot.

---

## Stage 0 — Census and debug colour

| Change | File |
|---|---|
| `r_vkLogRT 1`: per level, log GUI surfaces seen by the BLAS filter (model, shader, GUI name, entity/global) and whether they were skipped | `vk_accelstruct.cpp` |
| `r_logGuiSurfs 1`: every 60 frames, `R_RenderGuiSurf` calls in primary view vs subviews, plus peak; each GUI name on first draw | `tr_guisurf.cpp` |
| `VK_MAT_FLAG_GUI_SURF` (0x80, `HasGui()`) and reserved `GUI_LIVE` (0x100); `ReflPayload.hitFlags` | `vk_raytracing.h`, `rt_material.glsl`, `reflect_payload.glsl` |
| `r_rtReflectionDebugMode 10`: reflection hit on a GUI geometry = magenta; with a live slot = green | `reflect_ray.rchit`/`.rgen`, `vk_reflections.cpp` help text |

Check: mars_city1 start → bioscan room. The log lists the scanner GUIs as skipped. Peak
on-screen GUI count across mars_city1, admin and alphalabs1 sets the pool size in Stage 1.

Results (mars_city1, bioscan room):

| Measure | Value |
|---|---|
| BLAS filter | `func_static_2027`, `_4500`–`_4503`: `entitygui`, stages=0, coverage=3 (`MC_TRANSLUCENT`), all SKIPPED |
| GUIs drawn | 9 distinct (bioscan 1–4, cpu_marsmap3, cpu_one, 2 doors, reception), all primary view, recursion 0 |
| Peak per frame | primary 8, subview 0 |
| Pending | admin / alphalabs1 peak |

## Stage 1 — GUI slot pool and render-to-texture

| Change | File |
|---|---|
| Pool of 16 `_guiTex<N>` idImages, 512×384 RGBA8, mipped; `idUserInterface*` → slot, LRU by last frame seen; mapping reset in `VK_RT_BeginLevelLoad` | new `vk_gui_texture.cpp/.h` |
| `idGuiModel::EmitToTextureView(w, h)`: `EmitFullScreen`'s ortho view at 512×384, returned instead of `R_AddDrawViewCmd` | `GuiModel.cpp/.h` |
| `R_RenderGuiSurf` → `VK_GuiTex_CaptureFromGuiModel` after `EmitToCurrentView`: reuses the one `Redraw`, so no recursion and no PDA special case. Primary view, top-level GUI, once per GUI per refresh | `tr_guisurf.cpp` |
| Offscreen RGBA16F target + depth/aux attachments; render pass compatible with `hdrRenderPass`, so `VK_RB_DrawShaderPasses` and its pipelines run unchanged | `vk_gui_texture.cpp` |
| `VK_RB_DrawGuiTextures`: at frame start, before the first view's render pass, draw each queued view, blit to the slot, rebuild mips. `s_guiTexExtent` makes the draw-extent helpers unscaled | `vk_backend.cpp` |
| `r_rtGuiReflections` (gate), `r_rtGuiTexInterval` (frames between refreshes), `r_vkGuiTextureDump [slot]` | `vk_gui_texture.cpp`, `RenderSystem_init.cpp` |

No screens on screen → nothing queued → no cost.

Check: `r_vkLogRT 1` logs `VK GUITEX: slot N <- '<gui>'` on entering the bioscan room.
`r_vkGuiTextureDump` lists slots; `r_vkGuiTextureDump <slot>` writes `guitex_<slot>.tga`, which
shows the bioscan text readable and the right way up. Raster GUIs are unchanged.

## Stage 2 — Into the TLAS, bound to the slot

| Change | File |
|---|---|
| `VK_RT_NoShadowsExcluded`: translucent GUI surfaces join window glass in the BLAS; shared with the dynamic address refresh, which must walk the same surfaces | `vk_accelstruct.cpp` |
| `GUI_SURF` only for stage-less (translucent) GUI surfaces; GUI materials with stages keep their opaque `GUI_EMISSIVE` path untouched | `vk_material_table.cpp` |
| `VK_RT_BindGuiMaterial` at the 4 entry sites: GUI per instance (`tr_light.cpp:1671` rule) → slot; clear `GLASS`, set `EMISSIVE`+`GUI_LIVE`, `emissiveTexIndex` = slot image. Static sites fold slot+index into `staticSignature` | `vk_accelstruct.cpp` |
| Any-hit: reflection accepts `GUI_LIVE` (no terminate, closest wins) and ignores unbound; shadow and GI ignore `GUI_SURF`; AO already ignores non-opaque | `reflect_ray/shadow_ray/gi_ray.rahit` |
| `reflect_ray.rchit`: `GUI_LIVE` returns its emission even where dark | `reflect_ray.rchit` |
| Slot cleared to alpha 0; reflection any-hit ignores slot alpha < 0.1, so undrawn parts of the GUI quad show the housing (first test showed black squares) | `vk_gui_texture.cpp`, `reflect_ray.rahit` |
| Glow-only harvest excludes GUI surfaces (they are no longer BLAS-dropped) | `vk_rt_emitters.cpp` |
| `MatTable` log adds `gui-surf=` / `gui-live=` | `vk_material_table.cpp` |

`r_rtGuiReflections 0` unbinds every slot, which restores the pre-Stage-2 result (A/B).

Check: `r_vkLogRT 1` shows `gui-live` > 0 in the bioscan room. Debug mode 10 shows green on
the scanner screens in the glass and no magenta. No new shadow or AO lines around screens
(A/B `r_rtGuiReflections`).

## Stage 3 — Brightness and polish

| Change | File |
|---|---|
| `GUI_LIVE` skips the ×3 / 0.2 boost; scale by `r_rtGuiReflGain` (default 1) to match the raster screen | `rt_material.glsl` |
| Decide whether live GUIs feed probe GI. A/B in the bioscan room; the default follows the result | `vk_gi*.cpp` |
| Done early: entity GUIs within `r_rtGuiTexRadius` (256) that the view culled are redrawn by `VK_GuiTex_RefreshNearby` (`R_RenderView`, after `R_AddModelSurfaces`), nearest first, stopping when the pool is full. At 512 in index order, door GUIs behind walls evicted the bioscan screens every frame | `vk_gui_texture.cpp`, `tr_main.cpp` |
| `r_rtGuiReflections` (0/1) in `Dhewm3SettingsMenu.cpp`; GUI-texture pass gets its own profiler phase | settings menu, `vk_backend.cpp` |

Check: screen and its reflection side by side at similar tonemapped brightness (debug
mode 6 for raw radiance). Turning away and back shows no black flash in the reflection.
Frame cost from `vk_profile_report.py` in the bioscan room: < 0.1 ms.

## Risks

| Risk | Discriminator |
|---|---|
| Reflected text shimmers | Toggle slot mips; compare at distance |
| GI picks up screen glow where it didn't before | Stage 3 A/B; gate GI on `GUI_LIVE` |
| Pool too small in GUI-dense rooms (admin offices) | Stage 0 census; LRU eviction log |
