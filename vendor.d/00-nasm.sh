#!/usr/bin/env bash
# nasm (assembler) — only built if the system lacks nasm/yasm; the Docker
# build image installs both via apk, so this only runs on bare-host builds
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NASM_VERSION=3.02
NASM_SHA256=87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d
# tarball URL, @V@ = NASM_VERSION (update-vendor.sh probes with it too)
NASM_URL=https://www.nasm.us/pub/nasm/releasebuilds/@V@/nasm-@V@.tar.xz

built nasm && exit 0
{ command -v nasm >/dev/null || command -v yasm >/dev/null; } && exit 0

fetch "${NASM_URL//@V@/$NASM_VERSION}" nasm "$NASM_SHA256"
echo "==> building nasm"
(cd "$SRC/nasm" && ./configure --prefix="$PREFIX" && make -j"$JOBS" nasm && \
    install -Dm755 nasm "$PREFIX/bin/nasm")
mark nasm
