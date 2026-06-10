#!/usr/bin/env bash
# Smoke test for dist/webmify: exercises the externally observable claims
# (CLI contract, cwebp byte-identity, -q size ordering, --max scaling,
# rotation baking, mono audio, VP9/Opus codecs, faststart cues, stdin/stdout).
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

ff()     { ffmpeg -hide_banner -loglevel error -y "$@"; }
dims()   { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$1"; }
codecs() { ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$1" | paste -sd+; }
size()   { stat -c%s "$1"; }
# Matroska Cues (1C53BB6B) must precede the first Cluster (1F43B675)
cues_at_head() {
    python3 -c 'import sys
d = open(sys.argv[1], "rb").read()
c = d.find(bytes.fromhex("1C53BB6B")); k = d.find(bytes.fromhex("1F43B675"))
sys.exit(0 if 0 <= c < k else 1)' "$1"
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

# --- CLI contract --------------------------------------------------------------
t "--help exits 0 and prints usage"        bash -c "'$WEBMIFY' --help | grep -q usage"
t "--version names webmify and FFmpeg"     bash -c "'$WEBMIFY' --version | grep -q 'webmify .*FFmpeg'"
t "-q 11 (out of range) rejected"          rejects -q 11
t "-q 60 (old 0-100 scale) rejected"       rejects -q 60
t "--max bogus rejected"                   rejects --max bogus
t "--fast with --best rejected"            rejects --fast --best

# --- images --------------------------------------------------------------------
"$WEBMIFY" photo.png  q_def.webp
"$WEBMIFY" -q 8   photo.png q8.webp
"$WEBMIFY" -q 2   photo.png q2.webp
"$WEBMIFY" -q 9.5 photo.png q95.webp
"$WEBMIFY" -m 240  photo.png m240.webp
"$WEBMIFY" -m 2000 photo.png m2000.webp
"$WEBMIFY" - - < photo.png > piped.webp
cwebp -quiet -q 80 -m 6 -sharp_yuv photo.png -o ref80.webp

t "image: default == -q 8 byte-identical"             cmp -s q_def.webp q8.webp
t "image: -q 8 == cwebp -q 80 -m 6 -sharp_yuv"        cmp -s q8.webp ref80.webp
t "image: -q 2 smaller than -q 8"                     lt "$(size q2.webp)" "$(size q8.webp)"
t "image: -q 8 smaller than -q 9.5"                   lt "$(size q8.webp)" "$(size q95.webp)"
t "image: --max 240 fits the box (640x480 -> 240x180)" eq "$(dims m240.webp)" "240,180"
t "image: never upscaled (--max 2000)"                eq "$(dims m2000.webp)" "640,480"
t "image: stdin -> stdout pipe works"                 cmp -s piped.webp q_def.webp

# --- video ---------------------------------------------------------------------
"$WEBMIFY" tv.mp4 v_def.webm
"$WEBMIFY" -q 2 tv.mp4 v_q2.webm
"$WEBMIFY" -q 9 tv.mp4 v_q9.webm
"$WEBMIFY" rot.mp4 v_rot.webm
"$WEBMIFY" - - < tv.mkv > v_piped.webm
"$WEBMIFY" frag.mp4 v_frag.webm

t "video: VP9 + Opus"                                 eq "$(codecs v_def.webm)" "vp9+opus"
t "video: 720p source fits the default 480 box"       eq "$(dims v_def.webm)" "854,480"
t "video: mono source stays mono"                     eq "$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 v_def.webm)" "1"
t "video: -q 2 smaller than -q 9"                     lt "$(size v_q2.webm)" "$(size v_q9.webm)"
t "video: display-matrix rotation baked in"           eq "$(dims v_rot.webm)" "270,480"
t "video: seek cues at the head (faststart)"          cues_at_head v_def.webm
t "video: stdin -> stdout pipe (single-pass) works"   eq "$(codecs v_piped.webm)" "vp9+opus"
t "video: muted fragmented mp4 stays video"           eq "$(codecs v_frag.webm)" "vp9"
t "video: crafted mp4 prefix does not hang the probe" bash -c "timeout 5 '$WEBMIFY' - - < evil.mp4 > /dev/null 2>&1; [ \$? -ne 124 ]"

echo
[ "$fail" -eq 0 ] && echo "all $pass tests passed" || { echo "$fail of $((pass+fail)) tests FAILED"; exit 1; }
