#!/usr/bin/env bash
# Smoke test for dist/webify: exercises the externally observable claims of the
# simplified webify — video -> H.264/AAC MP4 (faststart, piped output too) and
# image/GIF -> AVIF (still + animated, alpha kept), the -q size ordering, the
# --max box, rotation/EXIF baking, mono audio, HDR tonemapping, interlaced
# sources deinterlaced, the dropped format/effort flags rejected, and
# stdin/stdout byte-identical to file i/o.
# Fixtures are generated on the host: needs ffmpeg, ffprobe, python3.
#
#   ./build.sh && ./test.sh
set -euo pipefail
cd "$(dirname "$0")"

WEBIFY="${WEBIFY:-$PWD/dist/webify}"
for tool in ffmpeg ffprobe python3; do
    command -v "$tool" >/dev/null || { echo "missing host tool: $tool"; exit 1; }
done
# the fixture recipes need host ffmpeg >= 6 (-display_rotation is a 6.0
# option); git/master builds report no leading number and skip the check
ffv=$(ffmpeg -version | sed -n '1s/^ffmpeg version n\{0,1\}\([0-9]\{1,\}\).*/\1/p')
if [ -n "$ffv" ] && [ "$ffv" -lt 6 ]; then
    echo "host ffmpeg is too old ($ffv): the fixture recipes need ffmpeg >= 6"
    exit 1
fi
[ -x "$WEBIFY" ] || { echo "missing $WEBIFY — run ./build.sh first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

pass=0 fail=0
t() { # <description> <command...> — command's exit code decides pass/fail
    local d="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "ok   - $d"; pass=$((pass+1))
    else                          echo "FAIL - $d"; fail=$((fail+1)); fi
}
eq()      { [ "$1" = "$2" ]; }
lt()      { [ "$1" -lt "$2" ]; }
has()     { case "$1" in *"$2"*) ;; *) return 1 ;; esac; } # $1 contains $2?
rejects() { ! "$WEBIFY" "$@" 2>/dev/null; }

# The encodes dominate the suite's wall time and are all independent (distinct
# outputs, fixture inputs only), so queue them all into one $(nproc)-way pool,
# drained once before the first assert that reads an output. Outputs are
# deterministic (bitexact muxing), so parallel runs change no bytes.
W=$(printf '%q' "$WEBIFY")
JOBS=()
enc()   { JOBS+=("$1"); } # queue one encode command line (runs via bash -c)
drain() {
    printf '%s\n' "${JOBS[@]}" | xargs -P "$(nproc)" -d '\n' -r -I CMD bash -c CMD
    JOBS=()
}

