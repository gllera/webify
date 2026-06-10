# webmify

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
webmify input.mp4 output.webm
webmify photo.jpg photo.webp
webmify -q 6 --max 720x1280 input.mov output.webm   # fit 720 tall, 1280 wide
cat input.mkv | webmify - - > output.webm   # '-' = stdin/stdout
cat anim.gif  | webmify - - > anim.webp
webmify --next input.mp4 output.webm        # AV1/Opus WebM instead of VP9
webmify --next photo.jpg photo.avif         # AVIF instead of WebP
webmify --legacy input.mkv output.mp4       # H.264/AAC MP4 instead
webmify --legacy photo.jpg photo.png        # lossless PNG instead
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
- `--next` — output the next-gen formats: video becomes AV1/Opus WebM
  (libaom) and every image becomes AVIF — animated GIF becomes *animated*
  AVIF (the `avis` brand, looping forever like the GIF did). `-q` buys the
  *same look* as the default formats, not the same internal number, so
  `--next` only changes the file size, never the quality. Every mapping is
  a piecewise fit of measured equal-SSIM points against the VP9/WebP
  output across photo, noise and graphics fixtures, rounded a hair
  *toward* the smaller file (`doc/next-calibration.md` has the data):
  video keeps its CRF nudged up where libaom outperforms libvpx
  (+`(crf−20)/6` above CRF 20, `--fast` +4 more) while the bitrate budget
  follows the shifted CRF and is converted by the same codec-efficiency
  table that weights the source cap (÷1.3); stills run near-linear to
  `-q 8` then dive with cwebp's premium top end (q 8 → CRF 28, 9.5 → 12,
  10 → 6); animations get their own much-higher curve (CRF 63 easing to
  56 at `-q 8`) because animated WebP is far weaker than stills WebP.
  Measured at the defaults: video −28% bytes at equal SSIM (low `-q`
  reaches −60%), stills −26%, and animated GIFs −96% — all within
  ±0.005 SSIM of their VP9/WebP counterpart. AVIF keeps alpha when the
  input *really* uses it (a fully opaque alpha channel is detected and
  dropped instead of wasting a stream) as the standard auxiliary alpha
  stream — near-lossless and tagged full-range as MIAF demands; EXIF
  rotation is baked in as usual. RGB-decoded stills above `-q 8` race a
  4:4:4 candidate against 4:2:0 (full-resolution chroma is what closes
  most of the gap to WebP's lossless race on sharp graphics) and ship it
  only while it stays ≤1.35× the 4:2:0 bytes — saturated noise explodes
  in 4:4:4 and falls back, so the winner stays smaller than the WebP
  output either way. True lossless stays WebP-only (AV1 lossless is far
  larger — graphics that want lossless should stay WebP). Effort tiers,
  each step measured to pay for its time like the VP9/WebP ones:
  `--fast` runs cpu-used 6 single-pass (about *half* the time of even
  the VP9 default), the default runs cpu-used 4 two-pass (~3× the VP9
  default's encode time — libaom is simply heavier; that time is what
  buys the size win). For video and animations `--best` keeps the
  default encoder settings, because libaom's deeper searches measured
  *bigger* (+0.3-1% bytes for 1.2-3.4× the time — at a fixed CRF they
  buy a sliver of quality, never bytes) — it still spools piped video so
  the stats pass always runs. Stills use `usage=allintra` +
  `still-picture` at speeds 6/4/2 (speed 7 measured the same wall time
  as 6 for −.008 SSIM — a strictly worse point; avifenc defaults to
  speed 6 — the webmify default digs one step deeper). Two honest
  caveats, same cause: stills dominated by pure smooth gradients can
  come out *bigger* than their WebP (libaom spends a byte floor on them
  at any CRF, at higher quality), and near-static animations hit that
  floor too (a 30-frame static-background GIF measured 1.8 KB AVIF vs
  294 B WebP — trivial sizes). Real photos and real motion are
  unaffected. Everything else —
  the `--max` box, frame-rate caps, two-pass, tonemapping, faststart
  cues, mono/Opus audio rules, stdin/stdout — works identically.
- `--legacy` — the same idea pointed backwards: output the
  maximum-compatibility formats. Video becomes H.264/AAC MP4 (vendored
  x264 + FFmpeg's native AAC encoder) with the moov up front — the real
  `+faststart` — and every image becomes PNG; animated GIF becomes APNG,
  looping forever. Images are lossless by definition, so they are always
  at least the WebP pipeline's quality (alpha kept exactly; a fully opaque
  alpha channel is still detected and dropped) and `-q` steers video only.
  Like `--next`, `-q` buys the *same look* as the VP9 pipeline, not the
  same number: the mapping is a linear fit of measured equal-SSIM points
  across noise, graphics and real-content fixtures (`x264crf =
  0.34·vp9crf + 16.5`, rounded toward the smaller file —
  `doc/legacy-calibration.md` has the data; the line is far flatter than
  the two CRF scales suggest), and the VP9-anchored rate budget is
  converted by the same codec-efficiency table that weights the source cap
  (÷0.8 — an H.264 source caps a `--legacy` job at exactly its own rate).
  Always single-pass: x264's two-pass targets a bitrate rather than a
  quality, and its CRF mode already plans ahead (lookahead/mbtree), so
  piped input loses nothing. Effort tiers map to x264 presets, each step
  measured to pay for its time: the default goes straight to `veryslow` —
  the ladder's last paying step (−19% bytes vs `slow` at equal SSIM for
  2.7× the time, still ~10× faster than the VP9 default end to end;
  `placebo` measured 3.7× the time for +1% bytes and is in no tier) —
  `--fast` is preset `fast` (~5× faster) with a +1 CRF nudge measured
  against VP9's own fast tier, and `--best` changes nothing: the default
  already runs the deepest settings that pay, and there is no stats pass
  to add. At equal look the size varies by content more than folklore
  suggests — measured 0.4–1.2× the VP9 bytes at the default `-q`
  (synthetic graphics far smaller, real content slightly bigger): the
  compatibility tax is mostly the encode time the veryslow default spends.
  Two honest caveats: below ≈`-q 3` the converted rate caps bind before
  SSIM parity (x264's CRF mode has no average-rate cap, only the VBV
  peak, so real content measured +.027 SSIM at 2× the bytes there), and
  the per-content spread around the mean mapping is wider than AV1's
  (real content −.023 at `-q 7` while graphics sit above parity). PNG
  effort: the default runs zlib level 9 plus the per-row "mixed" filter
  search (−15–19% bytes vs the encoder defaults; the filter search
  *alone* backfires on gradients — the pairing matters), `--fast` keeps
  the encoder defaults. Audio becomes AAC at 1.5× the Opus rates (96k
  stereo / 72k mono at the default) — AAC needs roughly that for equal
  quality and FFmpeg's native encoder sits at the weak end, so 1.5 errs
  the right way; the source-rate cap applies as usual. Piped *output*
  switches to fragmented MP4 (a pipe cannot seek the moov back to the
  head); piped APNG is assembled in memory like AVIF, so its frame count
  survives. Everything else — the `--max` box, frame-rate caps,
  tonemapping, EXIF/rotation, mono rules, stdin/stdout — works
  identically. Note: x264 is GPL — see the license note below.
