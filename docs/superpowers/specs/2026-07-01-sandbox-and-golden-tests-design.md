# Design: sandbox the transcoder + golden-hash the outputs

Date: 2026-07-01
Status: approved (user selected "#1 and #2"; the three sub-decisions below use the
recommended options, recorded here for review)

## Motivation

`webify`'s primary consumer (SRR `asset-process`) feeds it arbitrary bytes fetched
from the internet, and FFmpeg's demuxers/decoders are one of the most CVE-dense
attack surfaces in open source. The process-per-asset model is already the right
shape for containment — but nothing is currently exploiting it. Two gaps:

1. **Trust boundary.** A crafted input can make the process write outside its
   output, exhaust memory (decompression bomb / giant canvas), spin forever, or —
   on a decoder RCE — exec a shell or open a socket. None of that is fenced today.
2. **Test architecture.** The build pays for byte-exact determinism
   (`AVFMT_FLAG_BITEXACT`) but only cashes it in for one `cmp` (pipe vs file). Drift
   in webify's own code *or* a silent vendor-bump encoder change is not caught
   byte-exactly, and `./test.sh` depends on whatever `ffmpeg` the host happens to
   have — which the project's own history shows has already bitten fixture
   generation.

This design closes both.

## Decisions (recommended options)

- **seccomp posture:** Landlock + rlimits + a seccomp **allowlist**, all default-on,
  every fixture encode exercised under the sandbox in CI so a missing syscall fails
  the build rather than production. `WEBIFY_NO_SANDBOX=1` (all off) and
  `WEBIFY_NO_SECCOMP=1` (just the syscall filter off) are escape hatches. Fail
  posture is **closed**: an unlisted syscall raises `SIGSYS` (kill), so a gap
  degrades one asset, never corrupts an output.
- **Golden scope:** **per-arch** hash sets (`goldens/amd64.sha256`,
  `goldens/arm64.sha256`). x264/libaom SIMD & float paths are not guaranteed
  byte-identical across amd64/arm64, so each CI runner checks its own set.
- **Fuzz cadence:** a separate clang-instrumented build + libFuzzer job on a
  **weekly cron + workflow_dispatch**, bounded time budget, seed corpus from the
  fixtures. It does not slow the per-PR build.

---

## Part 1 — Sandbox

All sandbox code lives in `src/webify.cpp` (the one-source-file invariant holds),
in a new banner section `==== Sandbox ====`, applied from `main()` after argument
parsing and *before* any libav call opens the input. It is entirely additive: with
`WEBIFY_NO_SANDBOX=1` the binary behaves exactly as today.

### 1a. Filesystem lockdown (Landlock)

`sandbox_fs(const char *in_path, const char *out_path)`:

