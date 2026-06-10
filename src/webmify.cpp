/*
 * webmify — transcode any popular video file to VP9/Opus WebM, or any popular
 * image file to WebP (auto-detected). One option set covers both modes:
 * -q/--quality 0-10 (mapped to VP9 CRF for video, WebP quality for images)
 * and -m/--max [HxW | S][@F] (downscale to fit H px tall / W px wide — a
 * single number S bounds both, a missing side ("480x", "x854") is
 * unbounded — never upscale; @F is a video-only frame rate cap, off by
 * default).
 *
 * Everything is tuned for serving the result over the internet:
 *   - two-pass VP9 whenever the input can rewind (files, spooled stdin) —
 *     only then does libvpx use alt-ref frames and plan rates ahead, which
 *     measures 10-20% smaller at equal quality; streamed stdin single-passes
 *     (the stats pass runs at cpu-used 4; it barely affects the stats)
 *   - cpu-used 1 / frame-parallel 0 by default (Google's VOD setting);
 *     --fast single-passes at cpu-used 4 (~4x faster, the classic crf-33
 *     look) and --best two-passes at cpu-used 0 + arnr-maxframes 15 and
 *     spools piped input so the stats pass always runs (measured 1-7%
 *     smaller than default at equal-or-better SSIM, ~75% more time);
 *     tiles/threads follow Google's VOD table for the output width
 *   - the rate caps follow the input: >30fps output scales the budget up
 *     (Google's 60fps rows are ~1.7x their 30fps ones), and the source
 *     stream's own bitrate — weighted by how its codec compares to VP9 —
 *     caps the budget from above, so bits are never spent re-encoding
 *     compression artifacts more faithfully than the source stored them
 *   - the seek index (cues) is written at the head of the file (faststart)
 *   - mono audio stays mono (48k) instead of being upmixed to stereo (64k),
 *     -q scales the Opus bitrate along with the video quality, and a lossy
 *     source's own rate caps it (Opus loses nothing at the rate the source
 *     itself managed with a weaker codec)
 *   - images use libwebp method 6 (cwebp -m 6), the densest WebP encoding;
 *     single-frame RGB images are also tried lossless and the smaller wins,
 *     and their lossy candidate uses sharp RGB->YUV (cwebp -sharp_yuv:
 *     crisper text/edges for ~1% more bytes; animations skip it — measured
 *     ~20% more bytes there)
 *   - EXIF orientation (frame side data) and display-matrix rotation are
 *     baked into the pixels, including the mirrored variants
 *   - HDR video (PQ/HLG) is tonemapped to SDR bt709 (zscale linearize +
 *     hable), with the source peak taken from in-stream SEI or container
 *     metadata (1000-nit fallback) since ffmpeg 8's zscale strips HDR side
 *     data before tonemap can read it
 *   - --next switches to the next-gen formats at the same visual targets:
 *     video becomes AV1/Opus WebM via libaom and every image becomes AVIF
 *     (animated GIF -> animated AVIF, alpha kept as the auxiliary stream).
 *     -q maps to the visually equivalent setting, not the same number:
 *     every mapping below is a piecewise fit of measured equal-SSIM points
 *     against the non---next pipeline (photo/noise/graphics corpora, biased
 *     a hair *below* parity by design — see doc/next-calibration.md), and
 *     the VP9-anchored rate budget is converted by the same codec-efficiency
 *     table that weights the source cap — so --next changes the file size,
 *     never the look
 *   - progress on stderr when it is a terminal and the duration is known
 *
 * With no options, video is roughly the equivalent of two-pass:
 *   ffmpeg -i IN -vf scale=-2:480 -c:v libvpx-vp9 -crf 36 -b:v 530k
 *          -maxrate 778k -row-mt 1 -tile-columns 1 -threads 4
 *          -cpu-used 1 -deadline good -g 240 -pix_fmt yuv420p
 *          -c:a libopus -b:a 64k OUT.webm
 * except that smaller-than-480p input is not upscaled, and the bitrate caps
 * follow the job (anchored at 750k/1100k for 854x480 crf 33, the
 * single-pass/piped default): they scale with the output pixel count, the
 * CRF target, the >30fps output frame rate, and are capped from above by
 * the source stream's own codec-weighted bitrate.
 *
 * Official libav* API only; structure follows FFmpeg's doc/examples/transcode.c.
 */
#include <errno.h>
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
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#define WEBMIFY_VERSION "1.1"

