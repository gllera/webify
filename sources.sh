#!/usr/bin/env bash
# Assembles the complete corresponding source of a webify binary, which the
# GPL (see COPYING) requires next to every binary we publish: this repo at
# HEAD plus the exact upstream tarballs vendor.d pins, fetched from the same
# URLs the build fetches and verified against the same sha256s. Run by the
# release job in build.yml; safe by hand.
#
#   ./sources.sh webify-<tag>-source    # writes webify-<tag>-source.tar.gz
#
# The pins are read the way update-vendor.sh reads them (<P>_URL with @V@ for
# <P>_VERSION, or <P>_COMMIT for x264, and <P>_SHA256), so this never sources
# a vendor.d script and never touches vendor/.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

name=${1:?usage: $0 <name>   (writes <name>.tar.gz)}
die() { echo "sources.sh: $*" >&2; exit 1; }
var() { sed -n "s/^$2=//p" "$1"; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git archive --prefix="$name/" HEAD | tar -x -C "$tmp"
up="$tmp/$name/upstream"
mkdir -p "$up"

# the linked libraries only: 00-nasm.sh is an assembler the build runs, not
# part of the binary. The pins come from the archived tree, so they always
# match the repo copy shipped beside them, even in a dirty checkout.
for f in "$tmp/$name"/vendor.d/[1-9]*.sh; do
    p=$(sed -n 's/^\([A-Z0-9]*\)_URL=.*/\1/p' "$f")
    [ -n "$p" ] || die "${f##*/}: no <P>_URL pin"
    v=$(var "$f" "${p}_VERSION")
    [ -n "$v" ] || v=$(var "$f" "${p}_COMMIT")
    sum=$(var "$f" "${p}_SHA256")
    url=$(var "$f" "${p}_URL")
    url=${url//@V@/$v}
    [ -n "$v" ] && [[ $sum =~ ^[0-9a-f]{64}$ ]] || die "${f##*/}: incomplete pins"

    # name the file after the library when upstream's name doesn't say it
    # (zimg ships release-<v>.tar.gz)
    lib=${f##*/}; lib=${lib#*-}; lib=${lib%.sh}
    file=${url##*/}
    [[ $file == *"$lib"* ]] || file="$lib-$file"

    echo "==> $lib: $url"
    curl -fsSL --retry 3 -o "$up/$file" "$url"
    echo "$sum  $up/$file" | sha256sum -c --quiet - || die "$file: sha256 mismatch"
done
(cd "$up" && sha256sum -- * > SHA256SUMS)

tar -czf "$name.tar.gz" -C "$tmp" "$name"
echo "==> $name.tar.gz ($(du -h "$name.tar.gz" | cut -f1))"