- Query the running kernel's Landlock ABI (`landlock_create_ruleset(NULL, 0,
  LANDLOCK_CREATE_RULESET_VERSION)`). ABI < 1 or `ENOSYS` → **skip silently**
  (webify must still run on kernels without Landlock). Mask the requested access
  rights down to what the ABI supports so newer rights don't error on older
  kernels.
- Create a ruleset covering the filesystem access rights, then add path rules:
  - `$TMPDIR` (or `/tmp`) — read/write/create/remove (the input spool and the
    piped-output `webify-XXXXXX` temp both live here).
  - the input file (real path only; a `pipe:` input grants nothing — fd already
    open) — read.
  - the output's **parent directory** (real path only) — read/write/create/remove
    (avio creates+truncates it; movenc's faststart re-opens it for read).
- `prctl(PR_SET_NO_NEW_PRIVS)`, then `landlock_restrict_self`.
- Best-effort: any failure logs one `AV_LOG_WARNING` and continues (a sandbox that
  can't be installed must not brick the tool). Rationale: filesystem lockdown is
  the single highest-value, lowest-risk guard — it removes "write/read anywhere"
  from a decoder RCE, and webify only ever touches three path roots.

Syscalls are invoked via `syscall(__NR_landlock_*)` (musl has no wrappers); the
`__NR_*` constants come from `<linux/landlock.h>` + `<sys/syscall.h>` and resolve
per-arch, so amd64/arm64 both compile correct numbers.

### 1b. Syscall filter (seccomp-bpf allowlist)

`sandbox_seccomp()`:

- Skipped when `WEBIFY_NO_SANDBOX` or `WEBIFY_NO_SECCOMP` is set.
- `prctl(PR_SET_NO_NEW_PRIVS, 1)` then `prctl(PR_SET_SECCOMP,
  SECCOMP_MODE_FILTER, &prog)` with a hand-built BPF program: a flat allowlist of
  the syscalls a threaded transcode needs; the default action is
  `SECCOMP_RET_KILL_PROCESS` (fail-closed).
- The allowlist is generous and symbolic (`#ifdef __NR_x` guards for
  arch-specific names — e.g. arm64 has no `open`/`poll`/`access`, only the `*at`
  variants). Categories: process/thread lifecycle (`clone`, `clone3`, `futex`,
  `set_robust_list`, `rseq`, `exit`, `exit_group`, `tgkill`, the `rt_sig*`
  family, `sched_*affinity`, `sched_yield`, `membarrier`), memory (`mmap`,
  `munmap`, `mremap`, `mprotect`, `madvise`, `brk`), file I/O (`read`, `write`,
  `readv`, `writev`, `pread64`, `pwrite64`, `lseek`, `close`, `dup*`, `fcntl`,
  `ftruncate`, `openat`, `openat2`, `unlinkat`, `newfstatat`, `statx`, `fstat`,
  `getdents64`, `readlinkat`, `getcwd`, `faccessat2`, `ioctl` for `isatty`), and
  misc (`getrandom`, `clock_gettime`, `clock_nanosleep`, `nanosleep`, `sysinfo`,
  `uname`, `prlimit64`, `getrlimit`, `getpid`, `gettid`, `arch_prctl` on x86-64,
  `restart_syscall`). `execve`/`execveat`/`socket`/`connect`/`ptrace`/`mount` are
  **absent by construction** → any exec or network attempt kills the process.
- **CI-gated:** `./test.sh` runs every encode under the default (sandboxed)
  binary, so a missing syscall surfaces as a CI failure across the full fixture
  set on both arches before it can reach production. A dedicated assert also
  proves the sandbox is *active* (a build compiled with a deliberately-broken
  policy would fail it) and that `WEBIFY_NO_SANDBOX=1` disables it.

### 1c. Resource guards

- **`--max-pixels N`** (also `WEBIFY_MAX_PIXELS`): the largest allowed
  `width×height` of any decoded frame. Default **134217728** (128 MP — comfortably
  above 8K/108MP-camera content, far below a `60000×60000` bomb). Checked (i) after
  `avformat_find_stream_info` against the coded dimensions, and (ii) per decoded
  frame in `decode_packet`/`peek_first_frame` against the real frame dims (headers
  can lie). Over-cap → clean error exit, never an allocation. `0` disables.
- **`--timeout S`** (also `WEBIFY_TIMEOUT`): wall-clock ceiling via
  `setitimer(ITIMER_REAL)` + a `SIGALRM` handler that writes one line and
  `_exit(1)`. **Opt-in (default 0 = unlimited)** — a hardcoded default risks
  killing a legitimately long encode; batch callers (SRR) pass it explicitly.
  `RLIMIT_CPU` is set as a coarse backstop when `--timeout` is given.
- `RLIMIT_AS` is intentionally **not** forced by default (address-space caps
  interact badly with mmap-based allocators + thread stacks); `--max-pixels` is the
  always-on memory guard, and `WEBIFY_MEM_MB` offers an opt-in `RLIMIT_AS`.

### 1d. Fuzz harness

- A `#ifdef WEBIFY_FUZZER` block at the tail of `src/webify.cpp` replaces `main()`
  with `LLVMFuzzerTestOneInput`, driving the **demux + decode-first-frame** path
  (the `--peek`/probe CVE surface) over the input bytes **in memory** (reusing the
  existing `StdinIO` mem buffer + `mem_read`/`mem_seek` + `open_with_pb` +
  `avformat_find_stream_info` + a bounded first-frame decode). No stdout, no
  encode, `av_log` silenced, `peeked` reset each call. This keeps the
  one-source-file invariant and fuzzes the *real* code, not a copy.
