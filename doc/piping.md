# stdin/stdout behavior

Piping never changes the output: the bytes webify writes through a pipe
are identical to the bytes it would write to a file. Whenever a format
needs a seekable input or output to make that true, webify spools through
a temporary file (`$TMPDIR` or `/tmp`) instead of degrading the result.
The guarantee holds because webify's output is deterministic outright —
the muxers run in bitexact mode, so no random IDs or wallclock timestamps
are embedded and the same input with the same options always produces the
same bytes (test.sh compares the file run against the piped run with
`cmp`).

`-` selects stdin/stdout explicitly; omitting the output argument
entirely is the same as passing `-` for it.

How each case is handled:

- **Piped images** are slurped into memory (they are small and their
  demuxers want to seek). AVIF written to stdout is assembled in memory
  and dumped whole at the end — the muxer back-patches its header (item
  offsets/sizes), which needs a seekable sink.
- **Piped video input** is spooled to an unlinked temporary file that
  disappears when the process exits, however it exits. That keeps
  everything that needs to rewind the input working exactly as with a
  file argument: the HDR peak probe and containers that outright need
  seeking (MP4/MOV without faststart, AVI). The pipe is consumed to EOF
  before encoding starts.
- **Piped video output** is written to a temporary file and streamed to
  stdout at the end, so the `moov` atom lands at the head exactly like
  file output (MP4 `+faststart`). This temp file is *named* — movenc's
  faststart pass re-opens the output by name to shuffle the moov — so it
  cannot be pre-unlinked: a hard kill (`SIGKILL`) can leave a
  `webify-XXXXXX` file behind in `$TMPDIR`; every normal exit, including
  failures, removes it.

If a temporary file cannot be created at all, webify degrades gracefully
with a warning instead of failing: video input streams (the container
permitting), and MP4 output falls back to a fragmented MP4 (playable
everywhere MSE is, but not by every old player).
