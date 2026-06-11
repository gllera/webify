# `--legacy` quality-parity calibration (2026-06-10)

Same goal as `doc/next-calibration.md`, pointed backwards: `--legacy` must
deliver the **same visual quality** as the VP9/WebP pipeline at every `-q`,
erring **slightly below** when exact parity is unreachable, so the only
difference is the format (and the size — which for H.264 is usually, but
not always, bigger). Images need no quality mapping at all: PNG/APNG is
lossless, so the image side is always at least the WebP pipeline's quality
and only the *effort* dial was measured.

All numbers from gateway (x86_64, 16 cores), vendored x264 build 165 /
libvpx 1.16.0, host ffmpeg 6.1.1 (libx264 build 164) as the ladder encoder
and SSIM referee. Method as in next-calibration.md: RGB (gbrp) SSIM against
the source master scaled to output geometry, equal-SSIM points interpolated
per fixture from CRF ladders, curve fitted through per-`q` means, rounded
toward the lower-quality side.

## Fixtures

- `vid1`/`vid2`: 6 s 720p x264-crf12 masters of mandelbrot / testsrc2
  (noise / sharp synthetic graphics), as in next-calibration.md.
- `vid3`: 6 s of Big Buck Bunny (320x240), re-mastered at crf 12 so the
  source-rate cap never binds — **real content**. Added after a two-fixture
  fit validated fine on vid1/vid2 and then missed real content by −.03 SSIM
  at `-q 7` and +.02 at `-q 3`: mandelbrot and testsrc2 bracket the
  difficulty range but real video does not interpolate between them.
  Diversity matters even more here than for `--next`.

Two procedural lessons, both worth keeping:

- **Ladders must run at the binary's thread counts** (4 at 854 px wide, 2
  at 320). x264's threaded rate control turns conservative when the VBV
  cap binds: the same CRF measured −.03 SSIM and −15% bytes at host-default
  threads (16+) vs the shipped 2 — enough to corrupt every cap-bound
  equal-SSIM point.
- Ladders also carry the shipped VBV (`maxrate` from the *target point's*
  budget, 2x buffer), because near the caps the constraint, not the CRF,
  decides the quality.

## Video: VP9 -> x264 (preset veryslow, single pass)

Equal-SSIM x264 CRF per VP9 CRF (the `-q` inversion trick reaches every
point; vid3 at vp9-44 was cap-bound flat — its "equal point" would be an
extrapolation through noise and was excluded from the mean):

| vp9 crf | vid1 | vid2 | vid3 | mean | shipped |
|---|---|---|---|---|---|
| 12 | 18.1 | 26.2 | 18.5 | 20.9 | 21 |
| 20 | 22.6 | 26.8 | 19.6 | 23.0 | 23 |
| 26 | 24.7 | 27.7 | 23.4 | 25.3 | 25 |
| 33 | 26.6 | 30.3 | 26.8 | 27.9 | 28 |
| 36 | 27.4 | 30.6 | 28.4 | 28.8 | 29 |
| 44 | 31.3 | 32.0 | (cap) | 31.6 | 31 |

