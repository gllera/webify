# Video pipeline — VP9/Opus WebM (the default)

Video is encoded in two passes whenever the input can rewind (files, spooled
stdin) — only with first-pass stats does libvpx use alt-ref frames and plan
its rate budget ahead, which measured 10-20% smaller files at equal quality.
The defaults are the equivalent of:

```
ffmpeg -i input.mp4 ... -pass 1 -f null - && ffmpeg -i input.mp4 \
  -vf "scale=w='min(iw,W)':h='min(ih,H)':force_original_aspect_ratio=decrease:force_divisible_by=2" \
  -c:v libvpx-vp9 -crf 36 -b:v 530k -maxrate 778k \
  -row-mt 1 -tile-columns 1 -threads 4 -cpu-used 1 -deadline good \
  -frame-parallel 0 -g 240 -pix_fmt yuv420p \
  -c:a libopus -b:a 64k \
  -cues_to_front 1 -pass 2 output.webm
```

…including rotation/mirroring handling (display matrix and EXIF orientation,
baked into the pixels), aspect-ratio-preserving scaling, and input
colorspace/range tagging. The stats pass runs at `-cpu-used 4` (it barely
affects the stats). Tiles follow Google's VOD table for the output width,
with threads at double the table's column — row-mt keeps the extra threads
busy (0/2 below 512 px, 1/4 for 480p, 2/8 for 720-1080p, 3/16 above). The
seek index (cues) is written at the head of the file — the WebM equivalent
of MP4 faststart — so browsers can seek immediately.

## Quality (`-q`)

`-q` is mapped linearly onto VP9 CRF (`crf = (10−q)·6.3`). With no `-q`,
two-pass video targets crf 36 — measured slightly *better* SSIM/PSNR than
the old single-pass crf 33 at 8-20% fewer bytes; single-pass (piped input)
keeps crf 33 (= `-q 4.8`).

## Audio

Audio is Opus 64k stereo; mono sources stay mono at 48k instead of being
upmixed. The Opus bitrate scales with `-q` (64k stereo / 48k mono at the
default, half that at `-q 0`, ~1.5× at `-q 10`), and a lossy source's own
audio rate caps the Opus rate (Opus loses nothing at the rate a weaker
codec managed; lossless/PCM sources stay uncapped).
Subtitle/data/cover-art streams are ignored.

## Rate caps

The bitrate caps (average and peak) start from Google's published 480p VOD
numbers and follow both the job and the input. They scale with the output
pixel count and the CRF target (×2 per 6 CRF steps down), anchored at
exactly 750k/1100k for 854×480 crf 33, staying out of the way when `-q`
asks for more; output above 30fps scales them up by `(fps/30)^0.75`
(Google's 50-60fps rows run ~1.7× their 24-30fps ones at the same crf).
Google's minrate is dropped on purpose — a rate *floor* can only add bytes
at a fixed quality target (measured 0-2% smaller, SSIM unchanged). Finally
the source video stream's own bitrate, weighted by how its codec compares
to VP9 (×0.6 for the MPEG-1/2/4/WMV era, ×0.8 for H.264/VP8/Theora, ×1.0
for HEVC/VP9, ×1.3 for AV1), caps the budget from above: past the source's
own spend, bits only reproduce its compression artifacts more faithfully.
A 400kbps input measured 27% smaller output at near-identical SSIM against
that source. The ceiling is deliberately not tightened further when
downscaling or dropping frames — those only discard source information, so
the full source rate stays a valid, conservative ceiling for any smaller
rendition.

## HDR

HDR sources (PQ/HLG transfer) are tonemapped to SDR bt709 — without that
they would encode "successfully" with gray, washed-out colors. The chain is
the standard zscale linearize → hable tonemap → bt709 re-encode, run at
output resolution. The tonemapper needs the source's peak brightness, and
FFmpeg 8's zscale strips the HDR metadata before `tonemap` can read it, so
webify extracts it from the input itself: in-stream SEI (peeked from the
first decoded frame when the input can rewind), container metadata, or a
1000-nit assumption as the last resort. The output is tagged bt709/tv.

## Effort tiers (`--fast` / `--best`)

Both tiers keep the same `-q` quality target and trade bytes for encode
time. Every step in each tier measurably pays for its time — anything whose
gain was insignificant against its cost was rejected (libvpx's
`-deadline best` measured 5× slower than `--best` for −0.06% bytes and is
in no tier).

- `--fast`: a single pass at `-cpu-used 4` — the classic single-pass crf-33
  look, ~4× faster than the default end to end.
- `--best`: the final pass runs at `-cpu-used 0` with longer alt-ref noise
  reduction (`-arnr-maxframes 15`), and piped video is spooled to a temp
  file so the stats pass can always run — measured 1-7% smaller than the
  default at equal-or-better SSIM for ~75% more encode time (and 10-20%
  smaller for piped inputs that could not two-pass before).
