/*
 * webify — transcode any popular video file to VP9/Opus WebM, or any popular
 * image file to WebP (auto-detected). One option set covers both modes:
 * -q/--quality 0-10 (mapped to VP9 CRF for video, WebP quality for images)
 * and -m/--max [HxW | S][@F] (downscale to fit H px tall / W px wide — a
 * single number S bounds both, a missing side ("480x", "x854") is
 * unbounded — never upscale; @F is a video-only frame rate cap, off by
 * default).
 *
 * Everything is tuned for serving the result over the internet:
 *   - two-pass VP9 — only with a stats pass does libvpx use alt-ref frames
 *     and plan rates ahead, which measures 10-20% smaller at equal quality
 *     (the stats pass runs at cpu-used 4; it barely affects the stats).
 *     Piping changes nothing: piped video input is spooled to a temp file
 *     so the stats pass (and the HDR-peak peek) can rewind, and piped
 *     video output spools through a temp file so the seek index lands at
 *     the head — byte-identical to file i/o either way
 *   - cpu-used 1 / frame-parallel 0 by default (Google's VOD setting);
 *     --fast single-passes at cpu-used 4 (~4x faster, the classic crf-33
 *     look) and --best two-passes at cpu-used 0 + arnr-maxframes 15
 *     (measured 1-7% smaller than default at equal-or-better SSIM, ~75%
 *     more time); tiles/threads follow Google's VOD table for the width
 *   - the rate caps follow the input: >30fps output scales the budget up
 *     (Google's 60fps rows are ~1.7x their 30fps ones), and the source
 *     stream's own bitrate — weighted by how its codec compares to VP9 —
 *     caps the budget from above, so bits are never spent re-encoding
 *     compression artifacts more faithfully than the source stored them.
 *     When a stats pass runs, the source video and audio rates are
 *     *measured* from the packets it reads anyway — container headers lie
 *     or say nothing (mkv/webm declare no per-stream rates); --fast and
 *     --legacy keep the header-based caps
 *   - the seek index (cues) is written at the head of the file (faststart),
 *     piped output included (the temp-file spool above)
 *   - mono audio stays mono (48k) instead of being upmixed to stereo (64k),
 *     -q scales the Opus bitrate along with the video quality, and a lossy
 *     source's own rate caps it (Opus loses nothing at the rate the source
 *     itself managed with a weaker codec; measured like the video rate
 *     when a stats pass runs)
 *   - images use libwebp method 6 (cwebp -m 6), the densest WebP encoding;
 *     single-frame RGB images are also tried lossless and the smaller wins,
 *     and their lossy candidate uses sharp RGB->YUV (cwebp -sharp_yuv:
 *     crisper text/edges for ~1% more bytes; animations skip it — measured
 *     ~20% more bytes there)
 *   - EXIF orientation (frame side data) and display-matrix rotation are
 *     baked into the pixels, including the mirrored variants
 *   - interlaced video frames are deinterlaced (bwdif, always in the video
 *     chain): frames the decoder flags interlaced are rebuilt before any
 *     rotation/scaling touches their fields, progressive frames pass
 *     through untouched — combing both looks bad and wastes bits
 *   - HDR video (PQ/HLG) is tonemapped to SDR bt709 (zscale linearize +
 *     hable), with the source peak taken from in-stream SEI or container
 *     metadata (1000-nit fallback) since ffmpeg 8's zscale strips HDR side
 *     data before tonemap can read it; the final float -> 8-bit step is
 *     error-diffusion dithered or the smooth gradients tonemapping
 *     produces would band visibly
 *   - --next switches to the next-gen formats at the same visual targets:
 *     video becomes AV1/Opus WebM via libaom and every image becomes AVIF
 *     (animated GIF -> animated AVIF, alpha kept as the auxiliary stream).
 *     -q maps to the visually equivalent setting, not the same number:
 *     every mapping below is a piecewise fit of measured equal-SSIM points
 *     against the non---next pipeline (photo/noise/graphics corpora, biased
 *     a hair *below* parity by design — see doc/next-calibration.md), and
 *     the VP9-anchored rate budget is converted by the same codec-efficiency
 *     table that weights the source cap — so --next changes the file size,
 *     never the look. Everything stays 8-bit 4:2:0 — AV1 Main profile
 *     end-to-end, the only profile hardware decoders reliably implement
 *     (4:4:4 and film grain synthesis were tried and dropped on purpose:
 *     both trade decoder compatibility for bytes)
 *   - --legacy is the same idea pointed backwards: video becomes H.264/AAC
 *     MP4 (vendored x264) with the moov up front — piped output included
 *     via the temp-file spool (only its no-temp-file fallback still writes
 *     a fragmented MP4) — and every image becomes PNG
 *     (animated GIF -> APNG) — lossless by definition, so -q steers video
 *     only. The video -q mapping is the same equal-SSIM fit against the VP9
 *     pipeline (doc/legacy-calibration.md) and the rate caps are converted
 *     by the same codec-efficiency table (/0.8 — H.264 needs more bits for
 *     the look). Always single-pass: x264's two-pass targets a bitrate, not
 *     a quality, and its CRF mode already plans ahead (lookahead/mbtree)
 *   - progress on stderr when it is a terminal and the duration is known
 *
 * With no options, video is roughly the equivalent of two-pass:
 *   ffmpeg -i IN -vf scale=-2:480 -c:v libvpx-vp9 -crf 36 -b:v 530k
 *          -maxrate 778k -row-mt 1 -tile-columns 1 -threads 4
 *          -cpu-used 1 -deadline good -g 240 -pix_fmt yuv420p
 *          -c:a libopus -b:a 64k OUT.webm
 * except that smaller-than-480p input is not upscaled, and the bitrate caps
 * follow the job (anchored at 750k/1100k for 854x480 crf 33, the
 * single-pass default --fast keeps): they scale with the output pixel count, the
 * CRF target, the >30fps output frame rate, and are capped from above by
 * the source stream's own codec-weighted bitrate.
 *
 * Official libav* API only; structure follows FFmpeg's doc/examples/transcode.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

/* the release tag, baked in by the Docker build arg (build.yml passes
 * github.ref_name on tag builds); local/branch builds report "dev" */
#ifndef WEBIFY_VERSION
#define WEBIFY_VERSION "dev"
#endif

#define VIDEO_FILTERS "format=yuv420p" /* the scale step is built in init_video */
/* always first in the video chain: frames the decoder flagged interlaced are
 * deinterlaced, progressive frames pass through untouched (a zero-copy ref
 * forward), so this needs no detection step and handles mixed/telecined
 * streams per frame. It must run before transpose (rotation would turn the
 * fields into columns) and before scaling (fields are interleaved source
 * lines); send_frame keeps the frame rate */
#define DEINT_FILTER "bwdif=mode=send_frame:deint=interlaced,"
#define AUDIO_FILTERS "aresample=48000,aformat=sample_fmts=%s:channel_layouts=%s"
/* high-quality swscale conversions for the image pipeline (rounding flags are
 * no-ops where they don't apply; lanczos only kicks in for --max scaling) */
#define IMAGE_SWS "lanczos+accurate_rnd+full_chroma_int+full_chroma_inp"

/* -q/--quality and the --max box mean the same thing in both the video and
 * image pipelines, so callers don't care which one runs */
static struct {
    int    max_w;   /* 0 = unbounded */
    int    max_h;   /* 0 = unbounded (video: 480 when no box is given at all) */
    double quality; /* internal 0-100 scale (CLI -q is 0-10, x10 at parse),
                       higher = better; <0 = per-mode default */
    double max_fps; /* video only; 0 = keep every frame */
    int    effort;  /* -1 = --fast, 0 = default, +1 = --best; trades encode
                       time for bytes only — the -q quality target and the
                       --max box mean the same thing at every tier */
    int    next;    /* --next: the next-gen formats at the same visual target
                       per -q — AV1/Opus WebM for video, AVIF for images
                       (animated GIF -> animated AVIF) */
    int    legacy;  /* --legacy: the maximum-compatibility formats at the
                       same visual target per -q — H.264/AAC MP4 for video,
                       PNG for images (animated GIF -> APNG) */
} opt = { 0, 0, -1.0, 0, 0, 0, 0 };

/* progress on stderr, only when it is a terminal and the duration is known */
static struct {
    int         tty;
    int64_t     duration; /* of the input, in AV_TIME_BASE units */
    const char *label;    /* NULL = progress off */
    int         pct;      /* last percentage printed */
} prog = { 0, 0, NULL, -1 };

static void progress_tick(double t)
{
    int pct;

    if (!prog.tty || !prog.label || prog.duration <= 0)
        return;
    pct = (int)(t * 100.0 * AV_TIME_BASE / prog.duration);
    pct = FFMIN(FFMAX(pct, 0), 100);
    if (pct != prog.pct) {
        fprintf(stderr, "\r%s %3d%%", prog.label, pct);
        prog.pct = pct;
    }
}

static void progress_start(const char *label)
{
    prog.label = label;
    prog.pct   = -1; /* the first tick always prints */
}

static void progress_done(void)
{
    if (prog.tty && prog.label)
        fprintf(stderr, "\r\033[K"); /* leave a clean line behind */
    prog.label = NULL;
}

struct Pipe {
    int              in_index;    /* stream index in the input file  */
    int              out_index;   /* stream index in the output file */
    int              out_index_a; /* AVIF alpha: the auxiliary stream */
    AVCodecContext  *dec;
    AVCodecContext  *enc;
    AVCodecContext  *enc_a;     /* AVIF alpha plane encoder (or NULL)       */
    AVFilterGraph   *graph;
    AVFilterContext *src;
    AVFilterContext *sink;
    AVFrame         *dec_frame;
    AVFrame         *filt_frame;
    AVPacket        *enc_pkt;
    AVFrame         *first;     /* held 1st frame of a maybe-still image    */
    int              dual;      /* still + RGB: race lossy vs lossless      */
    int              rgb;       /* RGB-coded source (libwebp does the YUV)  */
    AVCodecContext  *enc_ll;    /* animated-WebP lossless race sibling enc  */
    AVFormatContext *ofmt_ll;   /* its in-memory muxer (dyn_buf)            */
    int              out_index_ll;
    int              prog;      /* report progress from this pipe's frames  */
    double           min_gap;   /* --max @F as a minimum pts spacing (s)    */
    double           next_keep; /* next pts (s) that won't be dropped       */
};

/* av_err2str's compound literal is not valid C++ */
static const char *err2str(int errnum)
{
    static char buf[AV_ERROR_MAX_STRING_SIZE];
    return av_make_error_string(buf, sizeof(buf), errnum);
}

/* libwebp_anim warns when it is given RGB, suggesting YUV instead — but
 * having libwebp do the conversion is exactly what we want (it matches
 * cwebp output bit for bit); drop that one advisory */
static void log_cb(void *ptr, int level, const char *fmt, va_list vl)
{
    if (fmt && strstr(fmt, "RGB-to-YUV"))
        return;
    av_log_default_callback(ptr, level, fmt, vl);
}

/* ---- stdin input -----------------------------------------------------------
 * Probing the first bytes lets us pick the right strategy: image files are
 * slurped whole into memory so demuxers that need a seekable input still work
 * (HEIC/AVIF item layout, TIFF's no-parser whole-file read); every other
 * piped input is spooled to an unlinked temp file so it rewinds like a file
 * (the stats pass, the HDR peek, end-indexed containers). Streaming the
 * probed prefix ahead of the live pipe survives only as the fallback when no
 * temp file can be created. */

#define PREFIX_SIZE (64 * 1024)
#define IO_BUFSIZE  (64 * 1024)

static const char *const image_demuxers[] = { "image2", "png_pipe", "jpeg_pipe",
    "bmp_pipe", "tiff_pipe", "webp_pipe", "gif", NULL };

static int is_image_demuxer(const char *name)
{
    for (int i = 0; image_demuxers[i]; i++)
        if (!strcmp(name, image_demuxers[i]))
            return 1;
    return 0;
}

struct StdinIO {
    AVIOContext *src;     /* the real pipe; closed early when slurping   */
    AVIOContext *pb;      /* what the demuxer reads                      */
    uint8_t     *buf;     /* probed prefix (video) or whole file (image) */
    size_t       size, pos;
    int          fd = -1; /* unlinked temp file when seeking is needed   */
};

static int mem_read(void *opaque, uint8_t *dst, int n)
{
    StdinIO *io = (StdinIO *)opaque;

    if (io->pos >= io->size)
        return AVERROR_EOF;
    n = (int)FFMIN((size_t)n, io->size - io->pos);
    memcpy(dst, io->buf + io->pos, n);
    io->pos += n;
    return n;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence)
{
    StdinIO *io = (StdinIO *)opaque;

    if (whence == AVSEEK_SIZE)
        return io->size;
    if (whence == SEEK_CUR)
        off += io->pos;
    else if (whence == SEEK_END)
        off += io->size;
    if (off < 0 || (size_t)off > io->size)
        return AVERROR(EINVAL);
    io->pos = off;
    return off;
}

static int file_read(void *opaque, uint8_t *dst, int n)
{
    StdinIO *io = (StdinIO *)opaque;
    ssize_t r = read(io->fd, dst, n);

    if (r == 0)
        return AVERROR_EOF;
    return r < 0 ? AVERROR(errno) : (int)r;
}

