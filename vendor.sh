#!/usr/bin/env bash
# Builds the minimal static FFmpeg stack (+ libvpx, libopus, dav1d, libaom,
# x264, libwebp, zimg) into vendor/out by running the per-library scripts in
# vendor.d/ in order. Each script pins its own upstream release version and
# tarball sha256 — official tarballs only, plus the minimal patches in
# patches/ (each one a few lines, applied verbatim and reviewable in-repo).
#
# The Docker build (see Dockerfile) does NOT run this file: it gives every
# vendor.d script its own build stage, so changing one library only
# recompiles that library. This wrapper is the all-in-one path for bare-host
# builds and local debugging.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

for script in vendor.d/[0-9]*.sh; do
    "$script"
done

echo
echo "==> vendored static libs:"
ls -lh vendor/out/lib/*.a
