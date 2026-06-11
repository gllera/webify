# `--legacy` — H.264/AAC MP4 + PNG/APNG

`--legacy` is the same idea as `--next` pointed backwards: output the
maximum-compatibility formats. Video becomes H.264/AAC MP4 (vendored x264 +
FFmpeg's native AAC encoder) with the moov up front — the real `+faststart`,
piped output included ([piping.md](piping.md)) — and every image becomes
PNG; animated GIF becomes APNG, looping forever.

Everything else — the `--max` box, frame-rate caps, tonemapping,
EXIF/rotation, mono rules, stdin/stdout — works identically. Note: x264 is
GPL — see the README's license note.

## Images

Images are lossless by definition, so they are always at least the WebP
pipeline's quality (alpha kept exactly; a fully opaque alpha channel is
still detected and dropped) and `-q` steers video only. PNG effort: the
default runs zlib level 9 plus the per-row "mixed" filter search (−15–19%
bytes vs the encoder defaults; the filter search *alone* backfires on
gradients — the pairing matters), `--fast` keeps the encoder defaults.

## Calibration

Like `--next`, `-q` buys the *same look* as the VP9 pipeline, not the same
number: the mapping is a linear fit of measured equal-SSIM points across
noise, graphics and real-content fixtures (`x264crf = 0.34·vp9crf + 16.5`,
rounded toward the smaller file — [legacy-calibration.md](legacy-calibration.md)
has the data; the line is far flatter than the two CRF scales suggest), and
the VP9-anchored rate budget is converted by the same codec-efficiency
table that weights the source cap (÷0.8 — an H.264 source caps a `--legacy`
job at exactly its own rate).

Always single-pass: x264's two-pass targets a bitrate rather than a
quality, and its CRF mode already plans ahead (lookahead/mbtree), so piped
input loses nothing.

At equal look the size varies by content more than folklore suggests —
measured 0.4–1.2× the VP9 bytes at the default `-q` (synthetic graphics far
smaller, real content slightly bigger): the compatibility tax is mostly the
encode time the veryslow default spends.

## Audio

Audio becomes AAC at 1.5× the Opus rates (96k stereo / 72k mono at the
default) — AAC needs roughly that for equal quality and FFmpeg's native
encoder sits at the weak end, so 1.5 errs the right way; the source-rate
cap applies as usual.

## Effort tiers

Effort tiers map to x264 presets, each step measured to pay for its time:

- The default goes straight to `veryslow` — the ladder's last paying step
  (−19% bytes vs `slow` at equal SSIM for 2.7× the time, still ~10× faster
  than the VP9 default end to end; `placebo` measured 3.7× the time for
  +1% bytes and is in no tier).
- `--fast` is preset `fast` (~5× faster) with a +1 CRF nudge measured
  against VP9's own fast tier.
- `--best` changes nothing: the default already runs the deepest settings
  that pay, and there is no stats pass to add.

## Known extremes

Below ≈`-q 3` the converted rate caps bind before SSIM parity (x264's CRF
mode has no average-rate cap, only the VBV peak, so real content measured
+.027 SSIM at 2× the bytes there), and the per-content spread around the
mean mapping is wider than AV1's (real content −.023 at `-q 7` while
graphics sit above parity).