static int64_t file_seek(void *opaque, int64_t off, int whence)
{
    StdinIO *io = (StdinIO *)opaque;

    if (whence == AVSEEK_SIZE) {
        struct stat st;
        return fstat(io->fd, &st) ? AVERROR(errno) : st.st_size;
    }
    off = lseek(io->fd, off, whence);
    return off < 0 ? AVERROR(errno) : off;
}

/* replay the probed prefix, then read straight from the pipe */
static int pre_read(void *opaque, uint8_t *dst, int n)
{
    StdinIO *io = (StdinIO *)opaque;

    if (io->pos < io->size)
        return mem_read(opaque, dst, n);
    return avio_read_partial(io->src, dst, n);
}

static const AVInputFormat *probe_format(uint8_t *buf, int size)
{
    AVProbeData pd = {};

    pd.filename = "";
    pd.buf      = buf;
    pd.buf_size = size;
    return av_probe_input_format(&pd, 1);
}

static int probe_is_image(const AVInputFormat *fmt, const uint8_t *buf, int size)
{
    if (!fmt)
        return 0;
    if (is_image_demuxer(fmt->name))
        return 1;
    if (size >= 12 && !memcmp(buf + 4, "ftyp", 4)) { /* HEIF/AVIF brands */
        static const char *const brands[] = { "heic", "heix", "hevc", "hevx",
                                              "mif1", "msf1", "avif", "avis",
                                              NULL };
        for (int i = 0; brands[i]; i++)
            if (!memcmp(buf + 8, brands[i], 4))
                return 1;
    }
    return 0;
}

/* the ffmpeg-CLI "pipe:" URL convention (main maps '-' onto it) */
static int is_pipe(const char *path)
{
    return !strncmp(path, "pipe:", 5);
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return AVERROR(errno);
        }
        p += w;
        n -= w;
    }
    return 0;
}

/* every webify temp file comes from here: webify-XXXXXX in $TMPDIR or
 * /tmp (doc/piping.md documents that name as the SIGKILL-leftover caveat).
 * Returns the mkstemp fd, path gets the name */
static int make_temp(char *path, size_t len)
{
    const char *dir = getenv("TMPDIR");

    snprintf(path, len, "%s/webify-XXXXXX", dir && *dir ? dir : "/tmp");
    return mkstemp(path);
}

/* forward the whole pipe to a temp file, unlinked right away so it is gone
 * when the process exits no matter how it exits */
static int spool_to_file(StdinIO *io)
{
    char path[512];
    uint8_t chunk[IO_BUFSIZE];
    int n, ret;

    if ((io->fd = make_temp(path, sizeof(path))) < 0)
        return AVERROR(errno);
    unlink(path);

    if ((ret = write_all(io->fd, io->buf, io->size)) < 0) /* probed prefix */
        return ret;
    while ((n = avio_read(io->src, chunk, (int)sizeof(chunk))) > 0)
        if ((ret = write_all(io->fd, chunk, n)) < 0)
            return ret;
    if (n < 0 && n != AVERROR_EOF)
        return n;

    avio_closep(&io->src);
    av_freep(&io->buf);
    io->size = io->pos = 0;
    return lseek(io->fd, 0, SEEK_SET) < 0 ? AVERROR(errno) : 0;
}

/* stream a finished temp output file to stdout (the piped-output spool:
 * the muxer wrote a regular seekable file, the pipe gets its bytes now) */
