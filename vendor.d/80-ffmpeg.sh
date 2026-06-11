#!/usr/bin/env bash
# FFmpeg — built against every library above (the Docker stage for this
# script re-runs whenever any of them changed), with the minimal patches
# from patches/ applied (each one a few lines, reviewable in-repo).
#
# Only what `webify` needs is compiled in:
#   - demuxers/decoders to READ popular video files (mp4/mkv/webm/avi/flv/ts/wmv/ogg...)
#     and image files (jpeg/png/webp/gif/bmp/tiff/heic/avif)
#   - encoders libvpx-vp9 + libopus (webm muxer), libwebp (webp muxer),
#     libaom-av1 for --next (webm + avif muxers), x264 + native aac (mp4
#     muxer) + png/apng for --legacy
#   - the filters used by the fixed pipeline (scale, format, aformat, aresample,
#     transpose/hflip/vflip for phone-rotation metadata, zscale/tonemap for
#     HDR -> SDR conversion, bwdif for interlaced sources)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

FFMPEG_VERSION=8.1.1
# pinned from a verified-good download
FFMPEG_SHA256=b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3

built ffmpeg && exit 0

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
        --enable-gpl \
        --enable-libvpx --enable-libopus --enable-libdav1d --enable-libwebp \
        --enable-libzimg --enable-libaom --enable-libx264 \
        --enable-protocol=file,pipe \
        --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,asf,ogg,wav,mp3,aac,flac \
        --enable-demuxer=image2,gif,image_bmp_pipe,image_jpeg_pipe,image_png_pipe,image_tiff_pipe,image_webp_pipe \
        --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,wmv3,vc1,h263,flv,vp6,vp6f,theora,mjpeg \
        --enable-decoder=aac,aac_latm,ac3,eac3,mp2,mp3,opus,vorbis,flac,alac,wmav1,wmav2,wmapro,dca,truehd,adpcm_ms,adpcm_ima_wav \
        --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_u8,pcm_f32le,pcm_alaw,pcm_mulaw \
        --enable-decoder=png,gif,bmp,tiff,webp \
        --enable-encoder=libvpx_vp9,libopus,libwebp,libwebp_anim,libaom_av1 \
        --enable-encoder=libx264,aac,png,apng \
        --enable-muxer=webm,webp,avif,mp4,image2pipe,apng \
        --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,av1,vc1,mjpeg,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca \
        --enable-parser=png,bmp,gif,webp \
        --enable-bsf=extract_extradata,vp9_superframe \
        --enable-filter=buffer,buffersink,abuffer,abuffersink,scale,format,aformat,aresample,transpose,hflip,vflip,setsar,anull,null,zscale,tonemap,bwdif && \
    make -j"$JOBS" && make install)
mark ffmpeg
