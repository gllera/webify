#!/usr/bin/env bash
# Checks every vendored library for a newer upstream release and rewrites the
# version + sha256 pins in vendor.d/*.sh to match. Run monthly by the
# vendor-update workflow (.github/workflows/vendor-update.yml), which opens a
# PR with the result so CI proves both arches still build before anything
# lands on main. Safe to run by hand: edits files in place, prints one line
# per library, exits 2 if any upstream check failed (and still applies the
# bumps that succeeded).
#
# Where the publisher signs a checksum file (opus, dav1d) the new sha256 pin
# is taken from that file and the build's own fetch verifies the bytes; for
# the rest the pin is the hash of a fresh download, the same trust-on-first-
# use model the original pins used.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

FAILED=0
fail() { echo "FAIL  $*" >&2; FAILED=1; }

get() { curl -fsSL --retry 3 --max-time 120 "$@"; }

# newest stable-looking version on stdin (anything with rc/beta/etc suffixes
# fails the all-digits-and-dots filter)
latest() { grep -E '^[0-9]+(\.[0-9]+)*$' | sort -V | tail -1; }

# is $2 strictly newer than $1?
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }

var() { sed -n "s/^$2=//p" "vendor.d/$1"; }
pin() { sed -i "s|^$2=.*|$2=$3|" "vendor.d/$1"; }

# ---- per-library "list all upstream versions" probes ------------------------

