#!/usr/bin/env bash
# libvpx — VP9 encoder only; decoding uses FFmpeg's native decoders
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

VPX_VERSION=1.16.0
# pinned from a verified-good download
VPX_SHA256=7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c

built vpx && exit 0

fetch "https://github.com/webmproject/libvpx/archive/refs/tags/v$VPX_VERSION.tar.gz" vpx "$VPX_SHA256"
echo "==> building libvpx"
(cd "$SRC/vpx" && \
    ./configure --prefix="$PREFIX" \
        --enable-static --disable-shared --enable-pic \
        --disable-vp8 --disable-vp9-decoder \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-libyuv --disable-webm-io \
        --extra-cflags="$SECTION_CFLAGS" && \
    make -j"$JOBS" && make install)
mark vpx
