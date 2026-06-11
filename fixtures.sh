#!/usr/bin/env bash
# fixtures.sh — fetch the real-content calibration fixtures from the repo's
# `fixtures-v1` GitHub Release into ./fixtures/ (gitignored), verifying the
# sha256 pins below. Quiet and instant when everything is already present.
#
# The fixtures live on the release instead of in the repo to keep clones
# lean, but are pinned here because the equal-SSIM calibration
# (doc/*-calibration.md) is fitted against these exact bytes: the video
# segments cannot be re-extracted byte-identically (stream-copy cut points
# move with the extracting ffmpeg) and the Kodak mirror is a hobby page.
# New bytes mean a new fixtures-vN release and a re-baseline — never a
# re-upload over the old assets.
#
# Provenance and licenses (full notes on the release page):
#   kodim23.png, kodim04.png — Kodak Lossless True Color Image Suite,
#       released by Eastman Kodak for unrestricted use (r0k.us/graphics/kodak)
#   tos.mp4 — Tears of Steel, CC-BY, (CC) Blender Foundation | mango.blender.org
#   bbb.mp4 — Big Buck Bunny, CC-BY, (c) 2008 Blender Foundation | peach.blender.org
set -euo pipefail
cd "$(dirname "$0")"

BASE=https://github.com/gllera/webify/releases/download/fixtures-v1
PINS='b7e5ed7a0458a6680839db8b2d6a58ff65dc0c5c50f18732c780c845c35f2f33 bbb.mp4
c50b71e7d4613dda41703f05929f02878f9a1621d89f751d63229e0cabb876db tos.mp4
e3b946107c5d3441c022f678d0c3caf1e224d81b1604ba840a4f88e562de61aa kodim04.png
e3111a2fd4da24af15d6459ef9eacfe54106b38e27b4a21821b75c3f5d2d5baf kodim23.png'

mkdir -p fixtures
while read -r sha f; do
    echo "$sha  fixtures/$f" | sha256sum -c --status 2>/dev/null && continue
    echo "fetching $f"
    curl -fsSL -o "fixtures/$f" "$BASE/$f"
    echo "$sha  fixtures/$f" | sha256sum -c --status ||
        { echo "sha256 mismatch: fixtures/$f" >&2; exit 1; }
done <<< "$PINS"