- A new Dockerfile stage `fuzz` (`FROM ffmpeg`, `apk add clang compiler-rt`)
  compiles `clang++ -g -O1 -fsanitize=fuzzer -DWEBIFY_FUZZER` against the vendored
  static libs (musl; libFuzzer without ASan to avoid the ASan-on-static-musl
  minefield — it still catches SIGSEGV/assert/hang/OOM, the crash-class demuxer
  bugs). `test/fuzz/corpus/` seeds from the smallest fixtures.
- `.github/workflows/fuzz.yml`: weekly cron + `workflow_dispatch`, builds the
  `fuzz` stage, runs `-max_total_time=<budget> -rss_limit_mb=…`, uploads any
  crash artifact.

---

## Part 2 — Golden hashes + hermetic tests

### 2a. Hermetic fixture generation

Golden hashes are only meaningful if the fixtures are byte-stable, and the encoded
fixtures (`tv.mp4`, `hdr.mp4`, `ilace.ts`, …) depend on the *host* ffmpeg/libx264
version. So the golden run happens inside a pinned image:

- A new Dockerfile stage **`test`** (`FROM alpine:3.24`, `apk add bash python3
  coreutils`) that **COPYs a digest-pinned static ffmpeg/ffprobe** (from
  `mwader/static-ffmpeg:<ver>@sha256:…`, per-arch) plus the built `/webify`, then
  runs `./test.sh`. Pinned ffmpeg → byte-exact fixtures → meaningful goldens, with
  no repo bloat and no host-tool dependency.
- `./build.sh` gains a `TEST=1` mode (or a `--target test`) to run it locally;
  `build.yml` runs the `test` stage instead of `apt-get install ffmpeg && ./test.sh`.

### 2b. Golden check in test.sh

- After the encode pool drains, `test.sh` computes `sha256sum` of each
  deterministic output (~20: the AVIF stills/anim/alpha/exif, the MP4s, the M4A —
  the piped variants stay `cmp`-checked against their file twins) and diffs against
  `goldens/$(arch).sha256` (lines `<sha256>␠␠<name>`). Any mismatch is a failing
  assert that prints expected-vs-got.
- `REBASELINE=1 ./test.sh` rewrites `goldens/<arch>.sha256` from the current
  outputs (and prints the file so a contributor can copy it from CI logs).
- Arch: `uname -m` → `x86_64`→`amd64`, `aarch64`→`arm64`.

### 2c. Auto-rebaseline on vendor bumps

A vendor bump legitimately changes encoder bytes, so the goldens must move with it:

- **`.github/workflows/rebaseline.yml`** (`workflow_dispatch`, `inputs.ref`): a
  matrix (amd64 + arm64) that builds, runs `REBASELINE=1` inside the `test` stage,
  and uploads `goldens-<arch>` artifacts; a final `commit` job downloads both,
  writes `goldens/{amd64,arm64}.sha256`, and commits them to the target branch.
- **`vendor-update.yml`** calls `rebaseline.yml` on the `vendor-updates` branch
  after opening the PR and before/with dispatching `build.yml` on the tag, so the
  build runs green against freshly-committed goldens. The PR diff then shows the
  encoder byte-change explicitly (the goldens delta) instead of hiding it.
- For an ordinary code change that intentionally alters output, the contributor
  runs `REBASELINE=1 ./test.sh` (amd64) and triggers `rebaseline.yml` for arm64
  (or copies the arm64 hash CI prints on failure). Unintentional drift stays a hard
  CI failure — which is the point.

---

## Non-goals (unchanged from the "what I would not do" analysis)

Splitting `webify.cpp`, library-ifying, a server mode, removing libmagic (a
separate proposal), or replacing the goto-cleanup style. None are in scope here.

## Test plan

- `./test.sh` stays green on both arches with the sandbox **on** (proves the
  allowlist covers every fixture path) and adds: sandbox-active assert,
  `WEBIFY_NO_SANDBOX` disables, `--max-pixels` rejects an oversized synthetic
  input, `--timeout` kills a stall, and the ~20 golden-hash asserts.
- The `test` Docker stage is the CI gate (hermetic ffmpeg).
- `fuzz.yml` runs clean for its budget on the seed corpus.
- Determinism/faststart/piping/alpha/HDR/EXIF/dein­terlace invariants all
  re-verified by the existing asserts, now byte-pinned by the goldens.
