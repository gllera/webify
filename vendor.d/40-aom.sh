#!/usr/bin/env bash
# libaom — AV1 encoder only; decoding stays on dav1d. HIGHBITDEPTH=0 drops
# the unused 10/12-bit encode paths (~2 MB of binary): webify only ever
# feeds it 8-bit yuv420p (SDR after tonemapping). Needs yasm — the aom
# cmake probe rejects Alpine's nasm 3.x.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

AOM_VERSION=3.15.3
# pinned from a verified-good download
AOM_SHA256=12c01dc731ed7b5a999e28120b45bf58a002ba3435a4ba2cfbc1e084a27c9c04
# tarball URL, @V@ = AOM_VERSION (update-vendor.sh probes with it too)
AOM_URL=https://storage.googleapis.com/aom-releases/libaom-@V@.tar.gz

built aom && exit 0

fetch "${AOM_URL//@V@/$AOM_VERSION}" aom "$AOM_SHA256"
echo "==> building libaom"
(mkdir -p "$SRC/aom-build" && cd "$SRC/aom-build" && \
    cmake "$SRC/aom" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=0 \
        -DCONFIG_AV1_DECODER=0 -DCONFIG_AV1_HIGHBITDEPTH=0 \
        -DENABLE_APPS=0 -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0 \
        -DENABLE_DOCS=0 \
        -DCMAKE_C_FLAGS="-O2 -fPIC $SECTION_CFLAGS" && \
    make -j"$JOBS" && make install)
mark aom
