# syntax=docker/dockerfile:1
# Builds `webify` as a fully static musl binary. Nothing is installed on the
# host: ./build.sh runs this and exports the binary to ./dist/webify.
#
# Every vendored library gets its own stage, and each stage COPYs only that
# library's vendor.d script — so its cache key is exactly that script's
# content. Bumping one library's version/flags recompiles that library plus
# the ffmpeg stage that links it; every other library stays cached. BuildKit
# also builds the independent library stages in parallel.

# pinned minor so the toolchain doesn't drift between builds
# (cmake + yasm are for libaom: its cmake nasm probe rejects nasm 3.x)
FROM alpine:3.24 AS base

RUN apk add --no-cache \
    autoconf automake libtool \
    bash build-base cmake coreutils curl diffutils meson nasm ninja perl pkgconf tar xz yasm \
    zlib-dev zlib-static

WORKDIR /build
COPY vendor.d/common.sh vendor.d/

FROM base AS opus
COPY vendor.d/10-opus.sh vendor.d/
RUN vendor.d/10-opus.sh

FROM base AS vpx
COPY vendor.d/20-vpx.sh vendor.d/
RUN vendor.d/20-vpx.sh

FROM base AS dav1d
COPY vendor.d/30-dav1d.sh vendor.d/
RUN vendor.d/30-dav1d.sh

FROM base AS aom
COPY vendor.d/40-aom.sh vendor.d/
RUN vendor.d/40-aom.sh

FROM base AS webp
COPY vendor.d/50-webp.sh vendor.d/
RUN vendor.d/50-webp.sh

FROM base AS x264
COPY vendor.d/60-x264.sh vendor.d/
RUN vendor.d/60-x264.sh

FROM base AS zimg
COPY vendor.d/70-zimg.sh vendor.d/
RUN vendor.d/70-zimg.sh

# ffmpeg links all of the above, so it re-runs when any library changed
FROM base AS ffmpeg
COPY --from=opus  /build/vendor/out vendor/out
COPY --from=vpx   /build/vendor/out vendor/out
COPY --from=dav1d /build/vendor/out vendor/out
COPY --from=aom   /build/vendor/out vendor/out
COPY --from=webp  /build/vendor/out vendor/out
COPY --from=x264  /build/vendor/out vendor/out
COPY --from=zimg  /build/vendor/out vendor/out
COPY patches ./patches
COPY vendor.d/80-ffmpeg.sh vendor.d/
RUN vendor.d/80-ffmpeg.sh

FROM base AS build
COPY --from=ffmpeg /build/vendor/out vendor/out
COPY src ./src
ENV PKG_CONFIG_PATH=/build/vendor/out/lib/pkgconfig
RUN libs="libavfilter libavformat libavcodec libswscale libswresample libavutil" && \
    g++ -Os -static -Wall -Wextra -ffunction-sections -fdata-sections \
        $(pkg-config --cflags $libs) \
        src/webify.cpp -o /webify \
        $(pkg-config --libs --static $libs) \
        -Wl,--gc-sections -s && \
    ls -lh /webify

# Optional: UPX=1 ./build.sh compresses the binary (~60% smaller, slower start).
# (named COMPRESS because upx itself reads the UPX env var as an options string)
ARG COMPRESS=0
RUN if [ "$COMPRESS" = "1" ]; then apk add --no-cache upx && upx --best --lzma /webify; fi

FROM scratch AS dist
COPY --from=build /webify /webify
