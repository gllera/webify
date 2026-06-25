# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this is

`webify` is a single static C++ binary (one source file, `src/webify.cpp`)
that converts any popular video to **H.264/AAC MP4** (faststart) and any
popular image to **AVIF** (animated GIF → animated AVIF), tuned for web
delivery. Input type is auto-detected; one option set (`-q`, `--max`) covers
both modes.

This is a deliberately **simplified** descendant of the original `webify`
(github.com/gllera/webify): there are no `--next` / `--legacy` format switches
and no `--fast` / `--best` effort tiers. Video is always x264 at preset
`veryslow` in CRF mode; images are always AVIF at AV1 Main profile (8-bit
4:2:0); `-q` is the only quality dial, adjusting the CRF (and the AAC bitrate).

It vendors a minimal FFmpeg + four codec libraries (official release tarballs,
sha256-pinned per library in `vendor.d/*.sh`), built in Docker (one stage per
library, see `Dockerfile`).

Two flags exist for asset-self-hosting integration (e.g. SRR's `asset-peek` /
`asset-process`), both emitting one line of JSON to stdout:

- `--peek <input>` — open + probe only (no encode), print
  `{"mimetype","extension","supported","encoding"}`. Three layers: (1) a video
  stream webify can *decode* (decoder check, not just a demuxer match) →
  `supported:true` + predicted output type; (2) else if FFmpeg opened it, take
  the type FFmpeg already knows (`ffmpeg_mime`: `iformat->mime_type`, else a
  demuxer-name map — audio is the case, e.g. mp3/wav/flac/ogg/m4a); (3) else
  `peek_identify` sniffs a non-media asset with the statically-linked **embedded**
  libmagic (pdf/svg/fonts). gzip is inflated here (zlib `gz*`) to sniff the
  *inner* type and tagged `encoding:"gzip"` — a static musl binary can't `dlopen`
  zlib for libmagic's MAGIC_COMPRESS. So libmagic only carries what FFmpeg can't
  open, which keeps the curated db tiny.
- `--json` — after a transcode to a **file** `<output>`, also print
  `{"mimetype","extension"}`. Purely additive: the media bytes are byte-identical
  to a run without it (asserted in `test.sh`); rejected with a stdout `<output>`.

`--peek` lives in `webify_peek` / `peek_identify` / `mime_to_ext` / `is_gzip`;
`--json` in the `emit_json` tail of `webify_run`. **libmagic** is vendored
(`vendor.d/50-libmagic.sh`, file 5.46, static) and its magic database is
recompiled to a **curated** subset (`MAGIC_SET` = documents, markup, fonts — the
only non-media web assets the FFmpeg path can't decode) so the embedded `.mgc`
is ~150 KB, not ~10 MB; the Dockerfile `build` stage embeds it via `ld -r -b
binary` (symbols `_binary_magic_mgc_{start,end}`). Unmapped types fall back to
`application/octet-stream`. Re-verify with `./test.sh` (the `--peek` / `--json`
blocks).

## Commands

```bash
./build.sh        # Docker build; exports the static binary to ./dist/webify
./test.sh         # behavioral smoke suite against dist/webify
                  #   needs host ffmpeg (>= 6), ffprobe, python3
./vendor.sh       # bare-host build of the vendored stack (Docker doesn't use it)
./update-vendor.sh# probe upstreams, rewrite the vendor.d version+sha256 pins
```

## Invariants — re-verify with ./test.sh before merging anything that could touch them

1. **Two output formats only**: video → H.264/AAC MP4, image/GIF → AVIF. No
   other muxer or encoder is compiled into the FFmpeg build.
2. **One effort tier, one quality dial**: `-q 0..10` maps to the x264/AVIF CRF
   and the AAC bitrate; nothing else trades quality or time.
3. **Piped i/o is byte-identical to file i/o.** Piped video output spools
   through a *named* temp file (faststart re-opens by URL); piped AVIF
   assembles in memory (the muxer back-patches). Asserted with `cmp` for the
   MP4 and brand checks for AVIF.
4. **Output is deterministic** (`AVFMT_FLAG_BITEXACT` on the muxer) — what
   makes piping byte-identical and `-q 8` == the default image.
5. **AVIF stays 8-bit 4:2:0 = AV1 Main profile** — the one profile hardware
   decoders reliably implement. Alpha rides as the auxiliary alpha stream only
   when the source really uses transparency (opaque alpha is dropped).
6. **Faststart**: MP4 `moov` at the head, piped output included (only the
   no-temp-file fallback degrades to a fragmented MP4).
7. **Never upscale; mono stays mono.** Video is pure CRF (no bitrate cap — CRF
   sets the size, `--max` sets the resolution); only the AAC audio bitrate is
   capped by a lossy source's own rate.
8. **EXIF/display-matrix rotation baked in; interlaced video deinterlaced
   (bwdif); HDR (PQ/HLG) video tonemapped to SDR bt709.**

## Quality settings

`-q 0..10` maps **directly** onto each encoder's own CRF (recommended
web-delivery values, not cross-format fits) — see the **"Quality settings"
banner section of `src/webify.cpp`** and `doc/calibration.md` for the four
linear curves (x264 / AVIF still / AVIF anim / AAC) and how to retune them.
There is no bitrate budget and no `calibrate.sh`: this build has no VP9/WebP
reference pipeline to fit against.

## Vendoring, CI, releases

- `vendor.d/*.sh`: one script per library, upstream release version +
  tarball sha256 pinned. Linked: **x264** (H.264 encode, GPL, pinned to the
  `stable` branch tip by commit hash), **libaom** (AV1/AVIF encode), **dav1d**
  (AV1 decode), **zimg** (HDR tonemap), **libmagic** (file 5.46 — `--peek`
  content sniffing, curated magic db; linked into webify directly, not through
  ffmpeg). `00-nasm.sh` only builds on bare hosts.
- `.github/workflows/build.yml`: native amd64 + arm64 builds, BuildKit layer
  cache per library via `type=gha`; the test suite gates the `release` job.
  Tag pattern `'[0-9]*'` publishes a GitHub Release.
- `.github/workflows/vendor-update.yml` (monthly): bumps pins, PRs on the
  rolling `vendor-updates` branch, tags `<ffmpeg-version>-<YYYYMMDD>`, and
  dispatches build.yml on the tag.
- License: **GPL-2.0+** (x264 is GPL; everything else is more permissive).

## Removed vs the original (don't re-add without a reason)

The VP9/WebP default pipeline, `--next` (AV1 WebM video), `--legacy` PNG/APNG
images, the `--fast`/`--best` tiers, the VP9 two-pass stats run, the WebP
lossless/lossy race, the libvpx/libopus/libwebp vendored libraries, and the
VP9-anchored equal-SSIM calibration + codec-weighted rate-budget machinery —
all removed on purpose to keep this a two-format, one-tier, direct-CRF tool.
