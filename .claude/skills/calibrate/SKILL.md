---
name: calibrate
description: Use when verifying or re-fitting webify's --next/--legacy equal-SSIM quality parity — after a vendored encoder bump (libaom/libvpx/libwebp/x264), after video-pipeline/filter/muxer code changes, or when asked whether the -q mappings still hold.
---

# Quality calibration (`--next` / `--legacy`)

Method, history and all measured numbers: `doc/next-calibration.md`,
`doc/legacy-calibration.md`. The harness is `calibrate.sh` (repo root) —
read its header for the knobs.

## Pick the check by what changed

| Change | Right check |
|---|---|
| Vendored encoder bump | SSIM sweep: two `calibrate.sh` runs, old vs new binary |
| Pipeline/filter/muxer code | A/B stream hash first (~minutes, exact); sweep only if streams differ |
| Re-fitting a `-q` mapping | Dense `QS=` ladder + the `-q` inversion trick per the doc checklists |

## SSIM sweep

```bash
DIR=/tmp/calib-old WEBIFY=<old-binary> PHOTOS="<two real photos>" ./calibrate.sh all
DIR=/tmp/calib-new PHOTOS="<same photos>" ./calibrate.sh all
```

- Judge **run-vs-run with identical fixtures**, never against the docs'
  absolute numbers (regenerated fixtures are equivalent, not byte-identical;
  absolute SSIM also shifts with referee scaling choices).
- Pass: per-q mean deltas stay in the documented bands (`--next` ≈ −.005…0,
  `--legacy` ≈ −.005…+.011) and no point moves >±.005 between the two runs.
- Exempt the documented extremes: `grad`/`graphic`/`vid4` byte floors,
  `chart` above q80, and `-q 1` where caps, not CRF, decide (judge on size).
- Distinct `DIR` per binary — the cache does not know which binary encoded.

## A/B stream hash (pipeline changes)

Encoded streams are deterministic, so byte-compare them directly:

```bash
git worktree add /tmp/webify-old <pre-change-ref> && (cd /tmp/webify-old && ./build.sh)
# encode the same fixtures with both binaries, then hash codec streams only:
ffmpeg -i out.webm -map 0:v -c copy -fflags +bitexact -f ivf - | sha256sum  # vp9/av1
ffmpeg -i out.mp4  -map 0:v -c copy -f h264 - | sha256sum                   # x264
```

Identical hashes ⇒ calibration untouched by construction. If they differ,
check whether only metadata moved before concluding anything: strip SEI with
`-bsf:v 'filter_units=remove_types=6'` and re-hash; a lone SPS-VUI byte is
the filter-chain timebase, not a quality change. (Pre-v1.2 WebM files embed
random SegmentUIDs — hash remuxed streams, never whole files.)

## Referee gotchas

- Host ffmpeg cannot decode animated WebP — ImageMagick `convert` referees
  those (entries print NA without it).
- Host ffmpeg decodes AVIF as opaque — alpha fidelity is `test.sh`'s job
  (needs `avifdec` to measure; see the alpha section of the next doc).
- Real content is mandatory: synthetic-only fits miss by whole CRF steps.
  Supply `PHOTOS=` and keep the BBB fixture available.
