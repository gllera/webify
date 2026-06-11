# `--next` quality-parity calibration (2026-06-10)

Goal, set by the project owner: `--next` must deliver the **same visual
quality** as the VP9/WebP pipeline at every `-q` and every input class —
never better, never worse — so the only difference is a smaller file. When
the two can't be made exactly equal, err **slightly below** the baseline's
quality so no size is paid back as quality the baseline never had.

This file records how the shipped mappings were measured so future tuning
starts from data, not vibes. All numbers from gateway (x86_64, 16 cores),
vendored libvpx 1.16.0 / libaom 3.14.1 / libwebp 1.6.0, host ffmpeg 6.1.1
as the referee.

## Method

- Quality metric: SSIM in planar RGB (`format=gbrp` on both sides, ffmpeg
  `ssim` filter, the `All` figure), computed against the original source.
  RGB catches chroma-subsampling and range errors that per-plane YUV SSIM
  is blind to; Y/U/V plane SSIM was used as a cross-check throughout.
- Sweeps ran through the **shipped binary itself** by inverting each
  `-q -> CRF` formula (pick the `-q` that lands on the CRF under test), so
  every measured point used the exact vendored encoder and settings. CRFs
  outside the formula's reach used a one-off scratch build with the image
  factor set to 1.0 (full 0–63 range). Host libaom was only used for
  relative shape checks (cpu-used ladders, 4:4:4 viability).
- Equal-SSIM points were interpolated per fixture from CRF ladders, then a
  curve was fitted through the per-`q` means, rounded toward the
  lower-quality side.

Fixtures (diversity matters — the equal point moves a lot by content):

- `photo1d`, `photo2d`: real photos (SRR thumbs), 2x lanczos-downscaled so
  the WebP source artifacts wash out (re-encoding webp-sourced pixels
  biases the comparison toward WebP).
- `mandel`: 720p mandelbrot frame — saturated noisy texture, worst case.
- `chart`: testsrc2 frame — sharp colored edges and text (screenshot-like).
- `grad`: smooth multi-color gradients (banding probe).
- `graphic`: flat single-color icon (degenerate case).
- `vid1`/`vid2`: 6 s 720p x264-crf12 masters of mandelbrot / testsrc2.
- `vid1.gif`/`vid2.gif`: 3 s 320x240 10 fps sierra2_4a-dithered GIFs of the
  same — typical GIF material.
- `alphagrad`: photo + radial alpha gradient.

## Stills: WebP -> AVIF (4:2:0, allintra, cpu-used 4)

Equal-SSIM AVIF CRF per WebP anchor (`q` = internal 0–100 scale):

| q (internal) | photo1d | photo2d | mandel | chart | old map | shipped |
|---|---|---|---|---|---|---|
| 10  | 48   | 47  | 52  | —    | 56 | 49 |
| 30  | 42   | 40  | 45  | ~46  | 49 | 43 |
| 50  | 38   | 35  | 38  | ~38  | 41 | 37 |
| 80  | 26.5 | 25  | 30  | ~25+ | 25 | 28 |
| 95  | 9    | 7   | ~16 | n/a  | 18 | 12 |
| 100 | 6    | 4.5 | 16  | n/a  | 16 | 6  |

The relationship is two-regime: near-linear to q 80, then cwebp's top end
buys disproportionate quality and the CRF has to dive. Shipped curve
(continuous at the break):

    q <= 80: crf = 52 - 0.30*q
    q >  80: crf = 28 - 1.10*(q - 80)