- `-h`, `--help` / `--version` — the usual; `--version` also reports the
  vendored FFmpeg version baked into the binary.

Exit status: 0 on success, 1 when a conversion fails, 2 for usage errors.

When stderr is a terminal and the input declares a duration, video
conversions print pass/percentage progress.

Every supported format works from stdin: images are buffered in memory, and
video containers that need a seekable input to keep all their features
(MP4/MOV without faststart, AVI) are spooled to an unlinked temporary file
(`$TMPDIR` or `/tmp`) that disappears when the process exits, however it
exits. Everything else streams — but a truly streamed video input cannot be
rewound for the stats pass, so it falls back to single-pass encoding (with a
warning). Piped video output is playable but carries no duration/seek index
in the header (`--legacy` switches to fragmented MP4 there, since the moov
cannot be seeked back to the head of a pipe). AVIF written to stdout is
assembled in memory and dumped whole at the end (its container back-patches
item offsets, which needs a seekable sink), and piped APNG is assembled the
same way (its frame count is back-patched too); piped AV1 WebM streams like
VP9 does — webmify pre-extracts the AV1 sequence header the muxer needs up
front, since libaom only delivers it alongside the first encoded packet.

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
`-cpu-used 4`/`-cpu-used 0` as described above. Tiles follow Google's VOD
table for the output width, with threads at double the table's column —
row-mt keeps the extra threads busy (0/2 below 512 px, 1/4 for 480p,
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
more bytes there, like `gif2webp` without `-sharp_yuv`) and always stay
lossy: lossless animation measured 2.7× bigger on dithered photo-like
GIFs. Graphics and flat-color GIFs are the exception — they measured
*smaller* lossless (testsrc2 −19%) — but only single frames race today.
The animated encoder itself is as small as the format allows: audited
against `gif2webp` at equal settings, within ±1% on every fixture.
Inputs: JPEG, PNG, WebP, GIF, BMP, TIFF, HEIC, AVIF.