ff()       { ffmpeg -hide_banner -loglevel error -y "$@"; }
probe()    { ffprobe -v error -select_streams "$1" -show_entries "stream=$2" -of csv=p=0 "$3"; }
dims()     { probe v:0 width,height "$1" | head -1; } # first line only: mpegts lists the stream once per program
codecs()   { ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$1" | paste -sd+; } # all streams
channels() { probe a:0 channels "$1"; }
trc()      { probe v:0 color_transfer "$1"; }
pixfmt()   { probe v:0 pix_fmt "$1"; }
size()     { stat -c%s "$1"; }
# MP4 faststart: the moov atom must precede the mdat atom
moov_at_head() {
    python3 -c 'import sys
d = open(sys.argv[1], "rb").read()
m = d.find(b"moov"); k = d.find(b"mdat")
sys.exit(0 if 0 <= m < k else 1)' "$1"
}
# combing metric of the first decoded frame, x1000: adjacent-row luma
# differences over two-rows-apart ones — interleaved fields from different
# moments push it well above 1000, clean progressive frames sit below
comb() {
    python3 - "$1" "$(dims "$1")" <<'EOF'
import subprocess as sp, sys
path = sys.argv[1]
w, h = map(int, sys.argv[2].split(",")[:2])
px = sp.run(
    ["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1",
     "-vf", "format=gray", "-f", "rawvideo", "-"],
    capture_output=True).stdout
def diff(step):
    s = 0
    for y in range(0, h - step - 1, 2):
        a, b = px[y*w:(y+1)*w], px[(y+step)*w:(y+step+1)*w]
        s += sum(abs(p - q) for p, q in zip(a, b))
    return s or 1
print(diff(1) * 1000 // diff(2))
EOF
}

# --- fixtures ----------------------------------------------------------------
ff -f lavfi -i "testsrc2=size=640x480:duration=1:rate=1" -frames:v 1 photo.png
ff -f lavfi -i "testsrc2=size=1280x720:duration=2:rate=30" \
   -f lavfi -i "sine=frequency=440:duration=2" \
   -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 1 -shortest tv.mp4
ff -i tv.mp4 -c copy tv.mkv                       # mkv: declares no per-stream rates
ff -display_rotation 90 -i tv.mp4 -c copy rot.mp4 # portrait via display matrix
ff -f lavfi -i "testsrc2=size=320x240:duration=1:rate=30" \
   -c:v libx264 -pix_fmt yuv420p -movflags +frag_keyframe+empty_moov frag.mp4 # muted, nb_frames unknown
python3 - > evil.mp4 <<'EOF'                      # crafted 64-bit atom size: garbage in must not hang
import struct, sys
out  = b"\x00\x00\x00\x10ftypisom\x00\x00\x02\x00"
out += b"\x00\x00\x00\x01free" + struct.pack(">Q", (1 << 64) - 8)
sys.stdout.buffer.write(out + b"\x00" * 4096)
EOF
ff -f lavfi -i "testsrc2=size=320x240:duration=1:rate=30" \
   -f lavfi -i "sine=frequency=440:duration=1" \
   -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 2 -shortest stereo.mp4
ff -f lavfi -i "sine=frequency=440:duration=1" audio.wav        # no video stream at all
ff -f lavfi -i "testsrc2=size=200x150:duration=1:rate=5" anim.gif
ff -f lavfi -i "color=c=red@0.5:size=320x240:rate=1,format=rgba" -frames:v 1 alpha.png
ff -f lavfi -i "color=c=red:size=320x240:rate=1,format=rgba" -frames:v 1 opaque.png # alpha channel, all 0xFF
ff -f lavfi -i "testsrc2=size=640x480:duration=1:rate=30" -c:v libx264 -pix_fmt yuv420p \
   -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc hdr.mp4 # PQ-tagged HDR
ff -f lavfi -i "testsrc2=size=640x480:duration=1:rate=50" \
   -vf "tinterlace=mode=interleave_top,setparams=field_mode=tff" \
   -c:v mpeg2video -flags +ildct+ilme -q:v 3 ilace.ts # truly interlaced 25i (fields 20ms apart)
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
# non-media assets for the --peek libmagic fallback
printf '%%PDF-1.4\n1 0 obj<<>>endobj\ntrailer<<>>\n%%%%EOF\n' > doc.pdf
printf '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"/>\n' > pic.svg
gzip -c pic.svg > pic.svgz                          # a gzip-compressed asset
printf 'plain text body\n' > note.txt

# --- CLI contract --------------------------------------------------------------
t "--help exits 0 and prints usage"        bash -c "$W --help | grep -q usage"
t "--version names webify and FFmpeg"      bash -c "$W --version | grep -q 'webify .*FFmpeg'"
t "-q 11 (out of range) rejected"          rejects -q 11 in out
t "-q 60 (old 0-100 scale) rejected"       rejects -q 60 in out
t "--max bogus rejected"                   rejects --max bogus in out
t "dropped flag --next rejected"           rejects --next in out
t "dropped flag --legacy rejected"         rejects --legacy in out
t "dropped flag --fast rejected"           rejects --fast in out
t "dropped flag --best rejected"           rejects --best in out
t "no file arguments rejected"             rejects
t "three file arguments rejected"          rejects in out extra
t "audio-only input rejected"              rejects audio.wav x.mp4

# --- --peek: identify (no encode); media via FFmpeg, the rest via libmagic -----
# webify-native output types (exact — webify owns them)
t "--peek image -> image/avif, supported"    eq "$("$WEBIFY" --peek photo.png)" '{"mimetype":"image/avif","extension":"avif","supported":true,"encoding":""}'
t "--peek animated gif -> image/avif"        eq "$("$WEBIFY" --peek anim.gif)"  '{"mimetype":"image/avif","extension":"avif","supported":true,"encoding":""}'
t "--peek video -> video/mp4, supported"     eq "$("$WEBIFY" --peek tv.mp4)"    '{"mimetype":"video/mp4","extension":"mp4","supported":true,"encoding":""}'
# libmagic fallback for non-media (substring: robust to db bumps)
t "--peek pdf -> application/pdf"            has "$("$WEBIFY" --peek doc.pdf)"  '"mimetype":"application/pdf","extension":"pdf","supported":false,"encoding":""'
t "--peek svg (undecodable) -> svg"         has "$("$WEBIFY" --peek pic.svg)"  '"mimetype":"image/svg+xml","extension":"svg","supported":false'
t "--peek gzipped svg -> inner type + gzip" has "$("$WEBIFY" --peek pic.svgz)" '"mimetype":"image/svg+xml","extension":"svg","supported":false,"encoding":"gzip"'
t "--peek text -> text/plain (built-in)"    has "$("$WEBIFY" --peek note.txt)" '"mimetype":"text/plain"'
t "--peek unmapped type -> unsupported"     has "$("$WEBIFY" --peek audio.wav)" '"supported":false'
t "--peek exits 0 even when unsupported"     bash -c "$W --peek audio.wav >/dev/null"
t "--peek with an <output> rejected"         rejects --peek photo.png out.x
t "--peek combined with --json rejected"     rejects --peek --json photo.png

# --- --json: transcode to a file, report {mimetype,extension} on stdout --------
t "--json image: stdout reports avif"        eq "$("$WEBIFY" --json photo.png j1.avif)" '{"mimetype":"image/avif","extension":"avif"}'
t "--json video: stdout reports mp4"         eq "$("$WEBIFY" --json tv.mp4 j1.mp4)"     '{"mimetype":"video/mp4","extension":"mp4"}'
t "--json writes a valid AVIF file"          bash -c "$W --json photo.png j2.avif >/dev/null && grep -aq ftypavif j2.avif"
t "--json bytes == a plain run (additive)"   bash -c "$W photo.png j3a.avif 2>/dev/null; $W --json photo.png j3b.avif >/dev/null; cmp -s j3a.avif j3b.avif"
t "--json to a stdout output rejected"       rejects --json photo.png -
t "--json with no explicit <output> rejected" rejects --json photo.png

# --- encodes (the assert blocks below only read the outputs) -------------------
# images -> AVIF
enc "$W photo.png  q_def.avif"
enc "$W -q 8   photo.png q8.avif"
enc "$W -q 2   photo.png q2.avif"
enc "$W -q 9   photo.png q9.avif"
enc "$W -m 240  photo.png m240.avif"
enc "$W -m 2000 photo.png m2000.avif"
enc "$W - - < photo.png > piped.avif"
enc "$W photo.png > noout.avif"  # <output> omitted: stdout
enc "$W anim.gif anim.avif"
enc "$W alpha.png alpha.avif"
enc "$W opaque.png opaque.avif"
enc "$W exif.jpg exif.avif"
# video -> H.264/AAC MP4
enc "$W tv.mp4 v_def.mp4"
enc "$W -q 2 tv.mp4 v_q2.mp4"
enc "$W -q 9 tv.mp4 v_q9.mp4"
enc "$W rot.mp4 v_rot.mp4"
enc "$W tv.mkv v_file.mp4"      # identity pair on mkv: also covers piping a container with no header rates
enc "$W - - < tv.mkv > v_piped.mp4"
enc "$W tv.mkv > v_noout.mp4"   # <output> omitted: stdout
enc "$W frag.mp4 v_frag.mp4"
enc "$W stereo.mp4 v_stereo.mp4"
enc "$W hdr.mp4 v_hdr.mp4"
enc "$W ilace.ts v_ilace.mp4"
drain

# --- images (AVIF) -------------------------------------------------------------
t "image: still -> AVIF (ftyp brand)"                 grep -aq ftypavif q_def.avif
t "image: default == -q 8 byte-identical"             cmp -s q_def.avif q8.avif
t "image: -q 2 smaller than -q 8"                     lt "$(size q2.avif)" "$(size q8.avif)"
t "image: -q 8 smaller than -q 9"                     lt "$(size q8.avif)" "$(size q9.avif)"
t "image: --max 240 fits the box (640x480 -> 240x180)" eq "$(dims m240.avif)" "240,180"
t "image: never upscaled (--max 2000)"                eq "$(dims m2000.avif)" "640,480"
t "image: stdin -> stdout is a valid AVIF"            grep -aq ftypavif piped.avif
t "image: omitted output goes to stdout"              grep -aq ftypavif noout.avif
t "image: default still stays 4:2:0"                  eq "$(pixfmt q_def.avif)" "yuv420p"
t "image: premium -q still stays 4:2:0 (Main profile)" eq "$(pixfmt q9.avif)" "yuv420p"
t "image: animated gif -> animated AVIF (avis brand)" grep -aq ftypavis anim.avif
t "image: EXIF orientation baked in (-> 480x640)"     eq "$(dims exif.avif)" "480,640"
t "image: alpha kept (auxiliary alpha stream)"        grep -aq "auxiliary:alpha" alpha.avif
t "image: fully opaque alpha channel dropped"         bash -c "! grep -aq 'auxiliary:alpha' opaque.avif"

# --- video (H.264/AAC MP4) -----------------------------------------------------
t "video: H.264 + AAC"                                eq "$(codecs v_def.mp4)" "h264+aac"
t "video: 720p source fits the default 480 box"       eq "$(dims v_def.mp4)" "854,480"
t "video: mono source stays mono"                     eq "$(channels v_def.mp4)" "1"
t "video: stereo source stays stereo"                 eq "$(channels v_stereo.mp4)" "2"
t "video: HDR (PQ) tonemapped to SDR bt709"           eq "$(trc v_hdr.mp4)" "bt709"
t "video: interlaced fixture really combs"            lt 1100 "$(comb ilace.ts)"
t "video: interlaced source deinterlaced (bwdif)"     lt "$(comb v_ilace.mp4)" 1000
t "video: -q 2 smaller than -q 9"                     lt "$(size v_q2.mp4)" "$(size v_q9.mp4)"
t "video: display-matrix rotation baked in"           eq "$(dims v_rot.mp4)" "270,480"
t "video: moov at the head (faststart)"               moov_at_head v_def.mp4
t "video: piped i/o byte-identical to file i/o"       cmp -s v_file.mp4 v_piped.mp4
t "video: omitted output goes to stdout"              cmp -s v_file.mp4 v_noout.mp4
t "video: piped output keeps moov at the head"        moov_at_head v_piped.mp4
t "video: muted fragmented mp4 stays video"           eq "$(codecs v_frag.mp4)" "h264"
t "video: crafted mp4 input does not hang"            bash -c "timeout 5 $W - - < evil.mp4 > /dev/null 2>&1; [ \$? -ne 124 ]"

echo
[ "$fail" -eq 0 ] && echo "all $pass tests passed" || { echo "$fail of $((pass+fail)) tests FAILED"; exit 1; }
