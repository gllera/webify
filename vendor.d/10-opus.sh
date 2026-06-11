#!/usr/bin/env bash
# libopus — Opus audio encoder
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

OPUS_VERSION=1.6.1
# sha256 matches the publisher's checksum file
OPUS_SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1

built opus && exit 0

fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz" opus "$OPUS_SHA256"
echo "==> building libopus"
(cd "$SRC/opus" && \
    CFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
        --enable-static --disable-shared --disable-doc --disable-extra-programs && \
    make -j"$JOBS" && make install)
mark opus