## Building

Everything builds inside Docker; nothing is installed on the host:

```
./build.sh                        # -> dist/webmify (static musl binary)
UPX=1 ./build.sh                  # also upx-compress (~60% smaller, slower start)
PLATFORM=linux/arm64 ./build.sh   # cross-build via qemu/binfmt (slow)
./test.sh                         # smoke test the built binary (needs host
                                  # ffmpeg, ffprobe, cwebp, python3)
```

## Design

- **Zero third-party code.** The whole program is one C++ file
  (`src/webmify.cpp`) written against the official FFmpeg API, modeled on
  FFmpeg's own `doc/examples/transcode.c`, compiled with a single `g++ -static`
  invocation in the Dockerfile.
- **Official upstream sources only**, pinned release tarballs (`vendor.sh`)
  verified against pinned sha256 checksums, all latest releases:
  FFmpeg 8.1.1, libvpx 1.16.0 (webmproject), libopus 1.6.1 (xiph),
  dav1d 1.5.3 (VideoLAN), libaom 3.14.1 (AOMedia), libwebp 1.6.0
  (webmproject), x264 build 165 (VideoLAN; no release tarballs exist, so
  it is pinned to the tip of the upstream `stable` branch by commit hash),
  zimg 3.0.6 (sekrit-twc), zlib (Alpine static package) —
  plus the minimal patches in `patches/` (each a few lines, reviewable
  in-repo, applied verbatim by `vendor.sh`; currently one: exposing
  libwebp's `use_sharp_yuv` as an encoder option, which FFmpeg does not
  surface).
- **Minimal FFmpeg** (`--disable-everything --enable-small` + whitelist), so
  the binary stays ~19 MB instead of ~80 MB (libaom — encoder-only,
  8-bit-only, since webmify never emits anything else — is ~6 MB of that;
  x264, built 8-bit 4:2:0-only for the same reason, ~1 MB):
  - *read*: mp4/mov/3gp, mkv/webm, avi, flv, mpeg-ts/ps, asf/wmv, ogg, wav,
    mp3, aac, flac containers; H.264, HEVC, VP8, VP9, AV1 (dav1d), MPEG-1/2/4,
    WMV/VC-1, H.263, VP6, Theora, MJPEG video; AAC, AC-3/E-AC-3, MP2/3, Opus,
    Vorbis, FLAC, ALAC, WMA, DTS, TrueHD, ADPCM, PCM audio; JPEG, PNG, WebP,
    GIF, BMP, TIFF, HEIC, AVIF images.
  - *write*: WebM with libvpx-vp9 + libopus; WebP with libwebp; with
    `--next`: WebM with libaom-av1 + libopus, AVIF with libaom-av1; with
    `--legacy`: MP4 with x264 + native AAC, PNG/APNG.
- **Fully static** (musl, `ldd` → "not a dynamic executable"); runs on any
  Linux of the same architecture, including `FROM scratch` containers.

Not included (rare; add a flag in `vendor.sh` and rebuild if needed): network
inputs, hardware acceleration, subtitle codecs, image-sequence inputs.

## License note

The binary statically links LGPL-2.1+ code (FFmpeg), BSD code (libvpx,
libopus, dav1d, libaom, libwebp), WTFPL code (zimg) — and, since
`--legacy`, **GPL-2.0+ code (x264)**, with FFmpeg built `--enable-gpl`.
The combined binary is therefore governed by the GPL-2.0+: if you
redistribute it, GPL terms apply (provide the full corresponding source).
Drop the x264 section and `--enable-gpl`/`--enable-libx264` from
`vendor.sh` to get a GPL-free build without `--legacy` video.
