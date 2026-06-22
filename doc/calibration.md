# Quality settings — how `-q` maps to encoder CRF

`webify` exposes one quality dial, `-q 0..10`, and maps it **directly** onto
each encoder's own CRF. There is no cross-format calibration (each encoder's
CRF is its own quality scale) and no separate bitrate budget: in CRF mode the
encoder spends exactly the bits the chosen quality needs, and `--max` controls
the resolution — so quality (`-q`) and the resolution box together determine
the file size. Higher `-q` = better quality = lower CRF = bigger file.

`-q` is scaled ×10 to an internal 0–100 quality `Q` at parse; when `-q` is
omitted the default is `-q 8` (`Q = 80`), a high-quality setting. All the
mappings live in the **"Quality settings" banner section of `src/webify.cpp`**:

| dial | function | formula (CRF) | `-q 0` | `-q 8` (default) | `-q 10` |
|------|----------|---------------|--------|------------------|---------|
| video (x264) | `x264_video_crf` | `33 − 0.16·Q`, clip 12–40 | 33 | 20 | 17 |
| still image (AVIF) | `avif_still_crf` | `52 − 0.36·Q`, clip 6–55 | 52 | 23 | 16 |
| animated image (AVIF) | `avif_anim_crf` | `62 − 0.36·Q`, clip 12–63 | 62 | 33 | 26 |
| audio (AAC) | `audio_bitrate` | `base·(0.5 + Q/160)`, base 128k stereo / 96k mono | 64k / 48k | 128k / 96k | 144k / 108k |

Why these ranges:

- **x264** — its CRF is 0–51, default 23, with ~18 considered "visually
  lossless" and ~28+ "low". The web-delivery range 17 (`-q 10`) → 33 (`-q 0`)
  puts the `-q 8` default at ~20, x264's good-quality zone, while leaving
  headroom both ways. Always preset `veryslow` (placebo costs ~4× the time for
  ~1% fewer bytes).
- **AVIF stills** — libaom CRF is 0–63. Images are downloaded once and viewed
  many times, so the still range biases high quality: 16 (`-q 10`, excellent)
  → 52 (`-q 0`), default ~23. Always all-intra (`still-picture`) at
  `cpu-used 4`, and 8-bit 4:2:0 = AV1 Main profile.
- **AVIF animations** — animated AVIF tolerates ~10 more CRF than stills
  (motion masks quantization, and the alternative is files that balloon), so
  the curve sits ~10 higher. Inter-coded at the video GOP.
- **AAC** — FFmpeg's native AAC encoder is on the weaker side, so the anchors
  are generous: 128k stereo / 96k mono at the default, scaling with `-q`, never
  above a lossy source's own rate (bits past the source rate cannot recover
  quality the input never had).

## Tuning

These are recommended starting points, not measured equal-quality fits. If you
want a different size/quality operating point, edit the four functions in the
"Quality settings" section and rebuild — they are plain linear maps with
explicit clips, so the effect of a change is easy to predict.

(The original [`webify`](https://github.com/gllera/webify) instead fit these
curves to reproduce a reference VP9/WebP look via an SSIM harness; this
simplified build drops that machinery in favor of the direct recommended CRF
mappings above.)
