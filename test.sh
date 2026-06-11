#!/usr/bin/env bash
# Smoke test for dist/webify: exercises the externally observable claims
# (CLI contract, cwebp byte-identity, -q size ordering, --max scaling,
# rotation baking, mono audio, VP9/Opus codecs, faststart cues,
# stdin/stdout byte-identical to file i/o, interlaced sources deinterlaced,
# --next: AV1/Opus WebM video, AVIF images w/ alpha + animated GIF -> AVIF,
# everything 4:2:0 = AV1 Main profile,
# --legacy: H.264/AAC MP4 video w/ faststart (piped output too), lossless
# PNG images w/ alpha + animated GIF -> APNG).
# Fixtures are generated on the host: needs ffmpeg, ffprobe, cwebp, python3.
#
#   ./build.sh && ./test.sh
set -euo pipefail
cd "$(dirname "$0")"

WEBIFY="${WEBIFY:-$PWD/dist/webify}"
for tool in ffmpeg ffprobe cwebp python3; do
    command -v "$tool" >/dev/null || { echo "missing host tool: $tool"; exit 1; }
done
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
eq()     { [ "$1" = "$2" ]; }
lt()     { [ "$1" -lt "$2" ]; }
rejects() { ! "$WEBIFY" "$@" 2>/dev/null; }

