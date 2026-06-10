#!/usr/bin/env bash
# Build webmify inside Docker and export the static binary to ./dist/webmify
#
#   ./build.sh                        normal build
#   UPX=1 ./build.sh                  also compress the binary with upx
#   PLATFORM=linux/arm64 ./build.sh   cross-build via qemu/binfmt (slow)
set -euo pipefail
cd "$(dirname "$0")"

args=(--target dist --output dist --build-arg "COMPRESS=${UPX:-0}")
[ -n "${PLATFORM:-}" ] && args+=(--platform "$PLATFORM")

docker build "${args[@]}" .

echo
echo "Built: $(pwd)/dist/webmify"
ls -lh dist/webmify
file dist/webmify 2>/dev/null || true
