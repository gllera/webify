#!/usr/bin/env bash
# libmagic (the `file` library) — content-type sniffing for `webify --peek`'s
# non-media fallback. Built static; the build stage embeds its compiled magic
# database into the binary so webify stays self-contained. No compression
# backends: webify inflates gzip itself with zlib, so MAGIC_COMPRESS (which
# dlopens zlib) is never used.
#
# The full database is ~10 MB. webify only ever sniffs the non-media web assets
# its FFmpeg path can't decode — in practice documents and markup (PDF, SVG/XML/
# HTML) plus fonts; raster/audio/video are decoded by FFmpeg (supported:true) and
# never reach this fallback. So we recompile just those magic/Magdir files and
# install THAT as magic.mgc (~150 KB vs ~10 MB), with built-in ASCII detection
# still covering text/plain/html/css/js. Anything unmapped falls back to
# application/octet-stream, the safe browser default (download). Add a Magdir
# file to MAGIC_SET to teach the fallback another type.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

FILE_VERSION=5.46
FILE_SHA256=c9cc77c7c560c543135edc555af609d5619dbef011997e988ce40a3d75d86088
# tarball URL, @V@ = FILE_VERSION (update-vendor.sh probes with it too)
FILE_URL=https://astron.com/pub/file/file-@V@.tar.gz

# magic/Magdir files compiled into the embedded database. Adding a web asset
# type webify should identify = add its Magdir file here. A rename upstream
# fails the build loudly (cp below), which is the right signal to revisit this.
MAGIC_SET="pdf sgml fonts"

built libmagic && exit 0

fetch "${FILE_URL//@V@/$FILE_VERSION}" file "$FILE_SHA256"
echo "==> building libmagic"
(cd "$SRC/file" && \
    CFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
        --enable-static --disable-shared \
        --disable-zlib --disable-bzlib --disable-xzlib \
        --disable-zstdlib --disable-lzlib --disable-libseccomp && \
    make -j"$JOBS" && make install && \
    echo "==> compiling curated magic database ($MAGIC_SET)" && \
    mkdir -p curated && \
    for m in $MAGIC_SET; do cp "magic/Magdir/$m" curated/; done && \
    src/file -C -m curated && \
    install -Dm644 curated.mgc "$PREFIX/share/misc/magic.mgc" && \
    ls -lh "$PREFIX/share/misc/magic.mgc")
mark libmagic
