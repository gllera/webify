#!/usr/bin/env bash
# calibrate.sh — parallel equal-SSIM calibration harness for --next/--legacy.
#
# Runs the fixture matrix from doc/next-calibration.md / doc/legacy-calibration.md
# through the shipped binary (so encoder settings and thread counts are exactly
# what ships — the legacy doc's procedural lesson), referees every output with
# RGB SSIM (gbrp, ffmpeg's "All" figure) against the master scaled to output
# geometry, and prints parity tables: SSIM delta and size ratio vs the VP9/WebP
# baseline at the same -q. Fixtures, encodes and SSIM values are cached in DIR,
# so re-runs only redo what is missing — delete DIR/out to force a re-encode.
#
#   ./calibrate.sh [next|legacy|all]     (default: all)
#
# Knobs (env):
#   DIR=...      workdir/cache (default /tmp/webify-calib)
#   WEBIFY=...   binary under test (default ./dist/webify)
#   J=N          parallel encode jobs (default: nproc — each job only uses
#                the binary's own small thread count, so whole-job
#                parallelism is what saturates the cores; the referee
#                stage runs lighter jobs at 2x this)
#   QS="..."     video -q points ("def" = no -q; default "def 1 3 4.8 7 9" —
#                q 1 probes the regime where the converted caps, not the CRF,
#                decide the quality)
#   SQS="..."    still -q points (default "def 9.5")
#   AQS="..."    animation -q points (default "def")
#   BBB=...      Big Buck Bunny source for the real-content fixture vid3
#                (default dist/BigBuckBunny_512kb.mp4; vid3 is skipped with a
#                warning if missing — real content is mandatory for a re-fit,
#                two synthetic fixtures alone mis-fit by whole CRF steps)
#   PHOTOS="a b" two photographic stills for photo1d/photo2d (2x lanczos-
#                downscaled so source artifacts wash out); skipped with a
#                warning if unset — supply real photos for a stills re-fit
#
# To sweep a CRF ladder for a curve re-fit instead of validating the shipped
# points, pass a dense QS and invert each -q -> CRF formula (the "-q inversion
# trick" in the docs: pick the -q that lands on the CRF under test).
#
# The fixture set deliberately includes the documented content extremes: grad
# (libaom's smooth-gradient byte floor), graphic (flat color, WebP's 60-byte
# lossless win), vid4.gif (static anim, the other byte-floor case), chart at
# high q (the lossless-race gap 4:2:0 AVIF cannot close), and alphagrad
# (alpha gradient; referee compares color planes only — host ffmpeg decodes
# AVIF opaque, so alpha fidelity needs avifdec and stays test.sh's job).
#
# Needs host ffmpeg/ffprobe (fixture generation + SSIM referee) and ImageMagick
# `convert` for the animated-WebP referee (host ffmpeg cannot decode animated
# WebP; those entries print NA without it). Legacy images are lossless PNG —
# nothing to calibrate there, so the image sections only run for --next.
set -euo pipefail
cd "$(dirname "$0")"

WEBIFY=$(realpath "${WEBIFY:-./dist/webify}")
DIR=${DIR:-/tmp/webify-calib}
MODE=${1:-all}
J=${J:-$(nproc)}
QS=${QS:-"def 1 3 4.8 7 9"}
SQS=${SQS:-"def 9.5"}
AQS=${AQS:-"def"}
BBB=${BBB:-dist/BigBuckBunny_512kb.mp4}
PHOTOS=${PHOTOS:-}

case "$MODE" in next|legacy|all) ;; *)
    echo "usage: $0 [next|legacy|all]" >&2; exit 2;;
esac

