# webify

A single, fully static binary that transcodes any popular video file to
**H.264/AAC MP4**, any popular audio file to **AAC M4A**, and any popular image
file to **AVIF** — with sane defaults tuned for serving the result over the
internet: H.264 in CRF mode at preset `veryslow`, the `moov` atom at the head of
the file (faststart), and AVIF at AV1 Main profile (8-bit 4:2:0, the one profile
hardware decoders reliably implement). Input type is auto-detected, and one
option set covers every mode:

```
webify input.mp4 output.mp4
webify song.mp3  song.m4a                        # audio-only -> AAC M4A
webify photo.jpg photo.avif
webify anim.gif  anim.avif                       # animated GIF -> animated AVIF
webify -q 6 --max 720x1280 input.mov output.mp4  # fit 720 tall, 1280 wide
webify input.mp4 > output.mp4                     # output omitted = stdout
cat input.mkv | webify - > output.mp4             # '-' = stdin (or explicit stdout)
cat photo.png | webify - > photo.avif
```

This is a deliberately simplified descendant of the original
[`webify`](https://github.com/gllera/webify): it always writes those two
formats, at one tuned effort level. There are **no** `--next` / `--legacy`
format switches and **no** `--fast` / `--best` effort tiers — `-q` is the only
quality dial.

## Options

Options mean the same thing for video and image inputs (and go before the
file arguments):

- `-q`, `--quality <0-10>` — target quality, higher is better (decimals
  allowed; default ≈8). Mapped **directly** onto each encoder's own CRF using
  recommended web-delivery values — the x264 CRF for video, the AVIF CRF for
  images, with the AAC bitrate scaling along. See
  [doc/calibration.md](doc/calibration.md) for the exact curves.
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
- `--json` — after writing to a **file** `<output>`, print the result's
  `{"mimetype","extension"}` as JSON to stdout (e.g.
  `{"mimetype":"image/avif","extension":"avif"}`). The media bytes are
  unchanged — `--json` only adds the report — so the output file must be a real
  path, not stdout (which carries the JSON). Lets a caller learn the produced
  type without re-sniffing the bytes.
- `--peek <input>` — identify `<input>` and print
  `{"mimetype","extension","supported","encoding"}` **without** encoding (just
  open + probe). If webify can transcode it, it predicts the output type
  (`image/avif` / `video/mp4`, `supported:true`). Otherwise `supported:false` and
  the browser-compatible source type, so a caller hosts the original unchanged:
  audio-only and other media FFmpeg recognizes are typed by **FFmpeg** itself
  (e.g. `audio/mpeg`, `audio/wav`); non-media (PDF, SVG, fonts, …) is sniffed by
  a statically-linked, **embedded** **libmagic** — looking *inside* a gzip
  wrapper and reporting the inner type with `encoding:"gzip"` (an HTTP
  `Content-Encoding`). Everything is in the binary, so there is no runtime
  dependency. Always exits 0; the JSON, not the exit code, carries the verdict.
  Takes no `<output>`.
- `-h`, `--help` / `--version` — the usual; `--version` also reports the
  vendored FFmpeg version baked into the binary.

Exit status: 0 on success, 1 when a conversion fails, 2 for usage errors.
When stderr is a terminal and the input declares a duration, video
conversions print percentage progress.

Both formats work from stdin/stdout (`-` — and the output argument can simply
be omitted, which means stdout), and piping never changes the result: the
bytes are identical to file i/o, with the seek-needing MP4 spooled through a
temporary file — see [doc/piping.md](doc/piping.md) for how each case is
handled.

## What it does for web delivery

- **Video → H.264/AAC MP4** with the `moov` atom up front (faststart), piped
  output included. x264 runs in pure CRF mode (its lookahead/mbtree plan the
  rate ahead, so no two-pass is needed) at preset `veryslow` — no bitrate cap,
  CRF spends exactly what the chosen quality needs and `--max` bounds the
  resolution.
- **Images → AVIF** (vendored libaom), still or animated (animated GIF →
  animated AVIF, alpha kept as the auxiliary alpha stream when it is really
  used). Stills encode all-intra; everything stays 8-bit 4:2:0 = AV1 Main
  profile.
- **Never upscale; mono stays mono** (96k base, scaling with `-q`);
  smaller-than-480p video is not enlarged.
- **EXIF orientation** and display-matrix rotation are baked into the pixels
  (including the mirrored variants).
- **Interlaced video is deinterlaced** (bwdif), and **HDR video (PQ/HLG) is
  tonemapped** to SDR bt709.
- **Deterministic output** (`AVFMT_FLAG_BITEXACT`), which is what makes the
  piped-output spool byte-identical to a file run.

## Getting it

Build it yourself — everything builds inside Docker, nothing is installed on
the host:

```
./build.sh                        # -> dist/webify (static musl binary)
UPX=1 ./build.sh                  # also upx-compress (~60% smaller, slower start)
PLATFORM=linux/arm64 ./build.sh   # cross-build via qemu/binfmt (slow)
./test.sh                         # smoke test the built binary (needs host
                                  # ffmpeg, ffprobe, python3)
```

## Design

- **Zero third-party code** — one C++ file (`src/webify.cpp`) against the
  official FFmpeg API.
- **Official upstream sources only** — every vendored library pinned to a
  release tarball + sha256 in its own `vendor.d/*.sh`, kept current by a
  monthly auto-update PR.
- **Minimal FFmpeg** — `--disable-everything` + whitelist: reads every popular
  video/image format, writes only MP4 and AVIF. Four vendored libraries:
  x264 (H.264 encode), libaom (AV1/AVIF encode), dav1d (AV1 decode), zimg
  (HDR tonemapping).
- **Fully static** (musl) — runs on any Linux of the same architecture,
  including `FROM scratch` containers.

Details — vendoring, the update workflow, CI: [doc/build.md](doc/build.md).

## Documentation

- [doc/build.md](doc/build.md) — building, vendoring, CI, releases
- [doc/piping.md](doc/piping.md) — stdin/stdout behavior
- [doc/calibration.md](doc/calibration.md) — where the `-q` → CRF curves come
  from

## License note

The binary statically links LGPL-2.1+ code (FFmpeg), BSD code (dav1d, libaom),
WTFPL code (zimg) — and **GPL-2.0+ code (x264)**, with FFmpeg built
`--enable-gpl`. The combined binary is therefore governed by the GPL-2.0+: if
you redistribute it, GPL terms apply (provide the full corresponding source).