The relationship is far flatter than the two CRF scales suggest (x264's
quality moves ~3x faster per CRF step than libvpx's). A single line fits
every balanced mean within ±0.3:

    x264_crf = 0.34 * vp9_crf + 16.5    (lrint, clamp 0-51)
    --fast: +1 (see Tiers)

The budget `f` keeps using the *VP9-scale* CRF (it tracks the look, which
is what the calibration pinned), then converts by the codec-efficiency
table the other way: `/0.8` — H.264 needs ~1.25x the bits for the VP9
look, and an H.264 source caps a `--legacy` job at exactly its own rate.
`bit_rate` stays 0 — a nonzero value flips libx264 from CRF into ABR — so
only the peak cap ships, as VBV (`maxrate` + 2 s `bufsize`).

Validation through the shipped binary (RGB SSIM delta vs the VP9 output at
the same `-q`, and size as a fraction of it):

| -q | vid1 | vid2 | vid3 (real) |
|---|---|---|---|
| def | −.011 (0.77x) | +.004 (0.44x) | −.008 (1.08x) |
| 3   | +.002 (1.47x) | +.005 (0.90x) | +.027 (1.96x) |
| 4.8 | −.007 (0.64x) | +.005 (0.37x) | −.014 (0.91x) |
| 7   | −.003 (0.35x) | +.010 (0.25x) | −.023 (0.41x) |
| 9   | −.006 (0.22x) | +.015 (0.29x) | −.002 (0.59x) |

Per-`q` means land at −.005…+.011 — the same band as `--next`'s published
validation — but the per-content spread is wider than AV1's. Two honest
extremes, both documented in the README:

- **Real content around `-q 7`** sits −.023 below parity while synthetic
  graphics sit +.010 above: at vp9-19 the per-fixture equal points span
  19–27, and no global mapping can satisfy both. The mean obeys the
  slightly-below rule; bending the curve to rescue vid3 would overspend on
  everything else (+25% bytes) for quality the rule says not to buy.
- **Below ≈`-q 3` the converted caps decide, not the CRF**: on noise and
  real content every ladder candidate pinned at the cap's size. VP9 is
  held to its *average* cap there while x264's CRF mode has no average
  cap at all (only the VBV peak, 1.83x VP9's average after conversion), so
  real content overshot to +.027 SSIM at 2.0x the bytes. Wasteful rather
  than ugly, and only at the bottom of the quality scale.

Size at equal look varies far more by content than folklore suggests:
x264 veryslow measured *smaller* than two-pass cpu-used-1 VP9 on both
synthetic fixtures at most `-q` (down to 0.2x), and slightly bigger on
real content (1.1x at the default). The maximum-compatibility tax is
mostly time, not bytes — paid by the veryslow default.

## Tiers

Preset ladder at fixed crf 27, threads 4 (SSIM / bytes / wall time):

| preset | vid1 | vid2 |
|---|---|---|
| faster   | .9201 / 516 K / 0.7 s | — |
| fast     | .9266 / 603 K / 0.9 s | .9714 / 330 K / 0.6 s |
| medium   | .9284 / 537 K / 1.1 s | .9730 / 310 K / 0.6 s |
| slow     | .9265 / 545 K / 1.6 s | .9741 / 307 K / 0.9 s |
| slower   | .9312 / 547 K / 2.5 s | — |
| veryslow | .9286 / 441 K / 4.3 s | .9743 / 249 K / 2.2 s |
| placebo  | .9297 / 445 K / 15.8 s | — |

medium -> slow buys nothing; slow -> veryslow buys **−19% bytes at equal
SSIM for 2.7x the time** (still ~10x faster than the VP9 default end to
end), so the default goes straight to veryslow. placebo is the classic
trap: 3.7x the time for **+1% bytes** — rejected, so `--best` changes
nothing for `--legacy` (the default already runs the deepest settings
that pay; there is no stats pass to add either). `--fast` is preset fast,
~5x faster.

`--fast` CRF offset: unlike libvpx's fast tier (−.010 SSIM at the same
CRF), preset fast holds quality almost flat (−.002), so matching the
*vpx-fast* look needs the x264 CRF pushed up only a little: equal-SSIM
measured at 28.2/29.3 (vid1/vid2) against vpx-fast's crf-33 targets,
vs 28.5 from the default curve. Shipped **+1** (lands −.003 below the
vpx-fast look; +0 would land above, paying bytes for quality the fast
tier never promises).

## Images: PNG / APNG effort

Lossless, so only deflate effort was measured (640x480 graphics, 720p
noise, gradients):

| variant | photo | chart | grad |
|---|---|---|---|
| encoder defaults        | 484 K / 0.11 s | 39.4 K | 29.4 K |
| level 9                 | 481 K          | 34.2 K | 26.6 K |
| pred mixed alone        | 476 K          | 32.7 K | **53.1 K** |
| level 9 + pred mixed    | 412 K / 0.86 s | 31.7 K | 26.1 K |
| level 9 + pred paeth    | 410 K          | 32.6 K | 26.1 K |

Level 9 + the per-row "mixed" filter search ships as the default/`--best`
setting: −15…−19% vs the encoder defaults at worst ~0.9 s per 720p frame.
The filter search *alone* backfires on gradients (+80%: row filters chosen
for raw entropy, then compressed at zlib's default level) — the pairing
matters. `--fast` keeps the encoder defaults. paeth vs mixed is a wash on
photos and loses on graphics; mixed ships.

## Audio: Opus -> AAC

No SSIM-style referee exists here, so this is a rate-rule conversion, not
a measured fit: AAC needs roughly 1.5x Opus's bitrate for the same quality
(public listening-test consensus), and FFmpeg's native encoder sits at the
weak end of AAC encoders, so 1.5x errs on the right side. Anchors become
96k stereo / 72k mono at the default `-q`, scaling with `-q` and capped by
the source rate exactly like the Opus path (floors 36k/24k).

## What needs no change

- **HDR video / EXIF / -m box / fps cap**: shared pre-encode pipeline.
- **Alpha**: PNG carries it natively (rgba); the fully-opaque-alpha drop
  uses the same peek as `--next`.
- **faststart**: `movflags +faststart` for files; pipes switch to
  fragmented MP4 (`frag_keyframe+empty_moov+default_base_moof`) since the
  moov cannot be seeked back on a pipe.

## Re-calibration checklist

If the vendored x264 or libvpx is bumped, re-run the equal-SSIM sweep at
vp9 crf 20/33/36/44 on vid1 + vid3 at least. Run ladders **at the shipped
thread counts and VBV settings** (see the procedural lessons above) — at
host-default threads the cap-bound points are wrong by whole CRF steps.
The `-q` inversion reaches any vp9-scale CRF; the x264 side is plain host
ffmpeg at preset veryslow.

`calibrate.sh` (repo root) automates the validation side: parallel
baseline/`--next`/`--legacy` encode matrix through the shipped binary
(thread counts therefore always correct), RGB-SSIM referee, parity report.
See the note in doc/next-calibration.md about fixture equivalence before
comparing against this file's absolute numbers.
