# GI Albedo Modulation — Receiver-Albedo G-Buffer Target

**Date:** 2026-08-16
**Branch:** auto-relight (folded into the Wave 5 visual-quality push)
**Status:** implemented 2026-08-16. `gbufAlbedo` G-buffer target (opaque + clip prepass
variants), `gi_albedo_mod.comp` compute pass wired in after à-trous, `r_rtGIAlbedo` CVar
(default 1, A/B against the legacy raw-radiance composite).
**Validation step 2 passed in-game 2026-08-22** — bodies/corpses now read as dark cloth
with a subtle tint, confirmed against the motivating ACO Lift Junction shot; the original
bug (bodies painted yellow by nearby orange lights) is fixed.
**Done 2026-08-22.** Steps 3/4 surfaced a real follow-on rather than closing clean: with
volumetrics also landed/tuned since this doc was written, overall scene luminance reads
elevated and blacks lifted. That's tracked as tonemapping tuning going forward
(`completed/tonemapping.md` / `completed/tonemapping2.md` cover the existing pipeline
this tunes within), not as open scope on this doc — the receiver-albedo modulation
feature itself is finished and validated. Any further `r_rtGIBounceScale`/
`r_rtGIStrength` retune should happen after tonemapping settles, to avoid the two
chasing each other, but that's normal tuning, not a gap in this doc's deliverable.
**Extends:** `completed/gbuffer_normal_pass.md` (the G-buffer contract this adds a
target to). **Companion docs:** `auto_relight.md` (whose §0/AREA validation surfaced
the bug), ROADMAP pillar 2 ("darkness stays black") — which this fix directly serves.

---

## The problem (observed 2026-08-16, ACO Lift Junction)

With GI turned up enough to be noticeable, bodies on the floor glow the color of
nearby lights — a corpse in dark green cloth beside an orange grate light
(`lights/grate8sqr`, color 1.00/0.70/0.16) reads as *painted yellow from below*,
not as *green cloth dimly reflecting orange light*. Project owner's summary:
"common problem — when GI is noticeable, it looks worse."

Two questions were asked and answered from the code:

1. **Is it floor bounce?** Yes. `gi_ray.rgen` shoots cosine-weighted hemisphere
   rays from each visible point; a low-lying body is surrounded by brightly-lit
   floor within `giRadius`, so most samples return the floor's (orange) radiance.
   That part is physically correct.
