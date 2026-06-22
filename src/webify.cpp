/*
 * webify — transcode any popular video file to H.264/AAC MP4, or any popular
 * image file to AVIF (auto-detected). One option set covers both modes:
 * -q/--quality 0-10 (mapped to the x264 CRF for video, the AVIF CRF for
 * images) and -m/--max [HxW | S][@F] (downscale to fit H px tall / W px wide —
 * a single number S bounds both, a missing side ("480x", "x854") is
 * unbounded — never upscale; @F is a video-only frame rate cap, off by
 * default).
 *
 * This is a deliberately simplified descendant of the original `webify`: it
 * always writes the two formats above, at one tuned effort level, with no
 * format or effort switches. Everything is still tuned for serving the result
 * over the internet:
 *   - video → H.264/AAC MP4 (vendored x264) with the moov atom up front
 *     (faststart), piped output included via a temp-file spool (only the
 *     no-temp-file fallback writes a fragmented MP4). x264 runs in pure CRF
 *     mode at preset veryslow (its lookahead/mbtree plan the rate ahead, so no
 *     two-pass is needed); -q maps directly onto a recommended CRF and the
 *     audio bitrate scales with it. No bitrate cap — CRF spends what the
 *     quality needs, and --max bounds the resolution
 *   - images → AVIF (vendored libaom), still or animated (animated GIF →
 *     animated AVIF, alpha kept as the auxiliary alpha stream). -q maps
 *     directly onto a recommended AVIF CRF; stills encode all-intra
 *     (still-picture), animations inter-code at the video gop. Everything stays
 *     8-bit 4:2:0 — AV1 Main profile, the one profile hardware decoders
 *     reliably implement
 *   - mono audio stays mono (96k base) instead of being upmixed to stereo
 *     (128k); -q scales the AAC bitrate, and a lossy source's own rate caps it
 *   - EXIF orientation (frame side data) and display-matrix rotation are baked
 *     into the pixels, including the mirrored variants
 *   - interlaced video frames are deinterlaced (bwdif, always in the video
 *     chain): frames the decoder flags interlaced are rebuilt before any
 *     rotation/scaling touches their fields, progressive frames pass through
 *     untouched
 *   - HDR video (PQ/HLG) is tonemapped to SDR bt709 (zscale linearize + hable),
 *     with the source peak taken from in-stream SEI or container metadata
 *     (1000-nit fallback) since ffmpeg 8's zscale strips HDR side data before
 *     tonemap can read it; the final float → 8-bit step is error-diffusion
 *     dithered or the smooth gradients tonemapping produces would band visibly
 *   - never upscale: smaller-than-480p video is not enlarged, and --max only
 *     ever shrinks
 *   - output is deterministic (AVFMT_FLAG_BITEXACT on the muxer), which is what
 *     makes the piped-output spool byte-identical to a file run
 *   - progress on stderr when it is a terminal and the duration is known
 *
 * -q is the only quality dial: it maps directly onto each encoder's own CRF,
 * using values recommended for web delivery (see doc/calibration.md). There is
 * no cross-format calibration and no separate bitrate budget — each encoder's
 * CRF is its own quality control, and --max picks the resolution.
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
 * no-ops where they don't apply; lanczos only kicks in for --max scaling, and
 * libaom takes no RGB so swscale always does the RGB→YUV subsampling) */
#define IMAGE_SWS "lanczos+accurate_rnd+full_chroma_int+full_chroma_inp"

/* -q/--quality and the --max box mean the same thing in both the video and
 * image pipelines, so callers don't care which one runs */
static struct {
    int    max_w;   /* 0 = unbounded */
    int    max_h;   /* 0 = unbounded (video: 480 when no box is given at all) */
    double quality; /* internal 0-100 scale (CLI -q is 0-10, x10 at parse),
                       higher = better; <0 = per-mode default */
    double max_fps; /* video only; 0 = keep every frame */
} opt = { 0, 0, -1.0, 0 };

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

/* ---- stdin input -----------------------------------------------------------
 * Probing the first bytes lets us pick the right strategy: image files are
 * slurped whole into memory so demuxers that need a seekable input still work
 * (HEIC/AVIF item layout, TIFF's no-parser whole-file read); every other
 * piped input is spooled to an unlinked temp file so it rewinds like a file
 * (the HDR peek, end-indexed containers). Streaming the probed prefix ahead of
 * the live pipe survives only as the fallback when no temp file can be
 * created. */

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
         * the output: the HDR-peak peek can always rewind, exactly as with a
         * file argument (and containers that outright need a seekable input —
         * mp4/mov without faststart, AVI — just work). Images are slurped
         * above for the same reason */
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
    avcodec_free_context(&p->enc);
    avcodec_free_context(&p->enc_a);
    avfilter_graph_free(&p->graph);
    av_frame_free(&p->dec_frame);
    av_frame_free(&p->filt_frame);
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

