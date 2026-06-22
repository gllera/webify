#!/usr/bin/env bash
# zimg — colorspace engine for the zscale filter: HDR -> SDR tonemapping;
# its only git submodule is googletest, so the release tarball builds
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ZIMG_VERSION=3.0.6
# pinned from a verified-good download
ZIMG_SHA256=be89390f13a5c9b2388ce0f44a5e89364a20c1c57ce46d382b1fcc3967057577

built zimg && exit 0

fetch "https://github.com/sekrit-twc/zimg/archive/refs/tags/release-$ZIMG_VERSION.tar.gz" zimg "$ZIMG_SHA256"
echo "==> building zimg"
(cd "$SRC/zimg" && ./autogen.sh && \
    CXXFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
        --enable-static --disable-shared && \
    make -j"$JOBS" && make install)
mark zimg