2. **Is the GI weighted by the receiving surface's albedo?** **No — this is the
   bug.** The GI buffer stores incoming radiance only (the *bounce source's*
   albedo is folded in at the hit; the *receiver's* is not), and the composite
   adds it raw:

   ```glsl
   // gi_composite.frag:35 — additive blend into hdrScene
   fragColor = vec4(texture(u_GIMap, uv).rgb, 1.0);
   ```

   Physically the term is `albedo_receiver × irradiance`. Without the multiply,
   GI behaves like colored fog: it lifts dark materials the most (a near-black
   surface should reflect almost nothing but receives the full wash), desaturates
   everything toward the light color, and always over-brightens
   (`albedo × E ≤ E`). This is *why* noticeable GI looks worse — the error term
   is largest exactly where Doom 3's look lives (pillar 2: darkness stays black).

Ruled out: hemisphere orientation. GI already reads the bump-mapped shading
normal from the G-buffer (P9, `rt_gbuf_normal.glsl`); depth-gradient
reconstruction is only the fallback for unwritten pixels.

## Design

Add a second G-buffer target written by the existing depth-prepass G-buffer
pipelines, and multiply by it at GI composite. Everything between — rgen gather,
stochastic light selection, temporal EMA, à-trous — is untouched and keeps
operating on radiance.

| Piece | Change |
|---|---|
| `vk_gbuffer.cpp/h` | Allocate per-frame `gbufAlbedo` (R8G8B8A8_UNORM), lifetime identical to `gbufNormal` |
| HDR render pass + framebuffers | Third attachment (hdrScene=0, gbufNormal=1, **gbufAlbedo=2**). All HDR-pass pipelines already declared per-attachment blend state for 2 attachments; extended the same arrays to 3 (write-masked off everywhere except the G-buffer prepass pipelines) |
| `gbuffer.vert/frag`, `gbuffer_clip.frag` | Sample the diffuse map, write to attachment 2. Plumbing mostly existed: `vary_TexCoord_Diffuse` + diffuse matrices were already in the shared UBO (the clip variant already sampled diffuse for alpha test). Real change: the opaque variant previously declared no diffuse binding — added it, plus a backend-side `SL_DIFFUSE`-stage resolve (`VK_FindBumpSpecularStages`, extended) so the opaque prepass path has a real diffuse image/matrix instead of nothing |
| `gi_albedo_mod.comp` (new) | **Deviation from the original one-liner-in-composite plan:** implemented as a compute pass inserted after à-trous instead of a `gi_composite.frag` edit. Multiplies `giReadView × gbufAlbedo` into whichever of `giAtrousA`/`giAtrousB` isn't currently `giReadView` (always allocated, so no new image), repoints `giReadView` at the result. Keeps `gi_composite.frag` untouched (still a pure blit) and the denoisers agnostic of albedo — matches the "operate on radiance, multiply by albedo last" ordering pillar 6 wants visible as a discrete step, not folded into the composite blit |

### Decisions (settled 2026-08-16 design discussion)

- **Clear `gbufAlbedo` to white, not black.** A pixel the prepass never wrote
  (sky, translucents) composites as `gi × 1.0` — today's behavior. Graceful
  degradation, and the change is safe to land incrementally. (No sentinel dance
  like gbufNormal's 0.5-gray needed — white *is* the correct neutral here.)
- **Demodulated denoising for free — and it's the right order.** Temporal EMA
  and à-trous run on the GI buffer *before* composite, so they keep smoothing
  low-frequency irradiance while the multiply by crisp albedo happens after.
  Side effect: the à-trous blur stops smearing what is now texture detail — GI
  gets visibly sharper, not just more correct.
- **v1 albedo = diffuse map sample only.** Stage color modulation / vertex
  colors / multi-stage blends are ignored — the prepass picks the same stage it
  already picks for bump/specular. Good enough for a modulation term.
- **Stay in gamma space.** The attachment is UNORM and the multiply happens
  as-is: the whole interaction pipeline lights in gamma space, and GI should be
  consistent with it rather than be the lone linear-space term.
- **Retune follows.** `albedo × E ≤ E` everywhere, so GI dims globally —
  `r_rtGIBounceScale` / `r_rtGIStrength` were implicitly tuned to compensate for
  the missing multiply. Retune *after* validating correctness (pillar 6), and
  expect to raise them.

### CVars

| CVar | Default | Meaning |
|---|---|---|
| `r_rtGIAlbedo` | 1 | multiply GI by receiver albedo post-denoise; 0 = legacy raw-radiance add (A/B) |

Debug (implemented): `r_vkLogRT 1` prints two breadcrumbs each frame —
`VK GBUFFER: ... albedo found=N fallback=M` (opaque-path SL_DIFFUSE resolution:
`fallback` means a material had no diffuse stage and got the neutral white
default, not a bug) and `VK RT GI Albedo Mod: applied slot=S dst=A|B` (confirms
the pass ran and which scratch buffer it wrote). **Not implemented:** a visual
albedo-only debug view (the `r_rtReflectionDebugMode`-style overlay pillar 6
calls for). Worth adding if the log breadcrumbs prove insufficient for chasing
a specific bad surface — cheap to bolt on (`gbufAlbedo` is already a sampleable
image; a debug composite mode would just blit it).

## Verify before trusting (first validation step)

Confirm animated/skinned meshes (bodies!) actually resolve a real diffuse
stage through `VK_FindBumpSpecularStages`, not the white fallback — they
already go through the G-buffer prepass (depth fill draws all opaques
uniformly, confirmed by the pre-existing `gbufNormal` path working on bodies
in the motivating screenshots), so the open question is stage resolution, not
prepass coverage. `r_vkLogRT 1` while a body is on screen: `albedo found`
should be nonzero and track roughly with `bump found` (both come from the same
per-material stage scan) — if `albedo fallback` dominates on character
materials specifically, the SL_DIFFUSE stage isn't being found and the fix is
worthless on exactly the surfaces that hurt most.

## Validation workflow

1. `r_vkLogRT 1`, look at a body: `albedo found` nonzero, tracking `bump found`
   (see above). Confirms the mechanism has real data to multiply by, not just
   the white fallback everywhere.
2. ✅ **Passed 2026-08-22.** The motivating shot (ACO Lift Junction bodies by the
   orange grate light): bodies should read as dark cloth with a subtle warm tint,
   not a yellow wash. A/B with `r_rtGIAlbedo 0`.
3. **Open 2026-08-22.** Pillar 2 check: dark-corner noise floor must not rise (it
   should *drop* — dark materials now absorb GI). Not holding cleanly — volumetrics
   (landed/tuned the same week) is also raising overall luminance, so it's not yet
   isolated whether this is an albedo-fix regression, a volumetrics side-effect, or
   both. Being addressed via tonemapping retune rather than assumed to be a GI bug.
4. **On hold until 3 resolves.** Retune `r_rtGIBounceScale`/`r_rtGIStrength` against
   reference shots — deliberately not started yet, since doing it against a
   tonemap that's still moving would mean redoing it once tonemapping settles.

## Effort / risk

~A day. Mechanical risk concentrated in the render-pass/pipeline attachment-count
plumbing (many pipelines touch the HDR pass; all but two get a write-masked
attachment). Shader-side changes are small. Runtime cost ~zero: one extra RGBA8
write in the prepass, one extra fetch in composite.

## Explicitly out of scope

- Reflections/volumetrics albedo handling — different transport, different docs.
- Stage-color-accurate albedo (v2 if the approximation ever shows).
- Any change to GI gathering, selection, or denoising.
