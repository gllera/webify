#!/usr/bin/env bash
# dav1d — AV1 decoder (FFmpeg has no native software AV1 decoding)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DAV1D_VERSION=1.5.4
# sha256 matches the publisher's checksum file
DAV1D_SHA256=686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5

built dav1d && exit 0

fetch "https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.xz" dav1d "$DAV1D_SHA256"
echo "==> building dav1d"
(cd "$SRC/dav1d" && \
    meson setup build --prefix="$PREFIX" --libdir=lib \
        --buildtype=release --default-library=static \
        -Denable_tools=false -Denable_tests=false \
        -Dc_args="$SECTION_CFLAGS" && \
    ninja -C build && ninja -C build install)
mark dav1d
