# Building, vendoring, CI

## Building

Everything builds inside Docker; nothing is installed on the host:

```
./build.sh                        # -> dist/webify (static musl binary)
UPX=1 ./build.sh                  # also upx-compress (~60% smaller, slower start)
PLATFORM=linux/arm64 ./build.sh   # cross-build via qemu/binfmt (slow)
./test.sh                         # smoke test the built binary (needs host
                                  # ffmpeg, ffprobe, python3)
```

The whole program is one C++ file (`src/webify.cpp`) written against the
official FFmpeg API, modeled on FFmpeg's own `doc/examples/transcode.c`,
compiled with a single `g++ -static` invocation in the Dockerfile. The
result is fully static (musl, `ldd` → "not a dynamic executable") and runs
on any Linux of the same architecture, including `FROM scratch` containers.

## Vendored libraries

Official upstream sources only, pinned release tarballs verified against
pinned sha256 checksums — one `vendor.d/*.sh` script per library holds its
version + checksum pin (see those files for the current versions). Four
libraries are linked, the minimum the two output formats need:

- **x264** (VideoLAN) — H.264 encoder. No release tarballs exist, so it is
  pinned to the tip of the upstream `stable` branch by commit hash. GPL.
- **libaom** (AOMedia) — AV1/AVIF encoder (encoder-only, 8-bit-only).
- **dav1d** (VideoLAN) — AV1 decoder, so AV1/AVIF inputs decode.
- **zimg** (sekrit-twc) — colorspace engine for the zscale filter (HDR → SDR
  tonemapping).

Plus zlib (Alpine static package). No patches are applied to FFmpeg.

`./vendor.sh` runs all the scripts in order for bare-host builds, while the
Dockerfile gives each library its own build stage so changing one library
only recompiles that library (plus the FFmpeg stage that links it).

## Keeping pins current

`./update-vendor.sh` checks every upstream for new releases and rewrites
the version + sha256 pins (the dav1d pin comes from the publisher's
checksum file; the rest hash a fresh download). A monthly workflow
(`.github/workflows/vendor-update.yml`, 1st of the month) runs it, opens a
PR with any bumps, tags the bump commit `<ffmpeg-version>-<YYYYMMDD>`, and
dispatches CI on the tag — green on both arches publishes a GitHub Release
with the binaries attached, and the PR is merged only when it is.

## Minimal FFmpeg

FFmpeg is built `--disable-everything --enable-small` + whitelist, so the
binary stays ~16 MB instead of ~80 MB (libaom — encoder-only, 8-bit-only,
since webify never emits anything else — is ~6 MB of that; x264, built
8-bit 4:2:0-only for the same reason, ~1 MB):

- *read*: mp4/mov/3gp, mkv/webm, avi, flv, mpeg-ts/ps, asf/wmv, ogg, wav,
  mp3, aac, flac containers; H.264, HEVC, VP8, VP9, AV1 (dav1d), MPEG-1/2/4,
  WMV/VC-1, H.263, VP6, Theora, MJPEG video; AAC, AC-3/E-AC-3, MP2/3, Opus,
  Vorbis, FLAC, ALAC, WMA, DTS, TrueHD, ADPCM, PCM audio; JPEG, PNG, WebP,
  GIF, BMP, TIFF, HEIC, AVIF images.
- *write*: MP4 with x264 + native AAC (video); AVIF with libaom-av1
  (images, still + animated).

Not included (rare; add a flag in `vendor.d/80-ffmpeg.sh` and rebuild if
needed): network inputs, hardware acceleration, subtitle codecs,
image-sequence inputs.

## CI

`.github/workflows/build.yml` builds amd64 + arm64 on native runners
(ubuntu-24.04 / ubuntu-24.04-arm), with the BuildKit layer cache persisted
in the GitHub Actions cache per arch — one Docker stage per vendored
library means bumping one library's pin recompiles only that library.
Version-tag builds (tags matching `[0-9]*` — see "Keeping pins current"
above for the scheme) additionally publish a GitHub Release with both
binary tarballs attached.
