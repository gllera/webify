---
name: calibrate
description: Use when verifying or re-fitting webify's --next/--legacy equal-SSIM quality parity — after a vendored encoder bump (libaom/libvpx/libwebp/x264), after video-pipeline/filter/muxer code changes, after touching the --fast/--best tier settings, or when asked whether the -q mappings still hold.
---

# Quality calibration (`--next` / `--legacy`)

Method, history and all measured numbers: `doc/next-calibration.md`,
`doc/legacy-calibration.md`. The harness is `calibrate.sh` (repo root) —
read its header for the knobs.

## The check

One run of the shipped binary; every judgment is **within this run** — no
old binaries, and never against the docs' absolute SSIM numbers
(regenerated fixtures are equivalent, not byte-identical; referee scaling
shifts the absolutes too):

```bash
DIR=/tmp/webify-calib ./calibrate.sh all
```

`PHOTOS` defaults to the pinned Kodak pair in `fixtures/` (kodim23
parrots, kodim04 portrait — real photographic content incl. film grain);
override it only to probe other content.

It prints one parity table per content type **and tier**, each judging
`--next`/`--legacy` against the baseline codec *at the same tier* (`av1f`
vs `vp9f`, `x264b` vs `vp9b`, …) — the tier CRF offsets (`--next --fast`
video crf+4 but stills/anims crf−4, `--legacy --fast` crf+1) exist
exactly so this holds.

- Pass: per-q mean ΔSSIM stays in the documented bands (`--next`
  ≈ −.005…0, `--legacy` ≈ −.005…+.011). The `--fast`/`--best` tables get
  the same reading but carry wider per-content spread (single-step
  offsets) — judge their means against the docs' tier measurements, not
  single fixtures.
- **Animations are judged on the live-action fixture (`vid5`) alone**:
  the anim curve deliberately tracks real-content equal points, so the
  synthetic GIFs ride +.02…+.09 *above* parity (at ≤0.27x of animated
  WebP — by design, see the anim section of the next doc) and their
  table means read strongly positive. `vid5` in −.005…0 (slightly worse
  allowed at q 9.5, its equal point is beyond the curve) = pass.
- The video `--legacy` table reads the same way in miniature: real
  content (`vid3`/`vid5`) at parity is what counts; vid1 (mandelbrot
  noise) sits ~−.02 below at every q — the documented per-content
  spread, not a regression.
- Tiers are **never cross-compared**: `--fast`/`--best` do not promise
  the default tier's quality (fast trades a sliver of quality for 4-15x
  speed; best trades time for bytes). A tier is only judged against the
  same-tier baseline.
- Exempt the documented extremes: `grad`/`graphic`/`vid4` byte floors,
  `chart` above q80, and `-q 1` (plus `--fast` video below the default
  `-q`) where caps, not CRF, decide — judge on size there.
- Cache hygiene: the cache does not know which binary encoded what — wipe
  `DIR/out` after rebuilding the binary, and after an interrupted run
  (truncated leftovers poison the referee).

## Re-fitting a `-q` mapping or a tier offset

Dense `QS=` ladder + the `-q` inversion trick per the doc checklists;
`TIERS=def` keeps dense ladders affordable. The tier CRF offsets are
re-fit per the docs' tier measurements, against the same-tier baseline.

## Referee gotchas

- Host ffmpeg cannot decode animated WebP — ImageMagick `convert` referees
  those (entries print NA without it). The same applies to integrity
  checks: a perfectly valid animated WebP fails an ffmpeg decode.
- Host ffmpeg decodes AVIF as opaque — alpha fidelity is `test.sh`'s job
  (needs `avifdec` to measure; see the alpha section of the next doc).
- Real content is mandatory: synthetic-only fits miss by whole CRF steps.
  The pinned `fixtures/` set covers it end to end: the Kodak photo
  pair (`PHOTOS` default), `tos.mp4` live action (`TOS` default → vid5)
  and `bbb.mp4` rendered animation (`BBB` default → vid3). The files are
  sha256-pinned assets of the `fixtures-v1` GitHub Release, auto-fetched
  by `calibrate.sh` (or `./fixtures.sh`) into the gitignored `fixtures/`.
