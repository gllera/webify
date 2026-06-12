# webify

A single, fully static binary that transcodes any popular video file to
VP9/Opus WebM — and any popular image file to WebP — with sane defaults
tuned for serving the result over the internet: two-pass VP9 (10-20% smaller
at equal quality), the seek index at the head of the file (faststart), and
the densest WebP encoding (`-m 6`). `--next` switches to the next-gen
formats — AV1/Opus WebM and AVIF — and `--legacy` to the
maximum-compatibility ones — H.264/AAC MP4 and PNG/APNG — both at the same
visual targets. Input type is auto-detected, and one option set covers both
modes:

```
webify input.mp4 output.webm
webify photo.jpg photo.webp
webify -q 6 --max 720x1280 input.mov output.webm   # fit 720 tall, 1280 wide
webify input.mp4 > output.webm             # output omitted = stdout
cat input.mkv | webify - > output.webm     # '-' = stdin (or explicit stdout)
cat anim.gif  | webify - > anim.webp
webify --next input.mp4 output.webm        # AV1/Opus WebM instead of VP9
webify --next photo.jpg photo.avif         # AVIF instead of WebP
webify --legacy input.mkv output.mp4       # H.264/AAC MP4 instead
webify --legacy photo.jpg photo.png        # lossless PNG instead
```

## Options

Options mean the same thing for video and image inputs (and go before the
file arguments):

- `-q`, `--quality <0-10>` — target quality, higher is better (decimals
  allowed; default ≈8). Images: WebP quality ×10. Video: mapped onto the
  encoder's CRF, with the audio bitrate scaling along. Details:
  [doc/video.md](doc/video.md), [doc/images.md](doc/images.md).
- `-m`, `--max [HxW | S][@F]` — size and frame-rate caps in one flag. The
  box part is height×width (`720x1280`), a single number `S` to bound both
  sides, or one-sided (`720x` = height only, `x1280` = width only):
  downscale to fit (applied after rotation), preserving aspect ratio; never
  upscales. With no box at all, images keep their resolution and video fits
  within a height of 480; giving a box replaces that default entirely. The
  `@F` part is video only and off by default: drop frames to cap the frame
  rate (e.g. `--max @30` halves the frame budget of a 60 fps screencast).
  Each part is optional: `720`, `480x854`, `720x`, `@30`, and `480x@30`
  are all valid.
- `--fast` / `--best` — trade bytes for encode time at the same `-q`
  quality target (mutually exclusive; the default is the tuned middle
  ground). `--fast` is ~4-15× faster for a few percent more bytes;
  `--best` digs 1-7% more bytes out of video. Every step in each tier
  measurably pays for its time. Details per pipeline:
  [doc/video.md](doc/video.md), [doc/images.md](doc/images.md).
- `--next` — output AV1/Opus WebM and AVIF (animated GIF → animated AVIF)
  instead. `-q` buys the *same look* as the default formats — every
  mapping is an equal-SSIM fit — so `--next` only changes the file size:
  measured at the defaults, video −28%, stills −26%, animated GIFs −86%
  on live action (synthetic/graphic anims −79…−92%, riding above parity),
  all within ±0.005 SSIM on real content. Everything stays 8-bit 4:2:0, i.e. AV1 Main
  profile end-to-end — the one profile hardware decoders reliably
  implement. Details and caveats: [doc/next.md](doc/next.md).
- `--legacy` — output H.264/AAC MP4 (real `+faststart`) and lossless
  PNG/APNG instead, again at the same visual target as the default
  pipeline (`-q` steers video only — PNG is lossless by definition).
  x264 is GPL — see the license note below. Details and caveats:
  [doc/legacy.md](doc/legacy.md).
- `-h`, `--help` / `--version` — the usual; `--version` also reports the
  vendored FFmpeg version baked into the binary.

Exit status: 0 on success, 1 when a conversion fails, 2 for usage errors.
When stderr is a terminal and the input declares a duration, video
conversions print pass/percentage progress.

Every supported format works from stdin/stdout (`-` — and the output
argument can simply be omitted, which means stdout), and piping never
changes the result: the bytes are identical to file i/o, with formats that
need seeking spooled through a temporary file — see
[doc/piping.md](doc/piping.md) for how each case is handled.

## Getting it

Prebuilt static binaries (linux amd64 + arm64) are attached to
[GitHub Releases](https://github.com/gllera/webify/releases); release tags
are `<ffmpeg-version>-<date>` snapshots of the vendored library set.

Or build it yourself — everything builds inside Docker, nothing is
installed on the host:

```
./build.sh                        # -> dist/webify (static musl binary)
UPX=1 ./build.sh                  # also upx-compress (~60% smaller, slower start)
PLATFORM=linux/arm64 ./build.sh   # cross-build via qemu/binfmt (slow)
./test.sh                         # smoke test the built binary (needs host
                                  # ffmpeg, ffprobe, cwebp, python3)
```

## Design

- **Zero third-party code** — one C++ file (`src/webify.cpp`) against the
  official FFmpeg API.
- **Official upstream sources only** — every vendored library pinned to a
  release tarball + sha256 in its own `vendor.d/*.sh`, kept current by a
  monthly auto-update PR.
- **Minimal FFmpeg** — `--disable-everything` + whitelist keeps the binary
  ~19 MB; reads every popular video/image format, writes only the six
  output formats.
- **Fully static** (musl) — runs on any Linux of the same architecture,
  including `FROM scratch` containers.

Details — vendoring, the update workflow, the full format lists, CI:
[doc/build.md](doc/build.md).

## Documentation

- [doc/video.md](doc/video.md) — the VP9/Opus pipeline: two-pass, rate
  caps, HDR tonemapping, effort tiers
- [doc/images.md](doc/images.md) — the WebP pipeline: cwebp parity, the
  lossless race, animations
- [doc/next.md](doc/next.md) — `--next` (AV1/AVIF) in depth
- [doc/legacy.md](doc/legacy.md) — `--legacy` (H.264/PNG) in depth
- [doc/sizes.md](doc/sizes.md) — size and encode-time charts across all
  codecs, quality points and effort tiers, at the same visual quality
- [doc/piping.md](doc/piping.md) — stdin/stdout behavior
- [doc/build.md](doc/build.md) — building, vendoring, CI, releases
- [doc/next-calibration.md](doc/next-calibration.md),
  [doc/legacy-calibration.md](doc/legacy-calibration.md) — the equal-SSIM
  calibration data behind `--next` and `--legacy`

## License note

The binary statically links LGPL-2.1+ code (FFmpeg), BSD code (libvpx,
libopus, dav1d, libaom, libwebp), WTFPL code (zimg) — and, since
`--legacy`, **GPL-2.0+ code (x264)**, with FFmpeg built `--enable-gpl`.
The combined binary is therefore governed by the GPL-2.0+: if you
redistribute it, GPL terms apply (provide the full corresponding source).
Drop `vendor.d/60-x264.sh` (and its Docker stage) and
`--enable-gpl`/`--enable-libx264` from `vendor.d/80-ffmpeg.sh` to get a
GPL-free build without `--legacy` video.