/* tile/thread count by output width: <512 -> 2, 480p-ish -> 4, 720-1080p -> 8,
 * 1440p+ -> 16 (the original VP9 tile-columns table, kept for the encoder
 * thread counts) */
static int width_tlog(int w)
{
    return w >= 2560 ? 3 : w >= 1280 ? 2 : w >= 512 ? 1 : 0;
}

static int width_threads(int w)
{
    return 2 << width_tlog(w);
}

/* the audio stream's header bitrate (mkv/webm declare none, which leaves the
 * audio cap off — AAC then rides the -q anchor) */
static int64_t source_audio_rate(const AVStream *ist)
{
    return ist->codecpar->bit_rate;
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
 * it keeps decoding to learn whether a second frame proves an animation and
 * whether the source carries real transparency (the AVIF auxiliary alpha
 * stream); a video peek wants only the orientation/HDR side data of the first
 * frame. */
static struct {
    int32_t m[9]; int set; double peak; int animated; int alpha;
} peeked;

static void peek_first_frame(AVFormatContext *ifmt, int vidx, int detect_anim)
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
            if (detect_anim && !peeked.alpha && frame_has_real_alpha(fr))
                peeked.alpha = 1;
            av_frame_unref(fr);
            if (frames >= 2)
                peeked.animated = 1;
            /* images keep decoding until both the animation verdict and the
             * alpha verdict are settled; a video peek wants only the first
             * frame's side data */
            if (peeked.animated && peeked.alpha)
                goto end;
        }
        if (frames > 0 && !detect_anim)
            break; /* the first frame settles it */
    }
end:
    avcodec_free_context(&dec);
    av_packet_free(&pkt);
    av_frame_free(&fr);
}

/* Image inputs become AVIF instead of MP4. Plain images arrive via the image
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

/* the geometry/timing/flags fields the AVIF alpha encoder inherits from the
 * already-configured color encoder; callers override what genuinely differs */
static void copy_enc_geometry(AVCodecContext *dst, const AVCodecContext *src)
{
    dst->width               = src->width;
    dst->height              = src->height;
    dst->pix_fmt             = src->pix_fmt;
    dst->time_base           = src->time_base;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->flags               = src->flags;
}

/* ==== Quality settings =======================================================
 * -q 0..10 (×10 to this internal 0..100 scale at parse) maps directly onto each
 * encoder's own CRF, using values recommended for web delivery: higher -q =
 * better quality = lower CRF = bigger file. There is no cross-format
 * calibration — each encoder's CRF is its own quality dial — and no separate
 * bitrate budget: CRF mode lets the encoder spend exactly the bits the chosen
 * quality needs, and --max controls the resolution (hence the size envelope).
 * Background on the ranges: doc/calibration.md.
 * ========================================================================== */

/* the internal 0..100 quality, with the default (no -q) applied: -q 8 — high
 * quality, the value the original webify defaulted images to */
static double quality(void)
{
    return opt.quality < 0 ? 80.0 : opt.quality;
}

/* x264 CRF for a -q: linear over the recommended web range, CRF 33 (-q 0, low)
 * down to 17 (-q 10, near visually lossless); the -q 8 default lands at ~20,
 * x264's "good quality" zone. x264 CRF is 0-51, default 23. */
static int x264_video_crf(void)
{
    return av_clip((int)lrint(33.0 - 0.16 * quality()), 12, 40);
}

/* still-image AVIF (libaom) CRF for a -q: linear over CRF 52 (-q 0, low) down
 * to 16 (-q 10, excellent); the -q 8 default lands at ~23, a high-quality AVIF
 * still. libaom CRF is 0-63. */
static int avif_still_crf(void)
{
    return av_clip((int)lrint(52.0 - 0.36 * quality()), 6, 55);
}

/* animated-AVIF CRF for a -q: ~10 CRF above the still curve — motion masks the
 * extra quantization and keeps animations from ballooning (CRF 62 at -q 0 down
 * to 26 at -q 10; -q 8 default ~33). */
static int avif_anim_crf(void)
{
    return av_clip((int)lrint(62.0 - 0.36 * quality()), 12, 63);
}

/* the AAC bitrate for a -q: recommended rates of 128k stereo / 96k mono at the
 * -q 8 default, half that at -q 0 and ~1.13x at -q 10. A lossy source's own
 * rate caps it (floored at a useful minimum) — bits past the source rate
 * cannot recover quality the input never had; lossless/PCM/multichannel
 * sources declare rates far above the cap and stay uncapped. */
