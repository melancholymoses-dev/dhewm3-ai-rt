# GI Albedo Modulation — Receiver-Albedo G-Buffer Target

**Date:** 2026-08-16
**Branch:** auto-relight (folded into the Wave 5 visual-quality push)
**Status:** designed, not started
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
| HDR render pass + framebuffers | Third attachment (hdrScene=0, gbufNormal=1, **gbufAlbedo=2**). All HDR-pass pipelines already declare per-attachment blend state for 2 attachments; extend the same arrays to 3 (write-masked off everywhere except the G-buffer prepass pipelines) |
| `gbuffer.vert/frag`, `gbuffer_clip.frag` | Sample the diffuse map, write to attachment 2. Plumbing mostly exists: `vary_TexCoord_Diffuse` + diffuse matrices are already in the shared UBO (the clip variant samples diffuse for alpha test today). Real change: the opaque variant currently declares no diffuse binding — declare it, and the backend must bind the diffuse texture for opaque prepass draws too |
| `gi_composite.frag` | Bind `gbufAlbedo`, output `gi × albedo`. One line |

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
| `r_rtGIAlbedo` | 1 | multiply GI by receiver albedo at composite; 0 = legacy raw-radiance add (A/B) |

Debug: extend the existing G-buffer debug modes with an albedo-view mode
(pillar 6 — overlay before tuning). The same view doubles as the skinned-mesh
verification below.

## Verify before trusting (first validation step)

Confirm animated/skinned meshes (bodies!) actually go through the G-buffer
prepass variant — they should (the depth fill draws all opaques), but the fix is
worthless on exactly the surfaces that hurt most if they don't. The albedo debug
view over a corpse settles it in one look: textured corpse = good; white
(clear-color) corpse = prepass gap to fix first.

## Validation workflow

1. Albedo debug view: scene reads like a diffuse-map pass; corpses textured
   (not white); sky/translucents white.
2. The motivating shot (ACO Lift Junction bodies by the orange grate light):
   bodies should read as dark cloth with a subtle warm tint, not a yellow wash.
   A/B with `r_rtGIAlbedo 0`.
3. Pillar 2 check: dark-corner noise floor must not rise (it should *drop* —
   dark materials now absorb GI).
4. Then retune `r_rtGIBounceScale`/`r_rtGIStrength` against reference shots.

## Effort / risk

~A day. Mechanical risk concentrated in the render-pass/pipeline attachment-count
plumbing (many pipelines touch the HDR pass; all but two get a write-masked
attachment). Shader-side changes are small. Runtime cost ~zero: one extra RGBA8
write in the prepass, one extra fetch in composite.

## Explicitly out of scope

- Reflections/volumetrics albedo handling — different transport, different docs.
- Stage-color-accurate albedo (v2 if the approximation ever shows).
- Any change to GI gathering, selection, or denoising.
