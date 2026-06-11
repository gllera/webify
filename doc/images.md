# Image pipeline — WebP (the default)

Images are the equivalent of `cwebp -q 80 -m 6` / `gif2webp`: lossy quality
80 at the densest WebP method, original resolution (unless a `--max` box is
given, scaled with lanczos), alpha and EXIF orientation preserved (the
latter baked into the pixels), animated GIF → animated WebP looping forever.
`-q` is WebP quality ×10 (default 8 = cwebp's 80).

RGB-coded sources (PNG, GIF, BMP, TIFF) are handed to libwebp as RGB so its
own high-quality converter does the chroma subsampling, exactly like cwebp;
YUV-coded sources (JPEG, WebP, HEIC, AVIF) feed their planes through
directly.

## The lossless race (stills)

Single-frame RGB images are additionally tried as *lossless* WebP and the
smaller file wins — flat-color graphics usually come out both smaller and
pixel-perfect, photos keep the lossy result — and their lossy candidate
uses the sharp (iterative) RGB→YUV conversion, so stills are byte-identical
to `cwebp -q 80 -m 6 -sharp_yuv`: crisper text and hard edges for ~1% more
bytes. When lossless wins the race, the winner is re-encoded at maximum
effort (`cwebp -lossless -q 100 -m 6`) — measured a further 1-3% smaller;
photos, whose lossless candidate just lost, never pay that time.

## Animations

Animations keep libwebp's fast converter (sharp measured ~20% more bytes
there, like `gif2webp` without `-sharp_yuv`) and always stay lossy:
lossless animation measured 2.7× bigger on dithered photo-like GIFs.
Graphics and flat-color GIFs are the exception — they measured *smaller*
lossless (testsrc2 −19%) — but only single frames race today. The animated
encoder itself is as small as the format allows: audited against `gif2webp`
at equal settings, within ±1% on every fixture.

## Effort tier (`--fast`)

`--fast` uses cwebp's default method (`-m 4`), skips the sharp RGB→YUV
conversion and the lossless race (stills ~3-9× faster for ~5% more bytes;
heavily dithered animations up to 15× faster for ~11%). Images already run
their densest settings by default, so `--best` changes nothing for them.

Inputs: JPEG, PNG, WebP, GIF, BMP, TIFF, HEIC, AVIF.