static int64_t audio_bitrate(int mono, int64_t src)
{
    int64_t base = mono ? 96000 : 128000;
    int64_t bps  = (int64_t)(base * (0.5 + quality() / 160.0));
    int64_t lo   = mono ? 32000 : 48000;

    if (src > 0 && src < bps)
        bps = FFMAX(src, lo);
    return bps;
}

/* ==== end of quality settings ============================================= */

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
                         char *buf, size_t size)
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
    av_log(NULL, AV_LOG_WARNING, "HDR input (%s, %.6g-nit peak):"
           " tonemapping to SDR bt709\n",
           dec->color_trc == AVCOL_TRC_SMPTE2084 ? "PQ" : "HLG",
           peak * 100);
}

/* images: AVIF via libaom, stills and animations alike. -q maps to the
 * recommended CRF (avif_still_crf / avif_anim_crf, in the quality-settings
 * section). Everything stays 8-bit 4:2:0 = AV1 Main profile. */
static void setup_avif(Pipe *p, AVDictionary **opts)
{
    int crf = peeked.animated ? avif_anim_crf() : avif_still_crf();

    p->enc->bit_rate     = 0;
    p->enc->thread_count = width_threads(p->enc->width);
    av_dict_set_int(opts, "crf", crf, 0);
    av_dict_set(opts, "row-mt", "1", 0);
    if (peeked.animated) {
        /* animated GIF -> animated AVIF (the muxer's 'avis' brand),
         * inter-coded at the video gop — all-intra would spend a full
         * keyframe on every GIF frame (gop_size left at its default of 12
         * would too, every 12) */
        p->enc->gop_size = 240;
        av_dict_set(opts, "cpu-used", "4", 0);
    } else {
        /* stills: all-intra at the tuned speed (avifenc defaults to speed 6;
         * webify digs deeper — files are downloaded many times) */
        av_dict_set(opts, "usage", "allintra", 0);
        av_dict_set(opts, "still-picture", "1", 0);
        av_dict_set(opts, "cpu-used", "4", 0);
    }
}

/* video: H.264 via libx264 in pure CRF mode (its lookahead/mbtree plan the
 * rate ahead, so single-pass already targets a quality; no bitrate cap — CRF
 * spends what the quality needs, --max bounds the resolution). preset
 * veryslow: the recommended quality/size tradeoff for download-once web video
 * (placebo costs ~4x the time for ~1% fewer bytes). bit_rate stays 0 — a
 * nonzero rate would flip libx264 into ABR mode. */
static void setup_video(Pipe *p, AVDictionary **opts)
{
    p->enc->bit_rate     = 0;
    p->enc->gop_size     = 240;
    p->enc->thread_count = width_threads(p->enc->width);
    av_dict_set_int(opts, "crf", x264_video_crf(), 0);
    av_dict_set(opts, "preset", "veryslow", 0);
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
    /* crf 0 ~ lossless: alpha gradients band visibly while costing few bits */
    av_dict_set(&aopts, "crf", "0", 0);
    if (!peeked.animated) {
        av_dict_set(&aopts, "usage", "allintra", 0);
        av_dict_set(&aopts, "still-picture", "1", 0);
    }
    av_dict_set(&aopts, "row-mt", "1", 0);
    av_dict_set(&aopts, "cpu-used", "4", 0);
    ret = avcodec_open2(p->enc_a, codec, &aopts);
    av_dict_free(&aopts);
    return ret;
}

/* image != 0 selects the AVIF pipeline (libaom, alpha kept when really used);
 * otherwise the H.264 video pipeline (libx264). */
