# webmify

A single, fully static binary that transcodes any popular video file to
VP9/Opus WebM — and any popular image file to WebP — with sane defaults
tuned for serving the result over the internet: two-pass VP9 (10-20% smaller
at equal quality), the seek index at the head of the file (faststart), and
the densest WebP encoding (`-m 6`). Input type is auto-detected, and one
option set covers both modes:

```
webmify input.mp4 output.webm
webmify photo.jpg photo.webp
webmify -q 6 --max 720x1280 input.mov output.webm   # fit 720 tall, 1280 wide
cat input.mkv | webmify - - > output.webm   # '-' = stdin/stdout
cat anim.gif  | webmify - - > anim.webp
```

Options mean the same thing for video and image inputs (and go before the
file arguments):

- `-q`, `--quality <0-10>` — target quality, higher is better (decimals
  allowed). Images: WebP quality ×10 (default 8 = cwebp's 80). Video: mapped
  linearly onto VP9 CRF (`crf = (10−q)·6.3`). With no `-q`, two-pass video
  targets crf 36 — measured slightly *better* SSIM/PSNR than the old
  single-pass crf 33 at 8-20% fewer bytes; single-pass (piped input) keeps
  crf 33 (= `-q 4.8`). The Opus bitrate scales along with it (64k stereo /
  48k mono at the default, half that at `-q 0`, ~1.5× at `-q 10`), capped
  at the source audio rate when that is lower.
- `-m`, `--max [HxW | S][@F]` — size and frame-rate caps in one flag. The
  box part is height×width (`720x1280`), a single number `S` to bound both
  sides (`720` = fit within 720×720), or one-sided with the other left
  unbounded (`720x` = height only, `x1280` = width only): downscale to fit
  (applied after rotation), preserving aspect ratio; never upscales. With
  no box at all, images keep their resolution and video fits within a
  height of 480; giving a box replaces that default entirely. The `@F`
  part is video only and off by default: drop frames to cap the frame rate
  (e.g. `--max @30` halves the frame budget of a 60 fps screencast). Each
  part is optional: `720`, `480x854`, `720x`, `@30`, and `480x@30` are all
  valid.
- `--fast` / `--best` — trade bytes for encode time at the same `-q` quality
  target (mutually exclusive; the default is the tuned middle ground). Every
  step in each tier measurably pays for its time — anything whose gain was
  insignificant against its cost was rejected (libvpx's `-deadline best`
  measured 5× slower than `--best` for −0.06% bytes and is in no tier).
  - `--fast`: video is encoded in a single pass at `-cpu-used 4` (the
    classic single-pass crf-33 look, ~4× faster than the default end to
    end); images use cwebp's default method (`-m 4`), skip the sharp
    RGB→YUV conversion and the lossless race (stills ~3-9× faster for
    ~5% more bytes; heavily dithered animations up to 15× faster for ~11%).
  - `--best`: the final pass runs at `-cpu-used 0` with longer alt-ref
    noise reduction (`-arnr-maxframes 15`), and piped video is spooled to a
    temp file so the stats pass can always run — measured 1-7% smaller than
    the default at equal-or-better SSIM for ~75% more encode time (and
    10-20% smaller for piped inputs that could not two-pass before).
    Images already run their densest settings by default.

When stderr is a terminal, video conversions print pass/percentage progress.

Every supported format works from stdin: images are buffered in memory, and
video containers that need a seekable input to keep all their features
(MP4/MOV without faststart, AVI) are spooled to an unlinked temporary file
(`$TMPDIR` or `/tmp`) that disappears when the process exits, however it
exits. Everything else streams — but a truly streamed video input cannot be
rewound for the stats pass, so it falls back to single-pass encoding (with a
warning). Piped video output is playable but carries no duration/seek index
in the header.

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
affects the stats), and `--fast`/`--best` move the final pass to
`-cpu-used 4`/`-cpu-used 0` as described above. Tiles/threads follow
Google's VOD table for the output width (0/2 below 512 px, 1/4 for 480p,
2/8 for 720-1080p, 3/16 above). The seek index (cues) is written at the
head of the file — the WebM equivalent of MP4 faststart — so browsers can
seek immediately. Audio is Opus 64k stereo; mono sources stay mono at 48k
instead of being upmixed, and a lossy source's own audio rate caps the Opus
rate (Opus loses nothing at the rate a weaker codec managed; lossless/PCM
sources stay uncapped).

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
rendition. Subtitle/data/cover-art streams are ignored.

