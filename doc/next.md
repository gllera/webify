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
- **Animations** get their own much-higher curve (CRF 63 easing to 56 at
  `-q 8`) because animated WebP is far weaker than stills WebP.

Measured at the defaults: video −28% bytes at equal SSIM (low `-q` reaches
−60%), stills −26%, and animated GIFs −96% — all within ±0.005 SSIM of
their VP9/WebP counterpart.

## Alpha and chroma

AVIF keeps alpha when the input *really* uses it (a fully opaque alpha
channel is detected and dropped instead of wasting a stream) as the
standard auxiliary alpha stream — near-lossless and tagged full-range as
MIAF demands; EXIF rotation is baked in as usual.

RGB-decoded stills above `-q 8` race a 4:4:4 candidate against 4:2:0
(full-resolution chroma is what closes most of the gap to WebP's lossless
race on sharp graphics) and ship it only while it stays ≤1.35× the 4:2:0
bytes — saturated noise explodes in 4:4:4 and falls back, so the winner
stays smaller than the WebP output either way. True lossless stays
WebP-only (AV1 lossless is far larger — graphics that want lossless should
stay WebP).

## Effort tiers

Each step measured to pay for its time like the VP9/WebP ones:

- `--fast` runs cpu-used 6 single-pass (about *half* the time of even the
  VP9 default).
- The default runs cpu-used 4 two-pass (~3× the VP9 default's encode time —
  libaom is simply heavier; that time is what buys the size win).
- For video and animations `--best` keeps the default encoder settings,
  because libaom's deeper searches measured *bigger* (+0.3-1% bytes for
  1.2-3.4× the time — at a fixed CRF they buy a sliver of quality, never
  bytes) — it still spools piped video so the stats pass always runs.
- Stills use `usage=allintra` + `still-picture` at speeds 6/4/2 (speed 7
  measured the same wall time as 6 for −.008 SSIM — a strictly worse
  point; avifenc defaults to speed 6 — the webify default digs one step
  deeper).

## Known extremes

Two honest caveats, same cause: stills dominated by pure smooth gradients
can come out *bigger* than their WebP (libaom spends a byte floor on them
at any CRF, at higher quality), and near-static animations hit that floor
too (a 30-frame static-background GIF measured 1.8 KB AVIF vs 294 B WebP —
trivial sizes). Real photos and real motion are unaffected.