static int drain_file_to_stdout(const char *path)
{
    uint8_t chunk[IO_BUFSIZE];
    ssize_t n;
    int ret = 0, fd = open(path, O_RDONLY);

    if (fd < 0)
        return AVERROR(errno);
    while ((n = read(fd, chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ret = AVERROR(errno);
            break;
        }
        if ((ret = write_all(STDOUT_FILENO, chunk, (size_t)n)) < 0)
            break;
    }
    close(fd);
    return ret;
}

/* open a demuxer on a custom avio context (the stdin paths) */
static int open_with_pb(AVFormatContext **ifmt, AVIOContext *pb)
{
    if (!(*ifmt = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    (*ifmt)->pb = pb;
    return avformat_open_input(ifmt, "", NULL, NULL);
}

static int open_stdin_input(const char *in_path, AVFormatContext **ifmt, StdinIO *io)
{
    uint8_t *iobuf;
    const AVInputFormat *fmt;
    int (*read_cb)(void *, uint8_t *, int) = pre_read;
    int64_t (*seek_cb)(void *, int64_t, int) = NULL;
    int n, ret, image;

    if ((ret = avio_open(&io->src, in_path, AVIO_FLAG_READ)) < 0)
        return ret;
    if (!(io->buf = (uint8_t *)av_malloc(PREFIX_SIZE + AVPROBE_PADDING_SIZE)))
        return AVERROR(ENOMEM);

    n = avio_read(io->src, io->buf, PREFIX_SIZE);
    if (n < 0 && n != AVERROR_EOF)
        return n;
    io->size = FFMAX(n, 0);
    memset(io->buf + io->size, 0, AVPROBE_PADDING_SIZE);
    fmt   = probe_format(io->buf, (int)io->size);
    image = probe_is_image(fmt, io->buf, (int)io->size);

    if (image) { /* images are small: slurp so the demuxer can seek */
        size_t cap = PREFIX_SIZE;

        for (;;) {
            if (io->size == cap) {
                uint8_t *nb = (uint8_t *)
                    av_realloc(io->buf, (cap *= 2) + AVPROBE_PADDING_SIZE);
                if (!nb)
                    return AVERROR(ENOMEM);
                io->buf = nb;
            }
            n = avio_read(io->src, io->buf + io->size, (int)(cap - io->size));
            if (n == AVERROR_EOF)
                break;
            if (n < 0)
                return n;
            io->size += n;
        }
        avio_closep(&io->src);
    } else if ((ret = spool_to_file(io)) < 0) {
        /* every piped video is spooled so the pipe changes nothing about
         * the output: the stats pass and the HDR-peak peek can always
         * rewind, exactly as with a file argument (and containers that
         * outright need a seekable input — mp4/mov without faststart,
         * AVI — just work). Images are slurped above for the same reason */
        if (io->fd >= 0) /* spool started; the pipe is partly consumed */
            return ret;
        av_log(NULL, AV_LOG_WARNING, "cannot spool piped input to a temp "
               "file (%s), streaming instead\n", err2str(ret));
    }

    if (image) {
        read_cb = mem_read;
        seek_cb = mem_seek;
    } else if (io->fd >= 0) {
        read_cb = file_read;
        seek_cb = file_seek;
    }
    if (!(iobuf = (uint8_t *)av_malloc(IO_BUFSIZE)))
        return AVERROR(ENOMEM);
    io->pb = avio_alloc_context(iobuf, IO_BUFSIZE, 0, io, read_cb, NULL, seek_cb);
    if (!io->pb) {
        av_free(iobuf);
        return AVERROR(ENOMEM);
    }
    return open_with_pb(ifmt, io->pb);
}

static void close_stdin_io(StdinIO *io)
{
    avio_closep(&io->src);
    if (io->pb) {
        av_freep(&io->pb->buffer);
        avio_context_free(&io->pb);
    }
    av_freep(&io->buf);
    if (io->fd >= 0)
        close(io->fd); /* already unlinked: this removes the temp file */
}

static int open_decoder(AVFormatContext *ifmt, int stream_index, AVCodecContext **out)
{
    AVStream *st = ifmt->streams[stream_index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *dec;
    int ret;

    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "no decoder for '%s' in this build\n",
               avcodec_get_name(st->codecpar->codec_id));
        return AVERROR_DECODER_NOT_FOUND;
    }
    if (!(dec = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_to_context(dec, st->codecpar)) < 0) {
        avcodec_free_context(&dec);
        return ret;
    }
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 0; /* auto */
    if (dec->codec_type == AVMEDIA_TYPE_VIDEO)
        dec->framerate = av_guess_frame_rate(ifmt, st, NULL);
    if ((ret = avcodec_open2(dec, codec, NULL)) < 0) {
        avcodec_free_context(&dec);
        return ret;
    }
    *out = dec;
    return 0;
}

static int alloc_pipe_buffers(Pipe *p)
{
    p->dec_frame  = av_frame_alloc();
    p->filt_frame = av_frame_alloc();
    p->enc_pkt    = av_packet_alloc();
    return (p->dec_frame && p->filt_frame && p->enc_pkt) ? 0 : AVERROR(ENOMEM);
}

static void free_pipe(Pipe *p)
{
    avcodec_free_context(&p->dec);
    if (p->enc)
        av_freep(&p->enc->stats_in); /* stats_in is owned by the user */
    avcodec_free_context(&p->enc);
    avcodec_free_context(&p->enc_a);
    avcodec_free_context(&p->enc_ll); /* ofmt_ll is freed by webify_run */
    avfilter_graph_free(&p->graph);
    av_frame_free(&p->dec_frame);
    av_frame_free(&p->filt_frame);
    av_frame_free(&p->first);
    av_packet_free(&p->enc_pkt);
}

/* buffer/abuffer "in" -> parsed filter spec -> buffersink/abuffersink "out";
 * sws_opts (e.g. "flags=...") applies to auto-inserted format conversions */
static int init_graph(Pipe *p, const char *src_name, const char *src_args,
                      const char *sink_name, const char *spec, const char *sws_opts)
{
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    int ret;

    p->graph = avfilter_graph_alloc();
    if (!inputs || !outputs || !p->graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (sws_opts && !(p->graph->scale_sws_opts = av_strdup(sws_opts))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avfilter_graph_create_filter(&p->src, avfilter_get_by_name(src_name),
                                       "in", src_args, NULL, p->graph);
    if (ret < 0)
        goto end;
    ret = avfilter_graph_create_filter(&p->sink, avfilter_get_by_name(sink_name),
                                       "out", NULL, NULL, p->graph);
    if (ret < 0)
        goto end;

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = p->src;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = p->sink;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(p->graph, spec, &inputs, &outputs, NULL)) < 0)
        goto end;
    ret = avfilter_graph_config(p->graph, NULL);
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

/* Rotation/mirroring stored as a display matrix — by phones on the stream,
 * or converted from EXIF orientation by the image decoders onto the frame.
 * The matrix-to-filter mapping is the same as the ffmpeg CLI's autorotate,
 * including the mirrored variants (EXIF orientations 2/4/5/7). */
static void build_rotate(const int32_t *m, char *buf, size_t size)
{
    double theta;

    buf[0] = '\0';
    if (!m)
        return;
    theta = -av_display_rotation_get(m);
    theta -= 360 * floor(theta / 360 + 0.9 / 360);

    if (fabs(theta - 90) < 1.0) {
        av_strlcpy(buf, m[3] > 0 ? "transpose=cclock_flip,"
                                 : "transpose=clock,", size);
    } else if (fabs(theta - 270) < 1.0) {
        av_strlcpy(buf, m[3] < 0 ? "transpose=clock_flip,"
                                 : "transpose=cclock,", size);
    } else if (fabs(theta - 180) < 1.0 || fabs(theta) < 1.0) {
        if (m[0] < 0)
            av_strlcat(buf, "hflip,", size);
        if (m[4] < 0)
            av_strlcat(buf, "vflip,", size);
    }
}

static int is_hdr_trc(enum AVColorTransferCharacteristic trc)
{
    return trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67;
}

/* tile-columns log2 follows Google's VOD table by output width; threads run
 * at double the column count, which row-mt keeps busy:
 * <512 -> 0/2, 480p-ish -> 1/4, 720-1080p -> 2/8, 1440p+ -> 3/16 */
static int width_tlog(int w)
{
    return w >= 2560 ? 3 : w >= 1280 ? 2 : w >= 512 ? 1 : 0;
}

static int width_threads(int w)
{
    return 2 << width_tlog(w);
}

/* the source video/audio rates measured during the stats pass (bits/s,
 * 0 = not measured): the stats pass reads every packet anyway, so it
 * counts the truth. Single-pass tiers (--fast, --legacy) never measure
 * and keep the header-based caps; pass 1 itself also runs on those (the
 * measurement happens during it — libvpx's first pass collects content
 * stats, not rate plans, so the mismatch is harmless) */
static int64_t measured_video_rate, measured_audio_rate;

/* the input video stream's bitrate, best effort: the rate measured during
 * the stats pass beats any header — headers lie or say nothing (mkv/webm
 * declare no per-stream rates at all) — else the stream's own header
 * value, else the container average minus the rates the other streams
 * declare, which overestimates the video share (mux overhead, undeclared
 * streams), erring on the side of a looser cap */
static int64_t source_video_rate(const AVFormatContext *ifmt, int vidx)
{
    int64_t rate;

    if (measured_video_rate > 0)
        return measured_video_rate;
    rate = ifmt->streams[vidx]->codecpar->bit_rate;
    if (rate <= 0 && ifmt->bit_rate > 0) {
        rate = ifmt->bit_rate;
        for (unsigned i = 0; i < ifmt->nb_streams; i++)
            if ((int)i != vidx && ifmt->streams[i]->codecpar->bit_rate > 0)
                rate -= ifmt->streams[i]->codecpar->bit_rate;
    }
    return rate;
}

/* same idea for the audio stream: measured rate first, else the header
 * value (mkv/webm declare none, which used to silently disable the audio
 * cap — a 26.5k AAC track was re-encoded at the full 48k anchor) */
static int64_t source_audio_rate(const AVStream *ist)
{
    return measured_audio_rate > 0 ? measured_audio_rate
                                   : ist->codecpar->bit_rate;
}

/* does the frame actually use transparency? checks the palette for
 * palettized formats and scans the alpha component otherwise — an image
 * whose alpha channel is fully opaque doesn't really have one */
static int frame_has_real_alpha(const AVFrame *fr)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((AVPixelFormat)fr->format);

    if (!d)
        return 0;
    if (d->flags & AV_PIX_FMT_FLAG_PAL) {
        const uint32_t *pal = (const uint32_t *)fr->data[1];

        for (int i = 0; i < 256; i++)
            if ((pal[i] >> 24) != 0xFFu)
                return 1;
        return 0;
    }
    if (!(d->flags & AV_PIX_FMT_FLAG_ALPHA))
        return 0;
    {
        const AVComponentDescriptor *c = &d->comp[d->nb_components - 1];
        const unsigned max = (1u << c->depth) - 1;

        for (int y = 0; y < fr->height; y++) {
            const uint8_t *px = fr->data[c->plane] +
                                (ptrdiff_t)y * fr->linesize[c->plane] + c->offset;

            for (int x = 0; x < fr->width; x++, px += c->step) {
                unsigned v = c->depth > 8
                           ? (d->flags & AV_PIX_FMT_FLAG_BE ? AV_RB16(px)
                                                            : AV_RL16(px))
                           : *px;

                if (((v >> c->shift) & max) != max)
                    return 1;
            }
        }
    }
    return 0;
}

/* is the frame achromatic — every pixel R==G==B, or an already-gray format?
 * Lets a monochrome --legacy source ship as a gray/ya8 PNG instead of a 3x
 * larger rgb24/rgba one (measured -34..-47% bytes, pixels byte-identical
 * after decode). Conservative by design: palettized formats check the
 * palette, RGB formats scan the pixels, and anything we can't cheaply prove
 * gray (YUV, exotic) returns 0 — so color output never regresses. */
static int frame_is_grayscale(const AVFrame *fr)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((AVPixelFormat)fr->format);

    if (!d)
        return 0;
    if (d->flags & AV_PIX_FMT_FLAG_PAL) {
        const uint32_t *pal = (const uint32_t *)fr->data[1];

        for (int i = 0; i < 256; i++) {
            uint32_t c = pal[i];

            if (((c >> 16) & 0xFFu) != ((c >> 8) & 0xFFu) ||
                ((c >> 8) & 0xFFu) != (c & 0xFFu))
                return 0;
        }
        return 1;
    }
    if (!(d->flags & AV_PIX_FMT_FLAG_RGB)) /* gray = one luma plane (+ alpha?) */
        return (d->nb_components - !!(d->flags & AV_PIX_FMT_FLAG_ALPHA)) == 1;
    {
        const AVComponentDescriptor *c = d->comp; /* [0]=R [1]=G [2]=B */
        const unsigned be = d->flags & AV_PIX_FMT_FLAG_BE;

        for (int y = 0; y < fr->height; y++) {
            const uint8_t *r = fr->data[c[0].plane] +
                               (ptrdiff_t)y * fr->linesize[c[0].plane] + c[0].offset;
            const uint8_t *g = fr->data[c[1].plane] +
                               (ptrdiff_t)y * fr->linesize[c[1].plane] + c[1].offset;
            const uint8_t *b = fr->data[c[2].plane] +
                               (ptrdiff_t)y * fr->linesize[c[2].plane] + c[2].offset;

            for (int x = 0; x < fr->width; x++,
                 r += c[0].step, g += c[1].step, b += c[2].step) {
                unsigned vr = c[0].depth > 8 ? (be ? AV_RB16(r) : AV_RL16(r)) : *r;
                unsigned vg = c[1].depth > 8 ? (be ? AV_RB16(g) : AV_RL16(g)) : *g;
                unsigned vb = c[2].depth > 8 ? (be ? AV_RB16(b) : AV_RL16(b)) : *b;

                vr = (vr >> c[0].shift) & ((1u << c[0].depth) - 1);
                vg = (vg >> c[1].shift) & ((1u << c[1].depth) - 1);
                vb = (vb >> c[2].shift) & ((1u << c[2].depth) - 1);
                if (vr != vg || vg != vb)
                    return 0;
            }
        }
    }
    return 1;
}

/* the source's peak brightness from HDR metadata payloads, in the 100-nit
 * units the tonemap chain wants: MaxCLL when present, else the mastering
 * display's max luminance, else 0 (the caller picks the fallback) */
static double peak_from_metadata(const uint8_t *cll, const uint8_t *mdm)
{
    if (cll && ((const AVContentLightMetadata *)cll)->MaxCLL > 0)
        return ((const AVContentLightMetadata *)cll)->MaxCLL / 100.0;
    if (mdm && ((const AVMasteringDisplayMetadata *)mdm)->has_luminance)
        return av_q2d(((const AVMasteringDisplayMetadata *)mdm)
                      ->max_luminance) / 100.0;
    return 0;
}

/* a stream's coded (container-level) side data payload, NULL when absent */
static const uint8_t *coded_sd(const AVStream *st, enum AVPacketSideDataType t)
{
    const AVPacketSideData *sd =
        av_packet_side_data_get(st->codecpar->coded_side_data,
                                st->codecpar->nb_coded_side_data, t);
    return sd ? sd->data : NULL;
}

/* EXIF rotation and HDR peak brightness (SEI) only surface as side data of a
 * *decoded frame*, and the filter graph (with its transpose/tonemap steps)
 * must exist before frames flow; so when the input can rewind — images
 * always can: slurped, spooled or real files — decode the first frame just
 * to look at it, and let the caller rewind. With detect_anim (every image
 * input, whose still/animation encoder setup differs before any frame flows)
 * it keeps decoding to learn whether a second frame proves an animation;
 * need_alpha (--next/--legacy) makes it also scan for real transparency and
 * decode until both are settled, while the default pipeline stops at the
 * animation verdict. On the --legacy path it also notes whether the
 * (single-frame) source is monochrome, so a gray PNG can stand in for a
 * 3x-larger rgb24 one. */
static struct {
    int32_t m[9]; int set; double peak; int animated; int alpha; int gray;
} peeked;

static void peek_first_frame(AVFormatContext *ifmt, int vidx, int detect_anim,
                             int need_alpha)
{
    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int ret, flushed = 0, frames = 0;

    peeked = {};
    if (!pkt || !fr || open_decoder(ifmt, vidx, &dec) < 0)
        goto end;
    for (;;) {
        if (!flushed && (ret = av_read_frame(ifmt, pkt)) >= 0) {
            ret = pkt->stream_index == vidx ? avcodec_send_packet(dec, pkt) : 0;
            av_packet_unref(pkt);
        } else if (!flushed) { /* tiny input: flush to get the frame out */
            flushed = 1;
            ret = avcodec_send_packet(dec, NULL);
        } else {
            break;
        }
        if (ret < 0)
            break;
        while (avcodec_receive_frame(dec, fr) >= 0) {
            if (++frames == 1) {
                const AVFrameSideData *sd =
                    av_frame_get_side_data(fr, AV_FRAME_DATA_DISPLAYMATRIX);
                const AVFrameSideData *cll =
                    av_frame_get_side_data(fr, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
                const AVFrameSideData *mdm =
                    av_frame_get_side_data(fr, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);

                if (sd && sd->size >= 9 * sizeof(int32_t)) {
                    memcpy(peeked.m, sd->data, sizeof(peeked.m));
                    peeked.set = 1;
                }
                peeked.peak = peak_from_metadata(cll ? cll->data : NULL,
                                                 mdm ? mdm->data : NULL);
            }
            if (detect_anim && need_alpha && !peeked.alpha &&
                frame_has_real_alpha(fr))
                peeked.alpha = 1;
            /* monochrome stills only (gray is used below gated on !animated):
             * scan the first frame; a second frame makes it an animation and
             * the legacy path stays rgb24/rgba regardless */
            if (detect_anim && opt.legacy && frames == 1)
                peeked.gray = frame_is_grayscale(fr);
            av_frame_unref(fr);
            if (frames >= 2)
                peeked.animated = 1;
            /* the default pipeline (need_alpha == 0) only wants the
             * still/animation verdict, so a second frame settles it; the
             * --next/--legacy paths keep decoding until alpha is known too */
            if (peeked.animated && (peeked.alpha || !need_alpha))
                goto end; /* every question answered */
        }
        if (frames > 0 && !detect_anim)
            break; /* the first frame settles it */
    }
end:
    avcodec_free_context(&dec);
    av_packet_free(&pkt);
    av_frame_free(&fr);
}

/* Image inputs become WebP instead of WebM. Plain images arrive via the image
 * pipe demuxers (or image2 when opened by file name); HEIC/AVIF arrive via the
 * mov demuxer as a lone one-frame video stream. */
static int input_is_image(const AVFormatContext *ifmt, int vidx, int aidx)
{
    if (is_image_demuxer(ifmt->iformat->name))
        return 1;
    if (ifmt->streams[vidx]->disposition & AV_DISPOSITION_STILL_IMAGE)
        return 1;
    /* exactly 1: heif/avif items report nb_frames = 1, while 0 means the
     * demuxer doesn't know the count (fragmented mp4) — that is video */
    return aidx < 0 && ifmt->streams[vidx]->nb_frames == 1 &&
           !strncmp(ifmt->iformat->name, "mov,", 4);
}

/* one frame in -> the encoder's single output packet (flushing it) */
static int encode_full(AVCodecContext *enc, AVFrame *frame, AVPacket *out)
{
    int ret = avcodec_send_frame(enc, frame);

    if (ret >= 0)
        ret = avcodec_send_frame(enc, NULL);
    if (ret < 0 && ret != AVERROR_EOF)
        return ret;
    return avcodec_receive_packet(enc, out);
}

/* the geometry/timing/flags fields every secondary encoder inherits from an
 * already-configured one (the AV1 priming clone, the AVIF alpha encoder,
 * the still-race re-encodes); callers override what genuinely differs */
static void copy_enc_geometry(AVCodecContext *dst, const AVCodecContext *src)
{
    dst->width               = src->width;
    dst->height              = src->height;
    dst->pix_fmt             = src->pix_fmt;
    dst->time_base           = src->time_base;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->flags               = src->flags;
}

/* libaom only delivers the AV1 sequence header alongside its first encoded
 * packet (aomedia bug #2208), but the webm muxer writing to a pipe needs it
 * in CodecPrivate when the header goes out — seekable outputs get
 * back-patched later, pipes error out. Since the piped-output spool, only
 * its no-temp-file fallback still writes WebM to a true pipe, but the
 * priming costs one black frame, so it simply always runs. The sequence
 * header depends on the
 * configuration, not the pixels: encode one black frame on a clone of the
 * just-configured encoder and lift the extradata its wrapper extracts.
 * Best-effort by design — on failure file outputs still self-heal. */
static void prime_av1_extradata(const AVCodecContext *base, const AVCodec *codec,
                                AVDictionary *opts /* consumed */,
                                uint8_t **extra, int *extra_size)
{
    AVCodecContext *c = avcodec_alloc_context3(codec);
    AVFrame *fr = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    const uint8_t *sd = NULL;
    size_t sd_size = 0;

    *extra      = NULL;
    *extra_size = 0;
    if (!c || !fr || !pkt)
        goto end;
    copy_enc_geometry(c, base);
    c->framerate    = base->framerate;
    c->bit_rate     = base->bit_rate;
    c->rc_max_rate  = base->rc_max_rate;
    c->gop_size     = base->gop_size;
    c->thread_count = base->thread_count;
    c->flags        = AV_CODEC_FLAG_GLOBAL_HEADER; /* no pass flags */
    if (avcodec_open2(c, codec, &opts) < 0)
        goto end;
    fr->format = c->pix_fmt;
    fr->width  = c->width;
    fr->height = c->height;
    fr->pts    = 0;
    if (av_frame_get_buffer(fr, 0) < 0)
        goto end;
    memset(fr->data[0], 16,  (size_t)fr->linesize[0] * fr->height);
    memset(fr->data[1], 128, (size_t)fr->linesize[1] * ((fr->height + 1) / 2));
    memset(fr->data[2], 128, (size_t)fr->linesize[2] * ((fr->height + 1) / 2));
    if (encode_full(c, fr, pkt) < 0)
        goto end;
    sd = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &sd_size);
    if (!sd && c->extradata_size > 0) {
        sd      = c->extradata;
        sd_size = c->extradata_size;
    }
    if (sd && sd_size &&
        (*extra = (uint8_t *)av_mallocz(sd_size + AV_INPUT_BUFFER_PADDING_SIZE))) {
        memcpy(*extra, sd, sd_size);
        *extra_size = (int)sd_size;
    }
end:
    avcodec_free_context(&c);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    av_dict_free(&opts);
}

/* bytes accumulated over a packet-dts span -> bits/s; 0 when the span is
 * missing or too short to be meaningful (1-2 packet streams) */
static int64_t span_rate(int64_t bytes, const int64_t t[2], AVRational tb)
{
    double s = t[0] == AV_NOPTS_VALUE ? 0 : (t[1] - t[0]) * av_q2d(tb);

    return s >= 0.1 ? (int64_t)(bytes * 8 / s) : 0;
}

/* ==== Calibration ============================================================
 * Every constant from here to the end of the section is a measurement, not
 * a design choice: the --next/--legacy CRF curves are equal-SSIM fits
 * against the default VP9/Opus/WebP pipeline (method and data in
 * doc/next-calibration.md and doc/legacy-calibration.md), and the rate-cap
 * anchors extend Google's published VP9 VOD settings. After bumping a
 * vendored encoder, changing the filter chains, or touching a --fast/--best
 * tier setting, re-verify parity with calibrate.sh (./fixtures.sh fetches
 * the pinned fixtures) and re-fit what drifted — always on real content,
 * never synthetics alone (every synthetic-only fit here has had to be
 * redone). Last re-fit: 2026-06-11 (anim curve + fast stills).
 * ========================================================================== */

/* How far a bit goes in the source codec relative to VP9. Reproducing what
 * an older codec stored needs fewer VP9 bits than the source spent (its bits
 * bought less quality); matching AV1 needs more. Caps the rate budget from
 * above and converts the VP9-anchored budget for --next/--legacy, so the
 * tiers can stay coarse. */
static double codec_weight(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AV1:    return 1.3;
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_HEVC:   return 1.0;
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_THEORA: return 0.8;
    default:                 return 0.6; /* mpeg1/2/4, wmv/vc-1, h.263, ... */
    }
}