#define VIDEO_FILTERS "format=yuv420p" /* the scale step is built in init_video */
#define AUDIO_FILTERS "aresample=48000,aformat=sample_fmts=flt:channel_layouts=%s"
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
} opt = { 0, 0, -1.0, 0, 0, 0 };

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
 * (HEIC/AVIF item layout, TIFF's no-parser whole-file read); video containers
 * whose index can sit at the end (mp4/mov without faststart, AVI) are spooled
 * to an unlinked temp file so no feature is lost; everything else streams,
 * with the probed prefix replayed ahead of the live pipe. */

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

/* walk top-level mp4 atoms in the prefix: a 'moov' before any 'mdat' means
 * faststart, i.e. the file is streamable without seeking */
static int prefix_has_early_moov(const uint8_t *buf, size_t len)
{
    uint64_t pos = 0;

    while (pos + 8 <= len) {
        uint64_t size = AV_RB32(buf + pos);

        if (!memcmp(buf + pos + 4, "moov", 4))
            return 1;
        if (!memcmp(buf + pos + 4, "mdat", 4))
            return 0;
        if (size == 1) { /* 64-bit atom size */
            if (pos + 16 > len)
                break;
            size = AV_RB64(buf + pos + 8);
        }
        if (size < 8 || size > len - pos) /* 0 = "extends to EOF"; <8 =
                                           * corrupt; past the prefix = done
                                           * (a crafted 64-bit size could
                                           * otherwise wrap pos and loop) */
            break;
        pos += size;
    }
    return 0; /* moov not up front: assume it sits at the end */
}

/* containers whose streaming path would drop features: mp4/mov keeps its
 * index (moov) at the end unless written with faststart, and AVI may be
 * non-interleaved, which the demuxer only handles with seekable input */
static int probe_needs_file(const AVInputFormat *fmt, const uint8_t *buf, size_t size)
{
    if (!fmt)
        return 0;
    if (!strcmp(fmt->name, "avi"))
        return 1;
    if (!strncmp(fmt->name, "mov,", 4))
        return !prefix_has_early_moov(buf, size);
    return 0;
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

/* forward the whole pipe to a temp file, unlinked right away so it is gone
 * when the process exits no matter how it exits */
static int spool_to_file(StdinIO *io)
{
    const char *dir = getenv("TMPDIR");
    char path[512];
    uint8_t chunk[IO_BUFSIZE];
    int n, ret;

    snprintf(path, sizeof(path), "%s/webmify-XXXXXX",
             dir && *dir ? dir : "/tmp");
    if ((io->fd = mkstemp(path)) < 0)
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
    } else if ((probe_needs_file(fmt, io->buf, io->size) || opt.effort > 0) &&
               (ret = spool_to_file(io)) < 0) {
        /* --best spools every piped video (not just the containers that
         * need it) so the stats pass can always run: two-pass measures
         * 10-20% smaller than the streamed single-pass */
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
    if (!(*ifmt = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    (*ifmt)->pb = io->pb;
    return avformat_open_input(ifmt, "", NULL, NULL);
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
    if ((ret = avcodec_parameters_to_context(dec, st->codecpar)) < 0)
        return ret;
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 0; /* auto */
    if (dec->codec_type == AVMEDIA_TYPE_VIDEO)
        dec->framerate = av_guess_frame_rate(ifmt, st, NULL);
    if ((ret = avcodec_open2(dec, codec, NULL)) < 0)
        return ret;
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

/* How far a bit goes in the source codec relative to VP9. Reproducing what
 * an older codec stored needs fewer VP9 bits than the source spent (its bits
 * bought less quality); matching AV1 needs more. Used only to *cap* the rate
 * budget, so the tiers can stay coarse. */
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

/* the input video stream's bitrate, best effort: the stream's own header
 * value, else the container average minus the rates the other streams
 * declare — which overestimates the video share (mux overhead, undeclared
 * streams), erring on the side of a looser cap */
static int64_t source_video_rate(const AVFormatContext *ifmt, int vidx)
{
    int64_t rate = ifmt->streams[vidx]->codecpar->bit_rate;

    if (rate <= 0 && ifmt->bit_rate > 0) {
        rate = ifmt->bit_rate;
        for (unsigned i = 0; i < ifmt->nb_streams; i++)
            if ((int)i != vidx && ifmt->streams[i]->codecpar->bit_rate > 0)
                rate -= ifmt->streams[i]->codecpar->bit_rate;
    }
    return rate;
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

/* EXIF rotation and HDR peak brightness (SEI) only surface as side data of a
 * *decoded frame*, and the filter graph (with its transpose/tonemap steps)
 * must exist before frames flow; so when the input can rewind — images
 * always can: slurped, spooled or real files — decode the first frame just
 * to look at it, and let the caller rewind. With detect_anim (--next image
 * inputs, whose still/animation and alpha/no-alpha encoder setups differ
 * before any frame flows) it keeps decoding to also learn whether a second
 * frame proves an animation and whether any frame really uses transparency,
 * stopping as soon as both are settled. */
static struct { int32_t m[9]; int set; double peak; int animated; int alpha; } peeked;

static void peek_first_frame(AVFormatContext *ifmt, int vidx, int detect_anim)
{
    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int ret, flushed = 0, frames = 0;

    peeked.set      = 0;
    peeked.peak     = 0;
    peeked.animated = 0;
    peeked.alpha    = 0;
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
            const AVFrameSideData *sd =
                av_frame_get_side_data(fr, AV_FRAME_DATA_DISPLAYMATRIX);

            if (++frames == 1) {
                if (sd && sd->size >= 9 * sizeof(int32_t)) {
                    memcpy(peeked.m, sd->data, sizeof(peeked.m));
                    peeked.set = 1;
                }
                if ((sd = av_frame_get_side_data(fr, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) &&
                    ((const AVContentLightMetadata *)sd->data)->MaxCLL > 0)
                    peeked.peak =
                        ((const AVContentLightMetadata *)sd->data)->MaxCLL / 100.0;
                else if ((sd = av_frame_get_side_data(fr, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) &&
                         ((const AVMasteringDisplayMetadata *)sd->data)->has_luminance)
                    peeked.peak = av_q2d(((const AVMasteringDisplayMetadata *)
                                          sd->data)->max_luminance) / 100.0;
            }
            if (detect_anim && !peeked.alpha && frame_has_real_alpha(fr))
                peeked.alpha = 1;
            av_frame_unref(fr);
            if (frames >= 2)
                peeked.animated = 1;
            if (peeked.animated && peeked.alpha)
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

/* libaom only delivers the AV1 sequence header alongside its first encoded
 * packet (aomedia bug #2208), but the webm muxer writing to a pipe needs it
 * in CodecPrivate when the header goes out — seekable outputs get
 * back-patched later, pipes error out. The sequence header depends on the
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
    c->width               = base->width;
    c->height              = base->height;
    c->pix_fmt             = base->pix_fmt;
    c->time_base           = base->time_base;
    c->framerate           = base->framerate;
    c->sample_aspect_ratio = base->sample_aspect_ratio;
    c->bit_rate            = base->bit_rate;
    c->rc_max_rate         = base->rc_max_rate;
    c->gop_size            = base->gop_size;
    c->thread_count        = base->thread_count;
    c->flags               = AV_CODEC_FLAG_GLOBAL_HEADER; /* no pass flags */
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
    if (avcodec_send_frame(c, fr) < 0 || avcodec_send_frame(c, NULL) < 0 ||
        avcodec_receive_packet(c, pkt) < 0)
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

/* image != 0 selects the WebP pipeline: no scaling, alpha kept, libwebp.
 * pass 0 = single-pass video; pass 1 = stats-gathering run (ofmt is NULL, no
 * output stream is created); pass 2 = final encode driven by `stats`. */
/* the calibrated still-image AVIF CRF for the current -q: equal-SSIM fit
 * against the WebP pipeline, near-linear to q 80 then diving with cwebp's
 * premium top end (see the mapping comment in init_video and
 * doc/next-calibration.md). Shared with the 444/420 race re-encode. */
static int avif_still_crf(void)
{
    double q = opt.quality < 0 ? 80.0 : opt.quality;
    double c = q <= 80 ? 52.0 - 0.30 * q : 28.0 - 1.10 * (q - 80.0);
    return (int)lrint(FFMAX(c, 0.0));
}

static int init_video(Pipe *p, AVFormatContext *ifmt, AVFormatContext *ofmt,
                      int stream_index, int image, int pass, const char *stats)
{
    AVStream *ist = ifmt->streams[stream_index];
    AVRational sar;
    const AVCodec *codec;
    AVStream *ost;
    AVDictionary *opts = NULL, *primeopts = NULL;
    const AVPacketSideData *psd;
    const int32_t *mat = NULL;
    uint8_t *extra = NULL;
    char args[512], spec[640], scale[256] = "", rotate[40], tonemap[224] = "";
    int ret, rgb = 0, hdr = 0, alpha = 0, extra_size = 0, next444 = 0;
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
        int srcrgb = desc &&
              (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL));
        /* --next stills from RGB sources race a 4:4:4 candidate above the
         * q-80 break: that is the premium-quality zone where the WebP
         * side's lossless race starts winning on sharp graphics, and
         * full-resolution chroma is what closes most of that gap. The race
         * in finish_still_next keeps 444 only while it is cheap enough to
         * stay under the WebP size (saturated noise explodes in 444).
         * YUV sources stay 4:2:0 — both pipelines then share the source's
         * chroma and parity is free. --fast skips it like it skips the
         * WebP race. */
        next444 = opt.next && srcrgb && !peeked.animated &&
                  opt.quality > 80.0 && opt.effort >= 0;
        rgb = !opt.next && srcrgb;
        snprintf(spec, sizeof(spec), "%s%sformat=%s", rotate, scale,
                 opt.next ? (peeked.alpha ? (next444 ? "yuva444p" : "yuva420p")
                                          : (next444 ? "yuv444p"  : "yuv420p"))
                 : rgb    ? av_get_pix_fmt_name(AV_PIX_FMT_RGB32)
                          : "yuv420p|yuva420p");
        /* --next keeps alpha only when the peek saw real transparency: a
         * fully opaque alpha channel would just waste an extra AV1 stream */
    } else {
        if (hdr) {
            /* HDR (PQ/HLG) source: linearize with zimg, tone-map to SDR in
             * linear light, re-encode as bt709 — without this the encode
             * "succeeds" but every color comes out gray and washed. After
             * the scaler so the float math runs at output resolution.
             * Untagged streams get the in-practice-universal bt2020 guess.
             * tonemap needs the source's peak brightness, but ffmpeg 8's
             * zscale strips HDR side data during conversion, so fish it out
             * of the input ourselves: the peeked first frame (in-stream
             * SEI), the container (mkv/mp4 boxes), or assume the typical
             * 1000-nit master. */
            const AVPacketSideData *sd;
            double peak = peeked.peak;

            if (peak <= 0 &&
                (sd = av_packet_side_data_get(ist->codecpar->coded_side_data,
                                              ist->codecpar->nb_coded_side_data,
                                              AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) &&
                ((const AVContentLightMetadata *)sd->data)->MaxCLL > 0)
                peak = ((const AVContentLightMetadata *)sd->data)->MaxCLL / 100.0;
            if (peak <= 0 &&
                (sd = av_packet_side_data_get(ist->codecpar->coded_side_data,
                                              ist->codecpar->nb_coded_side_data,
                                              AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) &&
                ((const AVMasteringDisplayMetadata *)sd->data)->has_luminance)
                peak = av_q2d(((const AVMasteringDisplayMetadata *)
                               sd->data)->max_luminance) / 100.0;
            if (peak <= 0)
                peak = 10.0;

            snprintf(tonemap, sizeof(tonemap),
                     "zscale=tin=%s%s%s:t=linear:npl=100,format=gbrpf32le,"
                     "zscale=p=bt709,tonemap=hable:desat=0:peak=%.6g,"
                     "zscale=t=bt709:m=bt709:r=tv,",
                     p->dec->color_trc == AVCOL_TRC_SMPTE2084
                         ? "smpte2084" : "arib-std-b67",
                     p->dec->colorspace == AVCOL_SPC_UNSPECIFIED
                         ? ":min=2020_ncl" : "",
                     p->dec->color_primaries == AVCOL_PRI_UNSPECIFIED
                         ? ":pin=2020" : "",
                     peak);
            if (pass != 1)
                av_log(NULL, AV_LOG_WARNING, "HDR input (%s, %.6g-nit peak):"
                       " tonemapping to SDR bt709\n",
                       p->dec->color_trc == AVCOL_TRC_SMPTE2084 ? "PQ" : "HLG",
                       peak * 100);
        }
        snprintf(spec, sizeof(spec), "%s%s%s%s", rotate, scale, tonemap,
                 VIDEO_FILTERS);
    }

    if ((ret = init_graph(p, "buffer", args, "buffersink", spec,
                          image ? "flags=" IMAGE_SWS : NULL)) < 0)
        return ret;

    /* libwebp_anim writes stills too and is needed for animated GIF -> WebP */
    codec = avcodec_find_encoder_by_name(opt.next ? "libaom-av1"
                                         : image ? "libwebp_anim" : "libvpx-vp9");
    if (!codec && image && !opt.next)
        codec = avcodec_find_encoder_by_name("libwebp");
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "%s encoder missing from this build\n",
               opt.next ? "libaom-av1" : image ? "libwebp" : "libvpx-vp9");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (!(p->enc = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);

    p->enc->width               = av_buffersink_get_w(p->sink);
    p->enc->height              = av_buffersink_get_h(p->sink);
    p->enc->pix_fmt             = (AVPixelFormat)av_buffersink_get_format(p->sink);
    if (opt.next && (p->enc->pix_fmt == AV_PIX_FMT_YUVA420P ||
                     p->enc->pix_fmt == AV_PIX_FMT_YUVA444P)) {
        /* libaom takes no alpha plane: the color planes of a yuva* frame
         * are a valid yuv* frame as-is, and the alpha plane rides as a
         * second AV1 stream that the avif muxer stores as the auxiliary
         * alpha item (the standard AVIF transparency layout) */
        p->enc->pix_fmt = p->enc->pix_fmt == AV_PIX_FMT_YUVA420P
                        ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV444P;
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

    if (image && opt.next) {
        /* AVIF. -q buys the same look as the WebP pipeline, not the same
         * number: each curve below is a piecewise fit of measured equal-SSIM
         * points against the WebP output across photo/noise/graphics
         * fixtures, rounded to the slightly-lower-quality side so the size
         * win is never paid back as quality (doc/next-calibration.md has
         * the data). Stills track cwebp's scale: near-linear up to q 80,
         * then cwebp's top end buys disproportionate quality and the crf
         * has to dive (q 80 -> crf 28, q 95 -> 12, q 100 -> 6). Animations
         * get their own much-higher curve: animated WebP is far weaker than
         * stills WebP, so AVIF matches it from crf 63 (q <= 50) easing to
         * 56 at q 80 and 32 at q 95 — measured -96% bytes at equal SSIM on
         * dithered GIFs. bit_rate 0 selects pure constant-quality mode — no
         * rate caps, like the WebP side. */
        double q   = opt.quality < 0 ? 80.0 : opt.quality;
        int    crf = peeked.animated
                   ? (int)lrint(q <= 50 ? 63.0
                              : q <= 80 ? 63.0 - (q - 50.0) * (7.0 / 30.0)
                                        : 56.0 - 1.60 * (q - 80.0))
                   : avif_still_crf();
        int   tlog = p->enc->width >= 2560 ? 3 : p->enc->width >= 1280 ? 2
                   : p->enc->width >= 512  ? 1 : 0;
        char crfs[16];

        snprintf(crfs, sizeof(crfs), "%d", crf);
        p->enc->bit_rate     = 0;
        p->enc->thread_count = 2 << tlog; /* same width table as video */
        av_dict_set(&opts, "crf", crfs, 0);
        av_dict_set(&opts, "row-mt", "1", 0);
        if (peeked.animated) {
            /* animated GIF -> animated AVIF (the muxer's 'avis' brand),
             * inter-coded at the video tiers' speeds — all-intra would
             * spend a full keyframe on every GIF frame (gop_size left at
             * its default of 12 would too, every 12). --best keeps the
             * default speed for the same reason as video: cpu-used 3
             * measured +0.2% bytes at 1.8x the time */
            p->enc->gop_size = 240;
            av_dict_set(&opts, "cpu-used", opt.effort < 0 ? "6" : "4", 0);
        } else {
            /* encoder effort (allintra speed 0-9): avifenc defaults to
             * speed 6; webmify's default digs deeper — files are downloaded
             * many times — --fast matches the quick end, --best the
             * slowest practical search. --fast stops at 6: speed 7 measured
             * the same wall time as 6 but -.008 SSIM (a strictly worse
             * point), while 6 stays within WebP's own fast-tier drop */
            av_dict_set(&opts, "usage", "allintra", 0); /* like avifenc */
            av_dict_set(&opts, "still-picture", "1", 0);
            av_dict_set(&opts, "cpu-used", opt.effort < 0 ? "6"
                        : opt.effort > 0 ? "2" : "4", 0);
        }
    } else if (image) {
        char q[32];

        snprintf(q, sizeof(q), "%g", opt.quality < 0 ? 80.0 : opt.quality);
        av_dict_set(&opts, "quality", q, 0); /* default matches cwebp's 80 */
        /* cwebp's -m 6: best compression the format allows, worth the extra
         * encode time for files that are downloaded many times. --fast
         * drops to cwebp's default -m 4: measured 2-15x faster (dithered
         * animations gain the most) for 2-11% more bytes */
        av_dict_set(&opts, "compression_level", opt.effort < 0 ? "4" : "6", 0);
    } else {
        /* -q 0-10 maps linearly onto VP9's CRF 63-0. With no -q, two-pass
         * defaults to crf 36: measured against the old single-pass crf 33 it
         * is slightly *better* on SSIM/PSNR while ~8-20% smaller, because
         * two-pass actually reaches its quality target. Single-pass (piped
         * input) keeps the classic crf 33 (= -q 4.8).
         *
         * The constrained-quality caps start from Google's published 480p
         * VOD numbers (750k avg / 1100k max at 854x480 crf 33) and follow
         * the job: the output pixel count, the CRF target (VP9 bitrate
         * roughly doubles per 6 CRF steps), and the output frame rate above
         * 30 (Google's 50-60fps rows run ~1.7x their 24-30fps ones at the
         * same crf, hence x(fps/30)^0.75). Google's minrate is dropped on
         * purpose: a rate *floor* can only add bytes at a fixed quality
         * target (measured 0-2% smaller, SSIM unchanged). Finally, the
         * source stream's own bitrate, weighted by its codec's efficiency
         * vs VP9, caps the budget from above: past the source's own spend,
         * bits only reproduce its artifacts more faithfully (measured 27%
         * smaller on a 400kbps input, SSIM vs that source -0.0025). The
         * ceiling is deliberately NOT rescaled when downscaling or dropping
         * frames — those only discard source information (and downscaling
         * washes artifacts out), so the full source rate stays a valid,
         * conservative ceiling for any smaller rendition. */
        int crf  = opt.quality < 0 ? (pass ? 36 : 33)
                 : (int)lrint((100.0 - opt.quality) * 63.0 / 100.0);
        if (opt.next) {
            /* libaom beats libvpx by a growing margin as CRF rises (equal
             * SSIM measured at +4 around crf 44, +2 around 33, none at or
             * below 20), so the CRF is nudged up to return that surplus as
             * bytes instead of quality the VP9 output never had. The fast
             * tier needs 4 more: single-pass cpu-used 6 loses far less
             * quality than vpx's fast tier does, measured at equal SSIM
             * against it (doc/next-calibration.md). The budget below is
             * computed from the shifted CRF — it tracks the quality level
             * actually encoded. */
            crf += crf > 20 ? (crf - 20) / 6 : 0;
            if (opt.effort < 0)
                crf += 4;
            crf = FFMIN(crf, 63);
        }
        double f = ((double)p->enc->width * p->enc->height) / (854.0 * 480.0)
                 * exp2((33 - crf) / 6.0);
        double  ofps = p->enc->framerate.num > 0 ? av_q2d(p->enc->framerate) : 0;
        int64_t src  = source_video_rate(ifmt, stream_index);
        /* tiles/threads follow Google's VOD table by output width:
         * <512 -> 0/2, 480p-ish -> 1/4, 720-1080p -> 2/8, 1440p+ -> 3/16 */
        int tlog = p->enc->width >= 2560 ? 3 : p->enc->width >= 1280 ? 2
                 : p->enc->width >= 512  ? 1 : 0;
        char crfs[16], tiles[4];

        if (ofps > 30)
            f *= pow(ofps / 30.0, 0.75);
        if (src >= 32000) /* absent (0) or absurd header rates: no cap */
            f = FFMIN(f, src * codec_weight(ist->codecpar->codec_id) / 750000.0);
        if (opt.next) /* the budget above is anchored on VP9 numbers; the same
                      * codec-efficiency table that weights the source cap
                      * converts it to AV1 (1.3x the quality per bit — note
                      * an AV1 source then caps a --next job at exactly its
                      * own rate). Together with the CRF nudge above, --next
                      * changes the size, not the look. */
            f /= codec_weight(AV_CODEC_ID_AV1);
        p->enc->bit_rate     = (int64_t)(750000 * f);
        p->enc->rc_max_rate  = (int64_t)(1100000 * f);
        p->enc->gop_size     = 240;
        p->enc->thread_count = 2 << tlog;
        snprintf(crfs, sizeof(crfs), "%d", crf);
        snprintf(tiles, sizeof(tiles), "%d", tlog);
        av_dict_set(&opts, "crf", crfs, 0);
        av_dict_set(&opts, "row-mt", "1", 0);
        av_dict_set(&opts, "tile-columns", tiles, 0);
        /* encoder effort by tier — each step has to pay for its time:
         * --fast cpu-used 4 (Google's stats-pass speed, ~4x faster than
         * the default), default cpu-used 1 (Google's VOD setting), --best
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
         * --best keeps the default encoder settings; it still buys its
         * other measured win, spooling piped input for a universal stats
         * pass */
        if (opt.next) {
            av_dict_set(&opts, "cpu-used", pass == 1 ? "6"
                        : opt.effort < 0 ? "6" : "4", 0);
        } else {
            av_dict_set(&opts, "cpu-used", pass == 1 ? "4"
                        : opt.effort < 0 ? "4" : opt.effort > 0 ? "0" : "1", 0);
            if (opt.effort > 0)
                av_dict_set(&opts, "arnr-maxframes", "15", 0);
            av_dict_set(&opts, "deadline", "good", 0);
            /* libvpx defaults to frame-parallel decoding mode, which turns
             * off backward-adaptive entropy coding; no browser needs that */
            av_dict_set(&opts, "frame-parallel", "0", 0);
        }
    }
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

    if (alpha) {
        AVDictionary *aopts = NULL;

        if (!(p->enc_a = avcodec_alloc_context3(codec)))
            return AVERROR(ENOMEM);
        p->enc_a->width               = p->enc->width;
        p->enc_a->height              = p->enc->height;
        p->enc_a->pix_fmt             = AV_PIX_FMT_GRAY8; /* monochrome AV1 —
                                         the muxer wants one plane here */
        p->enc_a->time_base           = p->enc->time_base;
        p->enc_a->sample_aspect_ratio = p->enc->sample_aspect_ratio;
        p->enc_a->thread_count        = p->enc->thread_count;
        p->enc_a->bit_rate            = 0;
        p->enc_a->flags               = p->enc->flags;
        /* alpha is full-range by definition (MIAF), and decoders assume so:
         * left at the limited-range default the bitstream says "tv" and
         * libavif-class decoders stretch 16-235 to 0-255, distorting every
         * gradient (measured SSIM 0.90 on a radial alpha vs 1.0 intended) */
        p->enc_a->color_range         = AVCOL_RANGE_JPEG;
        /* crf 0 ~ lossless: lossy WebP also stores its alpha plane
         * losslessly by default, and alpha gradients band visibly while
         * costing few bits */
        av_dict_set(&aopts, "crf", "0", 0);
        if (!peeked.animated) {
            av_dict_set(&aopts, "usage", "allintra", 0);
            av_dict_set(&aopts, "still-picture", "1", 0);
        }
        av_dict_set(&aopts, "row-mt", "1", 0);
        av_dict_set(&aopts, "cpu-used",
                    opt.effort < 0 ? (peeked.animated ? "6" : "7") : "4", 0);
        ret = avcodec_open2(p->enc_a, codec, &aopts);
        av_dict_free(&aopts);
        if (ret < 0)
            return ret;
    }

    /* a still might do better lossless: race them (--fast skips the race
     * and the sharp_yuv re-encode — the streamed packet ships as-is).
     * --next races 4:4:4 vs 4:2:0 instead, same hold-the-frame plumbing */
    p->dual = image && (rgb || next444) && opt.effort >= 0;
    p->prog = !image;

    if (ofmt) {
        if (!(ost = avformat_new_stream(ofmt, NULL)))
            return AVERROR(ENOMEM);
        if ((ret = avcodec_parameters_from_context(ost->codecpar, p->enc)) < 0)
            return ret;
        if (extra) { /* the primed AV1 sequence header (see above) */
            ost->codecpar->extradata      = extra;
            ost->codecpar->extradata_size = extra_size;
            extra = NULL;
        }
        ost->time_base = p->enc->time_base;
        p->out_index = ost->index;
        if (p->enc_a) {
            if (!(ost = avformat_new_stream(ofmt, NULL)))
                return AVERROR(ENOMEM);
            if ((ret = avcodec_parameters_from_context(ost->codecpar, p->enc_a)) < 0)
                return ret;
            ost->time_base = p->enc_a->time_base;
            p->out_index_a = ost->index;
        }
    }

    return alloc_pipe_buffers(p);
}

static int init_audio(Pipe *p, AVFormatContext *ifmt, AVFormatContext *ofmt,
                      int stream_index)
{
    AVStream *ist = ifmt->streams[stream_index];
    const AVCodec *codec;
    AVStream *ost;
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
    snprintf(spec, sizeof(spec), AUDIO_FILTERS, mono ? "mono" : "stereo");
    if ((ret = init_graph(p, "abuffer", args, "abuffersink", spec, NULL)) < 0)
        return ret;

    if (!(codec = avcodec_find_encoder_by_name("libopus"))) {
        av_log(NULL, AV_LOG_ERROR, "libopus encoder missing from this build\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (!(p->enc = avcodec_alloc_context3(codec)))
        return AVERROR(ENOMEM);

    p->enc->sample_rate = av_buffersink_get_sample_rate(p->sink);
    p->enc->sample_fmt  = (AVSampleFormat)av_buffersink_get_format(p->sink);
    if ((ret = av_buffersink_get_ch_layout(p->sink, &p->enc->ch_layout)) < 0)
        return ret;
    /* -q scales the audio too, anchored at 64k stereo / 48k mono for the
     * default (= -q 4.8): half of that at -q 0, ~1.5x at -q 10. The lossy
     * source's own rate then caps it (floored at Opus's useful minimum):
     * Opus packs at least as much quality per bit as any codec this build
     * decodes, so bits past the source rate cannot recover quality the
     * input never had. Lossless/PCM/multichannel sources declare rates far
     * above the cap and stay uncapped. */
    {
        double  q   = opt.quality < 0 ? 48.0 : opt.quality;
        int64_t bps = (int64_t)((mono ? 48000 : 64000) * (0.5 + q / 96.0));
        int64_t src = ist->codecpar->bit_rate;

        if (src > 0 && src < bps)
            bps = FFMAX(src, mono ? 16000 : 24000);
        p->enc->bit_rate = bps;
    }
    p->enc->time_base = AVRational{ 1, p->enc->sample_rate };
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        p->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(p->enc, codec, NULL)) < 0)
        return ret;
    /* opus consumes fixed-size frames; let the sink chunk them for us */
    av_buffersink_set_frame_size(p->sink, p->enc->frame_size);

    if (!(ost = avformat_new_stream(ofmt, NULL)))
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_from_context(ost->codecpar, p->enc)) < 0)
        return ret;
    ost->time_base = p->enc->time_base;
    p->out_index = ost->index;

    return alloc_pipe_buffers(p);
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
        p->enc_pkt->stream_index = out_index;
        av_packet_rescale_ts(p->enc_pkt, enc->time_base,
                             ofmt->streams[out_index]->time_base);
        if ((ret = av_interleaved_write_frame(ofmt, p->enc_pkt)) < 0)
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

/* fresh encoder context with the streaming context's geometry, for the
 * one-frame re-encodes of the still race */
static AVCodecContext *clone_image_enc(const AVCodecContext *base)
{
    AVCodecContext *c = avcodec_alloc_context3(base->codec);

    if (!c)
        return NULL;
    c->width               = base->width;
    c->height              = base->height;
    c->pix_fmt             = base->pix_fmt;
    c->time_base           = base->time_base;
    c->sample_aspect_ratio = base->sample_aspect_ratio;
    c->flags               = base->flags;
    return c;
}

/* --next premium still race (RGB source, q > 80): the held frame is
 * 4:4:4 — full-resolution chroma is what closes most of the gap to the
 * WebP side's lossless race on sharp graphics — but saturated-noise
 * chroma explodes in 4:4:4 (measured 1.9x the 4:2:0 bytes on the
 * mandelbrot fixture, *bigger* than the WebP output). So both get
 * encoded and 444 ships only while it costs <= 1.35x the 420 candidate:
 * graphics measure ~1.28, photos ~1.15, noise ~1.9, and 1.35 sits under
 * the smallest measured WebP/420 size ratio (1.5), keeping the winner
 * smaller than the WebP output either way (doc/next-calibration.md). */
static int finish_still_next(AVFormatContext *ofmt, Pipe *p)
{
    AVCodecContext *e420 = NULL;
    AVDictionary *opts = NULL;
    AVPacket *c444 = av_packet_alloc(), *c420 = av_packet_alloc();
    AVPacket *apkt = av_packet_alloc(), *pick;
    AVFrame *m = NULL, *f = NULL, *a = NULL;
    struct SwsContext *sws = NULL;
    char crfs[16];
    int ret;

    if (!c444 || !c420 || !apkt || !(m = av_frame_clone(p->first))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    /* color planes only: alpha rides its own stream, identical either way */
    m->format      = p->enc->pix_fmt;
    m->data[3]     = NULL;
    m->linesize[3] = 0;
    if ((ret = encode_full(p->enc, m, c444)) < 0)
        goto end;

    pick = c444;
    if (!(f = av_frame_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    f->format = AV_PIX_FMT_YUV420P;
    f->width  = m->width;
    f->height = m->height;
    if ((ret = av_frame_get_buffer(f, 0)) < 0)
        goto end;
    f->pts       = m->pts;
    f->duration  = m->duration;
    f->pict_type = AV_PICTURE_TYPE_NONE;
    sws = sws_getContext(m->width, m->height, AV_PIX_FMT_YUV444P,
                         f->width, f->height, AV_PIX_FMT_YUV420P,
                         SWS_LANCZOS | SWS_ACCURATE_RND, NULL, NULL, NULL);
    /* a failed 420 attempt is not fatal: the 444 candidate stands */
    if (sws && (e420 = clone_image_enc(p->enc))) {
        sws_scale(sws, m->data, m->linesize, 0, m->height,
                  f->data, f->linesize);
        e420->pix_fmt      = AV_PIX_FMT_YUV420P;
        e420->bit_rate     = 0; /* constant quality, not the 200k default */
        e420->thread_count = p->enc->thread_count;
        snprintf(crfs, sizeof(crfs), "%d", avif_still_crf());
        av_dict_set(&opts, "crf", crfs, 0);
        av_dict_set(&opts, "usage", "allintra", 0);
        av_dict_set(&opts, "still-picture", "1", 0);
        av_dict_set(&opts, "cpu-used", opt.effort > 0 ? "2" : "4", 0);
        av_dict_set(&opts, "row-mt", "1", 0);
        if (avcodec_open2(e420, p->enc->codec, &opts) >= 0 &&
            encode_full(e420, f, c420) >= 0 &&
            c420->size && c444->size > c420->size * 1.35)
            pick = c420;
    }
    if (pick == c420) /* keep the container header honest about subsampling */
        ofmt->streams[p->out_index]->codecpar->format = AV_PIX_FMT_YUV420P;
    pick->stream_index = p->out_index;
    av_packet_rescale_ts(pick, p->enc->time_base,
                         ofmt->streams[p->out_index]->time_base);
    if ((ret = av_interleaved_write_frame(ofmt, pick)) < 0)
        goto end;
    if (p->enc_a) { /* the held frame's alpha plane, like encode_write does */
        if (!(a = alpha_frame(p->first))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        if ((ret = encode_full(p->enc_a, a, apkt)) < 0)
            goto end;
        apkt->stream_index = p->out_index_a;
        av_packet_rescale_ts(apkt, p->enc_a->time_base,
                             ofmt->streams[p->out_index_a]->time_base);
        ret = av_interleaved_write_frame(ofmt, apkt);
    }
end:
    sws_freeContext(sws);
    av_dict_free(&opts);
    avcodec_free_context(&e420);
    av_packet_free(&c444);
    av_packet_free(&c420);
    av_packet_free(&apkt);
    av_frame_free(&m);
    av_frame_free(&f);
    av_frame_free(&a);
    av_frame_free(&p->first);
    return ret;
}

/* A single-frame RGB image: flat-color graphics often compress smaller as
 * lossless WebP than as lossy q80 — and lossless is by definition the best
 * quality — so encode both and keep the smaller file. Photos stay lossy
 * (their lossless attempt just comes out bigger and loses the race). */
static int finish_still(AVFormatContext *ofmt, Pipe *p)
{
    AVCodecContext *sy = NULL, *ll = NULL, *hq = NULL;
    AVDictionary *opts = NULL;
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
    if ((sy = clone_image_enc(p->enc))) {
        char q[32];

        snprintf(q, sizeof(q), "%g", opt.quality < 0 ? 80.0 : opt.quality);
        av_dict_set(&opts, "quality", q, 0);
        av_dict_set(&opts, "compression_level", "6", 0);
        av_dict_set(&opts, "sharp_yuv", "1", 0);
        if (avcodec_open2(sy, p->enc->codec, &opts) < 0)
            avcodec_free_context(&sy); /* fall back to the streaming ctx */
        av_dict_free(&opts);
        opts = NULL;
    }
    if ((ret = encode_full(sy ? sy : p->enc, p->first, lossy)) < 0)
        goto end;

    pick = lossy;
    if ((ll = clone_image_enc(p->enc))) {
        av_dict_set(&opts, "lossless", "1", 0);
        av_dict_set(&opts, "quality", "75", 0); /* = effort when lossless */
        av_dict_set(&opts, "compression_level", "6", 0);
        /* a failed lossless attempt is not fatal: the lossy one stands */
        if (avcodec_open2(ll, p->enc->codec, &opts) >= 0 &&
            encode_full(ll, p->first, less) >= 0 &&
            less->size && less->size < lossy->size)
            pick = less;
    }
    if (pick == less && (hq = clone_image_enc(p->enc))) {
        /* lossless won: re-run the winner at maximum effort (cwebp
         * -lossless -q 100 -m 6) — measured 1-3% smaller for ~20x the
         * lossless encode time, paid only on graphics that already won;
         * photos saw their lossless candidate lose and skip this */
        av_dict_free(&opts);
        opts = NULL;
        av_dict_set(&opts, "lossless", "1", 0);
        av_dict_set(&opts, "quality", "100", 0); /* = max effort */
        av_dict_set(&opts, "compression_level", "6", 0);
        if (avcodec_open2(hq, p->enc->codec, &opts) >= 0 &&
            encode_full(hq, p->first, best) >= 0 &&
            best->size && best->size < less->size)
            pick = best;
    }
    pick->stream_index = p->out_index;
    av_packet_rescale_ts(pick, p->enc->time_base,
                         ofmt->streams[p->out_index]->time_base);
    ret = av_interleaved_write_frame(ofmt, pick);
end:
    av_dict_free(&opts);
    avcodec_free_context(&sy);
    avcodec_free_context(&ll);
    avcodec_free_context(&hq);
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
        return opt.next ? finish_still_next(ofmt, p) : finish_still(ofmt, p);
    return encode_write(ofmt, p, NULL);                 /* flush encoder  */
}

/* Only with two-pass stats does libvpx use alt-ref frames and plan its rate
 * budget ahead, which measures 10-20% smaller files at equal quality (or
 * ~2 dB better quality where the rate caps bind). Inputs that can rewind get
 * a stats run; truly streamed stdin stays single-pass. */
static int first_pass(AVFormatContext *ifmt, int vidx, char **stats)
{
    Pipe p = {};
    AVPacket *pkt = av_packet_alloc();
    int ret;

    if (!pkt)
        return AVERROR(ENOMEM);
    prog.label = "pass 1/2:";
    prog.pct   = -1;
    if ((ret = init_video(&p, ifmt, NULL, vidx, 0, 1, NULL)) < 0)
        goto end;
    while ((ret = av_read_frame(ifmt, pkt)) >= 0) {
        if (pkt->stream_index == vidx)
            ret = decode_packet(NULL, &p, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF)
        goto end;
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

/* the stats run consumed the input; rewind by reopening, which covers both
 * real files and the spooled/slurped stdin paths behind the custom pb */
static int reopen_input(const char *in_path, AVFormatContext **ifmt, StdinIO *io)
{
    int64_t pos;
    int ret;

    avformat_close_input(ifmt); /* custom pb (io->pb) survives this */
    if (io->pb) {
        if ((pos = avio_seek(io->pb, 0, SEEK_SET)) < 0)
            return (int)pos;
        if (!(*ifmt = avformat_alloc_context()))
            return AVERROR(ENOMEM);
        (*ifmt)->pb = io->pb;
        ret = avformat_open_input(ifmt, "", NULL, NULL);
    } else {
        ret = avformat_open_input(ifmt, in_path, NULL, NULL);
    }
    if (ret < 0)
        return ret;
    return avformat_find_stream_info(*ifmt, NULL);
}

static int webmify_run(const char *in_path, const char *out_path)
{
    AVFormatContext *ifmt = NULL, *ofmt = NULL;
    Pipe video = {}, audio = {};
    StdinIO io = {};
    AVPacket *pkt = NULL;
    AVDictionary *muxopts = NULL;
    char *stats = NULL;
    int ret, vidx, aidx, image, spool_out = 0;

    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(log_cb);

    if (!strncmp(in_path, "pipe:", 5))
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
         ifmt->pb && (ifmt->pb->seekable & AVIO_SEEKABLE_NORMAL))) {
        peek_first_frame(ifmt, vidx, image && opt.next);
        if ((ret = reopen_input(in_path, &ifmt, &io)) < 0)
            goto end;
        vidx = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        aidx = image ? -1
             : av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (vidx < 0) {
            ret = AVERROR_STREAM_NOT_FOUND;
            goto end;
        }
    }

    if (!image && opt.effort >= 0 &&
        ifmt->pb && (ifmt->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        for (unsigned i = 0; i < ifmt->nb_streams; i++)
            if ((int)i != vidx)
                ifmt->streams[i]->discard = AVDISCARD_ALL;
        if ((ret = first_pass(ifmt, vidx, &stats)) < 0)
            goto end;
        if ((ret = reopen_input(in_path, &ifmt, &io)) < 0)
            goto end;
        vidx = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        aidx = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (vidx < 0) {
            ret = AVERROR_STREAM_NOT_FOUND;
            goto end;
        }
    } else if (!image && opt.effort >= 0) {
        av_log(NULL, AV_LOG_WARNING,
               "input is not seekable: encoding in a single pass "
               "(two-pass saves 10-20%% bandwidth at the same quality)\n");
    } /* --fast skips the stats pass on purpose: single-pass, no warning */

    /* let the demuxer drop packets of streams we won't transcode */
    for (unsigned i = 0; i < ifmt->nb_streams; i++)
        if ((int)i != vidx && (int)i != aidx)
            ifmt->streams[i]->discard = AVDISCARD_ALL;

    if ((ret = avformat_alloc_output_context2(&ofmt, NULL,
                                              image ? (opt.next ? "avif" : "webp")
                                                    : "webm",
                                              out_path)) < 0)
        goto end;

    if (!image) {
        prog.label = stats ? "pass 2/2:" : "encoding:";
        prog.pct   = -1;
    }
    if ((ret = init_video(&video, ifmt, ofmt, vidx, image, stats ? 2 : 0, stats)) < 0)
        goto end;
    if (aidx >= 0 && (ret = init_audio(&audio, ifmt, ofmt, aidx)) < 0)
        goto end;

    if (!(pkt = av_packet_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* the avif muxer (mov family) seeks back to patch item offsets/sizes;
     * for stdout, write into memory and dump the finished file at the end
     * (AVIF stills are small) */
    spool_out = image && opt.next && !strncmp(out_path, "pipe:", 5);
    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        ret = spool_out ? avio_open_dyn_buf(&ofmt->pb)
                        : avio_open(&ofmt->pb, out_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "cannot create '%s': %s\n", out_path,
                   err2str(ret));
            goto end;
        }
    }
    if (image) /* loop animations forever, like GIF (both the webp and the
                * avif muxer take a loop count, 0 = infinite) */
        av_dict_set(&muxopts, "loop", "0", 0);
    else /* faststart for WebM: put the seek index up front so browsers can
          * seek without fetching the file tail (skipped on pipes, where the
          * muxer cannot seek and writes no index at all) */
        av_dict_set(&muxopts, "cues_to_front", "1", 0);
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
    av_freep(&stats);
    av_packet_free(&pkt);
    avformat_close_input(&ifmt);
    close_stdin_io(&io);
    if (ofmt) {
        if (spool_out && ofmt->pb) {
            uint8_t *buf = NULL;
            int n = avio_close_dyn_buf(ofmt->pb, &buf);

            ofmt->pb = NULL;
            if (ret >= 0 && (ret = write_all(STDOUT_FILENO, buf, n)) < 0)
                av_log(NULL, AV_LOG_ERROR, "cannot write output: %s\n",
                       err2str(ret));
            av_free(buf);
        }
        if (!(ofmt->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
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
            "webmify: transcode any popular video to VP9/Opus WebM,\n"
            "         or any popular image to WebP (auto-detected)\n"
            "usage: webmify [options] <input> <output>   ('-' = stdin/stdout)\n"
            "  -q, --quality <0-10>   target quality, higher is better\n"
            "                         (default: 8 for images; video picks the\n"
            "                         classic 480p look at the smallest size)\n"
            "  --next                 output the next-gen formats: video\n"
            "                         becomes AV1/Opus WebM, images become\n"
            "                         AVIF (animated GIF -> animated AVIF).\n"
            "                         -q buys the same look as the default\n"
            "                         formats — only the file gets smaller\n"
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
            "                         --best runs the slowest searches and\n"
            "                         spools piped video so it can two-pass\n"
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
            fprintf(stderr, "webmify: --max fps must be 1-240, got '%s'\n", at + 1);
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
    fprintf(stderr, "webmify: --max box sides must be 1-16384, got '%s'\n", arg);
    return -1;
bad:
    fprintf(stderr, "webmify: --max expects [HxW | S][@F], got '%s'\n", arg);
    return -1;
}

int main(int argc, char **argv)
{
    enum { OPT_FAST = 1000, OPT_BEST, OPT_NEXT, OPT_VERSION };
    static const struct option longopts[] = {
        { "quality", required_argument, NULL, 'q' },
        { "max",     required_argument, NULL, 'm' },
        { "next",    no_argument,       NULL, OPT_NEXT },
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
            printf("webmify %s (FFmpeg %s)\n", WEBMIFY_VERSION, av_version_info());
            return 0;
        case 'q':
            opt.quality = strtod(optarg, &end);
            if (*end || end == optarg || opt.quality < 0 || opt.quality > 10) {
                fprintf(stderr, "webmify: quality must be 0-10, got '%s'\n", optarg);
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
        case OPT_FAST:
        case OPT_BEST:
            if (opt.effort) {
                fprintf(stderr, "webmify: --fast and --best are mutually exclusive\n");
                return 2;
            }
            opt.effort = c == OPT_FAST ? -1 : 1;
            break;
        default:
            return usage(stderr, 2);
        }
    }
    if (argc - optind != 2)
        return usage(stderr, 2);
    /* the '-' convention lives in the ffmpeg CLI, not libavformat */
    const char *in  = strcmp(argv[optind], "-") ? argv[optind] : "pipe:0";
    const char *out = strcmp(argv[optind + 1], "-") ? argv[optind + 1] : "pipe:1";
    return webmify_run(in, out);
}
