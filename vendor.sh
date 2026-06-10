#!/usr/bin/env bash
# Builds a minimal static FFmpeg (plus libvpx + libopus + dav1d + libaom +
# zimg) into vendor/out. Runs inside the Docker build image — see Dockerfile.
# Official upstream release tarballs only, plus the minimal patches in
# patches/ (each one a few lines, applied verbatim and reviewable in-repo).
#
# Only what `webmify` needs is compiled in:
#   - demuxers/decoders to READ popular video files (mp4/mkv/webm/avi/flv/ts/wmv/ogg...)
#     and image files (jpeg/png/webp/gif/bmp/tiff/heic/avif)
#   - encoders libvpx-vp9 + libopus (webm muxer), libwebp (webp muxer),
#     libaom-av1 for --next (webm + avif muxers)
#   - the filters used by the fixed pipeline (scale, format, aformat, aresample,
#     transpose/hflip/vflip for phone-rotation metadata, zscale/tonemap for
#     HDR -> SDR conversion)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/vendor/src"
PREFIX="$ROOT/vendor/out"
JOBS="${JOBS:-$(nproc)}"

NASM_VERSION=3.01
OPUS_VERSION=1.6.1
VPX_VERSION=1.16.0
DAV1D_VERSION=1.5.3
AOM_VERSION=3.14.1
WEBP_VERSION=1.6.0
ZIMG_VERSION=3.0.6
FFMPEG_VERSION=8.1.1

# sha256 of each release tarball; opus and dav1d match their publishers'
# checksum files, the rest are pinned from a verified-good download (the
# build fails loudly if upstream ever serves different bytes)
NASM_SHA256=b7324cbe86e767b65f26f467ed8b12ad80e124e3ccb89076855c98e43a9eddd4
OPUS_SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
VPX_SHA256=7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c
DAV1D_SHA256=732010aa5ef461fa93355ed2c6c5fedb48ddc4b74e697eaabe8907eaeb943011
AOM_SHA256=44bf90dbd23e734d50e70a8c41c285193922938bd0d3bc2ee56764d181d55ef5
WEBP_SHA256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
ZIMG_SHA256=be89390f13a5c9b2388ce0f44a5e89364a20c1c57ce46d382b1fcc3967057577
FFMPEG_SHA256=b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3

# -ffunction-sections/-fdata-sections lets the final link drop unused code
SECTION_CFLAGS="-ffunction-sections -fdata-sections"

export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

mkdir -p "$SRC" "$PREFIX"

fetch() { # <url> <dir-name> <sha256>
    local url="$1" name="$2" sum="$3"
    if [ ! -d "$SRC/$name" ]; then
        echo "==> downloading $name"
        curl -fL --retry 3 -o "$SRC/$name.tar" "$url"
        echo "$sum  $SRC/$name.tar" | sha256sum -c -
        mkdir -p "$SRC/$name"
        tar -xf "$SRC/$name.tar" -C "$SRC/$name" --strip-components=1
        rm -f "$SRC/$name.tar"
    fi
}

built() { [ -f "$PREFIX/.built-$1" ]; }
mark()  { touch "$PREFIX/.built-$1"; }

# --- nasm (assembler; only built if the system lacks nasm/yasm) -------------
if ! built nasm && ! command -v nasm >/dev/null && ! command -v yasm >/dev/null; then
    fetch "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VERSION/nasm-$NASM_VERSION.tar.xz" nasm "$NASM_SHA256"
    echo "==> building nasm"
    (cd "$SRC/nasm" && ./configure --prefix="$PREFIX" && make -j"$JOBS" nasm && \
        install -Dm755 nasm "$PREFIX/bin/nasm")
    mark nasm
fi

# --- libopus ----------------------------------------------------------------
if ! built opus; then
    fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz" opus "$OPUS_SHA256"
    echo "==> building libopus"
    (cd "$SRC/opus" && \
        CFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
            --enable-static --disable-shared --disable-doc --disable-extra-programs && \
        make -j"$JOBS" && make install)
    mark opus
fi

# --- libvpx (VP9 encoder only; decoding uses FFmpeg's native decoders) ------
if ! built vpx; then
    fetch "https://github.com/webmproject/libvpx/archive/refs/tags/v$VPX_VERSION.tar.gz" vpx "$VPX_SHA256"
    echo "==> building libvpx"
    (cd "$SRC/vpx" && \
        ./configure --prefix="$PREFIX" \
            --enable-static --disable-shared --enable-pic \
            --disable-vp8 --disable-vp9-decoder \
            --disable-examples --disable-tools --disable-docs --disable-unit-tests \
            --disable-libyuv --disable-webm-io \
            --extra-cflags="$SECTION_CFLAGS" && \
        make -j"$JOBS" && make install)
    mark vpx
fi

# --- dav1d (AV1 decoder — FFmpeg has no native software AV1 decoding) -------
if ! built dav1d; then
    fetch "https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.xz" dav1d "$DAV1D_SHA256"
    echo "==> building dav1d"
    (cd "$SRC/dav1d" && \
        meson setup build --prefix="$PREFIX" --libdir=lib \
            --buildtype=release --default-library=static \
            -Denable_tools=false -Denable_tests=false \
            -Dc_args="$SECTION_CFLAGS" && \
        ninja -C build && ninja -C build install)
    mark dav1d