/* -q with the image pipelines' default applied (cwebp's 80) */
static double image_quality(void)
{
    return opt.quality < 0 ? 80.0 : opt.quality;
}

/* --legacy maps -q onto the look the *default* pipeline would produce; with
 * no -q that look depends on whether VP9 would have run two passes (crf 36)
 * or one (crf 33) — webify_run records its two-pass decision here */
static int vp9_ref_twopass;

/* the VP9 CRF for a -q: 0-10 maps linearly onto CRF 63-0. With no -q,
 * two-pass defaults to crf 36: measured against the old single-pass crf 33
 * it is slightly *better* on SSIM/PSNR while ~8-20% smaller, because
 * two-pass actually reaches its quality target. Single-pass (--fast, or the
 * rare unspoolable pipe) keeps the classic crf 33 (= -q 4.8). */
static int vp9_video_crf(int twopass)
{
    if (opt.quality < 0)
        return twopass ? 36 : 33;
    return (int)lrint((100.0 - opt.quality) * 63.0 / 100.0);
}

/* the AV1 CRF that buys the same look as a given VP9 CRF: libaom beats
 * libvpx by a growing margin as CRF rises (equal SSIM measured at +4 around
 * crf 44, +2 around 33, none at or below 20), so the CRF is nudged up to
 * return that surplus as bytes instead of quality the VP9 output never had.
 * The fast tier needs 4 more: single-pass cpu-used 6 loses far less quality
 * than vpx's fast tier does, measured at equal SSIM against it
 * (doc/next-calibration.md). */
static int av1_video_crf(int vp9crf)
{
    int c = vp9crf + (vp9crf > 20 ? (vp9crf - 20) / 6 : 0);

    if (opt.effort < 0)
        c += 4;
    return FFMIN(c, 63);
}

/* the calibrated x264 CRF that buys the same look as a given VP9 CRF: a
 * linear fit of measured equal-SSIM points against the VP9 pipeline at the
 * shipped preset (veryslow), rounded toward the lower-quality side like the
 * --next curves (doc/legacy-calibration.md has the data). The line is much
 * flatter than the two CRF scales suggest (x264's quality-per-CRF-step
 * changes faster than libvpx's). --fast adds 1: preset fast holds quality
 * at a fixed CRF far better than vpx's fast tier drops it (measured -.002
 * vs -.010), and +1 lands just below the vpx-fast look it must match */
static int legacy_video_crf(int vp9crf)
{
    int c = (int)lrint(0.34 * vp9crf + 16.5);

    if (opt.effort < 0)
        c += 1;
    return av_clip(c, 0, 51);
}

/* the still-image AVIF CRF for a -q: equal-SSIM fit against the WebP
 * pipeline across photo/noise/graphics fixtures, rounded to the
 * slightly-lower-quality side so the size win is never paid back as quality
 * (doc/next-calibration.md has the data). Tracks cwebp's scale: near-linear
 * up to q 80, then cwebp's top end buys disproportionate quality and the
 * crf has to dive (q 80 -> crf 28, q 95 -> 12, q 100 -> 6). --fast rides 4
 * lower: cwebp's quick settings (-m 4) cost bytes but not quality, while
 * allintra cpu-used 6 drops ~.01-.02 SSIM on photographic detail at the
 * same CRF — the fast look needs the bits back (measured on the Kodak
 * fixtures; synthetic content never showed it; re-fit 2026-06-11). */
static int avif_still_crf(double q)
{
    double c = q <= 80 ? 52.0 - 0.30 * q : 28.0 - 1.10 * (q - 80.0);

    if (opt.effort < 0)
        c -= 4.0;
    return (int)lrint(FFMAX(c, 0.0));
}

/* the animated-AVIF CRF for a -q: animated WebP is far weaker than stills
 * WebP, so the curve sits much higher — but its equal point spreads
 * enormously by content (q 80: live action crf 33, dithered noise 49,
 * synthetic graphics 60), so it tracks the *live-action* equal points
 * (52 at q 50 -> 36 at q 80 -> 18 at q 95, ceiling 63 below q ~29): a mean
 * fit would pay the size win back as visible loss on faces, while graphics
 * merely overdeliver at sizes that stay under ~0.2x of animated WebP anyway
 * (-79..-89% at the default -q, was -96% under the old synthetic-only fit;
 * re-fit 2026-06-11 on real content). --fast rides 4 CRF lower: cpu-used 6
 * loses ~.0045 SSIM to the default speed at the same CRF on live action,
 * but animated WebP's fast tier loses nothing (-m 4 only costs bytes), so
 * the fast look needs the bits back (-4 measured back in band). Not under
 * the 63 ceiling though — there the default tier already sits below parity
 * on purpose. */
static int avif_anim_crf(double q)
{
    int c = (int)lrint(q <= 80 ? FFMIN(63.0, 78.7 - 0.533 * q)
                               : 36.0 - 1.20 * (q - 80.0));

    if (opt.effort < 0 && c < 63)
        c = FFMAX(c - 4, 0);
    return c;
}

/* The constrained-quality rate caps start from Google's published 480p VOD
 * numbers (750k avg / 1100k max at 854x480 crf 33) and follow the job: the
 * output pixel count, the CRF target (VP9 bitrate roughly doubles per 6 CRF
 * steps), and the output frame rate above 30 (Google's 50-60fps rows run
 * ~1.7x their 24-30fps ones at the same crf, hence x(fps/30)^0.75).
 * Google's minrate is dropped on purpose: a rate *floor* can only add bytes
 * at a fixed quality target (measured 0-2% smaller, SSIM unchanged). The
 * source stream's own bitrate, weighted by its codec's efficiency vs VP9,
 * caps the budget from above: past the source's own spend, bits only
 * reproduce its artifacts more faithfully (measured 27% smaller on a
 * 400kbps input, SSIM vs that source -0.0025). The ceiling is deliberately
 * NOT rescaled when downscaling or dropping frames — those only discard
 * source information (and downscaling washes artifacts out), so the full
 * source rate stays a valid, conservative ceiling for any smaller
 * rendition. --next/--legacy convert the VP9-anchored result by the same
 * codec-efficiency table that weights the source cap (1.3x the quality per
 * bit for AV1, 0.8x for H.264 — note an AV1/H.264 source then caps a
 * --next/--legacy job at exactly its own rate), so together with the CRF
 * remaps above those modes change the size, never the look.
 *
 * Returns the budget's scale factor relative to the anchors; crf is on the
 * VP9 scale, except --next passes its shifted CRF so the budget tracks the
 * quality level actually encoded. */
static const double rate_anchor_avg = 750000, rate_anchor_max = 1100000;

static double rate_budget_scale(int crf, int w, int h, double ofps,
                                int64_t src, enum AVCodecID src_codec)
{
    double f = ((double)w * h) / (854.0 * 480.0) * exp2((33 - crf) / 6.0);

    if (ofps > 30)
        f *= pow(ofps / 30.0, 0.75);
    if (src >= 32000) /* absent (0) or absurd header rates: no cap */
        f = FFMIN(f, src * codec_weight(src_codec) / rate_anchor_avg);
    if (opt.next)
        f /= codec_weight(AV_CODEC_ID_AV1);
    else if (opt.legacy)
        f /= codec_weight(AV_CODEC_ID_H264);
    return f;
}

/* the Opus bitrate for a -q: anchored at 64k stereo / 48k mono for the
 * default (= -q 4.8) — half of that at -q 0, ~1.5x at -q 10. The lossy
 * source's own rate then caps it (floored at Opus's useful minimum): Opus
 * packs at least as much quality per bit as any codec this build decodes,
 * so bits past the source rate cannot recover quality the input never had.
 * Lossless/PCM/multichannel sources declare rates far above the cap and
 * stay uncapped. --legacy's AAC needs ~1.5x Opus's rate for the same
 * quality (96k stereo at the default), and FFmpeg's native encoder sits at
 * the weak end of AAC encoders — 1.5 errs the right way. */
static int64_t audio_bitrate(int mono, int64_t src)
{
    double  q   = opt.quality < 0 ? 48.0 : opt.quality;
    int64_t bps = (int64_t)((mono ? 48000 : 64000) * (0.5 + q / 96.0));
    int64_t lo  = mono ? 16000 : 24000;

    if (opt.legacy) { /* AAC anchors: 1.5x the Opus rates */
        bps = bps * 3 / 2;
        lo  = lo * 3 / 2;
    }
    if (src > 0 && src < bps)
        bps = FFMAX(src, lo);
    return bps;
}

/* ==== end of calibration ================================================== */

/* one output stream mirroring an opened encoder: parameters copied, the
 * encoder's time base kept, the stream index returned */
static int add_stream(AVFormatContext *ofmt, const AVCodecContext *enc, int *index)
{
    AVStream *ost = avformat_new_stream(ofmt, NULL);
    int ret;

    if (!ost)
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_from_context(ost->codecpar, enc)) < 0)
        return ret;
    ost->time_base = enc->time_base;
    *index = ost->index;
    return 0;
}

/* the HDR (PQ/HLG) -> SDR tonemap chain: linearize with zimg, tone-map to
 * SDR in linear light, re-encode as bt709 — without this the encode
 * "succeeds" but every color comes out gray and washed. Runs after the
 * scaler so the float math runs at output resolution. Untagged streams get
 * the in-practice-universal bt2020 guess. tonemap needs the source's peak
 * brightness, but ffmpeg 8's zscale strips HDR side data during conversion,
 * so fish it out of the input ourselves: the peeked first frame (in-stream
 * SEI), the container (mkv/mp4 boxes), or assume the typical 1000-nit
 * master. The last zscale quantizes tonemap's float output down to 8 bits:
 * undithered, the smooth gradients tonemapping produces band visibly
 * (zscale's dither default is none). */
static void tonemap_spec(const AVCodecContext *dec, const AVStream *ist,
                         int pass, char *buf, size_t size)
{
    double peak = peeked.peak;

    if (peak <= 0)
        peak = peak_from_metadata(
            coded_sd(ist, AV_PKT_DATA_CONTENT_LIGHT_LEVEL),
            coded_sd(ist, AV_PKT_DATA_MASTERING_DISPLAY_METADATA));
    if (peak <= 0)
        peak = 10.0;

    snprintf(buf, size,
             "zscale=tin=%s%s%s:t=linear:npl=100,format=gbrpf32le,"
             "zscale=p=bt709,tonemap=hable:desat=0:peak=%.6g,"
             "zscale=t=bt709:m=bt709:r=tv:dither=error_diffusion,",
             dec->color_trc == AVCOL_TRC_SMPTE2084
                 ? "smpte2084" : "arib-std-b67",
             dec->colorspace == AVCOL_SPC_UNSPECIFIED
                 ? ":min=2020_ncl" : "",
             dec->color_primaries == AVCOL_PRI_UNSPECIFIED
                 ? ":pin=2020" : "",
             peak);
    if (pass != 1)
        av_log(NULL, AV_LOG_WARNING, "HDR input (%s, %.6g-nit peak):"
               " tonemapping to SDR bt709\n",
               dec->color_trc == AVCOL_TRC_SMPTE2084 ? "PQ" : "HLG",
               peak * 100);
}

/* --legacy images: PNG/APNG is lossless — -q has nothing left to buy (the
 * quality is already maximal, and so always at least the WebP pipeline's),
 * only deflate effort remains. Every tier takes zlib level 9 (lossless, so
 * strictly smaller — never a quality cost); default and --best add the
 * per-row "mixed" filter scan, but --fast keeps the encoder's default filter
 * because "mixed" *alone* backfires on gradients (it only pays paired with
 * the deeper search) */
static void setup_png(Pipe *p, AVDictionary **opts)
{
    p->enc->compression_level = 9;
    if (opt.effort >= 0)
        av_dict_set(opts, "pred", "mixed", 0);
}

/* --next images: AVIF, stills and animations alike. -q buys the same look
 * as the WebP pipeline, not the same number — the curves live in the
 * calibration section (avif_still_crf, avif_anim_crf) */
