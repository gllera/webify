#!/usr/bin/env bash
# Smoke test for dist/webmify: exercises the externally observable claims
# (CLI contract, cwebp byte-identity, -q size ordering, --max scaling,
# rotation baking, mono audio, VP9/Opus codecs, faststart cues, stdin/stdout,
# --next: AV1/Opus WebM video, AVIF images w/ alpha + animated GIF -> AVIF,
# --legacy: H.264/AAC MP4 video w/ faststart, lossless PNG images w/ alpha +
# animated GIF -> APNG).
# Fixtures are generated on the host: needs ffmpeg, ffprobe, cwebp, python3.
#
#   ./build.sh && ./test.sh
set -euo pipefail
cd "$(dirname "$0")"

WEBMIFY="${WEBMIFY:-$PWD/dist/webmify}"
for tool in ffmpeg ffprobe cwebp python3; do
    command -v "$tool" >/dev/null || { echo "missing host tool: $tool"; exit 1; }
done
[ -x "$WEBMIFY" ] || { echo "missing $WEBMIFY — run ./build.sh first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

pass=0 fail=0
t() { # <description> <command...> — command's exit code decides pass/fail
    local d="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "ok   - $d"; pass=$((pass+1))
    else                          echo "FAIL - $d"; fail=$((fail+1)); fi
}
eq()     { [ "$1" = "$2" ]; }
lt()     { [ "$1" -lt "$2" ]; }
rejects() { ! "$WEBMIFY" "$@" in out 2>/dev/null; }

ff()       { ffmpeg -hide_banner -loglevel error -y "$@"; }
dims()     { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$1"; }
codecs()   { ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$1" | paste -sd+; }
channels() { ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$1"; }
trc()      { ffprobe -v error -select_streams v:0 -show_entries stream=color_transfer -of csv=p=0 "$1"; }
pixfmt()   { ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$1"; }
size()     { stat -c%s "$1"; }
# Matroska Cues (1C53BB6B) must precede the first Cluster (1F43B675),
# walking real EBML elements — a flat byte search always "finds" the Cues
# at the head, inside the SeekHead's SeekID, which stores that same ID
cues_at_head() {
    python3 -c 'import sys
d = open(sys.argv[1], "rb").read()
def vint(i, strip):                      # EBML varint at i -> (value, size)
    n = 8 - d[i].bit_length() + 1        # length = leading zero bits + 1
    v = d[i] & (0xFF >> n) if strip else d[i]
    for j in range(1, n):
        v = v << 8 | d[i + j]
    return v, n
def children(i, end):                    # yield (id, payload off, payload size)
    while i < end:
        eid, n = vint(i, False); i += n
        size, n = vint(i, True)
        if size == (1 << (7 * n)) - 1:   # "unknown size": runs to EOF
            size = len(d) - i - n
        i += n
        yield eid, i, size
        i += size
seg = next((o, s) for e, o, s in children(0, len(d)) if e == 0x18538067)
for eid, off, size in children(seg[0], seg[0] + seg[1]):
    if eid == 0x1C53BB6B: sys.exit(0)    # Cues before any Cluster
    if eid == 0x1F43B675: sys.exit(1)    # Cluster first: cues at the tail
sys.exit(1)' "$1"
}
# MP4 faststart: the moov atom must precede the mdat atom
moov_at_head() {
    python3 -c 'import sys
d = open(sys.argv[1], "rb").read()
m = d.find(b"moov"); k = d.find(b"mdat")
sys.exit(0 if 0 <= m < k else 1)' "$1"
}
# the decoded pixels of two images, compared exactly (lossless check)
same_pixels() {
    md() { ffmpeg -v error -i "$1" -f framemd5 - | tail -1; }
    [ "$(md "$1")" = "$(md "$2")" ]
}

# --- fixtures ----------------------------------------------------------------
ff -f lavfi -i "testsrc2=size=640x480:duration=1:rate=1" -frames:v 1 photo.png
ff -f lavfi -i "testsrc2=size=1280x720:duration=2:rate=30" \
   -f lavfi -i "sine=frequency=440:duration=2" \
   -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 1 -shortest tv.mp4
ff -i tv.mp4 -c copy tv.mkv                       # mkv streams when piped (mp4 would spool)
ff -display_rotation 90 -i tv.mp4 -c copy rot.mp4 # portrait via display matrix
ff -f lavfi -i "testsrc2=size=320x240:duration=1:rate=30" \
   -c:v libx264 -pix_fmt yuv420p -movflags +frag_keyframe+empty_moov frag.mp4 # muted, nb_frames unknown
python3 - > evil.mp4 <<'EOF'                      # 64-bit atom size that would wrap the moov scan
import struct, sys
out  = b"\x00\x00\x00\x10ftypisom\x00\x00\x02\x00"
out += b"\x00\x00\x00\x01free" + struct.pack(">Q", (1 << 64) - 8)
sys.stdout.buffer.write(out + b"\x00" * 4096)
EOF
ff -f lavfi -i "testsrc2=size=320x240:duration=1:rate=30" \
   -f lavfi -i "sine=frequency=440:duration=1" \
   -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest tiny.mp4    # small clip for the slow tiers
ff -f lavfi -i "testsrc2=size=320x240:duration=1:rate=30" \
   -f lavfi -i "sine=frequency=440:duration=1" \
   -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 2 -shortest stereo.mp4
ff -f lavfi -i "sine=frequency=440:duration=1" audio.wav        # no video stream at all
ff -f lavfi -i "testsrc2=size=200x150:duration=1:rate=5" anim.gif
ff -f lavfi -i "color=c=red:size=320x240:duration=1:rate=1" -frames:v 1 flat.png
ff -f lavfi -i "color=c=red@0.5:size=320x240:rate=1,format=rgba" -frames:v 1 alpha.png
ff -f lavfi -i "color=c=red:size=320x240:rate=1,format=rgba" -frames:v 1 opaque.png # alpha channel, all 0xFF
ff -f lavfi -i "testsrc2=size=640x480:duration=1:rate=30" -c:v libx264 -pix_fmt yuv420p \
   -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc hdr.mp4 # PQ-tagged HDR
ff -f lavfi -i "testsrc2=size=640x480:duration=1:rate=1" -frames:v 1 -q:v 3 plain.jpg
python3 - <<'EOF'                                 # plain.jpg + EXIF Orientation=6 -> exif.jpg
import struct
jpg   = open("plain.jpg", "rb").read()
tiff  = b"II*\x00\x08\x00\x00\x00" + struct.pack("<H", 1)
tiff += struct.pack("<HHI", 0x0112, 3, 1) + struct.pack("<HH", 6, 0)  # rotate 90 cw
tiff += struct.pack("<I", 0)
exif  = b"Exif\x00\x00" + tiff
open("exif.jpg", "wb").write(jpg[:2] + b"\xff\xe1" +
                             struct.pack(">H", len(exif) + 2) + exif + jpg[2:])
EOF

# --- CLI contract --------------------------------------------------------------
t "--help exits 0 and prints usage"        bash -c "'$WEBMIFY' --help | grep -q usage"
t "--version names webmify and FFmpeg"     bash -c "'$WEBMIFY' --version | grep -q 'webmify .*FFmpeg'"
t "-q 11 (out of range) rejected"          rejects -q 11
t "-q 60 (old 0-100 scale) rejected"       rejects -q 60
t "--max bogus rejected"                   rejects --max bogus
t "--fast with --best rejected"            rejects --fast --best
t "audio-only input rejected"              bash -c "! '$WEBMIFY' audio.wav x.webm 2>/dev/null"

# --- images --------------------------------------------------------------------
"$WEBMIFY" photo.png  q_def.webp
"$WEBMIFY" -q 8   photo.png q8.webp
"$WEBMIFY" -q 2   photo.png q2.webp
"$WEBMIFY" -q 9.5 photo.png q95.webp
"$WEBMIFY" -m 240  photo.png m240.webp
"$WEBMIFY" -m 2000 photo.png m2000.webp
"$WEBMIFY" - - < photo.png > piped.webp
"$WEBMIFY" --fast photo.png q_fast.webp
"$WEBMIFY" flat.png flat.webp
"$WEBMIFY" anim.gif anim.webp
"$WEBMIFY" exif.jpg exif.webp
cwebp -quiet -q 80 -m 6 -sharp_yuv photo.png -o ref80.webp

t "image: default == -q 8 byte-identical"             cmp -s q_def.webp q8.webp
t "image: -q 8 == cwebp -q 80 -m 6 -sharp_yuv"        cmp -s q8.webp ref80.webp
t "image: -q 2 smaller than -q 8"                     lt "$(size q2.webp)" "$(size q8.webp)"
t "image: -q 8 smaller than -q 9.5"                   lt "$(size q8.webp)" "$(size q95.webp)"
t "image: --max 240 fits the box (640x480 -> 240x180)" eq "$(dims m240.webp)" "240,180"
t "image: never upscaled (--max 2000)"                eq "$(dims m2000.webp)" "640,480"
t "image: stdin -> stdout pipe works"                 cmp -s piped.webp q_def.webp
t "image: --fast tier produces a WebP"                grep -aq WEBP q_fast.webp
t "image: lossless wins the race on flat graphics"    grep -aq VP8L flat.webp
t "image: animated gif -> animated WebP (ANIM chunk)" grep -aq ANIM anim.webp
t "image: EXIF orientation baked in (-> 480x640)"     eq "$(dims exif.webp)" "480,640"

# --- video ---------------------------------------------------------------------
"$WEBMIFY" tv.mp4 v_def.webm
"$WEBMIFY" -q 2 tv.mp4 v_q2.webm
"$WEBMIFY" -q 9 tv.mp4 v_q9.webm
"$WEBMIFY" rot.mp4 v_rot.webm
"$WEBMIFY" - - < tv.mkv > v_piped.webm
"$WEBMIFY" frag.mp4 v_frag.webm
"$WEBMIFY" stereo.mp4 v_stereo.webm
"$WEBMIFY" hdr.mp4 v_hdr.webm
"$WEBMIFY" --fast tiny.mp4 v_fast.webm
"$WEBMIFY" --best tiny.mp4 v_best.webm

t "video: VP9 + Opus"                                 eq "$(codecs v_def.webm)" "vp9+opus"
t "video: 720p source fits the default 480 box"       eq "$(dims v_def.webm)" "854,480"
t "video: mono source stays mono"                     eq "$(channels v_def.webm)" "1"
t "video: stereo source stays stereo"                 eq "$(channels v_stereo.webm)" "2"
t "video: HDR (PQ) tonemapped to SDR bt709"           eq "$(trc v_hdr.webm)" "bt709"
t "video: --fast tier produces VP9+Opus"              eq "$(codecs v_fast.webm)" "vp9+opus"
t "video: --best tier produces VP9+Opus"              eq "$(codecs v_best.webm)" "vp9+opus"
t "video: -q 2 smaller than -q 9"                     lt "$(size v_q2.webm)" "$(size v_q9.webm)"
t "video: display-matrix rotation baked in"           eq "$(dims v_rot.webm)" "270,480"
t "video: seek cues at the head (faststart)"          cues_at_head v_def.webm
t "video: stdin -> stdout pipe (single-pass) works"   eq "$(codecs v_piped.webm)" "vp9+opus"
t "video: muted fragmented mp4 stays video"           eq "$(codecs v_frag.webm)" "vp9"
t "video: crafted mp4 prefix does not hang the probe" bash -c "timeout 5 '$WEBMIFY' - - < evil.mp4 > /dev/null 2>&1; [ \$? -ne 124 ]"

# --- --next: AV1/Opus WebM video, AVIF still images -------------------------------
"$WEBMIFY" --next tv.mp4 a_tv.webm
"$WEBMIFY" --next hdr.mp4 a_hdr.webm
"$WEBMIFY" --next anim.gif a_anim.avif
"$WEBMIFY" --next - - < tv.mkv > a_piped.webm
"$WEBMIFY" --next --fast tiny.mp4 a_fast.webm
"$WEBMIFY" --next --best tiny.mp4 a_best.webm
"$WEBMIFY" --next photo.png a_q.avif
"$WEBMIFY" --next -q 2 photo.png a_q2.avif
"$WEBMIFY" --next -q 9 photo.png a_q9.avif
"$WEBMIFY" --next alpha.png a_alpha.avif
"$WEBMIFY" --next opaque.png a_opaque.avif
"$WEBMIFY" --next exif.jpg a_exif.avif
"$WEBMIFY" --next - - < photo.png > a_piped.avif

t "next: video becomes AV1 + Opus"                     eq "$(codecs a_tv.webm)" "av1+opus"
t "next: 720p source fits the default 480 box"         eq "$(dims a_tv.webm)" "854,480"
t "next: smaller than the VP9 default at the same -q"  lt "$(size a_tv.webm)" "$(size v_def.webm)"
t "next: seek cues at the head (faststart)"            cues_at_head a_tv.webm
t "next: HDR (PQ) tonemapped to SDR bt709"             eq "$(trc a_hdr.webm)" "bt709"
t "next: animated gif -> animated AVIF (avis brand)"   grep -aq ftypavis a_anim.avif
t "next: stdin -> stdout video pipe works"             eq "$(codecs a_piped.webm)" "av1+opus"
t "next: --fast tier produces AV1+Opus"                eq "$(codecs a_fast.webm)" "av1+opus"
t "next: --best tier produces AV1+Opus"                eq "$(codecs a_best.webm)" "av1+opus"
t "next: still image -> AVIF (ftyp brand)"             grep -aq ftypavif a_q.avif
t "next: -q 2 smaller than -q 9"                       lt "$(size a_q2.avif)" "$(size a_q9.avif)"
t "next: EXIF orientation baked in (-> 480x640)"       eq "$(dims a_exif.avif)" "480,640"
t "next: alpha kept (auxiliary alpha stream)"          grep -aq "auxiliary:alpha" a_alpha.avif
t "next: fully opaque alpha channel dropped"           bash -c "! grep -aq 'auxiliary:alpha' a_opaque.avif"
t "next: stdin -> stdout still is a valid AVIF"        grep -aq ftypavif a_piped.avif
t "next: default still stays 4:2:0"                    eq "$(pixfmt a_q.avif)" "yuv420p"
t "next: premium -q RGB graphics race to 4:4:4"        eq "$(pixfmt a_q9.avif)" "yuv444p"
t "next: animated AVIF smaller than animated WebP"     lt "$(size a_anim.avif)" "$(size anim.webp)"

# --- --legacy: H.264/AAC MP4 video, PNG/APNG images ----------------------------
"$WEBMIFY" --legacy tv.mp4 l_tv.mp4
"$WEBMIFY" --legacy -q 2 tv.mp4 l_q2.mp4
"$WEBMIFY" --legacy -q 9 tv.mp4 l_q9.mp4
"$WEBMIFY" --legacy rot.mp4 l_rot.mp4
"$WEBMIFY" --legacy hdr.mp4 l_hdr.mp4
"$WEBMIFY" --legacy - - < tv.mkv > l_piped.mp4
"$WEBMIFY" --legacy --fast tiny.mp4 l_fast.mp4
"$WEBMIFY" --legacy --best tiny.mp4 l_best.mp4
"$WEBMIFY" --legacy photo.png l_q.png
"$WEBMIFY" --legacy -q 2 photo.png l_q2.png
"$WEBMIFY" --legacy plain.jpg l_jpg.png
"$WEBMIFY" --legacy alpha.png l_alpha.png
"$WEBMIFY" --legacy opaque.png l_opaque.png
"$WEBMIFY" --legacy exif.jpg l_exif.png
"$WEBMIFY" --legacy anim.gif l_anim.png
"$WEBMIFY" --legacy - - < photo.png > l_piped.png
"$WEBMIFY" --legacy - - < anim.gif > l_piped_anim.png

t "legacy: --next with --legacy rejected"              rejects --next --legacy
t "legacy: video becomes H.264 + AAC"                  eq "$(codecs l_tv.mp4)" "h264+aac"
t "legacy: 720p source fits the default 480 box"       eq "$(dims l_tv.mp4)" "854,480"
t "legacy: mono source stays mono"                     eq "$(channels l_tv.mp4)" "1"
t "legacy: moov at the head (faststart)"               moov_at_head l_tv.mp4
t "legacy: -q 2 smaller than -q 9"                     lt "$(size l_q2.mp4)" "$(size l_q9.mp4)"
t "legacy: display-matrix rotation baked in"           eq "$(dims l_rot.mp4)" "270,480"
t "legacy: HDR (PQ) tonemapped to SDR bt709"           eq "$(trc l_hdr.mp4)" "bt709"
t "legacy: stdin -> stdout video pipe (fragmented)"    eq "$(codecs l_piped.mp4)" "h264+aac"
t "legacy: --fast tier produces H.264+AAC"             eq "$(codecs l_fast.mp4)" "h264+aac"
t "legacy: --best tier produces H.264+AAC"             eq "$(codecs l_best.mp4)" "h264+aac"
t "legacy: still image -> PNG"                         eq "$(codecs l_q.png)" "png"
t "legacy: PNG is pixel-lossless vs the source"        same_pixels l_q.png photo.png
t "legacy: -q does not change a PNG (always lossless)" cmp -s l_q.png l_q2.png
t "legacy: YUV source (JPEG) comes out plain rgb24"    eq "$(pixfmt l_jpg.png)" "rgb24"
t "legacy: real alpha kept (rgba PNG)"                 eq "$(pixfmt l_alpha.png)" "rgba"
t "legacy: fully opaque alpha channel dropped"         eq "$(pixfmt l_opaque.png)" "rgb24"
t "legacy: EXIF orientation baked in (-> 480x640)"     eq "$(dims l_exif.png)" "480,640"
t "legacy: animated gif -> APNG (acTL chunk)"          grep -aq acTL l_anim.png
t "legacy: stdin -> stdout still is a PNG"             eq "$(codecs l_piped.png)" "png"
t "legacy: piped APNG keeps its frames (acTL intact)"  grep -aq acTL l_piped_anim.png

echo
[ "$fail" -eq 0 ] && echo "all $pass tests passed" || { echo "$fail of $((pass+fail)) tests FAILED"; exit 1; }
