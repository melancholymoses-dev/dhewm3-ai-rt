# Portal-Area Light Gathering — Topology-Aware GI/Vol Light Selection

**Date:** 2026-08-16
**Branch:** rt_perf2_b
**Status:** Stage 1 implemented 2026-08-16 (vk_gi.cpp collector: BFS area walk, CVars, dump/log
provenance; sphere path kept as `r_rtGIAreaLights 0` + areaNum==-1 fallback).
**Doorway validation passed in-game 2026-08-16** (Command Access Junction: area 78's ~22 lights
absent with the door closed, admitted at hop=1 once opened; provenance/dedupe confirmed via dump).
Remaining validation: big-room check, A/B perf comparison, door-transition pop watch.
**Latest (2026-08-22) — read this before the blow-by-blow below:** in-game dumps at a real
hallway boundary confirmed Stage 1.75 works as designed (admission set stable across the
threshold) and caught Stage 1.5 actually firing (view-flood union nearly doubling the candidate
pool when a hub area becomes visible down a corridor). Neither was the full story — the actual
volumetric on/off pop traced to `vol_march.comp` sharing GI's light buffer/ranking, so
Stage-1.5-admitted-but-march-unreachable lights could evict genuinely nearby ones. **Fixed**:
volumetrics now has its own dedicated, distance-filtered light selection (separate SSBO), see
the "Real fix implemented" entry near the end of this doc. `r_rtVolMaxLights` bumped 32→96 along
the way as an interim mitigation before the real fix landed; harmless to leave, mostly redundant
now. **Validated in-game 2026-08-22 — confirmed fixed** (user: "vol done"). Portal-area gathering
(Stages 1/1.5/1.75) + the dedicated vol selection are considered closed; Stage 3 (per-room
pin/ban curation sidecar) not started, not currently blocking anything.
**Stage 1.5 implemented 2026-08-19** (vk_gi.cpp: unions in every `portalArea_t` id's own
view-frustum flood stamped visible this frame — `viewCount == tr.viewCount` — with the BFS
set; zero new cost, reuses the flood the standard renderer already runs. Motivated by a
real observed pop: standing still in area 42 looking across at area 45's lights, which sat
just outside 2-hop range from area 42 but *inside* range from the adjacent area 49 — dump
comparison confirmed the BFS itself was working correctly (hop-boundary cliff, not an
adjacency bug), but a stationary "looking across the room" case has no BFS transition to
smooth, which is exactly the case Stage 1.5 (not Stage 2) addresses. Dump/log extended:
`hop=-1` with a real `area` means Stage-1.5-sourced; summary log reports
`visited=N(+M viewStamp)`. **Untested in-game** — validate via dump before/after the same
hallway/room boundary that motivated it.
Stage 2 (temporal blend across area transitions) work was implemented then **deliberately
reverted 2026-08-19** — a companion GI/Vol temporal camera-cut fix it depended on was still
broken, and Stage 1.5 was judged higher priority: it covers the more common case
(visible-but-out-of-hop-range) without needing transition detection at all. Stage 2 remains
valuable for the residual
case Stage 1.5 can't cover — a blind-corner room that becomes visible only after you're
already in it — but is not planned until Stage 1.5's real-world coverage is assessed.
**Stage 1.75 implemented 2026-08-19** (vk_gi.cpp: BFS now seeds from every portal area
touching a `+/-r_rtGIAreaBoundsEpsilon` (default 24u) box around the camera, via
`idRenderWorld::BoundsInAreas`, instead of the single hard `PointInArea(camera)` pick).
Root-caused from a report of volumetric lights changing while standing still at a
corridor/room threshold, facing perpendicular to the boundary and stepping a few inches
left/right — same view, same wall filling the screen, but the lit fog visibly popped.
`PointInArea` is a binary plane test: on either side of a threshold the *entire* hop
numbering for every light in both rooms flips (a light at hop=1 from one side is hop=0
from the other, which can cross a hop-based cutoff or the L1 importance/hysteresis
threshold even though nothing on screen changed) — Stage 1.5's view-flood union doesn't
help here because the portal you're straddling usually isn't the one facing your camera.
Dump/log extended: a `[Stage 1.75]` line lists the actual seed-area set found vs. what the
old single-point classification would have picked. **Untested in-game** — validate by
dumping while straddling the same threshold and confirming both areas appear in the
`seedAreas` list and the hop numbers no longer flip between the two dumps.
**Stage 1.75 in-game dump analysis 2026-08-22:** first real before/after capture at a hallway
threshold (`[Stage 1.75] seedAreas(2)=[42,49]` while straddling → `seedAreas(1)=[49]` a step
later). Confirmed working as designed — both dumps reach the identical six areas (42/49/48/50/
46/45) within `hops=2`, and neither dump has a `hop=-1` entry, so Stage 1.5 fired zero times in
this capture. Yet the user still saw volumetric lights pop at that same threshold. Root cause is
**downstream of the area walk, in the volumetric consumer, not in the collector**: `vol_march.comp`
only reads a fixed prefix (`r_rtVolMaxLights` = 32) of the GI light buffer, and that buffer is
sorted by L1 *importance*, not distance (the shader's old comment claiming "pre-sorted
nearest-first" was wrong — fixed). Ranking all ~65 admitted candidates by importance in each dump,
the rank-32 cutoff value itself moved from ≈0.0143 to ≈0.0212 between the two captures, because
*unrelated* lights elsewhere in the level (area 46/45's bright fixtures) gained importance as the
player walked deeper into area 49. That cutoff shift silently dropped `lights/
squarelight1_snd_noflicker` in **area 50** — a light ~750-770u away whose own importance barely
moved (0.0150→0.0144) — out of the volumetric window. Net effect: stepping through a doorway
perturbs many lights' distances at once, which reshuffles the *global* importance ranking, which
flips the hard rank-32 cutoff for whatever light happens to be sitting near it anywhere in the
level, not necessarily anything near the door. No crossfade exists at that boundary, so it reads
as a hard on/off pop.
Diagnostic added (uncommitted as of this note, same session): the `r_rtGILightDump` output now
also prints the final importance-sorted upload order with a `<-- VOL CUTOFF (r_rtVolMaxLights)`
marker, so a before/after at a pop shows exactly which light crossed the line and which
competitors pushed it. `r_rtVolMaxLights` promoted from `static` to extern (was file-local in
vk_vol.cpp) so vk_gi.cpp can read it for the marker; its description and the shader comment were
both corrected to stop claiming nearest-first.

**Second capture 2026-08-22, same session, zig-zag hallway (three dumps a few metres apart):**
this one caught Stage 1.5 actually firing, and it's a much bigger perturbation than the importance
drift above. Dump 1 (`seedAreas=[42,49]`): ~65 local candidates, zero `hop=-1`. Dump 2, ~3.7s
later in the same corridor (`seedAreas=[49]`): same ~65 local candidates *plus* `hop=-1` area 1
(~52 lights) and area 44 (4 lights) — the candidate pool nearly doubles, purely from the view-flood
union suddenly reaching a large hub area (area 1) that the BFS never reaches within `hops=2` in any
of the three dumps. Dump 3, ~6.3s later: area 1 present again (area 44 now legitimately BFS-reached
at hop=2 instead). User confirmed in-game the visible symptom is a clean **on/off at the boundary
crossing, not per-frame flicker** — consistent with `portalArea_t::viewCount`'s binary
frustum-through-portals test flipping once as you cross a threshold in a corridor, rather than
noise. Zig-zag corridors are exactly where that recursive portal-clip test is most marginal
(grazing sightlines down a passage). Area 1's burst includes lights with real importance
(`squareishlight_snd` 0.0813, `squarelight_breakable` 0.0592) so it isn't just noise-floor padding
— it genuinely competes for top-K slots. This is judged the dominant cause of the reported pop (the
rank-32-drift mechanism above is real but secondary/compounding).
**Decision: `r_rtVolMaxLights` bumped 32→64** ([vk_vol.cpp:71](../../neo/renderer/Vulkan/vk_vol.cpp))
as the cheap, immediate mitigation — pushes the cutoff deeper into the ranked list so a ~2x
candidate-pool swing from Stage 1.5 is less likely to evict something that was visibly
contributing. Chosen over per-area Stage-1.5 dwell/hysteresis or a reserved Stage-1.5 sub-quota
(both discussed, neither implemented) because it's zero-risk and sufficient as a first pass; those
remain the follow-up if 64 still isn't enough headroom for a hub-area reveal this size.
**64 tested in-game 2026-08-22, still some pop** — bumped again to **96** (halfway to the shader's
hard clamp of 128, see `min(params.maxLights, giLightBuf.numLights)` in vol_march.comp) as a
second cheap step. Flagged risk before bumping: unlike the 32→64 step, cost here isn't uniformly
cheap — lights that fail the sphere pre-cull are nearly free, but lights actually along a visible
march ray get a real ray-query shadow test per march step, and the Stage-1.5 hub-area-reveal
scenario that motivates the bump is exactly the case where some of those extra candidates *are*
on-screen/near visible rays. (Correction to a note made in-session at this point: there IS a GPU
timer that isolates the vol march phase — `r_vkRTProfile 1/2`, see `VK_RTPROF_PHASE_VOL` in
vk_backend.cpp, separate from `VolTemporal`/`VolBilateral`/`VolComposite`/GI. Use it before
assuming cost, not the "we have no way to tell" claim made when 96 was picked.)

**Real fix implemented same session, superseding the maxLights-bump-as-mitigation approach above:**
re-examining the reachability math, most of Stage 1.5's area-1 burst (camera distance 600-860u)
is provably **beyond `r_rtVolMaxDist`=512 even generously** (distance minus 1.5x sphere radius
still exceeds 512) — those lights can never appear in the fog at all, in any frame, regardless of
selection. They were never diluting a fair ranking; they were dead weight competing for and winning
slots away from genuinely march-relevant nearby lights. Raising `r_rtVolMaxLights` was treating the
symptom (not enough slots) rather than the cause (slots wasted on geometrically-impossible
candidates). Fix: volumetrics now gets its **own dedicated light selection**, built in
`VK_RT_UploadGILights` from the same admitted-candidate pool GI already computed (no extra
light-gathering cost) — walks GI's own importance-ordered selection (inheriting its near/far tier
balance and hero-light protection for free, see the distance-sort-vs-hero-light discussion above)
and keeps only candidates whose sphere/cone can possibly reach within `r_rtVolMaxDist` of the
camera, capped at `r_rtVolMaxLights`. Uploaded to a new `vkRT.volLightSsbo` (mirrors
`giLightSsbo`'s layout/lifecycle) that `vol_march.comp` binding 4 now reads instead of GI's buffer
— GI/reflections' own selection (`giLightSsbo`) is untouched, so a light legitimately useful for a
bounce far from the camera still gets it. `r_rtGILightDump` now prints both downstream selections
(`[final order]` = GI's, `[vol selection]` = vol's, with the skip reason implicit — reachable
candidates make the list, others don't). Files: vk_gi.cpp (selection + SSBO create/destroy),
vk_raytracing.h (new buffer fields), vk_vol.cpp (descriptor now points at the new buffer;
`r_rtVolMaxDist`/`r_rtVolMaxLights` promoted from `static` to extern), vol_march.comp (comments
only, binding is the same slot/layout). **Tested in-game 2026-08-22 — the hallway on/off pop is
confirmed fixed.**

Stage 3 not started.  Observed follow-up candidate from the dumps: lights currently off
(`color 0 0 0`, e.g. broken/triggered fixtures) are admitted and burn candidate slots — a
`maxChan > 0` guard in the collector would drop them per-frame safely.
**Companion docs:** `auto_relight.md` (§0 classifier is the admission layer this feeds),
`rt_optimization_tuning.md` (L1 stratified selection is the ranking layer this feeds).

---

## The problem (from the 2026-08-16 design session)

`VK_RT_UploadGILights` culls candidate lights with a **Euclidean sphere around the
camera**: `collectRadius = r_rtGIRadius × r_rtGILightCollectRadiusScale`, recomputed
from scratch every frame. Two failure modes, both observed in play:

1. **Goes dark in big rooms.** Walk into a large area whose lights are beyond the
   sphere and the candidate list empties — GI/vol have no notion of "this room has
   lights, they're just far away." Confirmed: `r_rtGIRadius` toward 0 → nothing;
   large (500) → nearby lights diluted by a flood of distant candidates competing
   for the same slots.
2. **Admits lights through walls.** Euclidean distance ignores geometry: a light
   96 units away *through a sealed wall* is admitted and burns an upload slot +
   stochastic-selection probability mass, while contributing nothing visible.

Root observation (project owner): this is machinery re-deriving, every frame from
raw positions, a judgment a human makes instantly by looking at the room — "which
lights matter *here*." The question is about **space/topology**, not camera radius.

## The discovery: id already computed the answer

The 2004 engine maintains, incrementally and for its own purposes, exactly the data
structure we need. None of this is new machinery — it is **live, tuned, shipping
code** that the RT collector has simply been ignoring:

| Structure | Where | What it gives us |
|---|---|---|
| `portalArea_t::lightRefs` | `RenderWorld_local.h:74` | Doubly-linked list of every light whose **volume actually reaches this area** |
| `idRenderLightLocal::references` | `tr_local.h:242` | Inverse view: every area a given light touches (`ownerNext` chain) |
| `R_CreateLightRefs` | `tr_lightrun.cpp:490` | Maintains the above by pushing the light's *frustum volume* through the BSP (`PushVolumeIntoTree`) or flooding through portals (`FlowLightThroughPortals`) — so refs respect walls, not just distance |
| `viewDef->areaNum` | `tr_local.h:444` | The camera's area, already computed every frame |
| `viewDef->connectedAreas` | `tr_local.h:446` | Per-frame bool array: which areas are reachable **without crossing a closed door** |
| `portal_t::intoArea` / `doublePortal_t::blockingBits` | `RenderWorld_local.h:44-63` | The area adjacency graph, with door open/closed state |
| `idRenderWorldLocal::portalAreas` / `numPortalAreas` | `RenderWorld_local.h` (public) | The area array itself — `vk_gi.cpp` already includes `RenderWorld_local.h` |

Critical implication — **the "dynamic lights" complication dissolves**: muzzle
flashes, projectile lights, and scripted lights are ordinary lightDefs whose refs
are refreshed by `R_CreateLightRefs` on every `UpdateLightDef`. The per-area lists
are always current. There is **no load-time bake, no cache invalidation problem**.
This design is a *different per-frame gathering loop*, not a caching system.

Bonus correctness inherited for free (`tr_lightrun.cpp:516-524`): a light's
`areaNum` is used by the original renderer to cull lights behind closed doors —
intent id already tuned per-map. And prelight shadow-casting lights use
`FlowLightThroughPortals`, meaning their area refs are *visibility-shaped*, not
just volume-shaped.

## Design

### Stage 1 — replace the sphere cull with an area walk (the core change)

In `VK_RT_UploadGILights`, replace the `for all lightDefs / distance-cull` loop
with:

```
1. startArea = viewDef->areaNum
   (if -1 — noclip outside world, some cinematics — fall back to the current
   camera-sphere path unchanged; keep that code as the fallback branch)
2. BFS over the portal graph from startArea:
     - follow portalArea_t::portals → portal_t::intoArea
     - skip portals whose doublePortal->blockingBits block view (closed doors);
       equivalently, intersect with viewDef->connectedAreas
     - depth-limit: r_rtGIAreaHops (default 2 — current room, neighbors,
       neighbors-of-neighbors)
3. For each visited area, walk portalArea_t::lightRefs (areaNext chain);
   dedupe lights spanning multiple areas with a per-frame visited stamp
   (CPU-local int array indexed by lightDef index — NOT tr.frameCount-keyed
   GPU state; see feedback_per_slot_counters)
4. Each unique light then flows through the EXISTING pipeline unchanged:
   VK_RT_ClassifyLight admission (§0) → saturation-weighted importance →
   L1 tier/quota/hysteresis selection → upload
```

Everything downstream — classifier, importance, stratification, hysteresis, the
shaders, the SSBO layout — is untouched. Volumetrics inherits automatically (same
buffer). This is a ~100-line change confined to the collector.

Distance doesn't disappear — it remains inside the importance formula (received
flux) and the L1 tiers. What changes is its **role**: from primary *admission*
criterion (wrong: walls don't care about radii) to *ranking* input (right: nearer
lights still usually matter more). A generous absolute distance cap
(`r_rtGIMaxLightDist`, default ~2048) stays as a safety net for pathological
mega-areas.

### Why this fixes both observed failures

- **Big room goes dark** → the room's own `lightRefs` list is the room's lights,
  regardless of its size. "In this room" replaces "within N units of me."
- **Through-wall admission** → a light only has a ref in your area if id's volume
  push / portal flood determined its frustum actually reaches your area. Sealed
  wall ⇒ no ref ⇒ no slot wasted. The stochastic sampler stops spending
  probability mass on invisible lights — a straight variance win at zero cost.

### Stage 2 — transition smoothing (only if Stage 1 shows pops)

Crossing a portal changes `startArea`, swapping the gathered set in one frame.
Three existing mechanisms already blunt this — evaluate before adding anything:

1. L1 hysteresis (incumbent ×1.15 boost) already resists selection churn.
2. The BFS overlap: at hops=2, moving one room shifts only the outer ring of the
   visited set, not the whole list.
3. GI/vol temporal EMA smooths the resulting radiance change over ~10 frames.

If pops are still visible: blend — for `r_rtGIAreaBlendFrames` (default ~8) after
an area change, gather from the union of old and new start areas. Cheap, bounded,
and only runs during transitions. Do not build this until an observed pop demands
it (pillar 6: overlays before tuning).

### Stage 3 — the curation hook (the art-problem escape hatch)

From the design session: *"if you could run through the levels in game and flag
the important ones, you'd do it."* The area walk makes that workflow natural,
because "the current area" is now a first-class concept the collector understands:

- `rtLightPin` console command: stand in a room, aim at / name a light (reuse the
  dump's identification), and record `(mapName, areaNum, lightIdx-stable-key,
  boost|ban)` to a **sidecar file** — `base/rtlights/<mapname>.rtl` — loaded at
  map start into a small per-area override table.
- Collector applies overrides after classification: `boost` multiplies importance
  (hero light for this room), `ban` drops it (that annoying fill).
- Sidecar files keep the **drop-in-assets promise** (pillar 5: no map edits) while
  admitting the honest limit of engine-side rules: some rooms will always need a
  human's eye. The classifier gets you a good default everywhere; the sidecar
  captures art direction where "good default" isn't "good."
- v1 scope: pin/ban + boost factor only. No positional edits, no color overrides —
  those are auto_relight/def-patch territory.

Stable light identity across loads is the one design wrinkle: lightDef indices
shift with entity spawn order. Key on quantized origin (e.g. origin/8 rounded)
+ material name — collisions effectively impossible within one area.

## Audit: our RT reach extensions vs. id's volume-exact refs (2026-08-16)

We deliberately let RT lighting reach beyond a light's id-defined volume in three
places. id's area refs are computed from the *exact* volume — so each extension was
audited against the flood logic. Key structural point: the walk gathers **every
light referenced by any visited area** and never re-tests volume-vs-camera, so
candidacy and shading reach stay decoupled; the halos apply to gathered candidates
exactly as today.

| Extension | Where | Verdict |
|---|---|---|
| Point-light 1.5× sphere halo | `vk_gi.cpp` entry build (`posRadius[3] = radius*1.5`), cutoff in `rt_LightContribAt` | Survives. Fails only if a halo crosses more portals than `hops` — impossible at Doom 3 room scale (halo ≤ ~0.5× box radius) |
| Projected cone 1.1× reach | same entry build | Survives (smaller margin, same argument) |
| Player-reflection 2× reach, inverse-square, no hard cutoff | `player_reflect.rchit:110` | Survives — its hit points are on the player, in the camera's area, the best-covered spot. Its divergent falloff math is a T4-family issue, not a gathering issue |

**Real structural mismatch:** the list is camera-centric but reflection rays can
hit rooms outside the BFS set (mirror showing a distant hall). Not new — the 512u
sphere has the same hole — but the fix is cheap and id-native: the view flood
already stamps every on-screen area (`portalArea_t::viewCount`, including through
mirror portals). **Stage 1.5 (✅ implemented 2026-08-19):** gather from {camera BFS
set} ∪ {view-stamped areas}. Bounce rays don't need it (≤ giRadius=128 from visible
surfaces, covered by hops=2 + view areas). Landed for a different motivating case
than originally written here (stationary hop-boundary pop, not mirror sightlines)
— same fix, same mechanism, both benefit.

Harmless permissivenesses, accepted: fogged-portal closure is ignored by the
blockingBits-only BFS (extra candidates; the fog lights themselves are
classifier-rejected); lights with origins embedded in solid (`light->areaNum ==
-1`, common for ceiling fixtures) still have volume refs — which is why the walk
reads per-area `lightRefs` chains and never trusts `light->areaNum`. Emissive
bounce lighting bypasses the light list entirely — unaffected.

## Transparent surfaces (windows) — verified 2026-08-16

The gathering layer is window-correct end to end: glass doesn't seal dmap areas
(window rooms are one area, or joined by a mapper visportal); no game code ever
sets `PS_BLOCK_VIEW` on a glass portal (`idVacuumSeparatorEntity` — id's
airlock/Mars-window entity, `Misc.cpp:2259` — blocks AIR|LOCATION but *not* view,
so the BFS crosses it); and `R_CreateLightRefs` pushes light volumes through the
opaque-brush BSP, which glass doesn't interrupt. Window-behind lights are gathered.

But the *transport* paths disagree about what happens next (all pre-existing,
independent of this plan — the area walk just makes them visible by reliably
delivering window-lights):

| Path | Glass | Where |
|---|---|---|
| Direct shadow masks | passes through (deliberate `isWindowGlass` BLAS exception) | `vk_accelstruct.cpp:621`, `shadow_ray.rahit` |
| GI bounce shadow rays | **blocked** — `gi_ray.rahit:57` accepts glass, terminate-on-first-hit reads it as occluded | `gi_ray.rahit` |
| Volumetric march | **blocked** — `gl_RayFlagsOpaqueEXT` overrides per-geometry non-opaque flags | `vol_march.comp:288` |

Consequences: window lights illuminate rooms directly but produce no GI bleed and
no volumetric shafts through the window — the classic sunbeam-through-glass shot
is currently impossible. Follow-up candidates (NOT Stage 1 scope):

- **GI:** make the GI shadow ray pass glass the way `shadow_ray.rahit` does
  (distinguish shadow rays from primary bounce rays in `gi_ray.rahit`, e.g. via a
  payload flag or a dedicated shadow hit group). Cheap, same pattern as shadows.
- **Vol:** drop `OpaqueEXT` and process non-opaque candidates in the march's ray
  query loop. Costlier — per-step candidate processing — so gate it on the
  existing `sceneHasGlass` probe (P6) so glass-free scenes pay nothing.
- Until then: window-behind lights admitted by the walk waste GI stochastic
  probability mass. If that shows up in practice, an interim `ban` via the
  Stage 3 sidecar handles the worst offenders by hand.

## What this deliberately does NOT do

- **No per-area precomputed light lists / bake** — the engine's live refs make it
  unnecessary (see above). If profiling later shows the per-frame walk matters
  (it won't — it's pointer-chasing over dozens of nodes), caching per-area
  *classified* results with a dirty bit on light updates is the obvious v2.
- **No BSP/cell system of our own** — the whole point is to ride id's.
- **No behavior change for reflections** — `player_reflect.rchit` consumes the
  same buffer and simply benefits from a better-selected list.
- **No shader changes at all.**

## Interactions with the existing stack

- **§0 classifier** (`vk_light_classify.cpp`): unchanged, still the admission
  gate. Fog/blend lights have area refs too — classification rejects them after
  gathering, exactly as today.
- **L1 stratified tiers**: unchanged. Tiering by distance *within* a
  topology-correct candidate set is strictly more meaningful than tiering a
  sphere-cull set.
- **auto_relight (Wave 5)**: synthesized lights are `AddLightDef` lights → they
  get area refs automatically → they participate in area gathering with zero
  extra work. The area walk also gives Wave 5's volumetric budget a natural
  scope: "hero lights *of this room*."
- **`r_rtGILightCollectRadiusScale`**: becomes fallback-path-only. Keep it
  registered (the -1-area fallback uses it) but document the demotion.
- **`r_rtGILightDump`**: extend the dump line with `area=N hops=H` — the debug
  view for validating the walk (pillar 6). First validation step on any map:
  dump in a doorway and confirm lights behind the closed door don't appear.

## CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_rtGIAreaLights` | 1 | master toggle; 0 = legacy camera-sphere collector (A/B) |
| `r_rtGIAreaHops` | 2 | BFS portal-hop depth from the camera's area |
| `r_rtGIMaxLightDist` | 2048 | absolute distance safety cap within visited areas |
| `r_rtGIAreaBlendFrames` | 8 | Stage 2 only, if built: union-gather frames after an area change |

## Validation workflow (before any tuning)

1. `r_rtGILightDump 1` at a doorway with the door **closed**, then **open** —
   the far room's lights must appear only in the second dump.
2. Same dump in the middle of a big room (commoutside landing pad): confirm the
   room's own lights are admitted regardless of distance, where the sphere cull
   previously dropped them.
3. `r_rtGIAreaLights 0/1` A/B in the ACO hallway from the 2026-08-15 session
   (the "75 candidates in 512u" spot): candidate count should drop, upload
   quality should visibly improve or hold, frame time should not rise.
4. Walk a door transition watching for GI pops (Stage 2 trigger).
5. Perf: `com_showFPS` + RT profiler `GI` phase — expect neutral-to-better
   (the walk visits dozens of refs vs. iterating every lightDef in the world).

## Effort / risk

- **Stage 1:** ~half a day. Risk low — confined to one collector function with
  the legacy path kept as runtime fallback (`r_rtGIAreaLights 0`), and the
  data structures it reads are maintained by 20-year-old shipping code.
- **Stage 2:** hours, only if needed.
- **Stage 3:** ~a day including sidecar load/save + stable keys. Risk low;
  purely additive.
- **Biggest unknown:** area granularity on huge outdoor-ish maps (commoutside) —
  one giant area would make hops irrelevant and lean on the distance cap +
  importance ranking, i.e. degrade gracefully to today's behavior. Dump-and-look
  before assuming.