static void setup_avif(Pipe *p, AVDictionary **opts)
{
    double q   = image_quality();
    int    crf = peeked.animated ? avif_anim_crf(q) : avif_still_crf(q);

    p->enc->bit_rate     = 0;
    p->enc->thread_count = width_threads(p->enc->width);
    av_dict_set_int(opts, "crf", crf, 0);
    av_dict_set(opts, "row-mt", "1", 0);
    if (peeked.animated) {
        /* animated GIF -> animated AVIF (the muxer's 'avis' brand),
         * inter-coded at the video tiers' speeds — all-intra would spend a
         * full keyframe on every GIF frame (gop_size left at its default
         * of 12 would too, every 12). --best keeps the default speed for
         * the same reason as video: cpu-used 3 measured +0.2% bytes at
         * 1.8x the time */
        p->enc->gop_size = 240;
        av_dict_set(opts, "cpu-used", opt.effort < 0 ? "6" : "4", 0);
    } else {
        /* encoder effort (allintra speed 0-9): avifenc defaults to
         * speed 6; webify's default digs deeper — files are downloaded
         * many times — --fast matches the quick end, --best the slowest
         * practical search. --fast briefly ran speed 8 (~2x faster than 6,
         * -.002 SSIM on synthetic fixtures) but real photos sank it:
         * -.018..-.038 vs WebP-fast on the Kodak fixtures — grain and skin
         * detail are exactly what the shallower search drops — so 6 is the
         * fast ceiling that holds the parity band
         * (doc/next-calibration.md). */
        av_dict_set(opts, "usage", "allintra", 0); /* like avifenc */
        av_dict_set(opts, "still-picture", "1", 0);
        av_dict_set(opts, "cpu-used", opt.effort < 0 ? "6"
                    : opt.effort > 0 ? "2" : "4", 0);
    }
}

/* default images: WebP via libwebp, matching cwebp bit for bit */
static void setup_webp(AVDictionary **opts)
{
    char q[32];

    snprintf(q, sizeof(q), "%g", image_quality());
    av_dict_set(opts, "quality", q, 0); /* default matches cwebp's 80 */
    /* cwebp's -m 6: best compression the format allows, worth the extra
     * encode time for files that are downloaded many times. --fast
     * drops to cwebp's default -m 4: measured 2-15x faster (dithered
     * animations gain the most) for 2-11% more bytes */
    av_dict_set(opts, "compression_level", opt.effort < 0 ? "4" : "6", 0);
}

/* video, all three pipelines (libvpx-vp9 / libaom-av1 / libx264): one
 * shared quality-and-budget computation — the CRF mappings and rate-cap
 * anchors live in the calibration section — then per-encoder effort tiers */
static void setup_video(Pipe *p, AVFormatContext *ifmt, int stream_index,
                        int pass, AVDictionary **opts)
{
    AVStream *ist  = ifmt->streams[stream_index];
    int       crf  = vp9_video_crf(pass || vp9_ref_twopass);
    double    ofps = p->enc->framerate.num > 0 ? av_q2d(p->enc->framerate) : 0;
    double    f;

    if (opt.next)
        crf = av1_video_crf(crf);
    f = rate_budget_scale(crf, p->enc->width, p->enc->height, ofps,
                          source_video_rate(ifmt, stream_index),
                          ist->codecpar->codec_id);

    /* a nonzero bit_rate would flip libx264 from CRF into ABR mode:
     * --legacy carries only the peak cap, as CRF + VBV (the
     * conventional 2-second buffer window) */
    p->enc->bit_rate     = opt.legacy ? 0 : (int64_t)(rate_anchor_avg * f);
    p->enc->rc_max_rate  = (int64_t)(rate_anchor_max * f);
    if (opt.legacy)
        p->enc->rc_buffer_size =
            (int)FFMIN(2 * p->enc->rc_max_rate, INT_MAX);
    p->enc->gop_size     = 240;
    p->enc->thread_count = width_threads(p->enc->width);
    if (opt.legacy) /* remapped last: the budget above tracks the look
                     * (the VP9-scale CRF), not x264's number for it */
        crf = legacy_video_crf(crf);
    av_dict_set_int(opts, "crf", crf, 0);
    if (!opt.legacy) {
        av_dict_set(opts, "row-mt", "1", 0);
        av_dict_set_int(opts, "tile-columns", width_tlog(p->enc->width), 0);
    }
    /* encoder effort by tier — each step has to pay for its time:
     * --fast cpu-used 4 (Google's stats-pass speed, ~4x faster than
     * the default; cpu-used 5 measured *strictly worse* at webify's
     * tile/thread counts — 1.5-1.8x slower than 4, -.002 SSIM, +1%
     * bytes — so 4 is the fast ceiling that pays), default cpu-used 1
     * (Google's VOD setting), --best
     * cpu-used 0 + longer alt-ref noise reduction (together measured
     * 1-7% smaller at equal-or-better SSIM, ~75% more time; deadline
     * "best" was measured 5x slower for -0.06% and rejected). The
     * stats pass is insensitive to encoder effort: always run it fast.
     * libaom (--next): --fast cpu-used 6 single-pass (about half the
     * time of even the VP9 default), default cpu-used 4 two-pass (~3x
     * the VP9 default's time — libaom is simply heavier). A deeper
     * final pass does NOT pay here, unlike libvpx: at a fixed CRF
     * libaom's slower speeds buy a sliver of quality, never bytes
     * (cpu-used 3 measured +1% bytes for 1.2x time, cpu-used 2 +0.8%
     * for 3.4x time, arnr-max-frames 15 +0.3% — all rejected), so
     * --best keeps the default encoder settings */
    if (opt.legacy) {
        /* the preset ladder at a fixed CRF: medium -> slow buys nothing
         * (+1.5% bytes), slow -> veryslow buys -19% bytes at equal SSIM
         * for 2.7x the time (still ~10x faster than the VP9 default), so
         * the default goes straight to veryslow; --fast (preset fast) is
         * ~5x faster. --best changes nothing: placebo measured 3.7x the
         * time for +1% bytes — there is no deeper setting that pays */
        av_dict_set(opts, "preset",
                    opt.effort < 0 ? "fast" : "veryslow", 0);
    } else if (opt.next) {
        av_dict_set(opts, "cpu-used", pass == 1 ? "6"
                    : opt.effort < 0 ? "6" : "4", 0);
    } else {
        av_dict_set(opts, "cpu-used", pass == 1 ? "4"
                    : opt.effort < 0 ? "4" : opt.effort > 0 ? "0" : "1", 0);
        if (opt.effort > 0)
            av_dict_set(opts, "arnr-maxframes", "15", 0);
        av_dict_set(opts, "deadline", "good", 0);
        /* libvpx defaults to frame-parallel decoding mode, which turns
         * off backward-adaptive entropy coding; no browser needs that */
        av_dict_set(opts, "frame-parallel", "0", 0);
    }
}

/* the AVIF transparency layout: the alpha plane rides as a second AV1
 * stream that the muxer stores as the auxiliary alpha item, encoded as
 * monochrome AV1 (the muxer wants one plane here) */
static int init_alpha(Pipe *p, const AVCodec *codec)
{
    AVDictionary *aopts = NULL;
    int ret;

    if (!(p->enc_a = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);
    copy_enc_geometry(p->enc_a, p->enc);
    p->enc_a->pix_fmt      = AV_PIX_FMT_GRAY8;
    p->enc_a->thread_count = p->enc->thread_count;
    p->enc_a->bit_rate     = 0;
    /* alpha is full-range by definition (MIAF), and decoders assume so:
     * left at the limited-range default the bitstream says "tv" and
     * libavif-class decoders stretch 16-235 to 0-255, distorting every
     * gradient (measured SSIM 0.90 on a radial alpha vs 1.0 intended) */
    p->enc_a->color_range  = AVCOL_RANGE_JPEG;
    /* crf 0 ~ lossless: lossy WebP also stores its alpha plane
     * losslessly by default, and alpha gradients band visibly while
     * costing few bits */
    av_dict_set(&aopts, "crf", "0", 0);
    if (!peeked.animated) {
        av_dict_set(&aopts, "usage", "allintra", 0);
        av_dict_set(&aopts, "still-picture", "1", 0);
    }
    av_dict_set(&aopts, "row-mt", "1", 0);
    /* still alpha keeps cpu-used 7 at --fast: the stills ladder's
     * speed-7 rejection is a lossy-mode result — at crf 0 speed
     * moves bytes, not the look */
    av_dict_set(&aopts, "cpu-used",
                opt.effort < 0 ? (peeked.animated ? "6" : "7") : "4", 0);
    ret = avcodec_open2(p->enc_a, codec, &aopts);
    av_dict_free(&aopts);
    return ret;
}

/* image != 0 selects the WebP pipeline: no scaling, alpha kept, libwebp.
 * pass 0 = single-pass video; pass 1 = stats-gathering run (ofmt is NULL, no
 * output stream is created); pass 2 = final encode driven by `stats`. */
static int init_video(Pipe *p, AVFormatContext *ifmt, AVFormatContext *ofmt,
                      int stream_index, int image, int pass, const char *stats)
{
    AVStream *ist = ifmt->streams[stream_index];
    AVRational sar;
    const AVCodec *codec;
    AVDictionary *opts = NULL, *primeopts = NULL;
    const AVPacketSideData *psd;
    const int32_t *mat = NULL;
    uint8_t *extra = NULL;
    char args[512], spec[768], scale[256] = "", rotate[40], tonemap[256] = "";
    int ret, rgb = 0, hdr, alpha = 0, extra_size = 0;
    int maxw = opt.max_w, maxh = opt.max_h;

    p->in_index = stream_index;
    if ((ret = open_decoder(ifmt, stream_index, &p->dec)) < 0)
        return ret;

    sar = p->dec->sample_aspect_ratio;
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d"
             ":colorspace=%d:range=%d",
             p->dec->width, p->dec->height, p->dec->pix_fmt,
             ist->time_base.num, ist->time_base.den, sar.num, FFMAX(sar.den, 1),
             p->dec->colorspace, p->dec->color_range);
    if (p->dec->framerate.num > 0 && p->dec->framerate.den > 0)
        av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d",
                    p->dec->framerate.num, p->dec->framerate.den);

    psd = av_packet_side_data_get(ist->codecpar->coded_side_data,
                                  ist->codecpar->nb_coded_side_data,
                                  AV_PKT_DATA_DISPLAYMATRIX);
    if (peeked.set) /* EXIF orientation found on the first decoded frame */
        mat = peeked.m;
    else if (psd && psd->size >= 9 * sizeof(int32_t))
        mat = (const int32_t *)psd->data;
    build_rotate(mat, rotate, sizeof(rotate));

    /* fit inside the max_w x max_h box after rotation, keeping aspect ratio
     * and never upscaling; video additionally needs even dims for yuv420p */
    if (!image && !maxw && !maxh)
        maxh = 480; /* classic default: fit within 480p */
    if (maxw || maxh)
        snprintf(scale, sizeof(scale),
                 "scale=w=min(iw\\,%d):h=min(ih\\,%d)"
                 ":force_original_aspect_ratio=decrease%s,",
                 maxw ? maxw : INT_MAX, maxh ? maxh : INT_MAX,
                 image ? ":flags=" IMAGE_SWS : ":force_divisible_by=2");
    hdr = is_hdr_trc(p->dec->color_trc);
    if (image) {
        /* RGB-coded sources (png/gif/bmp/tiff) go to libwebp as RGB so its
         * own converter does the subsampling — measurably closer to cwebp
         * than a swscale pre-conversion; YUV-coded sources (jpeg/webp/heic)
         * skip the round trip and keep alpha when they have it.
         * --next (AVIF, stills and animations alike): libaom
         * takes no RGB, so the high-quality swscale conversion (IMAGE_SWS)
         * does the subsampling; alpha survives in the yuva frame and
         * becomes the avif muxer's auxiliary alpha stream below */
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(p->dec->pix_fmt);

        if (hdr)
            av_log(NULL, AV_LOG_WARNING, "HDR image input: colors may come "
                   "out washed; only video inputs are tonemapped\n");
        /* --next stays 4:2:0 everywhere so every AVIF webify writes decodes
         * as AV1 Main profile: 4:4:4 chroma would help sharp graphics at
         * the premium -q end (a 444-vs-420 race used to live here) but
         * needs High profile (seq_profile 1), which hardware AV1 decoders
         * commonly lack — graphics that want pixel-exact chroma should use
         * the default pipeline's lossless WebP race instead */
        rgb = !opt.next && !opt.legacy && desc &&
              (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL));
        snprintf(spec, sizeof(spec), "%s%sformat=%s", rotate, scale,
                 opt.legacy ? (peeked.gray && !peeked.animated
                                 ? (peeked.alpha ? "ya8" : "gray")
                                 : (peeked.alpha ? "rgba" : "rgb24"))
                 : opt.next ? (peeked.alpha ? "yuva420p" : "yuv420p")
                 : rgb      ? av_get_pix_fmt_name(AV_PIX_FMT_RGB32)
                            : "yuv420p|yuva420p");
        /* --next keeps alpha only when the peek saw real transparency: a
         * fully opaque alpha channel would just waste an extra AV1 stream
         * (--legacy PNG likewise drops a fully opaque alpha channel — it
         * would only add bytes) */
    } else {
        if (hdr) /* tone-map PQ/HLG to SDR bt709 (see tonemap_spec) */
            tonemap_spec(p->dec, ist, pass, tonemap, sizeof(tonemap));
        snprintf(spec, sizeof(spec), DEINT_FILTER "%s%s%s%s", rotate, scale,
                 tonemap, VIDEO_FILTERS);
    }

    if ((ret = init_graph(p, "buffer", args, "buffersink", spec,
                          image ? "flags=" IMAGE_SWS : NULL)) < 0)
        return ret;

    /* libwebp_anim writes stills too and is needed for animated GIF -> WebP */
    const char *enc_name =
            opt.legacy ? (!image ? "libx264" : peeked.animated ? "apng" : "png")
            : opt.next ? "libaom-av1"
            : image    ? "libwebp_anim" : "libvpx-vp9";

    codec = avcodec_find_encoder_by_name(enc_name);
    if (!codec && image && !opt.next && !opt.legacy)
        codec = avcodec_find_encoder_by_name(enc_name = "libwebp");
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "%s encoder missing from this build\n",
               enc_name);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (!(p->enc = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);

    p->enc->width               = av_buffersink_get_w(p->sink);
    p->enc->height              = av_buffersink_get_h(p->sink);
    p->enc->pix_fmt             = (AVPixelFormat)av_buffersink_get_format(p->sink);
    if (opt.next && p->enc->pix_fmt == AV_PIX_FMT_YUVA420P) {
        /* libaom takes no alpha plane: the color planes of a yuva420p frame
         * are a valid yuv420p frame as-is, and the alpha plane rides as a
         * second AV1 stream that the avif muxer stores as the auxiliary
         * alpha item (the standard AVIF transparency layout) */
        p->enc->pix_fmt = AV_PIX_FMT_YUV420P;
        alpha = 1;
    }
    p->enc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(p->sink);
    p->enc->time_base           = av_buffersink_get_time_base(p->sink);
    p->enc->framerate           = av_buffersink_get_frame_rate(p->sink);
    p->enc->colorspace          = av_buffersink_get_colorspace(p->sink);
    p->enc->color_range         = av_buffersink_get_color_range(p->sink);
    if (!image && hdr) { /* tonemapped: the output really is bt709 now */
        p->enc->color_primaries = AVCOL_PRI_BT709;
        p->enc->color_trc       = AVCOL_TRC_BT709;
    }
    if (!image && opt.max_fps > 0) {
        p->min_gap = 1.0 / opt.max_fps; /* frames are dropped in decode_packet */
        if (p->enc->framerate.num > 0 && av_q2d(p->enc->framerate) > opt.max_fps)
            p->enc->framerate = av_d2q(opt.max_fps, 100000);
    }
    if (ofmt && (ofmt->oformat->flags & AVFMT_GLOBALHEADER))
        p->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (pass == 1)
        p->enc->flags |= AV_CODEC_FLAG_PASS1;
    else if (pass == 2) {
        p->enc->flags |= AV_CODEC_FLAG_PASS2;
        if (!(p->enc->stats_in = av_strdup(stats)))
            return AVERROR(ENOMEM);
    }

    if (image && opt.legacy)
        setup_png(p, &opts);
    else if (image && opt.next)
        setup_avif(p, &opts);
    else if (image)
        setup_webp(&opts);
    else
        setup_video(p, ifmt, stream_index, pass, &opts);
    if (opt.next && !image && ofmt) /* the open below consumes opts */
        av_dict_copy(&primeopts, opts, 0);
    ret = avcodec_open2(p->enc, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        av_dict_free(&primeopts);
        return ret;
    }
    if (primeopts) {
        if (!p->enc->extradata_size) {
            prime_av1_extradata(p->enc, codec, primeopts, &extra, &extra_size);
            if (!extra)
                av_log(NULL, AV_LOG_WARNING, "could not pre-extract the AV1 "
                       "sequence header; piped output may fail\n");
        } else {
            av_dict_free(&primeopts);
        }
    }

    if (alpha && (ret = init_alpha(p, codec)) < 0)
        return ret;

    /* a still might do better lossless: race them (--fast skips the race
     * and the sharp_yuv re-encode — the streamed packet ships as-is).
     * Animations race in a second muxer instead (init_anim_lossless), so the
     * still-hold path is off for them — peeked.animated is known up front */
    p->rgb  = rgb;
    p->dual = image && rgb && opt.effort >= 0 && !peeked.animated;
    p->prog = !image;

    if (ofmt) {
        if ((ret = add_stream(ofmt, p->enc, &p->out_index)) < 0)
            return ret;
        if (extra) { /* the primed AV1 sequence header (see above) */
            AVCodecParameters *par = ofmt->streams[p->out_index]->codecpar;

            par->extradata      = extra;
            par->extradata_size = extra_size;
            extra = NULL;
        }
        if (p->enc_a && (ret = add_stream(ofmt, p->enc_a, &p->out_index_a)) < 0)
            return ret;
    }

    return alloc_pipe_buffers(p);
}

