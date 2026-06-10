#!/usr/bin/env bash
# x264 — H.264 encoder for --legacy; decoding uses FFmpeg's native h264
# decoder. 8-bit 4:2:0 only, like the libaom build: webify never feeds it
# anything else. NOTE: x264 is GPL — see the README license note.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# x264 publishes no release tarballs: pinned to the tip of the upstream
# `stable` branch (X264_BUILD 165, 2025-06-08) by commit hash
X264_COMMIT=b35605ace3ddf7c1a5d67a2eb553f034aef41d55
# pinned from a verified-good download
X264_SHA256=cd71a7515b0e9a012e1ac9b1f8415bebcaf6fc97d4db32286642ac4c0fbe24f9

built x264 && exit 0

fetch "https://code.videolan.org/videolan/x264/-/archive/$X264_COMMIT/x264-$X264_COMMIT.tar.gz" x264 "$X264_SHA256"
echo "==> building x264"
(cd "$SRC/x264" && \
    ./configure --prefix="$PREFIX" \
        --enable-static --enable-pic --disable-cli \
        --disable-avs --disable-swscale --disable-lavf --disable-ffms \
        --disable-gpac --disable-lsmash --disable-opencl \
        --bit-depth=8 --chroma-format=420 \
        --extra-cflags="$SECTION_CFLAGS" && \
    make -j"$JOBS" && make install)
mark x264
