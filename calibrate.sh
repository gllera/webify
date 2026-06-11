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
#   TIERS="..."  effort tiers to sweep (default "def fast best"). The tiers
#                carry their own calibrated offsets (--next --fast crf+4,
#                --legacy --fast crf+1) and their own encoder paths, so each
#                tier is parity-checked against the baseline at the SAME
#                tier (vp9f vs av1f/x264f, ...). Set TIERS=def for dense
#                re-fit ladders — tier offsets are validated, not re-fit
#   BBB=...      Big Buck Bunny clip (>= 6 s) for the real-content fixture
#                vid3 (default: fixtures/bbb.mp4, a 6 s 320x180 segment of
#                the original render — keeps vid3 in the narrow 2-thread
#                encode regime; vid3 is skipped with a warning if missing —
#                real content is mandatory for a re-fit, two synthetic
#                fixtures alone mis-fit by whole CRF steps)
#   TOS=...      live-action source for fixture vid5 + the real-content GIF
#                vid5.gif (default: fixtures/tos.mp4, a 6 s Tears of Steel
#                segment — faces and film grain, the content class the
#                synthetic fixtures miss; skipped with a warning if absent)
#   PHOTOS="a b" two photographic stills for photo1d/photo2d (2x lanczos-
#                downscaled so source artifacts wash out). Defaults to the
#                Kodak True Color pair in fixtures/ (kodim23 parrots,
#                kodim04 portrait); skipped with a warning if neither the
#                override nor the default pair is present
#
# The fixtures/ defaults are sha256-pinned assets of the `fixtures-v1`
# GitHub Release; ./fixtures.sh (run automatically) fetches what is missing
# — see its header for provenance and the immutability rule.
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
TIERS=${TIERS:-"def fast best"}
BBB=${BBB:-fixtures/bbb.mp4}
TOS=${TOS:-fixtures/tos.mp4}
PHOTOS=${PHOTOS:-}

# the real-content fixtures are sha256-pinned assets of the fixtures-v1
# GitHub Release — fetch whatever is missing (not in the __measure children;
# offline runs degrade to the per-fixture skip warnings below)
[ "${1:-}" = __measure ] || ./fixtures.sh ||
    echo "WARNING: fixture fetch failed — missing fixtures will be skipped" >&2

[ -z "$PHOTOS" ] && [ -s fixtures/kodim23.png ] && [ -s fixtures/kodim04.png ] &&
    PHOTOS="fixtures/kodim23.png fixtures/kodim04.png"

# ---------------------------------------------------------------- internal
# measure one output (the referee's child re-invocation): RGB-SSIM it
# against its master, cache as <output>.ssim
if [ "${1:-}" = __measure ]; then
    o=$2; base=$(basename "$o"); fix=${base%%_*}
    ssim_of() { # ffmpeg input/filter args -> the run's "All:" SSIM figure
        ffmpeg -hide_banner -nostdin "$@" -f null - 2>&1 |
            grep -oP 'All:\K[0-9.]+' | tail -1 || true
    }
    ssim() { # $1 distorted, $2 reference master
        local ow oh
        read -r ow oh < <(ffprobe -v error -select_streams v:0 \
            -show_entries stream=width,height -of csv=p=0 "$1" | head -1 | tr ',' ' ')
        ssim_of -i "$1" -i "$2" -lavfi \
            "[0:v]format=gbrp[a];[1:v]scale=${ow}:${oh}:flags=lanczos,format=gbrp[b];[a][b]ssim"
    }
    case "$base" in # codec tags carry an f/b suffix for the --fast/--best tiers
        *_animwebp*_*) # host ffmpeg can't decode animated WebP: ImageMagick referees
            s=NA
            if command -v convert >/dev/null; then
                d=$(mktemp -d)
                if convert "$o" -coalesce "$d/f_%03d.png" 2>/dev/null; then
                    s=$(ssim_of -framerate 10 -i "$d/f_%03d.png" \
                        -i "$DIR/$fix.gif" -lavfi \
                        "[0:v]format=gbrp[a];[1:v]format=gbrp[b];[a][b]ssim")
                fi
                rm -rf "$d"
            fi;;
        *_animavif*_*) s=$(ssim "$o" "$DIR/$fix.gif");;
        *.webm|*.mp4) s=$(ssim "$o" "$DIR/$fix.mp4");;
        *.avif|*.webp) s=$(ssim "$o" "$DIR/$fix.png");;
    esac
    echo "${s:-NA}" > "$o.ssim"
    exit 0
fi

case "$MODE" in next|legacy|all) ;; *)
    echo "usage: $0 [next|legacy|all]" >&2; exit 2;;
esac

for t in ffmpeg ffprobe "$WEBIFY"; do
    command -v "$t" >/dev/null || { echo "missing: $t" >&2; exit 1; }
done

# ---------------------------------------------------------------- fixtures
mkdir -p "$DIR/out"
echo "== fixtures ($DIR)"
gen() { local f=$1; shift; [ -s "$DIR/$f" ] || ffmpeg -hide_banner -loglevel error \
            "$@" -y "$DIR/$f"; }
