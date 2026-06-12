# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this is

`webify` is a single static C++ binary (one source file, `src/webify.cpp`)
that converts any popular video to VP9/Opus WebM and any popular image to
WebP, tuned for web delivery. `--next` switches to the next-gen formats
(AV1/Opus WebM, AVIF) and `--legacy` to the maximum-compatibility ones
(H.264/AAC MP4 with faststart, PNG/APNG) — both calibrated so a given `-q`
buys the *same visual quality* as the default pipeline (equal-SSIM fits),
only smaller or bigger files. It vendors a minimal FFmpeg + codec stack:
official release tarballs, sha256-pinned per library in `vendor.d/*.sh`,
plus the tiny reviewable diffs in `patches/`, built in Docker
(one stage per library, see `Dockerfile`).

## Commands

```bash
./build.sh        # Docker build; exports the static binary to ./dist/webify
./test.sh         # 76-assert behavioral suite against dist/webify
                  #   needs host ffmpeg (>= 6), ffprobe, cwebp, python3
./fixtures.sh     # fetch the pinned calibration fixtures (release fixtures-v1)
./calibrate.sh    # equal-SSIM parity harness (see Calibration below)
./vendor.sh       # bare-host build of the vendored stack (Docker doesn't use it)
./update-vendor.sh# probe upstreams, rewrite the vendor.d version+sha256 pins
```

## Invariants — re-verify with ./test.sh before merging anything that could touch them

1. **Piped i/o is byte-identical to file i/o, in every mode.** Piped input is
   spooled to an unlinked temp file (stats pass / HDR peek can rewind); piped
   video output spools through a *named* temp file (faststart re-opens by
   URL). Asserted with `cmp` for VP9, AV1 and H.264.
2. **Output is deterministic** (`AVFMT_FLAG_BITEXACT` on the muxer). Known
   exception: x264's threaded ratecontrol is timing-sensitive when the VBV
   binds hard (`--legacy` near `-q 0`) — bytes can vary between runs there.
3. **`--next` stays 8-bit 4:2:0 = AV1 Main profile end-to-end** — the one
   profile hardware decoders reliably implement. No 4:4:4, no film grain
   synthesis (both implemented and removed on purpose; see below).
4. **`-q` means the same look everywhere**: every tier (`--fast`/`--best`)
   and pipeline (`--next`/`--legacy`) maps `-q` to the visually equivalent
   setting, never the same number.
5. **Faststart everywhere**: WebM cues / MP4 moov at the head, piped output
   included (only the no-temp-file fallback degrades).
6. **Stills are byte-identical to `cwebp -q 80 -m 6 -sharp_yuv`** — this ties
   the vendored libwebp version to the cwebp used by test.sh (CI downloads
   Google's prebuilt at the `vendor.d/50-webp.sh` pin), and depends on
   `patches/0001-libwebpenc-expose-sharp_yuv.patch`.
7. **Never upscale; mono stays mono; rate caps follow the source** (its
   codec-weighted measured bitrate caps the budget from above).

## Calibration

All fitted constants live in the **"Calibration" section of
`src/webify.cpp`** (banner-marked) — CRF curves, rate-cap anchors, audio
anchors, the codec-efficiency table. The data and method are in
`doc/next-calibration.md` and `doc/legacy-calibration.md`.

Re-run `./fixtures.sh && ./calibrate.sh` after: a vendored encoder bump
(libaom/libvpx/libwebp/x264), filter-chain or muxer changes, or any
`--fast`/`--best` tier change. Hard-won procedural lessons:

- **Fit on real content, never synthetics alone** — every synthetic-only fit
  here has had to be redone (the anim curve measured −.055 SSIM on live
  action; `--fast` stills sank −.011…−.038 on the Kodak photos).
- **Run ladders at the binary's own thread counts** — x264 threaded RC under
  a binding VBV loses whole CRF steps vs few-thread runs.
- Per-content spread is wide (~25 CRF for animations): bias fits toward the
  content that degrades visibly (faces/live action), let graphics overdeliver.

## Vendoring, CI, releases

- `vendor.d/*.sh`: one script per library, upstream release version +
  tarball sha256 pinned. x264 has no release tarballs — it pins the `stable`
  branch tip by commit hash.
- `.github/workflows/build.yml`: native amd64 + arm64 builds (arm runners
  are free on public repos only), BuildKit layer cache per library via
  `type=gha`. The test suite runs on both arches and gates the `release`
  job. Tag pattern is `'[0-9]*'` on purpose: `fixtures-*` tags must not
  trigger builds. Release binaries bake their tag into `--version` (Docker
  build arg `VERSION`); local and branch builds report `dev`.
- `.github/workflows/vendor-update.yml` (monthly): bumps pins, PRs on the
  rolling `vendor-updates` branch, tags `<ffmpeg-version>-<YYYYMMDD>`, and
  dispatches build.yml on the tag (GITHUB_TOKEN-pushed tags/PRs never fire
  `on: push`/`on: pull_request` — hence the explicit dispatch). PR creation
  needs the repo setting "Allow Actions to create PRs" (lives outside the
  repo; re-enable via `PUT /repos/.../actions/permissions/workflow` if the
  repo is ever recreated).
- **Fixture releases are immutable**: the calibration fixtures are
  sha256-pinned assets of the `fixtures-v1` GitHub Release (no canonical
  upstream bytes exist — stream-copy cuts move with ffmpeg versions). New
  bytes ⇒ new `fixtures-vN` release + re-baseline the calibration docs;
  never re-upload over old assets.
- License: **GPL-2.0+** (x264 is GPL; everything else is more permissive).

## Removed on purpose — don't re-add without new evidence

- **Film grain synthesis** (`--grain`, libaom denoise-noise-level): least-
  exercised AV1 decoder path, some hardware skips it. Lessons kept: set it
  on *every* pass or stats rate the wrong content; the size win needs caps
  that don't bind; too-strong noise defeats libaom's model.
- **4:4:4 still race at high `-q`**: needs AV1 High profile, which hardware
  decoders commonly lack. Measurements preserved in `doc/next-calibration.md`.
- **Google's minrate floor**: a rate floor only adds bytes at a fixed
  quality target.
- **Deadline "best" (VP9)**, **x264 placebo**, **deeper libaom speeds**:
  all measured not to pay for their time (numbers in the source comments).
