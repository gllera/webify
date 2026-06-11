# `--next` — AV1/Opus WebM + AVIF

`--next` outputs the next-gen formats: video becomes AV1/Opus WebM (libaom)
and every image becomes AVIF — animated GIF becomes *animated* AVIF (the
`avis` brand, looping forever like the GIF did). `-q` buys the *same look*
as the default formats, not the same internal number, so `--next` only
changes the file size, never the quality.

Everything else — the `--max` box, frame-rate caps, two-pass, tonemapping,
faststart cues, mono/Opus audio rules, stdin/stdout — works identically to
the default pipelines.

## Calibration

Every mapping is a piecewise fit of measured equal-SSIM points against the
VP9/WebP output across photo, noise and graphics fixtures, rounded a hair
*toward* the smaller file ([next-calibration.md](next-calibration.md) has
the data and method):

- **Video** keeps its CRF nudged up where libaom outperforms libvpx
  (+`(crf−20)/6` above CRF 20, `--fast` +4 more) while the bitrate budget
  follows the shifted CRF and is converted by the same codec-efficiency
  table that weights the source cap (÷1.3).
- **Stills** run near-linear to `-q 8` then dive with cwebp's premium top
  end (q 8 → CRF 28, 9.5 → 12, 10 → 6).
- **Animations** get their own much-higher curve (CRF 63 ceiling easing
  to 36 at `-q 8`, `--fast` 4 lower) because animated WebP is far weaker
  than stills WebP. The curve tracks *live-action* equal points: the
  per-content spread is huge (graphics' equal point sits ~25 CRF higher),
  and a mean fit paid the size win back as visible quality loss on faces
  while graphics merely overdeliver at sizes that stay under ~0.27× of
  animated WebP anyway.

Measured at the defaults: video −28% bytes at equal SSIM (low `-q` reaches
−60%), stills −26%, and animated GIFs −86% on live action (synthetic and
graphic anims −79…−92%, riding above parity) — all within ±0.005 SSIM of
their VP9/WebP counterpart on real content. [sizes.md](sizes.md) charts
size and encode time across the whole `-q` range and the effort tiers.

## Alpha and chroma

AVIF keeps alpha when the input *really* uses it (a fully opaque alpha
channel is detected and dropped instead of wasting a stream) as the
standard auxiliary alpha stream — near-lossless and tagged full-range as
MIAF demands; EXIF rotation is baked in as usual.

Chroma stays 4:2:0 everywhere — stills, animations and video — so every
file webify writes decodes as **AV1 Main profile**, the one profile
hardware decoders reliably implement. A 4:4:4 race for premium-`-q` RGB
stills used to live here (full-resolution chroma closes most of the gap to
WebP's lossless race on sharp graphics) but was dropped on purpose: 4:4:4
needs High profile (seq_profile 1), and trading decoder compatibility for
a niche size win is the wrong default for a web-delivery tool. Sharp
graphics that want pixel-exact chroma should use the default pipeline —
its lossless WebP race exists for exactly that content. True lossless
stays WebP-only anyway (AV1 lossless is far larger).

## Effort tiers

Each step measured to pay for its time like the VP9/WebP ones:

- `--fast` runs cpu-used 6 single-pass (about *half* the time of even the
  VP9 default).
- The default runs cpu-used 4 two-pass (~3× the VP9 default's encode time —
  libaom is simply heavier; that time is what buys the size win).
- For video and animations `--best` keeps the default encoder settings,
  because libaom's deeper searches measured *bigger* (+0.3-1% bytes for
  1.2-3.4× the time — at a fixed CRF they buy a sliver of quality, never
  bytes).
- Stills use `usage=allintra` + `still-picture` at speeds 6/4/2 (speed 7
  measured the same wall time as 6 for −.008 SSIM — a strictly worse
  point; avifenc defaults to speed 6 — the webify default digs one step
  deeper). `--fast` stills and animations also ride the CRF 4 lower: the
  WebP side's fast settings cost bytes but not quality, while libaom's
  faster speeds drop real photographic detail at the same CRF — the fast
  look needs the bits back (measured on real-content fixtures,
  doc/next-calibration.md).

## No film grain synthesis

AV1 can carry a *model* of the source's noise instead of the noise itself
(film grain synthesis): the encoder denoises, ships a grain table, and the
decoder re-synthesizes texture on playback. webify briefly had a `--grain`
knob for it and removed it on purpose: grain synthesis is AV1's
least-exercised decoder path — some hardware decoders skip or mishandle
the re-synthesis, and browsers may punt grainy streams to software
decode — and trading decoder compatibility for bytes is the wrong deal
for a web-delivery tool (the same reasoning that keeps chroma at 4:2:0
above).

## Known extremes

Two honest caveats, same cause: stills dominated by pure smooth gradients
can come out *bigger* than their WebP (libaom spends a byte floor on them
at any CRF, at higher quality), and near-static animations hit that floor
too (a 30-frame static-background GIF measured 1.8 KB AVIF vs 294 B WebP —
trivial sizes). Real photos and real motion are unaffected.
