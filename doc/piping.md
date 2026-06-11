# stdin/stdout behavior

Every supported format works from stdin: images are buffered in memory, and
video containers that need a seekable input to keep all their features
(MP4/MOV without faststart, AVI) are spooled to an unlinked temporary file
(`$TMPDIR` or `/tmp`) that disappears when the process exits, however it
exits. Everything else streams — but a truly streamed video input cannot be
rewound for the stats pass, so it falls back to single-pass encoding (with a
warning); `--best` spools instead, so its stats pass always runs.

Piped video output is playable but carries no duration/seek index in the
header (`--legacy` switches to fragmented MP4 there, since the moov cannot
be seeked back to the head of a pipe). AVIF written to stdout is assembled
in memory and dumped whole at the end (its container back-patches item
offsets, which needs a seekable sink), and piped APNG is assembled the same
way (its frame count is back-patched too); piped AV1 WebM streams like VP9
does — webify pre-extracts the AV1 sequence header the muxer needs up
front, since libaom only delivers it alongside the first encoded packet.
