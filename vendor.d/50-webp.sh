#!/usr/bin/env bash
# libwebp — official WebP encoder; libwebpmux is needed by libwebp_anim;
# decoding uses FFmpeg's native webp decoder
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

WEBP_VERSION=1.6.0
# pinned from a verified-good download
WEBP_SHA256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564

built webp && exit 0

fetch "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$WEBP_VERSION.tar.gz" webp "$WEBP_SHA256"
echo "==> building libwebp"
(cd "$SRC/webp" && \
    CFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
        --enable-static --disable-shared \
        --enable-libwebpmux --disable-libwebpdemux \
        --disable-gl --disable-sdl --disable-png --disable-jpeg \
        --disable-tiff --disable-gif --disable-wic && \
    make -j"$JOBS" && make install)
mark webp