The old "WebP q80 = avifenc q60 = quantizer 25" anchor was right in the
middle of the scale and wrong at both ends (q0 mapped to crf 63, far below
WebP q0's quality floor; q95 mapped to 18 where parity needs ~12).
Validation at the shipped curve: photos land 0–.005 SSIM *below* WebP at
−20…37% bytes; mandel/chart at parity, −26…59%.

Notes per content extreme:

- **mandel at q >= 95**: AVIF's RGB-All saturates ~.942 below WebP's .9439
  at any CRF — WebP's sharp_yuv chroma conversion wins on saturated
  synthetic edges (RGB-sourced stills only; luma is far better, Y .998).
  Accepted as slightly-lower, per the bias rule.
- **grad**: AVIF ~24 KB at *any* CRF vs WebP 4.5–11 KB — libaom spends a
  floor on smooth gradients (quality always >= WebP). No mapping fixes it;
  pure-gradient stills are the one known class where `--next` is bigger.
- **graphic** (flat color): WebP races to 60 B lossless; AVIF ~320 B lossy
  at SSIM .9867. Trivial sizes, accepted.

### 4:4:4 above the break

`chart` exposed the real gap: at q >= 95 the WebP side's lossless race wins
(SSIM 1.0 at 20 KB) while 4:2:0 AVIF is chroma-resolution-capped at .789 —
not "slightly" lower. Options measured (chart):

| variant | SSIM (All) | bytes |
|---|---|---|
| WebP lossless (race winner)    | 1.000 | 19 974 |
| AVIF 420 crf 12                | .789  |  8 977 |
| AVIF 444 crf 12                | .854  | 14 938 |
| AVIF 444 crf 0 (near-lossless) | .861  | 22 075 |

True parity is unreachable: even 444 crf 0 caps at .86 (the limited-range
RGB->YUV round trip itself is lossy; only RGB-domain lossless hits 1.0) and
is already *bigger* than lossless WebP — AV1 has no competitive lossless
mode for graphics. A compromise shipped for a while — RGB-decoded stills
with q > 80 raced a 4:4:4 candidate at the same mapped CRF (photos in 444
also stayed below WebP's size at those q: 13.5 vs 17.5 KB at q 95) — but
it was **removed**: 4:4:4 needs AV1 High profile (seq_profile 1), which
hardware decoders commonly lack, and webify ships Main-profile-only output
on purpose. The numbers above stay as the record of what 4:4:4 would buy.
Everything now encodes 4:2:0, and q > 80 graphics keep the known gap to
WebP's lossless race — use the default pipeline for that content.

## Video: VP9 -> AV1 (same content, two-pass, 480p box)

At the *same* CRF, libaom lands above libvpx by a margin that grows with
CRF (vid1: +.0035 SSIM at crf 44, +.0009 at 33, ~0 at <= 19). Equal-SSIM
offsets: +3–6 at crf 44, +2 at crf 33, none at or below ~20. Shipped:

    av1_crf = vp9_crf + max(0, (vp9_crf - 20) / 6)    (integer divide)
    --fast: +4 more (cpu-used 6 single-pass loses far less than vpx's
            fast tier; equal-SSIM measured at crf 38–40 vs vpx-fast crf 33)
    clamp 63; the rate budget uses the shifted CRF, then /1.3 (codec table)

Validation (vs vp9 at the same `-q`): vid1 -q3 .9796 vs .9806 at −63%
bytes; -q4.8 ≈equal at −19…31%; -q7/-q9 ≈equal (−.0004) at −20…37%.
`--best` parity confirmed with the standard offset (vid1 .98747 vs .98736,
−39%; vid2 equal, −41%). libaom's deeper speeds still don't pay at fixed
CRF (cpu-used 3: +1% bytes/1.2x time; 2: +0.8%/3.4x; arnr-max-frames 15:
+0.3%) — `--best` keeps default encoder settings, its win is the piped-
input spool for a universal two-pass.

## Animations: animated WebP -> animated AVIF

Animated WebP is far weaker than stills WebP, so the equal point sits much
higher than the stills curve:

| q (internal) | vid1.gif | vid2.gif | shipped |
|---|---|---|---|
| <= 50 | >63 (capped) | >63 | 63 |
| 80    | ~54          | ~59 | 56 |
| 95    | flat/uncertain | ~51 | 32 |

    q <= 50: crf = 63
    q <= 80: crf = 63 - (q - 50) * 7/30
    q >  80: crf = 56 - 1.60 * (q - 80)

At default q: vid1 .9026 vs WebP .9060 (slightly lower ✓) at **6.0 KB vs
226 KB (−97%)**; vid2 well above parity at −96%. The old shared-stills
mapping (crf 25) overdelivered hugely (+.02…+.12 SSIM) — that surplus is
now returned as bytes.

The animated-WebP side itself was audited against `gif2webp` (Google's
reference tool) at identical settings (`-lossy -q 80 -m 6`) on suspicion
the −96% gap meant an encoder bug. It doesn't: webify lands within ±1%
of gif2webp on every fixture (mandelbrot 261,312 vs 259,880 B; testsrc2
155,752 vs 156,964 B), and a static-background fixture compresses 30
frames into an identical 294 B from both — frame-diff/delta coding works.
The gap is the codec: animated WebP re-codes every changed region as an
independent VP8 intra still (no motion compensation), so full-frame
dithered motion costs ~8.7 KB/frame while AV1 inter-predicts it away.
Two loose ends, both content extremes: graphics-heavy anims can win as
*lossless* WebP — confirmed with our own encoder path (`libwebp_anim`
`lossless=1`): testsrc2 126,274 B lossless < 155,752 B lossy at SSIM 1.0,
flat-color 56 B vs 294 B, while dithered mandelbrot upholds the old rule
at 2.7x (704 KB vs 261 KB) — so an anim lossless-vs-lossy race like the
stills one could pay on graphics; and near-static anims hit libaom's
byte floor (static-bg: AVIF 1,847 B vs WebP 294 B — `--next` bigger,
trivial sizes, same family as the gradient-stills case).

## Tiers, stills (`--fast` cpu-used)

allintra cpu-used ladder at fixed crf (photo, host libaom): 4 = .9666/0.17s,
5 = .9649/0.14s, 6 = .9610/0.08s, **7 = .9537/0.07s**, 8 = .9525/0.07s.
Speed 7 is a strictly bad point (same time as 6, −.008 SSIM) and broke fast-
tier parity (−.021 vs WebP-fast's −.0004). `--fast` stills now use
**cpu-used 6**.

## Alpha

Real bug found: the auxiliary alpha stream was encoded with the default
*limited* range tag. MIAF/AVIF decoders (libavif, browsers) treat alpha as
full-range and stretch 16–235 -> 0–255, distorting every gradient —
measured SSIM .9015 (avifdec round-trip) vs WebP's lossless 1.0, mean
shifted 93.6 -> 92.1. Fixed by tagging encoder context and frames
`AVCOL_RANGE_JPEG`. Note host ffmpeg 6.1 cannot demux AVIF alpha at all
(decodes opaque — useless as a referee here; use `avifdec`).

The aux encoder's speeds are 4 by default and, at `--fast`, 6 for
animations but **7 for stills** — the speed-7 rejection in the stills
section above is a lossy-mode result and doesn't carry over: alpha rides
crf 0, where a faster speed costs bytes, never look.

## What needs no change

- **Audio**: `init_audio` has no `--next` branch; Opus settings identical
  by construction.
- **HDR video**: tonemapping happens before encoding in both paths; parity
  follows from the video result.
- **EXIF / -m box / fps cap**: shared pre-encode pipeline.

## Re-calibration checklist

If a vendored encoder is bumped (libaom/libvpx/libwebp), re-run the
equal-SSIM sweep at q 30/50/80/95 on the photo + chart fixtures at least;
the curves above only hold for the pinned versions. The `-q`-inversion
trick reaches any CRF in [16,63] for stills (factor 0.75) and the full
range for video; below stills-crf 16 use a scratch build with factor 1.0.