fi

# --- libaom (AV1 encoder only; decoding stays on dav1d). HIGHBITDEPTH=0
#     drops the unused 10/12-bit encode paths (~2 MB of binary): webmify only
#     ever feeds it 8-bit yuv420p (SDR after tonemapping). Needs yasm — the
#     aom cmake probe rejects Alpine's nasm 3.x. ------------------------------
if ! built aom; then
    fetch "https://storage.googleapis.com/aom-releases/libaom-$AOM_VERSION.tar.gz" aom "$AOM_SHA256"
    echo "==> building libaom"
    (mkdir -p "$SRC/aom-build" && cd "$SRC/aom-build" && \
        cmake "$SRC/aom" \
            -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_SHARED_LIBS=0 \
            -DCONFIG_AV1_DECODER=0 -DCONFIG_AV1_HIGHBITDEPTH=0 \
            -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0 -DENABLE_DOCS=0 \
            -DCMAKE_C_FLAGS="-O2 -fPIC $SECTION_CFLAGS" && \
        make -j"$JOBS" && make install)
    mark aom
fi

# --- libwebp (official WebP encoder; libwebpmux is needed by libwebp_anim;
#     decoding uses FFmpeg's native webp decoder) ----------------------------
if ! built webp; then
    fetch "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$WEBP_VERSION.tar.gz" webp "$WEBP_SHA256"
    echo "==> building libwebp"
    (cd "$SRC/webp" && \
        CFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
            --enable-static --disable-shared \
            --enable-libwebpmux --disable-libwebpdemux \
            --disable-gl --disable-sdl --disable-png --disable-jpeg \
            --disable-tiff --disable-gif --disable-wic && \
        make -j"$JOBS" && make install)
    mark webp
fi

# --- zimg (colorspace engine for the zscale filter: HDR -> SDR tonemapping;
#     its only git submodule is googletest, so the release tarball builds) ----
if ! built zimg; then
    fetch "https://github.com/sekrit-twc/zimg/archive/refs/tags/release-$ZIMG_VERSION.tar.gz" zimg "$ZIMG_SHA256"
    echo "==> building zimg"
    (cd "$SRC/zimg" && ./autogen.sh && \
        CXXFLAGS="-O2 -fPIC $SECTION_CFLAGS" ./configure --prefix="$PREFIX" \
            --enable-static --disable-shared && \
        make -j"$JOBS" && make install)
    mark zimg
fi

# --- FFmpeg -----------------------------------------------------------------
if ! built ffmpeg; then
    fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" ffmpeg "$FFMPEG_SHA256"
    if [ ! -f "$SRC/ffmpeg/.patched" ]; then
        for p in "$ROOT"/patches/*.patch; do
            echo "==> applying $(basename "$p")"
            patch -d "$SRC/ffmpeg" -p1 < "$p"
        done
        touch "$SRC/ffmpeg/.patched"
    fi
    echo "==> building ffmpeg"
    (cd "$SRC/ffmpeg" && \
        ./configure \
            --prefix="$PREFIX" \
            --pkg-config-flags=--static \
            --extra-cflags="-fPIC $SECTION_CFLAGS" \
            --enable-static --disable-shared --enable-pic --enable-small \
            --disable-programs --disable-doc --disable-avdevice \
            --disable-network --disable-autodetect --enable-pthreads --enable-zlib \
            --disable-debug \
            --disable-everything \
            --enable-libvpx --enable-libopus --enable-libdav1d --enable-libwebp \
            --enable-libzimg --enable-libaom \
            --enable-protocol=file,pipe \
            --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,asf,ogg,wav,mp3,aac,flac \
            --enable-demuxer=image2,gif,image_bmp_pipe,image_jpeg_pipe,image_png_pipe,image_tiff_pipe,image_webp_pipe \
            --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,wmv3,vc1,h263,flv,vp6,vp6f,theora,mjpeg \
            --enable-decoder=aac,aac_latm,ac3,eac3,mp2,mp3,opus,vorbis,flac,alac,wmav1,wmav2,wmapro,dca,truehd,adpcm_ms,adpcm_ima_wav \
            --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_u8,pcm_f32le,pcm_alaw,pcm_mulaw \
            --enable-decoder=png,gif,bmp,tiff,webp \
            --enable-encoder=libvpx_vp9,libopus,libwebp,libwebp_anim,libaom_av1 \
            --enable-muxer=webm,webp,avif \
            --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,av1,vc1,mjpeg,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca \
            --enable-parser=png,bmp,gif,webp \
            --enable-bsf=extract_extradata,vp9_superframe \
            --enable-filter=buffer,buffersink,abuffer,abuffersink,scale,format,aformat,aresample,transpose,hflip,vflip,setsar,anull,null,zscale,tonemap && \
        make -j"$JOBS" && make install)
    mark ffmpeg
fi

echo
echo "==> vendored static libs:"
ls -lh "$PREFIX"/lib/*.a
