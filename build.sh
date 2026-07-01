#!/usr/bin/env bash
# Build webify inside Docker and export the static binary to ./dist/webify
#
#   ./build.sh                        normal build (--version reports "dev")
#   UPX=1 ./build.sh                  also compress the binary with upx
#   PLATFORM=linux/arm64 ./build.sh   cross-build via qemu/binfmt (slow)
#   VERSION=x ./build.sh              bake a version string (CI passes the
#                                     release tag on tag builds)
#   TEST=1 ./build.sh                 also build the hermetic test image and run
#                                     the behavioral + golden suite (pinned
#                                     ffmpeg; add REBASELINE=1 to refresh goldens)
#   FUZZ=1 ./build.sh                 also export the libFuzzer binary to
#                                     ./dist/webify_fuzz
set -euo pipefail
cd "$(dirname "$0")"

args=(--build-arg "COMPRESS=${UPX:-0}")
[ -n "${PLATFORM:-}" ] && args+=(--platform "$PLATFORM")
[ -n "${VERSION:-}" ] && args+=(--build-arg "VERSION=$VERSION")

docker build --target dist --output dist "${args[@]}" .

echo
echo "Built: $(pwd)/dist/webify"
ls -lh dist/webify
file dist/webify 2>/dev/null || true

if [ "${FUZZ:-0}" = 1 ]; then
    echo
    echo "Building libFuzzer binary -> dist/webify_fuzz"
    docker build --target fuzz-bin --output dist "${args[@]}" .
    ls -lh dist/webify_fuzz
fi

if [ "${TEST:-0}" = 1 ]; then
    echo
    echo "Building hermetic test image and running the suite"
    docker build --target test -t webify-test:local "${args[@]}" .
    # REBASELINE=1 writes goldens/<arch>.sha256 back to the working tree
    run=(docker run --rm)
    if [ "${REBASELINE:-0}" = 1 ]; then
        run+=(-e REBASELINE=1 -v "$PWD/goldens:/src/goldens")
    fi
    "${run[@]}" webify-test:local
fi