# The ~50 encodes dominate the suite's wall time and are all independent
# (distinct outputs, fixture inputs only), while each webify run uses just
# its own small thread count — so all sections queue their encodes with
# enc into one $(nproc)-way pool, drained once before the first assert
# that reads an output: wall time is the longest single encode, not the
# sum of per-section maxima. Outputs are deterministic (bitexact muxing),
# so parallel runs change no bytes.
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
codecs()   { ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$1" | paste -sd+; } # all streams, so no -select_streams
channels() { probe a:0 channels "$1"; }
trc()      { probe v:0 color_transfer "$1"; }
pixfmt()   { probe v:0 pix_fmt "$1"; }
size()     { stat -c%s "$1"; }
stream_bytes() { # summed packet payload of one stream: <file> <v:0|a:0>
    # csv rows can carry a trailing comma (webm/opus packets), split it off
    ffprobe -v error -select_streams "$2" -show_entries packet=size \
            -of csv=p=0 "$1" | python3 -c 'import sys
print(sum(int(l.split(",")[0]) for l in sys.stdin if l.strip(",\n")))'
}
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
ff -f lavfi -i "testsrc2=size=320x240:duration=8:rate=30" \
   -f lavfi -i "sine=frequency=440:duration=8" \
   -c:v libx264 -pix_fmt yuv420p -b:v 60k -c:a aac -b:a 24k -shortest \
   lowrate.mkv                                    # low-rate mkv: caps must come from the stats-pass measurement
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

# --- CLI contract --------------------------------------------------------------
t "--help exits 0 and prints usage"        bash -c "$W --help | grep -q usage"
t "--version names webify and FFmpeg"     bash -c "$W --version | grep -q 'webify .*FFmpeg'"
t "-q 11 (out of range) rejected"          rejects -q 11 in out
t "-q 60 (old 0-100 scale) rejected"       rejects -q 60 in out
t "--max bogus rejected"                   rejects --max bogus in out
t "--fast with --best rejected"            rejects --fast --best in out
t "audio-only input rejected"              rejects audio.wav x.webm

# --- encodes (all sections; the assert blocks below only read the outputs) -----
# images
enc "$W photo.png  q_def.webp"
enc "$W -q 8   photo.png q8.webp"
enc "$W -q 2   photo.png q2.webp"
enc "$W -q 9.5 photo.png q95.webp"
enc "$W -m 240  photo.png m240.webp"
enc "$W -m 2000 photo.png m2000.webp"
enc "$W - - < photo.png > piped.webp"
enc "$W --fast photo.png q_fast.webp"
enc "$W flat.png flat.webp"
enc "$W anim.gif anim.webp"
enc "$W exif.jpg exif.webp"
enc "cwebp -quiet -q 80 -m 6 -sharp_yuv photo.png -o ref80.webp"
# video
enc "$W tv.mp4 v_def.webm"
enc "$W -q 2 tv.mp4 v_q2.webm"
enc "$W -q 9 tv.mp4 v_q9.webm"
enc "$W rot.mp4 v_rot.webm"
enc "$W tv.mkv v_file.webm"      # identity pair on mkv: also covers piping a container with no header rates
enc "$W - - < tv.mkv > v_piped.webm"
enc "$W lowrate.mkv v_lowrate.webm"
enc "$W --fast lowrate.mkv v_lowrate_fast.webm"   # no stats pass: header caps only
enc "$W frag.mp4 v_frag.webm"
enc "$W stereo.mp4 v_stereo.webm"
enc "$W hdr.mp4 v_hdr.webm"
enc "$W ilace.ts v_ilace.webm"
enc "$W --fast tiny.mp4 v_fast.webm"
enc "$W --best tiny.mp4 v_best.webm"
# --next
enc "$W --next tv.mp4 a_tv.webm"
enc "$W --next hdr.mp4 a_hdr.webm"
enc "$W --next anim.gif a_anim.avif"
enc "$W --next - - < tv.mp4 > a_piped.webm"    # file-run reference: a_tv.webm above
enc "$W --next --fast tiny.mp4 a_fast.webm"
enc "$W --next --best tiny.mp4 a_best.webm"
enc "$W --next photo.png a_q.avif"
enc "$W --next -q 2 photo.png a_q2.avif"
enc "$W --next -q 9 photo.png a_q9.avif"
enc "$W --next alpha.png a_alpha.avif"
enc "$W --next opaque.png a_opaque.avif"
enc "$W --next exif.jpg a_exif.avif"
enc "$W --next - - < photo.png > a_piped.avif"
# --legacy
enc "$W --legacy tv.mp4 l_tv.mp4"
enc "$W --legacy -q 2 tv.mp4 l_q2.mp4"
enc "$W --legacy -q 9 tv.mp4 l_q9.mp4"
enc "$W --legacy rot.mp4 l_rot.mp4"
enc "$W --legacy hdr.mp4 l_hdr.mp4"
enc "$W --legacy - - < tv.mp4 > l_piped.mp4"   # file-run reference: l_tv.mp4 above
enc "$W --legacy --fast tiny.mp4 l_fast.mp4"
enc "$W --legacy --best tiny.mp4 l_best.mp4"
enc "$W --legacy photo.png l_q.png"
enc "$W --legacy -q 2 photo.png l_q2.png"
enc "$W --legacy plain.jpg l_jpg.png"
enc "$W --legacy alpha.png l_alpha.png"
enc "$W --legacy opaque.png l_opaque.png"
enc "$W --legacy exif.jpg l_exif.png"
enc "$W --legacy anim.gif l_anim.png"
enc "$W --legacy - - < photo.png > l_piped.png"
enc "$W --legacy - - < anim.gif > l_piped_anim.png"
drain

# --- images --------------------------------------------------------------------
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
t "video: VP9 + Opus"                                 eq "$(codecs v_def.webm)" "vp9+opus"
t "video: 720p source fits the default 480 box"       eq "$(dims v_def.webm)" "854,480"
t "video: mono source stays mono"                     eq "$(channels v_def.webm)" "1"
t "video: stereo source stays stereo"                 eq "$(channels v_stereo.webm)" "2"
t "video: HDR (PQ) tonemapped to SDR bt709"           eq "$(trc v_hdr.webm)" "bt709"
t "video: interlaced fixture really combs"            lt 1100 "$(comb ilace.ts)"
t "video: interlaced source deinterlaced (bwdif)"     lt "$(comb v_ilace.webm)" 1000
t "video: --fast tier produces VP9+Opus"              eq "$(codecs v_fast.webm)" "vp9+opus"
t "video: --best tier produces VP9+Opus"              eq "$(codecs v_best.webm)" "vp9+opus"
t "video: -q 2 smaller than -q 9"                     lt "$(size v_q2.webm)" "$(size v_q9.webm)"
t "video: display-matrix rotation baked in"           eq "$(dims v_rot.webm)" "270,480"
t "video: seek cues at the head (faststart)"          cues_at_head v_def.webm
t "video: piped i/o byte-identical to file i/o"       cmp -s v_file.webm v_piped.webm
t "video: piped output keeps cues at the head"        cues_at_head v_piped.webm
# mkv declares no stream rates, so only the stats-pass measurement can cap:
# the video budget becomes the measured rate x0.8 (h264 codec weight), under
# the source's own spend; the audio cap is asserted against the uncapped
# --fast run (libopus treats bit_rate as a hint — VBR ran ~50% over the 24k
# cap on this fixture, so an absolute threshold would be fragile)
t "video: measured video rate caps an unmarked mkv"   lt "$(stream_bytes v_lowrate.webm v:0)" "$(stream_bytes lowrate.mkv v:0)"
t "video: measured audio rate caps an unmarked mkv"   lt "$(stream_bytes v_lowrate.webm a:0)" "$(stream_bytes v_lowrate_fast.webm a:0)"
t "video: muted fragmented mp4 stays video"           eq "$(codecs v_frag.webm)" "vp9"
t "video: crafted mp4 input does not hang"            bash -c "timeout 5 $W - - < evil.mp4 > /dev/null 2>&1; [ \$? -ne 124 ]"

# --- --next: AV1/Opus WebM video, AVIF still images -------------------------------
t "next: video becomes AV1 + Opus"                     eq "$(codecs a_tv.webm)" "av1+opus"
t "next: 720p source fits the default 480 box"         eq "$(dims a_tv.webm)" "854,480"
t "next: smaller than the VP9 default at the same -q"  lt "$(size a_tv.webm)" "$(size v_def.webm)"
t "next: seek cues at the head (faststart)"            cues_at_head a_tv.webm
t "next: HDR (PQ) tonemapped to SDR bt709"             eq "$(trc a_hdr.webm)" "bt709"
t "next: animated gif -> animated AVIF (avis brand)"   grep -aq ftypavis a_anim.avif
t "next: piped i/o byte-identical to file i/o"         cmp -s a_tv.webm a_piped.webm
t "next: --fast tier produces AV1+Opus"                eq "$(codecs a_fast.webm)" "av1+opus"
t "next: --best tier produces AV1+Opus"                eq "$(codecs a_best.webm)" "av1+opus"
t "next: still image -> AVIF (ftyp brand)"             grep -aq ftypavif a_q.avif
t "next: -q 2 smaller than -q 9"                       lt "$(size a_q2.avif)" "$(size a_q9.avif)"
t "next: EXIF orientation baked in (-> 480x640)"       eq "$(dims a_exif.avif)" "480,640"
t "next: alpha kept (auxiliary alpha stream)"          grep -aq "auxiliary:alpha" a_alpha.avif
t "next: fully opaque alpha channel dropped"           bash -c "! grep -aq 'auxiliary:alpha' a_opaque.avif"
t "next: stdin -> stdout still is a valid AVIF"        grep -aq ftypavif a_piped.avif
t "next: default still stays 4:2:0"                    eq "$(pixfmt a_q.avif)" "yuv420p"
t "next: premium -q still stays 4:2:0 (Main profile)"  eq "$(pixfmt a_q9.avif)" "yuv420p"
t "next: animated AVIF smaller than animated WebP"     lt "$(size a_anim.avif)" "$(size anim.webp)"

# --- --legacy: H.264/AAC MP4 video, PNG/APNG images ----------------------------
t "legacy: --next with --legacy rejected"              rejects --next --legacy in out
t "legacy: video becomes H.264 + AAC"                  eq "$(codecs l_tv.mp4)" "h264+aac"
t "legacy: 720p source fits the default 480 box"       eq "$(dims l_tv.mp4)" "854,480"
t "legacy: mono source stays mono"                     eq "$(channels l_tv.mp4)" "1"
t "legacy: moov at the head (faststart)"               moov_at_head l_tv.mp4
t "legacy: -q 2 smaller than -q 9"                     lt "$(size l_q2.mp4)" "$(size l_q9.mp4)"
t "legacy: display-matrix rotation baked in"           eq "$(dims l_rot.mp4)" "270,480"
t "legacy: HDR (PQ) tonemapped to SDR bt709"           eq "$(trc l_hdr.mp4)" "bt709"
t "legacy: piped i/o byte-identical to file i/o"       cmp -s l_tv.mp4 l_piped.mp4
t "legacy: piped mp4 keeps moov at the head"           moov_at_head l_piped.mp4
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
