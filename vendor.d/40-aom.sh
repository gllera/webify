#!/usr/bin/env bash
# libaom — AV1 encoder only; decoding stays on dav1d. HIGHBITDEPTH=0 drops
# the unused 10/12-bit encode paths (~2 MB of binary): webify only ever
# feeds it 8-bit yuv420p (SDR after tonemapping). Needs yasm — the aom
# cmake probe rejects Alpine's nasm 3.x.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

AOM_VERSION=3.14.1
# pinned from a verified-good download
AOM_SHA256=44bf90dbd23e734d50e70a8c41c285193922938bd0d3bc2ee56764d181d55ef5

built aom && exit 0

fetch "https://storage.googleapis.com/aom-releases/libaom-$AOM_VERSION.tar.gz" aom "$AOM_SHA256"
echo "==> building libaom"
(mkdir -p "$SRC/aom-build" && cd "$SRC/aom-build" && \
    cmake "$SRC/aom" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=0 \
        -DCONFIG_AV1_DECODER=0 -DCONFIG_AV1_HIGHBITDEPTH=0 \
        -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0 -DENABLE_DOCS=0 \
        -DCMAKE_C_FLAGS="-O2 -fPIC $SECTION_CFLAGS" && \
    make -j"$JOBS" && make install)
mark aom