static int init_video(Pipe *p, AVFormatContext *ifmt, AVFormatContext *ofmt,
                      int stream_index, int image)
{
    AVStream *ist = ifmt->streams[stream_index];
    AVRational sar;
    const AVCodec *codec;
    AVDictionary *opts = NULL;
    const AVPacketSideData *psd;
    const int32_t *mat = NULL;
    char args[512], spec[768], scale[256] = "", rotate[40], tonemap[256] = "";
    int ret, hdr, alpha = 0;
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
        /* AVIF stays 4:2:0 (= AV1 Main profile, the one hardware decoders
         * reliably implement); libaom takes no RGB, so the high-quality
         * swscale conversion (IMAGE_SWS) does the subsampling. Alpha survives
         * in the yuva frame and becomes the avif muxer's auxiliary alpha
         * stream below, but only when the peek saw real transparency (a fully
         * opaque alpha channel would just waste an extra AV1 stream). */
        if (hdr)
            av_log(NULL, AV_LOG_WARNING, "HDR image input: colors may come "
                   "out washed; only video inputs are tonemapped\n");
        snprintf(spec, sizeof(spec), "%s%sformat=%s", rotate, scale,
                 peeked.alpha ? "yuva420p" : "yuv420p");
    } else {
        if (hdr) /* tone-map PQ/HLG to SDR bt709 (see tonemap_spec) */
            tonemap_spec(p->dec, ist, tonemap, sizeof(tonemap));
        snprintf(spec, sizeof(spec), DEINT_FILTER "%s%s%s%s", rotate, scale,
                 tonemap, VIDEO_FILTERS);
    }

    if ((ret = init_graph(p, "buffer", args, "buffersink", spec,
                          image ? "flags=" IMAGE_SWS : NULL)) < 0)
        return ret;

    const char *enc_name = image ? "libaom-av1" : "libx264";

    codec = avcodec_find_encoder_by_name(enc_name);
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
    if (image && p->enc->pix_fmt == AV_PIX_FMT_YUVA420P) {
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

    if (image)
        setup_avif(p, &opts);
    else
        setup_video(p, &opts);
    ret = avcodec_open2(p->enc, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        return ret;

    if (alpha && (ret = init_alpha(p, codec)) < 0)
        return ret;

    p->prog = !image;

    if (ofmt) {
        if ((ret = add_stream(ofmt, p->enc, &p->out_index)) < 0)
            return ret;
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
    /* FFmpeg's native AAC encoder takes planar float (fltp) */
    snprintf(spec, sizeof(spec), AUDIO_FILTERS, "fltp", mono ? "mono" : "stereo");
    if ((ret = init_graph(p, "abuffer", args, "abuffersink", spec, NULL)) < 0)
        return ret;

    codec = avcodec_find_encoder_by_name("aac");
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "aac encoder missing from this build\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (!(p->enc = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);

    p->enc->sample_rate = av_buffersink_get_sample_rate(p->sink);
    p->enc->sample_fmt  = (AVSampleFormat)av_buffersink_get_format(p->sink);
    if ((ret = av_buffersink_get_ch_layout(p->sink, &p->enc->ch_layout)) < 0)
        return ret;
    /* -q scales the audio too, capped by a lossy source's own rate (the
     * anchors and the why live in audio_bitrate, in the calibration section) */
    p->enc->bit_rate  = audio_bitrate(mono, source_audio_rate(ist));
    p->enc->time_base = AVRational{ 1, p->enc->sample_rate };
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        p->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(p->enc, codec, NULL)) < 0)
        return ret;
    /* aac consumes fixed-size frames; let the sink chunk them for us */
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

    if (!p->enc_a)
        return encode_one(ofmt, p, p->enc, p->out_index, frame);
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
    return encode_write(ofmt, p, NULL);                 /* flush encoder  */
}

/* the EXIF/HDR peek (and the image animation/alpha detection) consumed the
 * input; rewind by reopening, which covers both real files and the
 * spooled/slurped stdin paths behind the custom pb. Reopening invalidates the
 * stream indices, so they are rebound here; the discard flags reset too — the
 * caller re-applies those */
static int input_can_rewind(const AVFormatContext *ifmt)
{
    return ifmt->pb && (ifmt->pb->seekable & AVIO_SEEKABLE_NORMAL);
}

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

/* let the demuxer drop packets of the streams webify won't read */
static void discard_other_streams(AVFormatContext *ifmt, int vidx, int aidx)
{
    for (unsigned i = 0; i < ifmt->nb_streams; i++)
        if ((int)i != vidx && (int)i != aidx)
            ifmt->streams[i]->discard = AVDISCARD_ALL;
}