# ---------------------------------------------------------------- internal
# measure one output: RGB-SSIM it against its master, cache as <output>.ssim
if [ "${2:-}" = __measure ]; then
    o=$3; base=$(basename "$o"); fix=${base%%_*}
    ssim() { # $1 distorted, $2 reference master
        local ow oh
        read -r ow oh < <(ffprobe -v error -select_streams v:0 \
            -show_entries stream=width,height -of csv=p=0 "$1" | head -1 | tr ',' ' ')
        ffmpeg -hide_banner -nostdin -i "$1" -i "$2" -lavfi \
            "[0:v]format=gbrp[a];[1:v]scale=${ow}:${oh}:flags=lanczos,format=gbrp[b];[a][b]ssim" \
            -f null - 2>&1 | grep -oP 'All:\K[0-9.]+' | tail -1 || true
    }
    case "$base" in
        *_animwebp_*) # host ffmpeg can't decode animated WebP: ImageMagick referees
            s=NA
            if command -v convert >/dev/null; then
                d=$(mktemp -d)
                if convert "$o" -coalesce "$d/f_%03d.png" 2>/dev/null; then
                    s=$(ffmpeg -hide_banner -nostdin -framerate 10 -i "$d/f_%03d.png" \
                        -i "$DIR/$fix.gif" -lavfi \
                        "[0:v]format=gbrp[a];[1:v]format=gbrp[b];[a][b]ssim" \
                        -f null - 2>&1 | grep -oP 'All:\K[0-9.]+' | tail -1 || true)
                fi
                rm -rf "$d"
            fi;;
        *_animavif_*) s=$(ssim "$o" "$DIR/$fix.gif");;
        *.webm|*.mp4) s=$(ssim "$o" "$DIR/$fix.mp4");;
        *.avif|*.webp) s=$(ssim "$o" "$DIR/$fix.png");;
    esac
    echo "${s:-NA}" > "$o.ssim"
    exit 0
fi

for t in ffmpeg ffprobe "$WEBIFY"; do
    command -v "$t" >/dev/null || { echo "missing: $t" >&2; exit 1; }
done

# ---------------------------------------------------------------- fixtures
mkdir -p "$DIR/out"
echo "== fixtures ($DIR)"
gen() { local f=$1; shift; [ -s "$DIR/$f" ] || ffmpeg -hide_banner -loglevel error \
            "$@" -y "$DIR/$f"; }
gen vid1.mp4 -t 6 -f lavfi -i mandelbrot=size=1280x720 \
    -c:v libx264 -crf 12 -pix_fmt yuv420p &
gen vid2.mp4 -t 6 -f lavfi -i testsrc2=size=1280x720 \
    -c:v libx264 -crf 12 -pix_fmt yuv420p &
gen vid1.gif -t 3 -f lavfi -i mandelbrot=size=320x240 -filter_complex \
    "[0:v]fps=10,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a" &
gen vid2.gif -t 3 -f lavfi -i testsrc2=size=320x240 -filter_complex \
    "[0:v]fps=10,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a" &
gen vid4.gif -t 3 -f lavfi -i smptebars=size=320x240 -filter_complex \
    "[0:v]fps=10,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a" &
gen mandel.png -f lavfi -i mandelbrot=size=1280x720 -frames:v 1 &
gen chart.png -f lavfi -i testsrc2=size=1280x720 -frames:v 1 &
gen grad.png -f lavfi -i gradients=size=1280x720:nb_colors=4 -frames:v 1 &
gen graphic.png -f lavfi -i color=c=red:size=256x256 -frames:v 1 &
if [ -s "$BBB" ]; then # pinned segment so re-runs measure the same content
    gen vid3.mp4 -ss 60 -t 6 -i "$BBB" -an -c:v libx264 -crf 12 -pix_fmt yuv420p &
else
    echo "WARNING: no Big Buck Bunny source ($BBB): skipping vid3 — synthetic" \
         "fixtures alone mis-fit real content by whole CRF steps" >&2
fi
if [ -n "$PHOTOS" ]; then
    set -- $PHOTOS
    gen photo1d.png -i "$1" -vf scale=iw/2:ih/2:flags=lanczos &
    [ -n "${2:-}" ] && gen photo2d.png -i "$2" -vf scale=iw/2:ih/2:flags=lanczos &
else
    echo "WARNING: PHOTOS unset: skipping photo1d/photo2d — the stills curve" \
         "was fitted mostly on photographic content" >&2
fi
wait
[ -s "$DIR/photo1d.png" ] && gen alphagrad.png -i "$DIR/photo1d.png" -vf \
    "format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='clip(255-hypot(X-W/2,Y-H/2),0,255)'"

# ---------------------------------------------------------------- encodes
# one job per line, heaviest first so the pool never drains into a long tail;
# existing outputs are skipped (the cache)
vids="vid1 vid2"; [ -s "$DIR/vid3.mp4" ] && vids="$vids vid3"
stills="mandel chart grad graphic"
[ -s "$DIR/photo1d.png" ] && stills="photo1d photo2d alphagrad $stills"
job() { # $1 output basename, rest = webify args
    local o="$DIR/out/$1"; shift
    [ -s "$o" ] || printf '%s %s %s\n' "$WEBIFY" "$*" "$o" >> "$DIR/joblist"
}
qarg() { [ "$1" = def ] && echo "" || echo "-q $1"; }
: > "$DIR/joblist"
for f in $vids; do for q in $QS; do
    [ "$MODE" = legacy ] || job "${f}_av1_${q}.webm" --next $(qarg "$q") "$DIR/$f.mp4"
