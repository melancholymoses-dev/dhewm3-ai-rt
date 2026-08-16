# Portal-Area Light Gathering — Topology-Aware GI/Vol Light Selection

**Date:** 2026-08-16
**Branch:** rt_perf2_b
**Status:** Stage 1 implemented 2026-08-16 (vk_gi.cpp collector: BFS area walk, CVars, dump/log
provenance; sphere path kept as `r_rtGIAreaLights 0` + areaNum==-1 fallback), **untested in-game** —
run the validation workflow below.  Stages 1.5 (view-stamped areas), 2, 3 not started.
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
mirror portals). **Stage 1.5:** gather from {camera BFS set} ∪ {view-stamped
areas}. Bounce rays don't need it (≤ giRadius=128 from visible surfaces, covered
by hops=2 + view areas).

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