HDR sources (PQ/HLG transfer) are tonemapped to SDR bt709 — without that
they would encode "successfully" with gray, washed-out colors. The chain is
the standard zscale linearize → hable tonemap → bt709 re-encode, run at
output resolution. The tonemapper needs the source's peak brightness, and
FFmpeg 8's zscale strips the HDR metadata before `tonemap` can read it, so
webmify extracts it from the input itself: in-stream SEI (peeked from the
first decoded frame when the input can rewind), container metadata, or a
1000-nit assumption as the last resort. The output is tagged bt709/tv.

Images are the equivalent of `cwebp -q 80 -m 6` / `gif2webp`: lossy quality
80 at the densest WebP method, original resolution (unless a `--max` box is
given, scaled with lanczos), alpha and EXIF orientation preserved (the
latter baked into the pixels), animated GIF → animated WebP looping forever.
RGB-coded sources (PNG, GIF, BMP, TIFF) are handed to libwebp as RGB so its
own high-quality converter does the chroma subsampling, exactly like cwebp;
YUV-coded sources (JPEG, WebP, HEIC, AVIF) feed their planes through
directly. Single-frame RGB images are additionally tried as *lossless* WebP
and the smaller file wins — flat-color graphics usually come out both
smaller and pixel-perfect, photos keep the lossy result — and their lossy
candidate uses the sharp (iterative) RGB→YUV conversion, so stills are
byte-identical to `cwebp -q 80 -m 6 -sharp_yuv`: crisper text and hard
edges for ~1% more bytes. When lossless wins the race, the winner is
re-encoded at maximum effort (`cwebp -lossless -q 100 -m 6`) — measured a
further 1-3% smaller; photos, whose lossless candidate just lost, never pay
that time. Animations keep libwebp's fast converter (sharp measured ~20%
more bytes there, like `gif2webp` without `-sharp_yuv`), and stay lossy:
lossless animation measured 2.5x bigger even on flat-color GIF content.
Inputs: JPEG, PNG, WebP, GIF, BMP, TIFF, HEIC, AVIF.

## Building

Everything builds inside Docker; nothing is installed on the host:

```
./build.sh                        # -> dist/webmify (static musl binary)
UPX=1 ./build.sh                  # also upx-compress (~60% smaller, slower start)
PLATFORM=linux/arm64 ./build.sh   # cross-build via qemu/binfmt (slow)
```

## Design

- **Zero third-party code.** The whole program is one C++ file
  (`src/webmify.cpp`) written against the official FFmpeg API, modeled on
  FFmpeg's own `doc/examples/transcode.c`, compiled with a single `g++ -static`
  invocation in the Dockerfile.
- **Official upstream sources only**, pinned release tarballs (`vendor.sh`),
  all latest releases:
  FFmpeg 8.1.1, libvpx 1.16.0 (webmproject), libopus 1.6.1 (xiph),
  dav1d 1.5.3 (VideoLAN), libwebp 1.6.0 (webmproject), zimg 3.0.6
  (sekrit-twc), zlib (Alpine static package) — plus the minimal patches in
  `patches/` (each a few lines, reviewable in-repo, applied verbatim by
  `vendor.sh`; currently one: exposing libwebp's `use_sharp_yuv` as an
  encoder option, which FFmpeg does not surface).
- **Minimal FFmpeg** (`--disable-everything --enable-small` + whitelist), so the
  binary stays ~10 MB instead of ~80 MB:
  - *read*: mp4/mov/3gp, mkv/webm, avi, flv, mpeg-ts/ps, asf/wmv, ogg, wav,
    mp3, aac, flac containers; H.264, HEVC, VP8, VP9, AV1 (dav1d), MPEG-1/2/4,
    WMV/VC-1, H.263, VP6, Theora, MJPEG video; AAC, AC-3/E-AC-3, MP2/3, Opus,
    Vorbis, FLAC, ALAC, WMA, DTS, TrueHD, ADPCM, PCM audio; JPEG, PNG, WebP,
    GIF, BMP, TIFF, HEIC, AVIF images.
  - *write*: WebM with libvpx-vp9 + libopus; WebP with libwebp.
- **Fully static** (musl, `ldd` → "not a dynamic executable"); runs on any
  Linux of the same architecture, including `FROM scratch` containers.

Not included (rare; add a flag in `vendor.sh` and rebuild if needed): network
inputs, hardware acceleration, subtitle codecs, image-sequence inputs.

## License note

The binary statically links LGPL-2.1+ code (FFmpeg), BSD code (libvpx,
libopus, dav1d, libwebp) and WTFPL code (zimg). No GPL components are
included. If you redistribute the binary, LGPL terms apply (provide
source/relink ability).