done; done
for f in $vids; do for q in $QS; do
    job "${f}_vp9_${q}.webm" $(qarg "$q") "$DIR/$f.mp4"
done; done
for f in $vids; do for q in $QS; do
    [ "$MODE" = next ] || job "${f}_x264_${q}.mp4" --legacy $(qarg "$q") "$DIR/$f.mp4"
done; done
if [ "$MODE" != legacy ]; then
    for f in vid1 vid2 vid4; do for q in $AQS; do
        job "${f}_animwebp_${q}.webp" $(qarg "$q") "$DIR/$f.gif"
        job "${f}_animavif_${q}.avif" --next $(qarg "$q") "$DIR/$f.gif"
    done; done
    for i in $stills; do for q in $SQS; do
        job "${i}_webp_${q}.webp" $(qarg "$q") "$DIR/$i.png"
        job "${i}_avif_${q}.avif" --next $(qarg "$q") "$DIR/$i.png"
    done; done
fi
echo "== encoding $(wc -l < "$DIR/joblist") missing outputs, $J-way parallel"
xargs -P "$J" -d '\n' -r -I CMD bash -c CMD < "$DIR/joblist"

# ---------------------------------------------------------------- referee
: > "$DIR/measlist"
for o in "$DIR"/out/*.webm "$DIR"/out/*.mp4 "$DIR"/out/*.avif "$DIR"/out/*.webp; do
    [ -e "$o" ] && [ ! -s "$o.ssim" ] && echo "$o" >> "$DIR/measlist"
done
echo "== measuring $(wc -l < "$DIR/measlist") outputs"
xargs -P $(( J * 2 )) -d '\n' -r -I OUT "$0" "$MODE" __measure OUT < "$DIR/measlist"

: > "$DIR/results.csv"
for o in "$DIR"/out/*.ssim; do
    f=${o%.ssim}
    echo "$(basename "$f"),$(stat -c%s "$f"),$(cat "$o")" >> "$DIR/results.csv"
done
sort -o "$DIR/results.csv" "$DIR/results.csv"

# ---------------------------------------------------------------- report
python3 - "$DIR/results.csv" <<'EOF'
import csv, sys
from collections import defaultdict

rows = defaultdict(dict)  # (fix, q) -> codec -> (bytes, ssim)
for name, size, ssim in csv.reader(open(sys.argv[1])):
    fix, codec, qtag = name.rsplit('.', 1)[0].split('_', 2)
    rows[(fix, qtag)][codec] = (int(size), None if ssim == 'NA' else float(ssim))

def qkey(q): return (0, 0) if q == 'def' else (1, float(q))
def table(title, base, others):
    keys = sorted({k for k in rows if base in rows[k]},
                  key=lambda k: (qkey(k[1]), k[0]))
    if not keys: return
    print(f"\n== {title} (baseline {base}: SSIM/KB; others: SSIM delta, size ratio)")
    deltas = defaultdict(list); ratios = defaultdict(list)
    for k in keys:
        bsz, bss = rows[k][base]
        line = f"  {k[0]:<8} q={k[1]:<4} {base} {bss:.4f} {bsz/1024:8.1f}K"
        for c in others:
            if c not in rows[k]: continue
            sz, ss = rows[k][c]
            d = None if ss is None or bss is None else ss - bss
            r = sz / bsz
            line += f" | {c} {'NA     ' if d is None else f'{d:+.4f}'} {r:5.2f}x"
            if d is not None: deltas[c].append(d)
            ratios[c].append(r)
    # means as a one-line summary per codec
        print(line)
    for c in others:
        if ratios[c]:
            ds = deltas[c]
            print(f"  mean {c}: delta {sum(ds)/len(ds):+.4f}" if ds else
                  f"  mean {c}: delta NA", end='')
            print(f", size {sum(ratios[c])/len(ratios[c]):.2f}x of {base}")

table('video', 'vp9', ['av1', 'x264'])
table('stills', 'webp', ['avif'])
table('animations', 'animwebp', ['animavif'])
EOF