/* hand a finished in-memory output to its destination: stdout for a pipe,
 * otherwise the named file (the avif muxer assembles in memory for pipes) */
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
    char tmp_out[512] = "";
    const char *sink, *oname;
    int ret, vidx, aidx, image, mem_out = 0;
    int out_pipe = is_pipe(out_path);

    av_log_set_level(AV_LOG_WARNING);

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

    /* images peek for animation/alpha/orientation; HDR videos peek for the
     * source peak — both consume the input, so rewind after (HDR videos that
     * cannot rewind fall back to tag defaults) */
    if (image ||
        (is_hdr_trc((AVColorTransferCharacteristic)
                    ifmt->streams[vidx]->codecpar->color_trc) &&
         input_can_rewind(ifmt))) {
        peek_first_frame(ifmt, vidx, image);
        if ((ret = reopen_input(in_path, &ifmt, &io, image, &vidx, &aidx)) < 0)
            goto end;
    }

    discard_other_streams(ifmt, vidx, aidx);

    /* piped video output spools through a named temp file so the pipe changes
     * nothing about the bytes: faststart re-shuffles the finished MP4, a
     * seek-back a pipe cannot do. Named, not an unlinked fd: movenc's faststart
     * pass re-opens the output by URL to read it back. The file is streamed to
     * stdout and removed at the end. Images never need this — they are
     * assembled in memory below */
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

    oname = image ? "avif" : "mp4";
    if ((ret = avformat_alloc_output_context2(&ofmt, NULL, oname, sink)) < 0)
        goto end;
    /* deterministic output: the same input and options always produce the
     * same bytes. This is what makes the piped-output spool byte-identical to
     * a file run (and same-input reruns cache-friendly) */
    ofmt->flags |= AVFMT_FLAG_BITEXACT;

    if (!image)
        progress_start("encoding:");
    if ((ret = init_video(&video, ifmt, ofmt, vidx, image)) < 0)
        goto end;
    if (aidx >= 0 && (ret = init_audio(&audio, ifmt, ofmt, aidx)) < 0)
        goto end;

    if (!(pkt = av_packet_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* the avif muxer (mov family) seeks back to patch item offsets/sizes; for
     * stdout, write into memory and dump the finished file at the end (images
     * stay off disk on purpose — video uses the temp spool above) */
    mem_out = out_pipe && image;
    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        ret = mem_out ? avio_open_dyn_buf(&ofmt->pb)
                      : avio_open(&ofmt->pb, sink, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "cannot create '%s': %s\n", out_path,
                   err2str(ret));
            goto end;
        }
    }
    if (image) { /* loop animations forever, like GIF (avif takes a loop count;
                  * 0 = infinite — a no-op on a single-frame still) */
        av_dict_set(&muxopts, "loop", "0", 0);
    } else {
        /* the MP4 spelling of faststart; pipes get it too via the temp-file
         * spool above — only its no-temp-file fallback drops to a fragmented
         * MP4 (playable everywhere MSE is, but not by every old player) */
        av_dict_set(&muxopts, "movflags",
                    is_pipe(sink)
                        ? "+frag_keyframe+empty_moov+default_base_moof"
                        : "+faststart", 0);
    }
    ret = avformat_write_header(ofmt, &muxopts);
    av_dict_free(&muxopts);
    if (ret < 0)
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

end:
    progress_done(); /* leave stderr on a clean line for any error below */
    free_pipe(&video);
    free_pipe(&audio);
    av_packet_free(&pkt);
    avformat_close_input(&ifmt);
    close_stdin_io(&io);
    if (ofmt) {
        if (mem_out && ofmt->pb) {
            uint8_t *buf = NULL;
            int n = avio_close_dyn_buf(ofmt->pb, &buf);

            ofmt->pb = NULL;
            if (ret >= 0 && (ret = emit_output(out_pipe, out_path, buf, n)) < 0)
                av_log(NULL, AV_LOG_ERROR, "cannot write output: %s\n",
                       err2str(ret));
            av_free(buf);
        }
        if (!(ofmt->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
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
            "webify: transcode any popular video to H.264/AAC MP4,\n"
            "         or any popular image to AVIF (auto-detected)\n"
            "usage: webify [options] <input> [output]\n"
            "       '-' = stdin/stdout; omitting [output] writes to stdout\n"
            "  -q, --quality <0-10>   target quality, higher is better\n"
            "                         (default: 8 for images; video picks the\n"
            "                         classic 480p look at the smallest size)\n"
            "  -m, --max [HxW|S][@F]  downscale to fit H px tall / W px wide,\n"
            "                         never upscale; a single number S bounds\n"
            "                         both sides, a missing side is unbounded\n"
            "                         (480x854, 720, 480x, x854); video fits\n"
            "                         a height of 480 when no box is given at\n"
            "                         all. @F drops frames to cap the frame\n"
            "                         rate (video only; @30 halves a 60fps\n"
            "                         clip); combine freely: 480x854@30, 480x@30\n"
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
    enum { OPT_VERSION = 1000 };
    static const struct option longopts[] = {
        { "quality", required_argument, NULL, 'q' },
        { "max",     required_argument, NULL, 'm' },
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
            opt.quality *= 10; /* the internal scale is 0-100 */
            break;
        case 'm':
            if (parse_max(optarg) < 0)
                return 2;
            break;
        default:
            return usage(stderr, 2);
        }
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