nasm_versions()   { get https://www.nasm.us/pub/nasm/releasebuilds/ | grep -oE 'href="[0-9]+(\.[0-9]+)*/"' | grep -oE '[0-9]+(\.[0-9]+)*'; }
opus_versions()   { get https://downloads.xiph.org/releases/opus/ | grep -oE 'opus-[0-9]+(\.[0-9]+)*\.tar\.gz' | sed -E 's/^opus-//; s/\.tar\.gz$//'; }
vpx_versions()    { git ls-remote --tags https://github.com/webmproject/libvpx.git 'v*' | grep -oE 'refs/tags/v[0-9]+(\.[0-9]+)*$' | sed 's|.*/v||'; }
dav1d_versions()  { get https://downloads.videolan.org/pub/videolan/dav1d/ | grep -oE 'href="[0-9]+(\.[0-9]+)*/"' | grep -oE '[0-9]+(\.[0-9]+)*'; }
aom_versions()    { get 'https://storage.googleapis.com/aom-releases/' | grep -oE '<Key>libaom-[0-9]+(\.[0-9]+)*\.tar\.gz</Key>' | grep -oE '[0-9]+(\.[0-9]+)*'; }
webp_versions()   { get 'https://storage.googleapis.com/downloads.webmproject.org/?prefix=releases/webp/libwebp-' | grep -oE '<Key>releases/webp/libwebp-[0-9]+(\.[0-9]+)*\.tar\.gz</Key>' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)*'; }
zimg_versions()   { git ls-remote --tags https://github.com/sekrit-twc/zimg.git 'release-*' | grep -oE 'refs/tags/release-[0-9]+(\.[0-9]+)*$' | sed 's|.*release-||'; }
ffmpeg_versions() { get https://ffmpeg.org/releases/ | grep -oE 'ffmpeg-[0-9]+(\.[0-9]+)*\.tar\.xz' | grep -oE '[0-9]+(\.[0-9]+)*'; }

# ---- publisher checksum files (sha pinned from the file, fetch verifies) ----

opus_sha()  { get https://downloads.xiph.org/releases/opus/SHA256SUMS.txt | grep -E " \*?opus-$1\.tar\.gz$" | head -1 | cut -d' ' -f1; }
dav1d_sha() { get "https://downloads.videolan.org/pub/videolan/dav1d/$1/dav1d-$1.tar.xz.sha256" | cut -d' ' -f1; }

# ---- generic bump: check <script> <VAR prefix> <url, @V@=version> \
#                          <versions-fn> [publisher-sha-fn] ------------------

check() {
    local f=$1 prefix=$2 tmpl=$3 versions_fn=$4 sha_fn=${5:-}
    local cur new url sha
    cur=$(var "$f" "${prefix}_VERSION")
    new=$("$versions_fn" | latest) || true
    if [ -z "$new" ]; then fail "$f: could not determine latest upstream version"; return; fi
    if ! newer "$cur" "$new"; then echo "ok    $f: $cur (latest)"; return; fi
    url=${tmpl//@V@/$new}
    if [ -n "$sha_fn" ]; then
        sha=$("$sha_fn" "$new") || true
    else
        sha=$(get "$url" | sha256sum | cut -d' ' -f1) || true
    fi
    if ! [[ ${sha:-} =~ ^[0-9a-f]{64}$ ]]; then fail "$f: could not pin sha256 for $new"; return; fi
    pin "$f" "${prefix}_VERSION" "$new"
    pin "$f" "${prefix}_SHA256" "$sha"
    echo "bump  $f: $cur -> $new"
}

# x264 has no releases: "latest" is the tip of the upstream stable branch.
# The in-file comment carries the X264_BUILD number (from x264.h in the
# tarball) and commit date (GitLab API) — refresh those too.
check_x264() {
    local f=60-x264.sh cur new url tmp sha build date
    cur=$(var "$f" X264_COMMIT)
    new=$(git ls-remote https://code.videolan.org/videolan/x264.git refs/heads/stable | cut -f1) || true
    if [ -z "$new" ]; then fail "$f: could not resolve stable branch tip"; return; fi
    if [ "$cur" = "$new" ]; then echo "ok    $f: ${cur:0:9} (stable tip)"; return; fi
    url="https://code.videolan.org/videolan/x264/-/archive/$new/x264-$new.tar.gz"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    if ! get "$url" -o "$tmp/x264.tar.gz"; then fail "$f: download failed: $url"; return; fi
    sha=$(sha256sum "$tmp/x264.tar.gz" | cut -d' ' -f1)
    tar -xzf "$tmp/x264.tar.gz" -C "$tmp" --strip-components=1
    build=$(sed -n 's/^#define X264_BUILD *//p' "$tmp/x264.h" | head -1)
    [[ $build =~ ^[0-9]+$ ]] || build='?'
    date=$(get "https://code.videolan.org/api/v4/projects/videolan%2Fx264/repository/commits/$new" \
        | grep -oE '"committed_date":"[0-9]{4}-[0-9]{2}-[0-9]{2}' | head -1 | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}$') || true
    [[ ${date:-} =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || date='?'
    pin "$f" X264_COMMIT "$new"
    pin "$f" X264_SHA256 "$sha"
    sed -i -E "s/\(X264_BUILD [0-9?]+, [0-9?-]+\)/(X264_BUILD $build, $date)/" "vendor.d/$f"
    echo "bump  $f: ${cur:0:9} -> ${new:0:9} (build $build, $date)"
}

check 00-nasm.sh   NASM   'https://www.nasm.us/pub/nasm/releasebuilds/@V@/nasm-@V@.tar.xz'                  nasm_versions
check 10-opus.sh   OPUS   'https://downloads.xiph.org/releases/opus/opus-@V@.tar.gz'                        opus_versions opus_sha
check 20-vpx.sh    VPX    'https://github.com/webmproject/libvpx/archive/refs/tags/v@V@.tar.gz'             vpx_versions
check 30-dav1d.sh  DAV1D  'https://downloads.videolan.org/pub/videolan/dav1d/@V@/dav1d-@V@.tar.xz'          dav1d_versions dav1d_sha
check 40-aom.sh    AOM    'https://storage.googleapis.com/aom-releases/libaom-@V@.tar.gz'                   aom_versions
check 50-webp.sh   WEBP   'https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-@V@.tar.gz' webp_versions
check_x264
check 70-zimg.sh   ZIMG   'https://github.com/sekrit-twc/zimg/archive/refs/tags/release-@V@.tar.gz'         zimg_versions
check 80-ffmpeg.sh FFMPEG 'https://ffmpeg.org/releases/ffmpeg-@V@.tar.xz'                                   ffmpeg_versions

exit $((FAILED ? 2 : 0))