static int init_audio(Pipe *p, AVFormatContext *ifmt, AVFormatContext *ofmt,
                      int stream_index)
{
    AVStream *ist = ifmt->streams[stream_index];
    const AVCodec *codec;
    char args[512], layout[128], spec[128];
    int ret, mono;

    p->in_index = stream_index;
    if ((ret = open_decoder(ifmt, stream_index, &p->dec)) < 0)
        return ret;

    if (p->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&p->dec->ch_layout, p->dec->ch_layout.nb_channels);
    av_channel_layout_describe(&p->dec->ch_layout, layout, sizeof(layout));
    snprintf(args, sizeof(args),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             ist->time_base.num, ist->time_base.den, p->dec->sample_rate,
             av_get_sample_fmt_name(p->dec->sample_fmt), layout);

    /* mono sources stay mono (an upmix would just spend bits twice on the
     * same signal); anything else is downmixed to stereo */
    mono = p->dec->ch_layout.nb_channels == 1;
    snprintf(spec, sizeof(spec), AUDIO_FILTERS,
             opt.legacy ? "fltp" : "flt", mono ? "mono" : "stereo");
    if ((ret = init_graph(p, "abuffer", args, "abuffersink", spec, NULL)) < 0)
        return ret;

    /* --legacy pairs the MP4 with FFmpeg's native AAC encoder */
    const char *enc_name = opt.legacy ? "aac" : "libopus";

    codec = avcodec_find_encoder_by_name(enc_name);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "%s encoder missing from this build\n",
               enc_name);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (!(p->enc = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);

    p->enc->sample_rate = av_buffersink_get_sample_rate(p->sink);
    p->enc->sample_fmt  = (AVSampleFormat)av_buffersink_get_format(p->sink);
    if ((ret = av_buffersink_get_ch_layout(p->sink, &p->enc->ch_layout)) < 0)
        return ret;
    /* -q scales the audio too, capped by a lossy source's own rate (the
     * anchors and the why live in audio_bitrate, in the calibration
     * section) */
    p->enc->bit_rate  = audio_bitrate(mono, source_audio_rate(ist));
    p->enc->time_base = AVRational{ 1, p->enc->sample_rate };
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        p->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(p->enc, codec, NULL)) < 0)
        return ret;
    /* opus consumes fixed-size frames; let the sink chunk them for us */
    av_buffersink_set_frame_size(p->sink, p->enc->frame_size);

    if ((ret = add_stream(ofmt, p->enc, &p->out_index)) < 0)
        return ret;

    return alloc_pipe_buffers(p);
}

/* tag, rescale and hand one encoded packet to the muxer's interleaver */
static int write_packet(AVFormatContext *ofmt, AVPacket *pkt,
                        AVRational enc_tb, int out_index)
{
    pkt->stream_index = out_index;
    av_packet_rescale_ts(pkt, enc_tb, ofmt->streams[out_index]->time_base);
    return av_interleaved_write_frame(ofmt, pkt);
}

/* frame == NULL flushes the encoder */
static int encode_one(AVFormatContext *ofmt, Pipe *p, AVCodecContext *enc,
                      int out_index, AVFrame *frame)
{
    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0)
        return ret;
    for (;;) {
        ret = avcodec_receive_packet(enc, p->enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        if (!ofmt) { /* stats-gathering first pass: nothing is written */
            av_packet_unref(p->enc_pkt);
            continue;
        }
        if ((ret = write_packet(ofmt, p->enc_pkt, enc->time_base, out_index)) < 0)
            return ret;
    }
}

/* the alpha plane of a yuva420p frame as a monochrome (gray8) frame: the
 * avif muxer requires its auxiliary alpha stream to be single-plane, which
 * libaom encodes as monochrome AV1 */
static AVFrame *alpha_frame(const AVFrame *src)
{
    AVFrame *f = av_frame_alloc();

    if (!f)
        return NULL;
    f->format = AV_PIX_FMT_GRAY8;
    f->width  = src->width;
    f->height = src->height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return NULL;
    }
    av_image_copy_plane(f->data[0], f->linesize[0], src->data[3],
                        src->linesize[3], src->width, src->height);
    f->pts         = src->pts;
    f->duration    = src->duration;
    f->pict_type   = AV_PICTURE_TYPE_NONE;
    f->color_range = AVCOL_RANGE_JPEG; /* matches the alpha encoder */
    return f;
}

