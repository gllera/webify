# shellcheck shell=bash
# Shared by the per-library build scripts in this directory (sourced, not
# executed). Each script pins its own version + tarball sha256 — the build
# fails loudly if upstream ever serves different bytes.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/vendor/src"
PREFIX="$ROOT/vendor/out"
JOBS="${JOBS:-$(nproc)}"

# -ffunction-sections/-fdata-sections lets the final link drop unused code
# shellcheck disable=SC2034  # consumed by the sourcing vendor.d/*.sh scripts
SECTION_CFLAGS="-ffunction-sections -fdata-sections"

export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

mkdir -p "$SRC" "$PREFIX"

fetch() { # <url> <dir-name> <sha256>
    local url="$1" name="$2" sum="$3"
    # the guard dir must appear only after a verified, complete extraction:
    # extract into a .tmp dir and mv it into place last, so an interrupted
    # run can't make the next one skip the download and the checksum
    if [ ! -d "$SRC/$name" ]; then
        echo "==> downloading $name"
        curl -fL --retry 3 -o "$SRC/$name.tar" "$url"
        echo "$sum  $SRC/$name.tar" | sha256sum -c -
        rm -rf "$SRC/$name.tmp"
        mkdir -p "$SRC/$name.tmp"
        tar -xf "$SRC/$name.tar" -C "$SRC/$name.tmp" --strip-components=1
        rm -f "$SRC/$name.tar"
        mv "$SRC/$name.tmp" "$SRC/$name"
    fi
}

built() { [ -f "$PREFIX/.built-$1" ]; }
# mark also deletes the library's source/build trees: each library lives in
# its own Docker stage, so incremental rebuilds within one library never
# happen anyway, and keeping build trees out of the layer keeps the CI layer
# cache small
mark()  { touch "$PREFIX/.built-$1"; rm -rf "$SRC/${1:?}" "$SRC/$1-build"; }
