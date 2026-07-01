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
# (cmake + yasm are for libaom: its cmake nasm probe rejects nasm 3.x;
# linux-headers for zimg's arm64 cpu detection: asm/hwcap.h)
FROM alpine:3.24 AS base

RUN apk add --no-cache \
    autoconf automake libtool \
    bash build-base cmake coreutils curl diffutils linux-headers meson nasm ninja perl pkgconf tar xz yasm \
    zlib-dev zlib-static

WORKDIR /build
COPY vendor.d/common.sh vendor.d/

FROM base AS dav1d
COPY vendor.d/30-dav1d.sh vendor.d/
RUN vendor.d/30-dav1d.sh

FROM base AS aom
COPY vendor.d/40-aom.sh vendor.d/
RUN vendor.d/40-aom.sh

FROM base AS x264
COPY vendor.d/60-x264.sh vendor.d/
RUN vendor.d/60-x264.sh

FROM base AS zimg
COPY vendor.d/70-zimg.sh vendor.d/
RUN vendor.d/70-zimg.sh

# libmagic (file): linked directly into webify, not through ffmpeg
FROM base AS libmagic
COPY vendor.d/50-libmagic.sh vendor.d/
RUN vendor.d/50-libmagic.sh

# ffmpeg links all of the above, so it re-runs when any library changed
FROM base AS ffmpeg
COPY --from=dav1d /build/vendor/out vendor/out
COPY --from=aom   /build/vendor/out vendor/out
COPY --from=x264  /build/vendor/out vendor/out
COPY --from=zimg  /build/vendor/out vendor/out
COPY vendor.d/80-ffmpeg.sh vendor.d/
RUN vendor.d/80-ffmpeg.sh

FROM base AS build
COPY --from=ffmpeg   /build/vendor/out vendor/out
COPY --from=libmagic /build/vendor/out vendor/out
COPY src ./src
ENV PKG_CONFIG_PATH=/build/vendor/out/lib/pkgconfig
# the version --version reports: CI passes the release tag on tag builds,
# everything else self-identifies as a dev build
ARG VERSION=dev
RUN libs="libavfilter libavformat libavcodec libswscale libswresample libavutil" && \
    # embed the compiled libmagic database as an object (symbols
    # _binary_magic_mgc_{start,end}) so the binary needs no external magic file
    cp vendor/out/share/misc/magic.mgc magic.mgc && \
    ld -r -b binary magic.mgc -o magic_mgc.o && \
    # -no-pie: drop the static-PIE self-relocation table (musl/gcc default
    # to PIE); the vendored libs are --enable-pic so they link clean. ~3% smaller.
    # libmagic + zlib link directly (no .pc): -I/-L vendor/out, -lmagic, -lz.
    g++ -Os -static -no-pie -fno-pie -Wall -Wextra -ffunction-sections -fdata-sections \
        -DWEBIFY_VERSION="\"$VERSION\"" \
        $(pkg-config --cflags $libs) -Ivendor/out/include \
        src/webify.cpp magic_mgc.o -o /webify \
        $(pkg-config --libs --static $libs) -Lvendor/out/lib -lmagic -lz \
        -Wl,--gc-sections -s && \
    ls -lh /webify

# Optional: UPX=1 ./build.sh compresses the binary (~60% smaller, slower start).
# (named COMPRESS because upx itself reads the UPX env var as an options string)
ARG COMPRESS=0
RUN if [ "$COMPRESS" = "1" ]; then apk add --no-cache upx && upx --best --lzma /webify; fi

# libFuzzer build of the demux + first-frame decode path (the --peek CVE
# surface), compiled with clang against the same vendored static libs. Not part
# of the default build — fuzz.yml (weekly) builds `fuzz-bin` and runs it. Dynamic
# (not -static): the libFuzzer/compiler-rt runtime needs it. -Wno-unused-function
# because main() and the encode path are #ifdef'd out under WEBIFY_FUZZER.
FROM ffmpeg AS fuzz
RUN apk add --no-cache clang compiler-rt
COPY --from=libmagic /build/vendor/out vendor/out
COPY src ./src
ENV PKG_CONFIG_PATH=/build/vendor/out/lib/pkgconfig
RUN libs="libavfilter libavformat libavcodec libswscale libswresample libavutil" && \
    cp vendor/out/share/misc/magic.mgc magic.mgc && \
    ld -r -b binary magic.mgc -o magic_mgc.o && \
    clang++ -g -O1 -DWEBIFY_FUZZER -fsanitize=fuzzer -Wno-unused-function \
        $(pkg-config --cflags $libs) -Ivendor/out/include \
        src/webify.cpp magic_mgc.o -o /webify_fuzz \
        $(pkg-config --libs --static $libs) -Lvendor/out/lib -lmagic -lz && \
    ls -lh /webify_fuzz

FROM scratch AS fuzz-bin
COPY --from=fuzz /webify_fuzz /webify_fuzz

# Hermetic behavioral + golden test image. ffmpeg/ffprobe are pinned by digest
# (mwader/static-ffmpeg, multi-arch index) so the generated fixtures — and thus
# the golden hashes — are byte-stable and independent of the host toolchain.
# `docker run` it to gate a build (WEBIFY_GOLDEN=1 enforces the hashes); run with
# `-e REBASELINE=1 -v "$PWD/goldens:/src/goldens"` to refresh them on the host.
FROM alpine:3.24 AS test
RUN apk add --no-cache bash coreutils findutils grep gawk sed python3
COPY --from=mwader/static-ffmpeg:8.0@sha256:415a41fa3167b890b9703d20bd0f00bf1e9dab8a4b6c27fef1445b2bf5f1ab4a \
     /ffmpeg /ffprobe /usr/local/bin/
COPY --from=build /webify /usr/local/bin/webify
WORKDIR /src
COPY test.sh ./
COPY goldens ./goldens
ENV WEBIFY=/usr/local/bin/webify WEBIFY_GOLDEN=1
ENTRYPOINT ["bash", "./test.sh"]

FROM scratch AS dist
COPY --from=build /webify /webify