static int encode_write(AVFormatContext *ofmt, Pipe *p, AVFrame *frame)
{
    AVFrame *m = NULL, *a = NULL;
    int ret;

    if (!p->enc_a) {
        ret = encode_one(ofmt, p, p->enc, p->out_index, frame);
        if (ret >= 0 && p->enc_ll) /* feed the animated-WebP lossless race */
            ret = encode_one(p->ofmt_ll, p, p->enc_ll, p->out_index_ll, frame);
        return ret;
    }
    /* AVIF with transparency: color planes to the main stream, the alpha
     * plane to the auxiliary one (NULL falls through and flushes both) */
    if (frame) {
        if (!(m = av_frame_clone(frame)) || !(a = alpha_frame(frame))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        m->format      = p->enc->pix_fmt; /* planes 0-2 of yuva* as-is */
        m->data[3]     = NULL;
        m->linesize[3] = 0;
    }
    if ((ret = encode_one(ofmt, p, p->enc, p->out_index, m)) >= 0)
        ret = encode_one(ofmt, p, p->enc_a, p->out_index_a, a);
end:
    av_frame_free(&m);
    av_frame_free(&a);
    return ret;
}

static int drain_sink(AVFormatContext *ofmt, Pipe *p)
{
    for (;;) {
        int ret = av_buffersink_get_frame(p->sink, p->filt_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        p->filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
        if (p->dual && !p->first) {
            /* maybe a still: hold the frame for the lossy/lossless race */
            p->first = av_frame_clone(p->filt_frame);
            av_frame_unref(p->filt_frame);
            if (!p->first)
                return AVERROR(ENOMEM);
            continue;
        }
        if (p->first) { /* a second frame: an animation after all */
            p->dual = 0;
            ret = encode_write(ofmt, p, p->first);
            av_frame_free(&p->first);
            if (ret < 0)
                return ret;
        }
        ret = encode_write(ofmt, p, p->filt_frame);
        av_frame_unref(p->filt_frame);
        if (ret < 0)
            return ret;
    }
}

/* pkt == NULL flushes the decoder */
static int decode_packet(AVFormatContext *ofmt, Pipe *p, AVPacket *pkt)
{
    int ret = avcodec_send_packet(p->dec, pkt);
    if (ret < 0 && ret != AVERROR_EOF)
        return ret;
    for (;;) {
        ret = avcodec_receive_frame(p->dec, p->dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        p->dec_frame->pts = p->dec_frame->best_effort_timestamp;
        if ((p->prog || p->min_gap > 0) && p->dec_frame->pts != AV_NOPTS_VALUE) {
            double t = p->dec_frame->pts * av_q2d(p->dec->pkt_timebase);

            if (p->prog)
                progress_tick(t);
            if (p->min_gap > 0) { /* --max @F: enforce a minimum pts gap */
                if (t < p->next_keep) {
                    av_frame_unref(p->dec_frame);
                    continue;
                }
                p->next_keep = t + p->min_gap * 0.999; /* float-safe spacing */
            }
        }
        /* flags=0: the graph consumes our reference (we don't reuse the frame) */
        ret = av_buffersrc_add_frame_flags(p->src, p->dec_frame, 0);
        av_frame_unref(p->dec_frame);
        if (ret < 0)
            return ret;
        if ((ret = drain_sink(ofmt, p)) < 0)
            return ret;
    }
}

/* one-frame re-encode of the held still on a fresh clone of the streaming
 * context (libwebp's "quality" doubles as the effort dial when lossless) */
static int reencode_still(Pipe *p, int lossless, double quality, int sharp,
                          AVPacket *out)
{
    AVCodecContext *c = avcodec_alloc_context3(p->enc->codec);
    AVDictionary *opts = NULL;
    char q[32];
    int ret = AVERROR(ENOMEM);

    if (c) {
        copy_enc_geometry(c, p->enc);
        snprintf(q, sizeof(q), "%g", quality);
        if (lossless)
            av_dict_set(&opts, "lossless", "1", 0);
        av_dict_set(&opts, "quality", q, 0);
        av_dict_set(&opts, "compression_level", "6", 0);
        if (sharp)
            av_dict_set(&opts, "sharp_yuv", "1", 0);
        if ((ret = avcodec_open2(c, p->enc->codec, &opts)) >= 0)
            ret = encode_full(c, p->first, out);
    }
    av_dict_free(&opts);
    avcodec_free_context(&c);
    return ret;
}

/* A single-frame RGB image: flat-color graphics often compress smaller as
 * lossless WebP than as lossy q80 — and lossless is by definition the best
 * quality — so encode both and keep the smaller file. Photos stay lossy
 * (their lossless attempt just comes out bigger and loses the race). */
static int finish_still(AVFormatContext *ofmt, Pipe *p)
{
    AVPacket *lossy = av_packet_alloc(), *less = av_packet_alloc();
    AVPacket *best = av_packet_alloc(), *pick;
    int ret;

    if (!lossy || !less || !best) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* cwebp's -sharp_yuv: iterative RGB->YUV conversion that keeps text and
     * hard edges crisp for ~1% more bytes (the option is added to the
     * vendored ffmpeg by patches/0001-*). Stills only: animations would pay
     * a measured ~20% more bytes for it, so the streaming context (p->enc)
     * keeps libwebp's fast converter and confirmed stills re-encode here. */
    if (reencode_still(p, 0, image_quality(), 1, lossy) < 0 &&
        (ret = encode_full(p->enc, p->first, lossy)) < 0) /* streaming-ctx
                                                             fallback */
        goto end;

    pick = lossy;
    /* the lossless probe is a throwaway whose only job is to decide the race
     * (the smaller of lossy vs lossless wins). It runs at quality 0 —
     * libwebp's fastest lossless effort, ~22-30% quicker than the old 75 —
     * because the lossy/lossless size gap is fixed by content class (graphics
     * win by 10-100x, photos lose by as much), never by probe effort: a
     * 16-image sweep including content engineered to straddle the crossover
     * never flipped the decision between effort 0 and 75. A race winner still
     * ships the quality-100 re-encode below (libwebp's max effort, the
     * smaller candidate), so output stays lossless and the same size to within
     * the encoder's sub-0.1% effort noise — byte-for-byte identical except on
     * the rare graphic whose probe size ties q100's, which now ships the
     * (equal-size) q100 bytes. A failed lossless attempt is not fatal: the
     * lossy one stands. */
    if (reencode_still(p, 1, 0, 0, less) >= 0 &&
        less->size && less->size < lossy->size) {
        pick = less;
        /* lossless won: re-run the winner at maximum effort (cwebp
         * -lossless -q 100 -m 6) — measured 1-3% smaller for ~20x the
         * lossless encode time, paid only on graphics that already won;
         * photos saw their lossless candidate lose and skip this */
        if (reencode_still(p, 1, 100, 0, best) >= 0 &&
            best->size && best->size < less->size)
            pick = best;
    }
    ret = write_packet(ofmt, pick, p->enc->time_base, p->out_index);
end:
    av_packet_free(&lossy);
    av_packet_free(&less);
    av_packet_free(&best);
    av_frame_free(&p->first);
    return ret;
}

static int flush_pipe(AVFormatContext *ofmt, Pipe *p)
{
    int ret;

    if (!p->dec) /* pipe was never initialized */
        return 0;
    if ((ret = decode_packet(ofmt, p, NULL)) < 0)       /* drain decoder  */
        return ret;
    if ((ret = av_buffersrc_add_frame_flags(p->src, NULL, 0)) < 0) /* EOF graph */
        return ret;
    if ((ret = drain_sink(ofmt, p)) < 0)
        return ret;
    if (p->first) /* exactly one frame arrived: the still race */
        return finish_still(ofmt, p);
    return encode_write(ofmt, p, NULL);                 /* flush encoder  */
}

/* Only with two-pass stats does libvpx use alt-ref frames and plan its rate
 * budget ahead, which measures 10-20% smaller files at equal quality (or
 * ~2 dB better quality where the rate caps bind). Piped stdin is spooled so
 * it can rewind like a file; only --fast (by design) and the rare
 * unspoolable pipe stay single-pass. */
static int first_pass(AVFormatContext *ifmt, int vidx, int aidx, char **stats)
{
    Pipe p = {};
    AVPacket *pkt = av_packet_alloc();
    int64_t bytes[2] = { 0, 0 };
    int64_t span[2][2] = { { AV_NOPTS_VALUE, 0 }, { AV_NOPTS_VALUE, 0 } };
    int ret;

    if (!pkt)
        return AVERROR(ENOMEM);
    progress_start("pass 1/2:");
    if ((ret = init_video(&p, ifmt, NULL, vidx, 0, 1, NULL)) < 0)
        goto end;
    while ((ret = av_read_frame(ifmt, pkt)) >= 0) {
        /* this pass touches every packet anyway: measure the true source
         * rates (audio packets arrive here only to be counted) */
        int side = pkt->stream_index == vidx ? 0
                 : pkt->stream_index == aidx ? 1 : -1;
        if (side >= 0) {
            int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

            bytes[side] += pkt->size;
            if (ts != AV_NOPTS_VALUE) {
                if (span[side][0] == AV_NOPTS_VALUE)
                    span[side][0] = ts;
                span[side][1] = ts + FFMAX(pkt->duration, 0);
            }
        }
        if (side == 0)
            ret = decode_packet(NULL, &p, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF)
        goto end;
    measured_video_rate = span_rate(bytes[0], span[0],
                                    ifmt->streams[vidx]->time_base);
    if (aidx >= 0)
        measured_audio_rate = span_rate(bytes[1], span[1],
                                        ifmt->streams[aidx]->time_base);
    if ((ret = flush_pipe(NULL, &p)) < 0)
        goto end;
    /* no stats (e.g. zero frames) is not fatal: the caller single-passes */
    if (p.enc->stats_out && !(*stats = av_strdup(p.enc->stats_out)))
        ret = AVERROR(ENOMEM);
end:
    free_pipe(&p);
    av_packet_free(&pkt);
    return ret;
}

/* can the input be read a second time? real files and the spooled/slurped
 * stdin paths can; only the unspoolable-pipe fallback cannot, which costs
 * the stats pass, the HDR-peak peek and end-indexed containers */
static int input_can_rewind(const AVFormatContext *ifmt)
{
    return ifmt->pb && (ifmt->pb->seekable & AVIO_SEEKABLE_NORMAL);
}

/* the stats run or the metadata peek consumed the input; rewind by
 * reopening, which covers both real files and the spooled/slurped stdin
 * paths behind the custom pb. Reopening invalidates the stream indices,
 * so they are rebound here; the discard flags reset too — the caller
 * re-applies those */
static int reopen_input(const char *in_path, AVFormatContext **ifmt,
                        StdinIO *io, int image, int *vidx, int *aidx)
{
    int64_t pos;
    int ret;

    avformat_close_input(ifmt); /* custom pb (io->pb) survives this */
    if (io->pb) {
        if ((pos = avio_seek(io->pb, 0, SEEK_SET)) < 0)
            return (int)pos;
        ret = open_with_pb(ifmt, io->pb);
    } else {
        ret = avformat_open_input(ifmt, in_path, NULL, NULL);
    }
    if (ret < 0)
        return ret;
    if ((ret = avformat_find_stream_info(*ifmt, NULL)) < 0)
        return ret;
    *vidx = av_find_best_stream(*ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    *aidx = image ? -1
          : av_find_best_stream(*ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    return *vidx < 0 ? AVERROR_STREAM_NOT_FOUND : 0;
}

/* let the demuxer drop packets of the streams webify won't read. The audio
 * stream is kept even for the stats pass, which never decodes it — only so
 * its bytes can be counted for the measured rate caps. (reopen_input resets
 * the flags, so this runs again for the final pass) */
static void discard_other_streams(AVFormatContext *ifmt, int vidx, int aidx)
{
    for (unsigned i = 0; i < ifmt->nb_streams; i++)
        if ((int)i != vidx && (int)i != aidx)
            ifmt->streams[i]->discard = AVDISCARD_ALL;
}

/* The animated-WebP lossless-vs-lossy race: a second muxer that encodes the
 * same filtered frames as lossless WebP into memory, in parallel with the
 * lossy stream. Lossless animation is SSIM 1.0 by definition, so whenever it
 * comes out smaller (flat-color and graphics GIFs — measured -19..-81%) it is
 * a free win; photographic animations lose the race and ship the lossy stream
 * unchanged. Both candidates are buffered so the smaller can be chosen at the
 * end. Set up only for the default pipeline on an RGB animation at >= default
 * effort (--fast and YUV sources skip it). */
static int init_anim_lossless(Pipe *p, const char *muxer)
{
    AVDictionary *opts = NULL, *muxopts = NULL;
    int ret;

    if (!(p->enc_ll = avcodec_alloc_context3(p->enc->codec)))
        return AVERROR(ENOMEM);
    copy_enc_geometry(p->enc_ll, p->enc);
    p->enc_ll->framerate    = p->enc->framerate;
    p->enc_ll->thread_count = p->enc->thread_count;
    av_dict_set(&opts, "lossless", "1", 0);
    av_dict_set(&opts, "quality", "75", 0);          /* lossless: the effort dial */
    av_dict_set(&opts, "compression_level", "6", 0); /* like the default lossy tier */
    ret = avcodec_open2(p->enc_ll, p->enc->codec, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        return ret;

    if ((ret = avformat_alloc_output_context2(&p->ofmt_ll, NULL, muxer, NULL)) < 0)
        return ret;
    p->ofmt_ll->flags |= AVFMT_FLAG_BITEXACT; /* same determinism as the primary */
    if ((ret = avio_open_dyn_buf(&p->ofmt_ll->pb)) < 0)
        return ret;
    if ((ret = add_stream(p->ofmt_ll, p->enc_ll, &p->out_index_ll)) < 0)
        return ret;
    av_dict_set(&muxopts, "loop", "0", 0); /* infinite, like the lossy stream */
    ret = avformat_write_header(p->ofmt_ll, &muxopts);
    av_dict_free(&muxopts);
    return ret;
}

/* hand a finished in-memory output to its destination: stdout for a pipe,
 * otherwise the named file (the image muxers that assemble in memory — avif,
 * apng, and the webp race — all land here) */
static int emit_output(int to_pipe, const char *path, const uint8_t *buf, int n)
{
    AVIOContext *pb = NULL;
    int ret;

    if (to_pipe)
        return write_all(STDOUT_FILENO, buf, n);
    if ((ret = avio_open(&pb, path, AVIO_FLAG_WRITE)) < 0)
        return ret;
    avio_write(pb, buf, n);
    return avio_closep(&pb); /* flushes; surfaces a write error */
}

static int webify_run(const char *in_path, const char *out_path)
{
    AVFormatContext *ifmt = NULL, *ofmt = NULL;
    Pipe video = {}, audio = {};
    StdinIO io = {};
    AVPacket *pkt = NULL;
    AVDictionary *muxopts = NULL;
    char *stats = NULL, tmp_out[512] = "";
    const char *sink, *oname;
    int ret, vidx, aidx, image, spool_out = 0, race = 0, mem_out = 0;
    int out_pipe = is_pipe(out_path);

    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(log_cb);

    if (is_pipe(in_path))
        ret = open_stdin_input(in_path, &ifmt, &io);
    else
        ret = avformat_open_input(&ifmt, in_path, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot open '%s': %s\n", in_path, err2str(ret));
        goto end;
    }
    if ((ret = avformat_find_stream_info(ifmt, NULL)) < 0)
        goto end;

    vidx = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0 || (ifmt->streams[vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        av_log(NULL, AV_LOG_ERROR, "'%s' has no video stream\n", in_path);
        ret = AVERROR_STREAM_NOT_FOUND;
        goto end;
    }
    aidx = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0); /* optional */
    image = input_is_image(ifmt, vidx, aidx);
    if (image)
        aidx = -1; /* the image pipeline takes no audio */

    prog.tty      = isatty(STDERR_FILENO);
    prog.duration = ifmt->duration;

    /* the EXIF-orientation / HDR-peak check consumes the input: rewind
     * after (HDR videos that cannot rewind fall back to tag defaults) */
    if (image ||
        (is_hdr_trc((AVColorTransferCharacteristic)
                    ifmt->streams[vidx]->codecpar->color_trc) &&
         input_can_rewind(ifmt))) {
        peek_first_frame(ifmt, vidx, image, opt.next || opt.legacy);
        if ((ret = reopen_input(in_path, &ifmt, &io, image, &vidx, &aidx)) < 0)
            goto end;
    }

    if (!image) {
        if (opt.legacy) {
            /* no stats pass ever (x264's CRF mode plans ahead by itself),
             * but remember when the default pipeline *would* have
             * two-passed: the no--q look to match is then crf 36, not the
             * streamed 33 */
            vp9_ref_twopass = opt.effort >= 0 && input_can_rewind(ifmt);
        } else if (opt.effort >= 0 && input_can_rewind(ifmt)) {
            discard_other_streams(ifmt, vidx, aidx);
            if ((ret = first_pass(ifmt, vidx, aidx, &stats)) < 0)
                goto end;
            if ((ret = reopen_input(in_path, &ifmt, &io, image, &vidx, &aidx)) < 0)
                goto end;
        } else if (opt.effort >= 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "input is not seekable: encoding in a single pass "
                   "(two-pass saves 10-20%% bandwidth at the same quality)\n");
        } /* --fast skips the stats pass on purpose: single-pass, no warning */
    }

    discard_other_streams(ifmt, vidx, aidx);

    /* piped video output spools through a named temp file so the pipe
     * changes nothing about the bytes: faststart re-shuffles the finished
     * MP4 and cues_to_front back-patches the WebM header, both seek-back
     * operations a pipe cannot do. Named, not an unlinked fd: movenc's
     * faststart pass re-opens the output by URL to read it back. The file
     * is streamed to stdout and removed at the end. Images never need
     * this — they stream, or are assembled in memory below */
    if (!image && out_pipe) {
        int fd = make_temp(tmp_out, sizeof(tmp_out));

        if (fd < 0) {
            av_log(NULL, AV_LOG_WARNING, "cannot create a temp file for "
                   "piped output (%s): writing a streamable file instead\n",
                   err2str(AVERROR(errno)));
            tmp_out[0] = '\0';
        } else {
            close(fd);
        }
    }
    /* the muxer's actual sink: the temp spool when one was made, the real
     * output otherwise — every "can the muxer seek?" decision keys off it */
    sink = tmp_out[0] ? tmp_out : out_path;

    /* still PNGs go through image2pipe (one packet = the whole file, written
     * to our own avio like every other muxer here; plain image2 insists on
     * opening files itself) */
    oname = image ? (opt.next        ? "avif"
                   : !opt.legacy     ? "webp"
                   : peeked.animated ? "apng"
                                     : "image2pipe")
                  : opt.legacy ? "mp4" : "webm";
    if ((ret = avformat_alloc_output_context2(&ofmt, NULL, oname, sink)) < 0)
        goto end;
    /* deterministic output: the same input and options always produce the
     * same bytes. This is what makes the piped-output spool byte-identical
     * to a file run (and same-input reruns cache-friendly) — without it
     * the webm muxer stamps a random SegmentUID, random track UIDs and a
     * wallclock DateUTC into every file */
    ofmt->flags |= AVFMT_FLAG_BITEXACT;

    if (!image)
        progress_start(stats ? "pass 2/2:" : "encoding:");
    if ((ret = init_video(&video, ifmt, ofmt, vidx, image, stats ? 2 : 0, stats)) < 0)
        goto end;
    if (aidx >= 0 && (ret = init_audio(&audio, ifmt, ofmt, aidx)) < 0)
        goto end;

    if (!(pkt = av_packet_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* the avif muxer (mov family) seeks back to patch item offsets/sizes,
     * and the apng muxer back-patches its frame count (acTL) the same way;
     * for stdout, write into memory and dump the finished file at the end
     * (images stay off disk on purpose — video uses the temp spool above) */
    spool_out = out_pipe && (!strcmp(oname, "avif") || !strcmp(oname, "apng"));
    /* default-pipeline RGB animations race a lossless WebP candidate: both are
     * assembled in memory so the smaller can win (see init_anim_lossless) */
    race = image && !opt.next && !opt.legacy && opt.effort >= 0 &&
           peeked.animated && video.rgb;
    mem_out = spool_out || race;
    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        ret = mem_out ? avio_open_dyn_buf(&ofmt->pb)
                      : avio_open(&ofmt->pb, sink, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "cannot create '%s': %s\n", out_path,
                   err2str(ret));
            goto end;
        }
    }
    if (image) { /* loop animations forever, like GIF (webp and avif muxers
                  * take a loop count, apng a play count; 0 = infinite) */
        if (!opt.legacy)
            av_dict_set(&muxopts, "loop", "0", 0);
        else if (peeked.animated)
            av_dict_set(&muxopts, "plays", "0", 0);
    } else if (opt.legacy) {
        /* the MP4 spelling of faststart; pipes get it too via the temp-file
         * spool above — only its no-temp-file fallback drops to a fragmented
         * MP4 (playable everywhere MSE is, but not by every old player) */
        av_dict_set(&muxopts, "movflags",
                    is_pipe(sink)
                        ? "+frag_keyframe+empty_moov+default_base_moof"
                        : "+faststart", 0);
    } else /* faststart for WebM: put the seek index up front so browsers can
            * seek without fetching the file tail (pipes again covered by the
            * temp-file spool; on its fallback the muxer cannot seek and
            * writes no index at all) */
        av_dict_set(&muxopts, "cues_to_front", "1", 0);
    ret = avformat_write_header(ofmt, &muxopts);
    av_dict_free(&muxopts);
    if (ret < 0)
        goto end;
    if (race && (ret = init_anim_lossless(&video, oname)) < 0)
        goto end;

    while ((ret = av_read_frame(ifmt, pkt)) >= 0) {
        if (pkt->stream_index == video.in_index)
            ret = decode_packet(ofmt, &video, pkt);
        else if (audio.dec && pkt->stream_index == audio.in_index)
            ret = decode_packet(ofmt, &audio, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF)
        goto end;

    if ((ret = flush_pipe(ofmt, &video)) < 0)
        goto end;
    if ((ret = flush_pipe(ofmt, &audio)) < 0)
        goto end;
    ret = av_write_trailer(ofmt);
    if (race && ret >= 0)
        ret = av_write_trailer(video.ofmt_ll);

end:
    progress_done(); /* leave stderr on a clean line for any error below */
    free_pipe(&video);
    free_pipe(&audio);
    av_freep(&stats);
    av_packet_free(&pkt);
    avformat_close_input(&ifmt);
    close_stdin_io(&io);
    if (ofmt) {
        if (mem_out && ofmt->pb) {
            uint8_t *lossy = NULL, *less = NULL;
            int nlossy = avio_close_dyn_buf(ofmt->pb, &lossy), nless = 0;
            const uint8_t *win = lossy;
            int wn = nlossy;

            ofmt->pb = NULL;
            if (race && video.ofmt_ll && video.ofmt_ll->pb) {
                nless = avio_close_dyn_buf(video.ofmt_ll->pb, &less);
                video.ofmt_ll->pb = NULL;
                if (ret >= 0 && nless > 0 && nless < nlossy) { /* lossless won */
                    win = less;
                    wn  = nless;
                }
            }
            if (ret >= 0 && (ret = emit_output(out_pipe, out_path, win, wn)) < 0)
                av_log(NULL, AV_LOG_ERROR, "cannot write output: %s\n",
                       err2str(ret));
            av_free(lossy);
            av_free(less);
        }
        if (!(ofmt->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (video.ofmt_ll) { /* error path may leave its dyn_buf open */
        if (video.ofmt_ll->pb)
            avio_closep(&video.ofmt_ll->pb);
        avformat_free_context(video.ofmt_ll);
    }
    if (tmp_out[0]) { /* piped-output spool: hand the finished file over */
        if (ret >= 0 && (ret = drain_file_to_stdout(tmp_out)) < 0)
            av_log(NULL, AV_LOG_ERROR, "cannot write output: %s\n",
                   err2str(ret));
        unlink(tmp_out);
    }
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "transcode failed: %s\n", err2str(ret));
        return 1;
    }
    return 0;
}

static int usage(FILE *f, int status)
{
    fprintf(f,
            "webify: transcode any popular video to VP9/Opus WebM,\n"
            "         or any popular image to WebP (auto-detected)\n"
            "usage: webify [options] <input> [output]\n"
            "       '-' = stdin/stdout; omitting [output] writes to stdout\n"
            "  -q, --quality <0-10>   target quality, higher is better\n"
            "                         (default: 8 for images; video picks the\n"
            "                         classic 480p look at the smallest size)\n"
            "  --next                 output the next-gen formats: video\n"
            "                         becomes AV1/Opus WebM, images become\n"
            "                         AVIF (animated GIF -> animated AVIF).\n"
            "                         -q buys the same look as the default\n"
            "                         formats — only the file gets smaller\n"
            "  --legacy               output the maximum-compatibility\n"
            "                         formats: video becomes H.264/AAC MP4\n"
            "                         (faststart), images become PNG\n"
            "                         (animated GIF -> APNG; lossless, so -q\n"
            "                         steers video only). -q buys the same\n"
            "                         look as the default formats — only\n"
            "                         the file gets bigger\n"
            "  -m, --max [HxW|S][@F]  downscale to fit H px tall / W px wide,\n"
            "                         never upscale; a single number S bounds\n"
            "                         both sides, a missing side is unbounded\n"
            "                         (480x854, 720, 480x, x854); video fits\n"
            "                         a height of 480 when no box is given at\n"
            "                         all. @F drops frames to cap the frame\n"
            "                         rate (video only; @30 halves a 60fps\n"
            "                         clip); combine freely: 480x854@30, 480x@30\n"
            "  --fast / --best        trade bytes for encode time at the same\n"
            "                         -q quality target. --fast single-passes\n"
            "                         video and picks quick image settings\n"
            "                         (4-15x faster, ~5-20%% more bytes);\n"
            "                         --best runs the slowest searches\n"
            "                         (~2x slower, 1-7%% fewer bytes); the\n"
            "                         default is the tuned middle ground\n"
            "  -h, --help             show this help\n"
            "      --version          print version (incl. vendored FFmpeg)\n");
    return status;
}

/* -m/--max [HxW | S][@F]: a pixel box (height first; a single number bounds
 * both sides, a missing side is unbounded) and/or an @fps cap, e.g. "720",
 * "480x854", "480x" (height only), "x854" (width only), "@30", "480x@30" */
static int parse_max(const char *arg)
{
    const char *at = strchr(arg, '@');
    char *end;

    if (at) {
        double fps = strtod(at + 1, &end);

        if (*end || end == at + 1 || fps < 1 || fps > 240) {
            fprintf(stderr, "webify: --max fps must be 1-240, got '%s'\n", at + 1);
            return -1;
        }
        opt.max_fps = fps;
    }
    if (at != arg) { /* a box precedes the optional @F */
        const char *p = arg, *stop = at ? at : arg + strlen(arg);
        long h = 0, w = 0;

        if (*p != 'x') { /* H, or the single number that bounds both */
            h = strtol(p, &end, 10);
            if (end == p)
                goto bad;
            if (h < 1 || h > 16384)
                goto range;
            p = end;
            if (p == stop) { /* bare number: bounds both sides */
                opt.max_h = opt.max_w = (int)h;
                return 0;
            }
        }
        if (*p != 'x')
            goto bad;
        if (++p != stop) { /* xW / HxW; "Hx" leaves the width unbounded */
            w = strtol(p, &end, 10);
            if (end == p || end != stop)
                goto bad;
            if (w < 1 || w > 16384)
                goto range;
        }
        if (!h && !w) /* a bare "x" caps nothing */
            goto bad;
        opt.max_h = (int)h;
        opt.max_w = (int)w;
    }
    return 0;
range:
    fprintf(stderr, "webify: --max box sides must be 1-16384, got '%s'\n", arg);
    return -1;
bad:
    fprintf(stderr, "webify: --max expects [HxW | S][@F], got '%s'\n", arg);
    return -1;
}

int main(int argc, char **argv)
{
    enum { OPT_FAST = 1000, OPT_BEST, OPT_NEXT, OPT_LEGACY, OPT_VERSION };
    static const struct option longopts[] = {
        { "quality", required_argument, NULL, 'q' },
        { "max",     required_argument, NULL, 'm' },
        { "next",    no_argument,       NULL, OPT_NEXT },
        { "legacy",  no_argument,       NULL, OPT_LEGACY },
        { "fast",    no_argument,       NULL, OPT_FAST },
        { "best",    no_argument,       NULL, OPT_BEST },
        { "help",    no_argument,       NULL, 'h' },
        { "version", no_argument,       NULL, OPT_VERSION },
        { NULL, 0, NULL, 0 },
    };
    int c;
    char *end;

    while ((c = getopt_long(argc, argv, "hq:m:", longopts, NULL)) != -1) {
        switch (c) {
        case 'h':
            return usage(stdout, 0);
        case OPT_VERSION:
            printf("webify %s (FFmpeg %s)\n", WEBIFY_VERSION, av_version_info());
            return 0;
        case 'q':
            opt.quality = strtod(optarg, &end);
            if (*end || end == optarg || opt.quality < 0 || opt.quality > 10) {
                fprintf(stderr, "webify: quality must be 0-10, got '%s'\n", optarg);
                return 2;
            }
            opt.quality *= 10; /* the internal scale (and WebP's) is 0-100 */
            break;
        case 'm':
            if (parse_max(optarg) < 0)
                return 2;
            break;
        case OPT_NEXT:
            opt.next = 1;
            break;
        case OPT_LEGACY:
            opt.legacy = 1;
            break;
        case OPT_FAST:
        case OPT_BEST:
            if (opt.effort) {
                fprintf(stderr, "webify: --fast and --best are mutually exclusive\n");
                return 2;
            }
            opt.effort = c == OPT_FAST ? -1 : 1;
            break;
        default:
            return usage(stderr, 2);
        }
    }
    if (opt.next && opt.legacy) {
        fprintf(stderr, "webify: --next and --legacy are mutually exclusive\n");
        return 2;
    }
    if (argc - optind < 1 || argc - optind > 2)
        return usage(stderr, 2);
    /* the '-' convention lives in the ffmpeg CLI, not libavformat;
     * <output> may be omitted entirely and defaults to stdout */
    const char *in  = strcmp(argv[optind], "-") ? argv[optind] : "pipe:0";
    const char *out = argc - optind < 2 || !strcmp(argv[optind + 1], "-")
                          ? "pipe:1" : argv[optind + 1];
    return webify_run(in, out);
}