# every video master is near-lossless x264; every GIF master is the same
# 10fps dithered-palette pipeline
MASTER="-c:v libx264 -crf 12 -pix_fmt yuv420p"
GIFPAL="[0:v]fps=10,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a"
gen vid1.mp4 -t 6 -f lavfi -i mandelbrot=size=1280x720 $MASTER &
gen vid2.mp4 -t 6 -f lavfi -i testsrc2=size=1280x720 $MASTER &
gen vid1.gif -t 3 -f lavfi -i mandelbrot=size=320x240 -filter_complex "$GIFPAL" &
gen vid2.gif -t 3 -f lavfi -i testsrc2=size=320x240 -filter_complex "$GIFPAL" &
gen vid4.gif -t 3 -f lavfi -i smptebars=size=320x240 -filter_complex "$GIFPAL" &
gen mandel.png -f lavfi -i mandelbrot=size=1280x720 -frames:v 1 &
gen chart.png -f lavfi -i testsrc2=size=1280x720 -frames:v 1 &
gen grad.png -f lavfi -i gradients=size=1280x720:nb_colors=4 -frames:v 1 &
gen graphic.png -f lavfi -i color=c=red:size=256x256 -frames:v 1 &
if [ -s "$BBB" ]; then # committed segment so re-runs measure the same content
    gen vid3.mp4 -t 6 -i "$BBB" -an $MASTER &
else
    echo "WARNING: no Big Buck Bunny source ($BBB): skipping vid3 — synthetic" \
         "fixtures alone mis-fit real content by whole CRF steps" >&2
fi
if [ -s "$TOS" ]; then
    gen vid5.mp4 -t 6 -i "$TOS" -an $MASTER &
    gen vid5.gif -t 3 -i "$TOS" -filter_complex \
        "[0:v]scale=320:-2,fps=10,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a" &
else
    echo "WARNING: no live-action source ($TOS): skipping vid5/vid5.gif —" \
         "synthetic fixtures alone mis-fit real content by whole CRF steps" >&2
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
[ -s "$DIR/vid5.mp4" ] && vids="$vids vid5"
anims="vid1 vid2 vid4"; [ -s "$DIR/vid5.gif" ] && anims="$anims vid5"
stills="mandel chart grad graphic"
[ -s "$DIR/photo1d.png" ] && stills="photo1d photo2d alphagrad $stills"
job() { # $1 output basename, rest = webify args
    local o="$DIR/out/$1"; shift
    [ -s "$o" ] || printf '%s %s %s\n' "$WEBIFY" "$*" "$o" >> "$DIR/joblist"
}
qarg() { [ "$1" = def ] && echo "" || echo "-q $1"; }
targ() { [ "$1" = def ] && echo "" || echo "--$1"; }     # tier flag
tsuf() { [ "$1" = def ] && echo "" || echo "${1:0:1}"; } # codec tag suffix: f/b
: > "$DIR/joblist"
for f in $vids; do for q in $QS; do for t in $TIERS; do
    [ "$MODE" = legacy ] || job "${f}_av1$(tsuf "$t")_${q}.webm" \
        --next $(targ "$t") $(qarg "$q") "$DIR/$f.mp4"
done; done; done
for f in $vids; do for q in $QS; do for t in $TIERS; do
    job "${f}_vp9$(tsuf "$t")_${q}.webm" $(targ "$t") $(qarg "$q") "$DIR/$f.mp4"
done; done; done
for f in $vids; do for q in $QS; do for t in $TIERS; do
    [ "$MODE" = next ] || job "${f}_x264$(tsuf "$t")_${q}.mp4" \
        --legacy $(targ "$t") $(qarg "$q") "$DIR/$f.mp4"
done; done; done
if [ "$MODE" != legacy ]; then
    for f in $anims; do for q in $AQS; do for t in $TIERS; do
        job "${f}_animwebp$(tsuf "$t")_${q}.webp" $(targ "$t") $(qarg "$q") "$DIR/$f.gif"
        job "${f}_animavif$(tsuf "$t")_${q}.avif" --next $(targ "$t") $(qarg "$q") "$DIR/$f.gif"
    done; done; done
    for i in $stills; do for q in $SQS; do for t in $TIERS; do
        job "${i}_webp$(tsuf "$t")_${q}.webp" $(targ "$t") $(qarg "$q") "$DIR/$i.png"
        job "${i}_avif$(tsuf "$t")_${q}.avif" --next $(targ "$t") $(qarg "$q") "$DIR/$i.png"
    done; done; done
fi
echo "== encoding $(wc -l < "$DIR/joblist") missing outputs, $J-way parallel"
xargs -P "$J" -d '\n' -r -I CMD bash -c CMD < "$DIR/joblist"

# ---------------------------------------------------------------- referee
: > "$DIR/measlist"
for o in "$DIR"/out/*.webm "$DIR"/out/*.mp4 "$DIR"/out/*.avif "$DIR"/out/*.webp; do
    [ -e "$o" ] && [ ! -s "$o.ssim" ] && echo "$o" >> "$DIR/measlist"
done
echo "== measuring $(wc -l < "$DIR/measlist") outputs"
xargs -P $(( J * 2 )) -d '\n' -r -I OUT "$0" __measure OUT < "$DIR/measlist"

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
        print(line)
    # means as a one-line summary per codec
    for c in others:
        if ratios[c]:
            ds = deltas[c]
            print(f"  mean {c}: delta {sum(ds)/len(ds):+.4f}" if ds else
                  f"  mean {c}: delta NA", end='')
            print(f", size {sum(ratios[c])/len(ratios[c]):.2f}x of {base}")

# one table per content type and tier; a tier's table only appears when its
# baseline was encoded (TIERS knob), since table() skips empty key sets
for base, others, title in [('vp9', ['av1', 'x264'], 'video'),
                            ('webp', ['avif'], 'stills'),
                            ('animwebp', ['animavif'], 'animations')]:
    for suf, tier in [('', ''), ('f', ' --fast'), ('b', ' --best')]:
        table(title + tier, base + suf, [c + suf for c in others])
EOF
