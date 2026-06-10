# syntax=docker/dockerfile:1
# Builds `webmify` as a fully static musl binary. Nothing is installed on the
# host: ./build.sh runs this and exports the binary to ./dist/webmify.

FROM alpine:3 AS build

RUN apk add --no-cache \
    autoconf automake libtool \
    bash build-base coreutils curl diffutils meson nasm ninja perl pkgconf tar xz \
    zlib-dev zlib-static

WORKDIR /build

# Heavy, rarely-changing layer: minimal static ffmpeg + libvpx + libopus +
# dav1d + zimg, with the tiny local patches from patches/ applied.
COPY vendor.sh ./
COPY patches ./patches
RUN ./vendor.sh

COPY src ./src
ENV PKG_CONFIG_PATH=/build/vendor/out/lib/pkgconfig
RUN libs="libavfilter libavformat libavcodec libswscale libswresample libavutil" && \
    g++ -Os -static -ffunction-sections -fdata-sections \
        $(pkg-config --cflags $libs) \
        src/webmify.cpp -o /webmify \
        $(pkg-config --libs --static $libs) \
        -Wl,--gc-sections -s && \
    ls -lh /webmify

# Optional: UPX=1 ./build.sh compresses the binary (~60% smaller, slower start).
# (named COMPRESS because upx itself reads the UPX env var as an options string)
ARG COMPRESS=0
RUN if [ "$COMPRESS" = "1" ]; then apk add --no-cache upx && upx --best --lzma /webmify; fi

FROM scratch AS dist
COPY --from=build /webmify /webmify
