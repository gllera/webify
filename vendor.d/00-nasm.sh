#!/usr/bin/env bash
# nasm (assembler) — only built if the system lacks nasm/yasm; the Docker
# build image installs both via apk, so this only runs on bare-host builds
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NASM_VERSION=3.01
NASM_SHA256=b7324cbe86e767b65f26f467ed8b12ad80e124e3ccb89076855c98e43a9eddd4

built nasm && exit 0
{ command -v nasm >/dev/null || command -v yasm >/dev/null; } && exit 0

fetch "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VERSION/nasm-$NASM_VERSION.tar.xz" nasm "$NASM_SHA256"
echo "==> building nasm"
(cd "$SRC/nasm" && ./configure --prefix="$PREFIX" && make -j"$JOBS" nasm && \
    install -Dm755 nasm "$PREFIX/bin/nasm")
mark nasm
